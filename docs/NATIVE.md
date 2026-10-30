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

## How the application's CPU checks are handled

HyperKit refuses a CPU whose CPUID vendor is not `GenuineIntel` and compares a guest linear address that AMD does not report.
Both are single branches in HyperKit. When a process called `com.docker.hyper...` creates its VM, the kext finds those two
branches **by the bytes around them** in that process's executable memory and changes them **in memory, once**
(`kext/Source/HvPatch.cpp`). Nothing on disk changes, nothing is signed again. If the bytes are not found exactly once (another Docker
Desktop build) the process is left alone and `rhv-info log` says so (`rhv-info log | grep "client fix-ups"`).
This needs AMFI relaxed on the machine (`amfi_get_out_of_my_way=0x1`), like the shim. `rhv_clientpatch=0` switches the engine off.
HyperKit's CPUID answers are also cleaned of AMD-only features by the kext (only for HyperKit).

## kern.hv_support

Some clients (QEMU, Parallels) look at `sysctl kern.hv_support`. The kext can write the kernel variable behind it:
`rhv-info hv sysctl 1` (and `0` to put it back). The address is decoded from the kernel's own `hv_get_support()` and
checked through `sysctlbyname` before and after the write. The boot-arg `rhv_hv_sysctl=1` does the same at boot; **that boot
path has not been tested**, use the command.

## Known limits, please read

- Only tested on macOS 15.8.1, Ryzen 7 5800H, OpenCore, Docker Desktop 4.97.0. Parallels is **not** native yet (use the shim: `./rhv parallels`).
- Some gaps in the kernel code are known and not closed: a race between `hv_vm_map` and `hv_vm_destroy` from two threads of one process, a
  stale vCPU target after `exec()` with a live VM, no entitlement check (any local process can create a VM; with `rhv_allow_users=1`
  every user can). Do not use it on a machine where untrusted local code runs.
- The kext patches the memory of the process that calls it. That is the whole point and also the reason to keep it opt-in.
- `hv_vcpu_run_until`'s deadline is ignored, TPR shadow and the in-kernel APIC are not offered; sleep/wake and long runs are untested.
- Keep a working EFI without the kext. A kernel extension that panics needs a way back.

## Going back

Remove `rhv_hv=1` (and `rhv_hv_sysctl=1`) from the boot-args and reboot: the kext then behaves exactly like 0.6.
