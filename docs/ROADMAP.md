# Roadmap

Each phase has a verifiable acceptance criterion. We do not move on until it is met on hardware.

## Phase 0: diagnostics

- [x] Repository skeleton, ABI, unit tests of the pure functions
- [x] Kext: CPUID/MSR probe on all cores, IOService, IOUserClient
- [x] Kext: opt-in enabling of `EFER.SVME` + `VM_HSAVE_PA` on every core
- [x] The `rhv-info` tool (info, log, counters, log level, self-test)
- [x] Kernel ring log with levels, event counters, `RHV_SYNC` before dangerous operations
- [x] Emergency switch `-rhv_off`, the `rhv_selftest` self-test
- [x] SVM disable/enable on sleep and wake (not verified on hardware)
- [x] The `collect-debug.sh` and `install-to-efi.sh` scripts
- [x] **Build on a Ryzentosh**: `make test tools shim kext` passes on Sequoia 15.8.1 (SDK 26.2), the kext's symbols resolve against kernel 24.6 (`kmutil libraries`)
- [x] Load the kext, `rhv-info` shows SVM supported and NPT (5800H: family 0x19 model 0x50, 16 cores, 32768 ASIDs)
- [x] With `rhv_enable_svm=1` the kext enables SVM on all cores: `selftest: 16 of 16 CPUs ok`, the system is stable. An hour under load and `kextunload` are not verified yet

**Done when:** `rhv-info` shows `active on N` = the number of cores, and the system lives for an hour under load.

## Phase 1: the first guest (done on hardware)

Details and instructions: `docs/PHASE1.md`.

