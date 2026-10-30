# Native mode (experimental): the kext is the hypervisor provider

Until 0.6 the project worked through a **shim**: a library injected into the application (`DYLD_INSERT_LIBRARIES`) that
replaced Hypervisor.framework's calls. In native mode the kext registers with xnu in the place of Apple's AppleHV (the
`hv_support_kext` interface the kernel offers), so the **real, unmodified Hypervisor.framework** talks to it. No injected
library, no wrappers, no re-signed application files.

It is **opt-in** and experimental: add `rhv_hv=1` to the boot-args (`./rhv kext --native` does it for you).

## What works

- **Docker Desktop 4.97.0 (Intel build)** with Docker's own, unmodified files: the engine starts, `docker run hello-world`,
  30 containers at once, builds, pulls, 16 busy vCPUs, disk and memory tests passed (the author's Ryzen 7 5800H, 16 threads).
  `./rhv docker-desktop` only sets `UseVirtualizationFramework=false` in Docker's settings and removes the shim wrappers if
  an earlier version installed them.
- The public Hypervisor.framework API tests (`tools/hvf-smoke`): VM and vCPU lifecycle, memory mapping, register and
  VMCS access, MSRs, FP state, interrupts from another thread.
- QEMU with `-accel hvf` boots Linux without the shim, if `kern.hv_support` is switched to 1 (see below).
- **Parallels Desktop 26.5.0 (build 57613)** (0.7.1, experimental): Windows 10 (LTSC 1809) boots to the desktop on 1 and on 4 vCPUs,
  Parallels Tools, the network and `prlctl exec` work. Set it up with `./rhv parallels native`. See "Parallels in native mode" below.

## How the application's CPU checks are handled

HyperKit refuses a CPU whose CPUID vendor is not `GenuineIntel` and compares a guest linear address that AMD does not report.
Both are single branches in HyperKit. When a process called `com.docker.hyper...` creates its VM, the kext finds those two
branches **by the bytes around them** in that process's executable memory and changes them **in memory, once**
(`kext/Source/HvPatch.cpp`). Nothing on disk changes, nothing is signed again. If the bytes are not found exactly once (another Docker
Desktop build) the process is left alone and `rhv-info log` says so (`rhv-info log | grep "client fix-ups"`).
This needs AMFI relaxed on the machine (`amfi_get_out_of_my_way=0x1`), like the shim. `rhv_clientpatch=0` switches the engine off.
HyperKit's CPUID answers are also cleaned of AMD-only features by the kext (only for HyperKit).

## Parallels in native mode (0.7.1, experimental; the keyboard and the boot service are new in 0.7.2)

`./rhv parallels native` (needs the kext booted with `rhv_hv=1` and `kern.hv_support` = 1). Parallels then runs on Apple's own
Hypervisor.framework and the kext: the only `hv_*` calls with a thin wrapper are `hv_vcpu_run` and `hv_vcpu_run_until` (for the keyboard, below). What is still done outside the kext is the CPU-vendor check: Parallels runs it
while it builds its VM configuration, *before* its first `hv_vm_create`, so the kext's client patcher (which runs at `hv_vm_create`) is too late.
A small stand-in library (`shim/hijack_patchonly.c`, installed like the shim, in a private copy of the VM process) patches that one branch in
memory, in `prl_vm_app` and in `libMonitor.dylib` (also after a guest reset, when Parallels loads it again). Log: `~/.ryzenhv/prl-patch.log`.

What the kext does for a Parallels VM (`kext/Source/HvFrontend.cpp`), the same things the shim did:
- CPUID: the monitor's answer is cleaned on the way back into the guest. The hypervisor bit and the Hyper-V leaves are hidden (Windows then does not use
  the hypercall page, which Parallels fills with the Intel `VMCALL`: a `#UD` and a triple fault on AMD), and leaf 4 and the AMD cache/core leaves give a
  topology that Windows Home also runs on several vCPUs.
- `HV_X86_TPR` round trip: the monitor writes the 8-bit task priority and reads it back to decide whether it may inject an interrupt and what to
  answer to the guest's `CR8` reads; the core's `V_TPR` is the 4-bit class (`>>4` in, `<<4` out). Without it Windows hangs at the boot logo.
- the LAPIC page (GPA `0xFEE00000`) is not mapped, so the guest's APIC accesses reach the monitor; XSAVE images with `XSTATE_BV` wider than `XCR0` are
  cut to what `XCR0` allows; the commpage that `hv_vmx_tsc_clock` reads is filled in.

The keyboard (0.7.2; in 0.7.1 it did not work): the monitor drops keys until the guest writes 0 to the POST code port `0x80` (Parallels' own firmware does under Parallels' own hypervisor, not
here). The stand-in library therefore replaces `hv_vcpu_run` and `hv_vcpu_run_until` (the monitor uses the second) in libMonitor's imports only; after each run it
calls the monitor's own port handler with 0, on the first guest access to the keyboard controller (ports `0x60`/`0x64`) and, as a safety net for a VM restored from
a saved state, once a second while the monitor's flag is still clear. Tested on Windows 10: keys sent with `prlctl send-key-event` reach the guest after a cold boot
and after a resume. The offsets belong to Parallels 26.5.0 (57613); another build is left alone (the function prologue is checked). `RYZENHV_NO_KBD_ARM=1` switches it off.

Limits: only Windows 10 was tried. TPR shadow is not offered, so every `CR8` access is an exit and a boot is
about two to three times slower than with the shim (about 90 s to the desktop on the author's machine). Suspending and resuming the VM works (tested a few times);
sleep/wake of the Mac, 3D acceleration, snapshots and long runs are untested. `kern.hv_support` is 0 after every reboot: `./rhv autostart install` (0.7.2, a launchd service that turns it on once the kext is up; also a button in the app) does it for you,
otherwise run `rhv-info hv sysctl 1` before starting a VM. The service has been installed and run on the author's machine; a real boot with it had not been observed at the time of writing.

## kern.hv_support

Some clients (QEMU, Parallels) look at `sysctl kern.hv_support`. The kext can write the kernel variable behind it:
`rhv-info hv sysctl 1` (and `0` to put it back). The address is decoded from the kernel's own `hv_get_support()` and
checked through `sysctlbyname` before and after the write. The boot-arg `rhv_hv_sysctl=1` does the same at boot; **that boot
path has not been tested**, use the command.

## Known limits, please read

- Only tested on macOS 15.8.1, Ryzen 7 5800H, OpenCore, Docker Desktop 4.97.0 and Parallels 26.5.0 with a Windows 10 guest. macOS 26 (Tahoe) has not been tried
  by the author; a report of a panic on 26.7.1 is what 0.7.1's check of `EFER.SVME` before every `VMRUN` is for (issue 1; not tested on Tahoe).
- Some gaps in the kernel code are known and not closed: a race between `hv_vm_map` and `hv_vm_destroy` from two threads of one process, a
  stale vCPU target after `exec()` with a live VM, no entitlement check (any local process can create a VM; with `rhv_allow_users=1`
  every user can). Do not use it on a machine where untrusted local code runs.
- The kext patches the memory of the process that calls it. That is the whole point and also the reason to keep it opt-in.
- `hv_vcpu_run_until`'s deadline is ignored (Docker and Parallels use `hv_vcpu_run`), TPR shadow and the in-kernel APIC are not offered; sleep/wake and long runs are untested.
- Keep a working EFI without the kext. A kernel extension that panics needs a way back.

## Going back

Remove `rhv_hv=1` (and `rhv_hv_sysctl=1`) from the boot-args and reboot: the kext then behaves exactly like 0.6.
