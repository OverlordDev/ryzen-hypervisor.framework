# Docker Desktop on a Ryzen

Docker Desktop for Mac (Intel) runs on an AMD CPU through RyzenHV: the real app, the real `docker` CLI, Compose, builds, published
ports. Tested on a Ryzen 7 5800H, macOS 15.8.1, **Docker Desktop 4.97.0 (build 244584)**: `hello-world`, an image build, nginx with
`-p 8080:80` reachable from the Mac, a 16-way CPU load in a container, three start / hard kill / restart cycles.

## Why it works

Docker Desktop on an Intel Mac can use two VM engines: Apple's Virtualization framework (a system service, out of our reach) and
**HyperKit**, which is still shipped as `Contents/Resources/bin/com.docker.hyperkit`. HyperKit calls the *core* Hypervisor.framework
API only (`hv_vm_*`, `hv_vcpu_*`, `hv_vmx_vcpu_read/write_vmcs`, `hv_vmx_read_capability`) and emulates the APIC, IOAPIC, PIC, PIT,
HPET and virtio devices itself. That is exactly what the shim implements. A few things stood in the way:

| problem | solution |
|---|---|
| HyperKit refuses a CPU whose CPUID vendor is not `GenuineIntel` (an instruction, it cannot be intercepted) | one conditional jump becomes nops, patched **on disk** before HyperKit is signed (below); a copy that could not be patched falls back to the shim patching it in memory |
| HyperKit compares the guest linear address of an EPT violation with the one it decodes; AMD does not report that address | the "equal" branch of the comparison becomes unconditional, the same way |
| HyperKit insists on NMI-window exiting and virtual NMIs in the VMX capabilities | the shim offers both to HyperKit only and reports the NMI window as open (`rhv_vcpu_run`) |
| the guest (Linux) trusts AMD-only CPUID bits HyperKit does not emulate: MWAITX (the delay loop takes a #UD), IBPB/IBRS (`wrmsr 0x49` is a #GP) | the shim clears them in the answers HyperKit writes (`cpuid_hyperkit_fixup`) |
| HyperKit starts its vCPU threads concurrently and asserts that `hv_vcpu_create` returns the thread's own index | the shim reads the thread name (`vcpu:<n>`) and hands out that slot; in native mode the kext parses the same name out of the caller's stack behind the `hv_vcpu_create` arguments and honours it (`rhv_vcpuhint=0` restores the old next-free-slot handout) |
| the backend (`com.docker.backend`) quits with "Hypervisor check failed" unless `hv_vm_create()` works | the backend runs with the shim loaded too |

Both patches find their place by the bytes around them (`shim/rhv_patch.c` and `tools/rhv-hkpatch` share the same engine, tested by `tests/patch_test.c` and `tests/hkpatch_test.c`), not by an address, and are applied only when the pattern occurs exactly once; if it does not fit, HyperKit is left alone and says so in `~/.ryzenhv/hyperkit.log`.

**Patching on disk instead of in memory** is what keeps AMFI out of the picture: `install` (and `install-native`) copies HyperKit to `com.docker.hyperkit.real`, writes the two fixes into the copy's own bytes (`rhv-hkpatch`), and signs the copy only afterwards -- so the signature describes the patched bytes and every page of the running process validates. Nothing modifies HyperKit while it runs; the shim and the kext find their patterns in the patched form and write nothing ("already in place"). Code signing stays on for Firefox and everything else, and `amfi_get_out_of_my_way=0x1` is not needed. Only when the patterns do not fit a build does the copy stay unpatched and the in-memory patching take over, which then needs the AMFI boot-arg or the kext's per-process signing relax (`rhv-info log` says which happened). HyperKit is a copy (`com.docker.hyperkit.real`), ad-hoc signed with the hypervisor entitlement only: the app sandbox of Docker's own signature would keep it away from the kext. Docker's original files stay next to it as `.orig` / `.real`.

## Install

In **native** mode (the kext is the hypervisor provider, `kern.hv_support` on, e.g. after `./rhv kext --native --no-amfi`) HyperKit talks to the system Hypervisor.framework and no shim is loaded; the disk patch is required there, because without it the kext would patch HyperKit in memory, and on a kernel where the per-process signing relax does not decode (Tahoe, `rhv-info log` shows "kernel not as expected") that ends in "Code Signature Invalid" under load. In **shim** mode the same disk patch applies, and the shim then has nothing left to patch in memory either.

From the release bundle:

```bash
./rhv docker-desktop get      # downloads Docker Desktop 4.97.0 (630 MB) from docker.com and copies it to /Applications (skip if you have it)
./rhv docker-desktop          # patches HyperKit on disk and re-signs it, wraps HyperKit (and in shim mode the backend), selects the HyperKit engine
open -a Docker                # first start: Docker may ask for your password to install its network helper
docker run --rm hello-world
```

From a checkout: `make shim tools`, then `./scripts/docker-desktop-shim.sh install` (shim mode) or `./scripts/docker-desktop-shim.sh install-native` (native mode, from the kext-serving-Hypervisor.framework setup).

What the script changes, all inside `/Applications/Docker.app` and your home folder (no sudo when you own the app):

- `Contents/Resources/bin/com.docker.hyperkit` becomes a wrapper; the patched-and-signed binary is `com.docker.hyperkit.real`, Docker's own is `.orig`.
- `Contents/MacOS/com.docker.backend` becomes a wrapper in shim mode; Docker's binary is `com.docker.backend.real` (native mode leaves the backend as Docker ships it and restores it if an earlier shim install wrapped it).
- `~/Library/Group Containers/group.com.docker/settings-store.json`: `UseVirtualizationFramework` is set to `false`.
  That file is JSON, not a plist; it is edited with osascript/perl (`json_set_bool` in the scripts). A plain `plutil -replace`
  fails on macOS 26 with `Conversion to OpenStep format is not supported`: plutil reads the JSON through its OpenStep plist
  reader and OpenStep is the one plist format CoreFoundation cannot write.
- `~/.ryzenhv/lib/libryzenhv.dylib`: the shim the wrappers load.

Undo: `./rhv docker-desktop undo` (or `./scripts/docker-desktop-shim.sh uninstall`).

## Limits

- Tested on Docker Desktop **4.97.0 (build 244584)**. The patches are found by their surrounding bytes, so a build with the same code around those branches works too; any other build is left unpatched and reported in `~/.ryzenhv/hyperkit.log`.
  Docker Desktop updates replace the app, so run the install again afterwards (and use `get` to go back to 4.97.0 if a newer build
  ships a different HyperKit). Turn automatic updates off in Docker Desktop > Settings > Software updates.
- The VM defaults to all cores and 8 GB (Docker Desktop > Settings > Resources). Idle it uses about 0.5% CPU and 370 MB.
- A HyperKit that was killed leaves its pid file behind, and the next one then fails with "Failed to set up pidfile" when the number belongs to some other process (pid numbers are reused within hours on a busy machine). The wrapper removes a stale one before starting -- it only trusts the file when `ps` says the process really is a HyperKit -- and the `hv-sysctl-daemon.sh` launchd service sweeps all users' pid files at boot and every 5 minutes, so a kill mid-session cannot wedge the next start either.
  HyperKit's own messages are appended to `~/.ryzenhv/hyperkit.log`.
- Not tried: Kubernetes, Docker Desktop extensions, sleep/wake, running together with Parallels, other macOS versions.
- Docker Desktop is Docker's product under Docker's licence. This repository contains nothing of it: the script patches the copy on your machine.