- [x] VMCB with static offset checks (`tests/vmcb_layout.cpp`)
- [x] The `VMRUN` / `#VMEXIT` loop, GIF, host and guest `VMSAVE`/`VMLOAD` (`SvmRun.S`)
- [x] A real-mode guest; `HLT`, `OUT`, `CPUID` exits
- [x] Exits are handled in userland through the IOUserClient (`VmRun`, `VmGetRegs`, `VmSetRegs`)
- [x] A minimal NPT for 1 MiB of guest memory (moved up from phase 2: without it the guest would see the host's physical memory)
- [x] A ~10 ms time slice so that the process with the guest can be interrupted
- [x] **Run on hardware:** `sudo rhv-guest-test` prints `PASS` (24 exits, vmInvalidExits 0)
- [ ] Check the VMCB offsets and bits against AMD APM Vol. 2
- [ ] 50 runs in a row without a panic and without `vmInvalidExits`

**Done when:** the test guest prints a string via `OUT`, gets an answer to `CPUID` and stops on `HLT`.

## Phase 2: memory (done on hardware)

Details and instructions: `docs/PHASE2.md`.

- [x] NPT with 4 KiB pages for arbitrary guest addresses up to 2^48, plain C++ (`Npt.hpp`), host tests with ASan/UBSan, allocator failure injection and an independent table walk
- [x] `VmMap` / `VmUnmap` / `VmProtect` (the analogues of `hv_vm_map` / `hv_vm_unmap` / `hv_vm_protect`), including partial unmap
- [x] Pinning of process pages (`IOMemoryDescriptor::prepare`), unpinning on unmap and when the VM dies
- [x] NPF with a reason (`rhv_npf_*`), counters, a VM without built-in memory (`RHV_VMCREATE_NO_RAM`)
- [x] **Run on hardware:** `sudo rhv-map-test` prints `PASS: all stages`
- [ ] ASIDs and selective TLB flush (everything is flushed on every `VMRUN` for now)
- [ ] A guest with paging in long mode executes code from mapped memory (needs the FPU/interrupts of phase 3)

**Done when:** `rhv-map-test` passes on hardware, and a guest with paging (long mode) executes code from mapped memory.

## Phase 3: VMX compatibility (in progress: stages 1–5 done on hardware, QEMU next, see `docs/PHASE3.md`)

- [x] A table mapping VMCS fields → VMCB, reading/writing through `hv_vmx_vcpu_*` (in the kernel: `VmcsRead`/`VmcsWrite`; `rhv-vmx-test` PASS)
- [x] Synthesis of VMX exit reason and qualification (I/O, MSR, CPUID, HLT, EPT violation, exceptions, CR/DR)
- [ ] Emulation of `hv_vmx_read_capability`
- [x] Interrupt injection (`event_inj`, interrupt window), host interrupts reach the kernel while the guest runs; [ ] timer, `hv_vcpu_interrupt` from another thread
- [x] Guest and host x87/SSE/AVX and XCR0 (`rhv-long-test`), long mode with paging
- [x] The shim forwards the main API to the kext (`hvf-smoke` on the public API PASS)

**Done when:** QEMU with `-accel hvf` boots Linux to a shell.

## Phase 4: compatibility and stability

- [ ] Front end A: interception at kernel level, applications work without `DYLD_INSERT_LIBRARIES`
- [ ] Docker Desktop / Android Emulator
- [ ] Sleep/wake (S3): verify on hardware (the code exists, see phase 0), add saving of VM state
- [ ] Access model (who may create VMs), resource limits
- [ ] Compatibility with new AMD Vanilla patches

## Phase 5: everything native (0.7.x, Parallels and Docker on the kext as the hypervisor provider)

Where it stands: Docker Desktop runs natively; Parallels runs natively with Windows 10 (1 and 4 vCPUs), but through a small stand-in library that
patches the CPU-vendor check. What is missing, in the order it is worth doing:

1. [x] **Keyboard in a native Parallels VM** (done, `shim/hijack_patchonly.c`; the monitor calls `hv_vcpu_run_until`, so both run functions are replaced). The monitor drops keys until the guest writes 0 to port `0x80`; the shim arranges that, the native path does
   not. Plan: the stand-in library (already inside `prl_vm_app`) replaces only `hv_vcpu_run` in libMonitor's imports and, on the first guest access
   to ports `0x60`/`0x64` (and after a reset), calls the monitor's port handler with 0, as `parallels_arm_keyboard` in `shim/hv_shim.c` does.
   User space only: no kext change, no reboot. Verified: keys sent with `prlctl send-key-event` open the Start menu after a cold boot and after a resume
   (the first-access trigger fires 2 s after the VM starts; for a restored VM a once-a-second check does it). Still to verify: typing from the VM window, other layouts.
2. [~] `kern.hv_support` at every boot without a command (installed and run on the author's machine; a real boot with it is not yet observed): `./rhv autostart install` (launchd daemon, `scripts/hv-sysctl-daemon.sh`), a button in the app.
   Still to do there: remove a stale `hyperkit.pid` of Docker when the daemon runs (a leftover from the previous boot stops HyperKit).
3. [x] **TPR shadow in native mode, on by default since 0.7.3** (speed: about half of the exits of a Windows boot are `CR8`). The framework does not strip the bit from the
   primary controls word (`fixup_capability` only touches the entry/exit/secondary words), so the kext can offer it per VM for Parallels (caps are filled in
   `vmCreate`, which knows `vm->parallels`) and make thread trap 9 (`hv_vmx_vcpu_set_apic_address`) succeed. The core already has the TPR shadow
   (exit 43). Needs a kext build and a reboot. Done when: a Windows 10 boot reaches the desktop in about the time the shim needs (~60 s) with no `CR8` exits.
4. [ ] The Parallels window: opened by the owner from the Parallels app, the mouse and shared folders (Mac drives in the guest) work; to check: resizing, clipboard, typing from the window.
5. [ ] Sleep/wake of the Mac with a running native VM, snapshots, `kextunload`. Done: suspend and resume of a native VM (several cycles, and the library can be replaced between them).
6. [ ] Docker Desktop and Parallels at the same time under load; Windows 11 (TPM/Secure Boot) and a Linux guest in Parallels; the other Parallels builds.
7. [ ] **No wrapper in `/Applications`.** Today the vendor check is patched by a stand-in library that needs `prl_vm_app` replaced by a wrapper. Options: (a) the
   kext patches `prl_vm_app` at `exec` (a kauth listener) and watches for `libMonitor.dylib` being mapped; the callback context and the race with the
   check have to be proven first; (b) a Lilu plugin (UserPatcher) with the same patterns; (c) leave the wrapper. This is a design decision, not just work.
8. [ ] Release 0.7.2: the boot service and the native Parallels button in the app (the app zip is built by `app/build-app.sh`), `rhv` menu entries, docs.
9. [x] Issue 1 (Tahoe): the reporter's log showed `cpus=1` at start: the other cores were not online yet, so SVM was on for one core. 0.7.4 rescans (see NATIVE.md). Confirmed by the
   reporter on macOS 26.7.1, Ryzen 9 5900X: `rescan: the rendezvous reached 24 CPUs (highest index 23), 23 of them new`, SVM on 24 of 24, a Windows 11 LTSC VM in Parallels (shim mode) runs,
   also with 3D acceleration on an RX 6800. Still open there: Docker in native mode (the reporter asked whether the native boot-args can be mixed with the Parallels shim).
10. [~] **VirtualBox 7.x** (0.7.3 offers the TPR shadow by default, so the first point below is done) (no kernel driver on macOS, runs through Hypervisor.framework, so through the native front end): 7.2.22 starts, its BIOS/EFI and Windows
    Setup run, no panic. Needs `rhv-info hv tpr 1` (the TPR shadow is off by default): without it the guest does ~70k `CR8` exits a second per vCPU and Setup
    stays on its first screen for minutes; with it Setup reaches the disk page in about a minute and the `CR8` exits are gone from the sample. To do: decide the
    default (a per-client policy or a boot-service call), `hv_vm_space_*` (not for speed: VirtualBox prints "Turtle execution mode" for the NEM engine in general, the disassembly of VBoxVMM 7.2.22 shows the message does not depend on the ASID; with ASIDs it would only map memory through `hv_vm_map_space` and call `hv_vcpu_set_space`, so it is a compatibility item), fewer kernel calls per exit (about six now),
    a run with a real disk. VirtualBox 6.1.x has its own driver that wants to switch AMD-V on itself and stops with `VERR_SVM_IN_USE` while RyzenHV holds SVME
    (issue 2); 7.x has the override `VBOX_HWVIRTEX_IGNORE_SVM_IN_USE`, not tried on 6.1.
11. [x] **A native Parallels VM hangs on suspend when the Parallels Desktop app is not open** (three times by 2026-10-30; the third on 0.7.4, caused on purpose by ending the app's
    `prl_client_app` while the VM ran: the dispatcher then logs "Stopping VMs in progress" and suspends the VM). With the app open, suspend (23 s) and resume (4 s) work on 0.7.4 and the
    kext logs `vcpuN destroy done` and `vm_destroy done`. Without the app the VM's own log ends at `Waiting the screenshot file to be written` (suspended.png and the .mem file are on disk), the
    process stays in `U` with one `vm.state-machine` thread inside `close()`, all vCPU threads are parked in a condition wait and **no kext trap is in progress** (the kext log has no
    `vm_destroy`). The kernel stack of that thread (`spindump`, symbolized with the kernel's symbols after taking the KASLR slide 0xe8000 off) is only xnu frames: syscall, close, vnode_put /
    vnode release, `vm_object_sleep`, `lck_rw_sleep`, thread_block; no frame of RyzenHV or of any other kext. So it is a file close inside macOS that waits for a busy page object, started by a Parallels that
    has no GUI client; which file (config.sav, suspended.png or the .mem file are not in `lsof` any more) is not known. Not proven to be independent of the kext (an Intel Mac with Parallels' own hypervisor
    cannot be compared here), but nothing points at it. After the hang `prlctl` answers "Parallels Desktop is shutting down" and only a reboot clears it. Rule in the README: keep the app open while a VM runs.
    An open point: a warning in `rhv doctor` / the wrapper when the VM starts without the app.

12. [~] **VMware Fusion 26.0.0** (Intel build, "Monitor Mode: ULM", it imports `hv_vm_space_*`, shadow VMCS and `hv_vm_sync_tsc`; it uses none of the spaces): the VM used to show a black
    screen. Cause found and fixed in 0.7.5: the BIOS sends itself an SMI through the LAPIC ICR (write of 0x200 to 0xFEE00300); VMware enters SMM by setting the NMI-window control and then writing
    CS=0x3000 (SMBASE 0x30000, 4 GB limit), RIP and the rest in one batch. `tVcpuRun` answered the NMI-window request early and returned before `runEx`, so the registers of that batch
    (RIP, RSP, RFLAGS, GPRs) were dropped while the VMCS fields were kept: the guest ran with the new CS and the old RIP into zeros (#UD loop in the BIOS). Now `setRegs` is applied first. With it
    the BIOS finishes, VMware's PCI enumeration and A20 handling go through (it unmaps and maps the RAM above 1 MB each time A20 changes) and **Windows starts: the boot logo appears and stays**.
    Open: the guest then sits in a loop in the Windows kernel (64-bit, IF=0, rip fffff802f5375100/101, rflags 0x46, only timeslice exits, vmx at 100% CPU). Not known what it waits for
    (a timer or the TSC are the first suspects: `hv_vm_sync_tsc` is only answered, VMware's TSC offset and `RDTSC` exiting toggling have not been checked). To look at the guest's code the kext
    needs to read guest memory by virtual address. The debug build used for this (a ring of the last 400 exits with CS:RIP and registers, a ring of the last 128 traps, a probe of the guest CS at
    every entry point, `rhv-info hv flightdump`) was not kept: it has to be written again, and it should be a runtime switch so that a question does not cost a reboot.

## Risks

- The first kernel crashes are unavoidable. Debugging: serial/netdump from a second machine, or at least a fallback OpenCore entry.
- The private ABI between the framework and the kernel may differ between macOS versions.
- Apple is winding down Intel support, so the project's horizon is limited. But SVM, NPT and kernel programming are worth it on their own.
