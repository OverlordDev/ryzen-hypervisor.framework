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
- **VirtualBox 7.2.22** (0.7.3, experimental): see "VirtualBox 7.x" below.
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
Before the first patch the kext logs the process's code-signing flags (`rhv-info log | grep "client fix-ups"`): `signing flags of com.docker.hyperkit: …, enforcement off` is what a machine with AMFI relaxed shows; `enforcement ON` comes with a warning, because on macOS 26.7.1 a HyperKit with enforcement on was killed (`Code Signature Invalid`) right after the patch (issue 1).
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
and after a resume. The handler and its two flags are found in `libMonitor.dylib` by their bytes (`rhv_find_monitor_kbd` in `shim/rhv_patch.c`: the start of the handler, then the instructions that test and set the flags, whose addresses are read from their rip-relative operands), not by address, and only if the code is there exactly once; otherwise the monitor is left alone and `~/.ryzenhv/prl-patch.log` says `POST code handler: pattern not found`. Tested only on 26.5.0 (57613); a build with different code in that function needs a new pattern. `RYZENHV_NO_KBD_ARM=1` switches it off.

Limits: only Windows 10 was tried. Since 0.7.3 the kext offers the TPR shadow (see below), which cut the Windows boot from about 90 s to about 20 s on the author's
machine (the guest's tools answer after ~19 s; before, every `CR8` access of Windows was an exit). Suspending and resuming the VM works **when the Parallels Desktop app is running**;
with only its background service (`prl_client_app --start-services`, what runs after a restart until you open the app) a suspend hung three times in the author's tests (the
state files were written, the VM stayed "suspending", one thread stuck in `close()` inside macOS without any kext frame in its stack, and the process could not exit until a reboot; ROADMAP item 11); keep the app open, or shut the VM down instead. Sleep/wake of the Mac, 3D
acceleration, snapshots and long runs are untested. `kern.hv_support` is 0 after every reboot: `./rhv autostart install` (0.7.2, a launchd service that turns it on once
the kext is up; also a button in the app) does it for you (seen working on two real boots), otherwise run `rhv-info hv sysctl 1` before starting a VM.

## The TPR shadow (0.7.3)

Windows changes its interrupt priority through `CR8` all the time. Without the VMX "use TPR shadow" control every such access is a VM exit, and a Windows boot is mostly those
exits (~70k to ~200k a second); VirtualBox's Windows Setup did not get past its first screen in minutes. The kext now offers the control (VMX capability bit 21 of the
primary processor controls, which Apple's framework passes through) to every client except HyperKit's, keeps the guest's CR8 in the core's `V_TPR`, and lets the
client read and write it as `HV_X86_TPR`, the 8-bit APIC task priority (`>>4` into `V_TPR`, `<<4` out). `hv_vmx_vcpu_set_apic_address` answers success (the core never
reads the virtual-APIC page). Switch it off with the boot-arg `rhv_tpr_shadow=0`, or for the VMs created from then on with `rhv-info hv tpr 0` (`rhv-info hv` shows
whether it is on). For VMX control words the kext also tells a client the real limits (the bits that must be 1 and the ones that may be) through thread trap 12, as
`hv_vmx_read_capability` does; HyperKit keeps its old answer.

## VirtualBox 7.x (0.7.3, experimental)

VirtualBox 7 has no kernel driver on macOS: it runs its VMs through Hypervisor.framework (its "NEM" back end), so it uses the native front end like Docker and Parallels. Tested
with 7.2.22: an EFI VM with 2 vCPUs boots the Windows 10 installer from an ISO and reaches the disk page in about a minute. It needs `rhv_hv=1`, `kern.hv_support` = 1 and the
TPR shadow (on by default). Its log says `NEM: Failed to create ASID` (`hv_vm_space_*` are not implemented, VirtualBox goes on without) and `Turtle execution mode` (VirtualBox prints that for its NEM engine
in general, it is not a consequence of the missing spaces). It makes about six kernel calls per exit. A Windows 10 installation onto a disk was run to the end by a user (reported, not reproduced by the author). **VirtualBox 6.x is a different story**: it loads its own driver, which wants to switch AMD-V
on itself and stops with `VERR_SVM_IN_USE` while RyzenHV keeps `EFER.SVME` on every core (issue 2). 7.x has the override `VBOX_HWVIRTEX_IGNORE_SVM_IN_USE`; it has not been tried on 6.x.

## kern.hv_support

Some clients (QEMU, Parallels) look at `sysctl kern.hv_support`. The kext can write the kernel variable behind it:
`rhv-info hv sysctl 1` (and `0` to put it back). The address is decoded from the kernel's own `hv_get_support()` and
checked through `sysctlbyname` before and after the write. The boot-arg `rhv_hv_sysctl=1` does the same at boot; **that boot
path has not been tested**, use the command.

## Known limits, please read

- Only tested on macOS 15.8.1, Ryzen 7 5800H, OpenCore, Docker Desktop 4.97.0 and Parallels 26.5.0 with a Windows 10 guest. macOS 26 (Tahoe) has not been tried
  by the author; issue 1 (26.7.1) is what the check of `EFER.SVME` before every `VMRUN` (0.7.1) and the rescan above (0.7.4) are for; neither is tested on Tahoe.
- **CPUs that are not online when the kext starts** (0.7.4; issue 1, a Ryzen 9 5900X on macOS 26.7.1 logged `cpus=1`): the CPU count is taken once, in `start()`. `svm::rescan()` runs a
  rendezvous again, probes the cores it has not seen, gives them a host save area and turns SVM on, if `enableAll()` ran before. It is called at VM and vCPU creation and, once per run, in `VmRun`
  when `ensureThisCpu()` says the core was never seen (-2) or SVM is not on everywhere. `rhv-info hv svmlimit N` makes the SVM code forget the cores from N up (the reporter's state) and
  `rhv-info hv rescan` brings them back; the log line `rescan: the rendezvous reached N CPUs` says whether the late cores answer at all. Tested on the 5800H with `svmlimit 1`:
  `rescan`, `hvf-smoke`, Docker Desktop, a Parallels Windows 10 cold boot (21 s), a VirtualBox VM, and `svmlimit 1` while the Parallels VM ran (the guest went on). Not tested on Tahoe.
- Some gaps in the kernel code are known and not closed: a race between `hv_vm_map` and `hv_vm_destroy` from two threads of one process, a
  stale vCPU target after `exec()` with a live VM, no entitlement check (any local process can create a VM; with `rhv_allow_users=1`
  every user can). Do not use it on a machine where untrusted local code runs.
- The kext patches the memory of the process that calls it. That is the whole point and also the reason to keep it opt-in.
- `hv_vcpu_run_until`'s deadline is ignored (Docker, Parallels and VirtualBox use `hv_vcpu_run` or the Mac-side loop), the in-kernel APIC and `hv_vm_space_*` are not offered; sleep/wake and long runs are untested.
- Keep a working EFI without the kext. A kernel extension that panics needs a way back.

## Going back

Remove `rhv_hv=1` (and `rhv_hv_sysctl=1`) from the boot-args and reboot: the kext then behaves exactly like 0.6.
