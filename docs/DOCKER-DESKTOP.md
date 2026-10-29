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
| HyperKit refuses a CPU whose CPUID vendor is not `GenuineIntel` (an instruction, it cannot be intercepted) | one conditional jump in a patched copy of the binary becomes nops |
| HyperKit compares the guest linear address of an EPT violation with the one it decodes; AMD does not report that address | the "equal" branch of the comparison becomes unconditional |
| HyperKit insists on NMI-window exiting and virtual NMIs in the VMX capabilities | the shim offers both to HyperKit only and reports the NMI window as open (`rhv_vcpu_run`) |
| the guest (Linux) trusts AMD-only CPUID bits HyperKit does not emulate: MWAITX (the delay loop takes a #UD), IBPB/IBRS (`wrmsr 0x49` is a #GP) | the shim clears them in the answers HyperKit writes (`cpuid_hyperkit_fixup`) |
| HyperKit starts its vCPU threads concurrently and asserts that `hv_vcpu_create` returns the thread's own index | the shim reads the thread name (`vcpu:<n>`) and hands out that slot |
| the backend (`com.docker.backend`) quits with "Hypervisor check failed" unless `hv_vm_create()` works | the backend runs with the shim loaded too |

The patched HyperKit is a copy (`com.docker.hyperkit.real`), ad-hoc signed with the hypervisor entitlement only: the app sandbox of
Docker's own signature would keep it away from the kext. Docker's original files stay next to it as `.orig` / `.real`.

## Install

You need the kext running (`docs/RYZENTOSH.md`, boot-args `rhv_enable_svm=1 rhv_allow_users=1 amfi_get_out_of_my_way=0x1`). AMFI has to
be relaxed because the shim is loaded into signed, hardened Docker binaries.

From the release bundle:

```bash
./rhv docker-desktop get      # downloads Docker Desktop 4.97.0 (630 MB) from docker.com and copies it to /Applications (skip if you have it)
./rhv docker-desktop          # patches HyperKit, wraps HyperKit and the backend, selects the HyperKit engine in the settings
open -a Docker                # first start: Docker may ask for your password to install its network helper
docker run --rm hello-world
```

From a checkout: `make shim`, then `./scripts/docker-desktop-shim.sh install` (it looks for `build/libryzenhv.dylib`).

What the script changes, all inside `/Applications/Docker.app` and your home folder (no sudo when you own the app):

- `Contents/Resources/bin/com.docker.hyperkit` becomes a wrapper; the patched binary is `com.docker.hyperkit.real`, Docker's own is `.orig`.
- `Contents/MacOS/com.docker.backend` becomes a wrapper; Docker's binary is `com.docker.backend.real`.
- `~/Library/Group Containers/group.com.docker/settings-store.json`: `UseVirtualizationFramework` is set to `false`.
- `~/.ryzenhv/lib/libryzenhv.dylib`: the shim the wrappers load.

Undo: `./rhv docker-desktop undo` (or `./scripts/docker-desktop-shim.sh uninstall`).

## Limits

- Only the HyperKit in Docker Desktop **4.97.0 (build 244584)**: the patches are checked byte for byte and refused on any other build.
  Docker Desktop updates replace the app, so run the install again afterwards (and use `get` to go back to 4.97.0 if a newer build
  ships a different HyperKit). Turn automatic updates off in Docker Desktop > Settings > Software updates.
- The VM defaults to all cores and 8 GB (Docker Desktop > Settings > Resources). Idle it uses about 0.5% CPU and 370 MB.
- A HyperKit that was killed leaves its pid file behind; the wrapper removes a stale one before starting, so a force-quit is harmless.
  HyperKit's own messages are appended to `~/.ryzenhv/hyperkit.log`.
- Not tried: Kubernetes, Docker Desktop extensions, sleep/wake, running together with Parallels, other macOS versions.
- Docker Desktop is Docker's product under Docker's licence. This repository contains nothing of it: the script patches the copy on your machine.
