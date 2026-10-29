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

## Risks

- The first kernel crashes are unavoidable. Debugging: serial/netdump from a second machine, or at least a fallback OpenCore entry.
- The private ABI between the framework and the kernel may differ between macOS versions.
- Apple is winding down Intel support, so the project's horizon is limited. But SVM, NPT and kernel programming are worth it on their own.
