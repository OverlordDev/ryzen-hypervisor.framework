# Phase 1: the first guest

Goal: run 25 bytes of real-mode code inside a real SVM virtual machine. The guest prints a string
through `OUT`, executes `CPUID` and stops on `HLT`. Userland plays the role of the "hardware".

> **Status: done on hardware (5800H): `rhv-guest-test` prints `PASS`, 24 exits, `vmInvalidExits 0`.** The original warning below is kept as a reminder that this path leads to a kernel panic when changed. None of this had run on a real Ryzen at the time of writing. This is the first code
> that enters a guest by itself through `VMRUN`; a mistake here is a kernel panic, not an error message.
> Do not run it until phase 0 passed on your machine (`rhv-info` green, `selftest` N of N).

## How to run

```
# boot-args: rhv_enable_svm=1 rhv_selftest=1 (and a fallback EFI on a USB stick!)
make kext tools DEBUG=1
sudo ./build/rhv-guest-test
./build/rhv-info log           # what the kernel saw
./build/rhv-info stats         # vmCreates, vmRuns, vmExits, vmInvalidExits
```

Expected output:

```
VM created, 1024 KiB guest RAM
--- guest output ---
Hello from the guest!
--- guest executed CPUID leaf 0x40000000 ---
--- guest halted at rip 0x1016 ---
  rax=0000000052485631 ...
PASS: output, CPUID and HLT all handled (24 exits)
```

24 exits: 22 bytes through `OUT` (21 characters + `\n`), `CPUID`, `HLT`. If the guest ran longer than a time slice, `TIMESLICE` exits are added.

## What the code does

| file | role |
|---|---|
| `kext/Source/VMCB.hpp` | the VMCB structure, intercept bits, exit codes. Offsets are checked by `static_assert` (`make test`) |
| `kext/Source/SvmRun.S` | `rhv_svm_vmrun`: `clgi`, host and guest `vmsave`/`vmload`, loading and saving GPRs, `vmrun`, `stgi` |
| `kext/Source/PhysMem.cpp` | allocation of physically contiguous pages for the VMCB, bitmaps and NPT |
| `kext/Source/Vm.cpp` | the VM: reset into real mode, NPT for 1 MiB, the run loop, exit classification |
| `kext/Source/RyzenHVUserClient.cpp` | `VmCreate/Destroy/Reset/SetRegs/GetRegs/Run`, mapping guest memory into the process |
| `tools/rhv-guest-test` | the test guest and the exit handler |

> **Fixed in phase 3:** the first versions entered the guest with host interrupts disabled (`IF=0`), and with
> `V_INTR_MASKING` physical interrupts are masked precisely by the host's `IF`. The guest ran with this core's interrupts
> blocked, so the time slice described below did not fire. It was found from the counters: `vmRuns == vmExits` with 100 ms
> of guest running. Now `SvmRun.S` does `sti` right before `vmrun` and `cli` right after the exit (KVM does the same).

### The run loop (`Vm::run`)

1. Disable host interrupts (`ml_set_interrupts_enabled(0)`).
2. Make sure SVM is enabled on all cores (`activeOnAllUsable`). The check is done with interrupts disabled on purpose:
   sleep or unload cannot turn SVM off until this core answers the rendezvous.
3. `rhv_svm_vmrun`. The guest runs until an intercepted event happens.
4. Restore interrupts. If the exit was caused by an external interrupt (`INTR`/`NMI`), the host handles it
   right here and the loop continues. After ~10 ms we return to userland with `RHV_EXIT_TIMESLICE`
   so that the process can be interrupted by a signal.
5. The other exits (`HLT`, `IO`, `CPUID`, `MSR`, `NPF`, ...) go to userland without emulation in the kernel.

### What is intercepted

Everything that can harm the host or has to be decided by userland: `CPUID`, `HLT`, **all** I/O ports
and **all** MSRs (bitmaps of ones), `INVD`, `WBINVD`, `MONITOR/MWAIT`, reads and writes of **all** DRs,
the SVM instructions (`VMRUN`, `VMLOAD`, ...), `SHUTDOWN` (a triple fault; without interception it would reboot
the whole machine), and also `INTR`, `NMI`, `SMI`, `INIT`. `V_INTR_MASKING` is on: the guest's `EFLAGS.IF` does not affect host interrupts.

### Guest memory

1 MiB at guest physical addresses `0 .. 0xFFFFF`, ordinary wired kernel memory, not contiguous. The NPT
(PML4 → PDPT → PD → PT, entries `P|RW|US`) maps it page by page. Without NPT a real-mode guest would read
the host's physical memory directly, so a minimal NPT was needed already here, not in phase 2.
An access beyond 1 MiB gives `RHV_EXIT_NPF` with the address in `info2`.

Userland maps this memory into its process with `IOConnectMapMemory64` and writes the guest code directly.

## How to read failures

| symptom | what it means |
|---|---|
| `VmCreate failed: 0x...` | no root, or SVM is not enabled on all cores (`rhv_enable_svm=1`), or no NPT/NRIPS. The cause is in `rhv-info log` |
| `FAIL ... INVALID` | `VMRUN` rejected the VMCB. In the log, `VMCB dump`: check `efer` (the `SVME` bit), `asid` (not 0), the `VMRUN` intercept in `v4`, `nested_ctl`, physical addresses |
| `unexpected exit: SHUTDOWN` | the guest went into a triple fault: wrong guest code or wrong reset state |
| `unexpected exit: NPF` | the guest accessed guest memory that does not exist; `info2` is the address |
| `unexpected exit: UNHANDLED` | an intercept is on that the test does not expect; look up the exit code in table C of the AMD manual |
| panic, then reboot | the last `RHV_SYNC` line in the log shows the step: `first VMRUN` means it crashed inside `vmrun` or right after |
| the system hangs | right at `VMRUN`: look at the assembly part (`SvmRun.S`), most likely the host state was lost |

For panics add `-v keepsyms=1 debug=0x100` and photograph the screen, then `-rhv_off` and `scripts/collect-debug.sh`.

## What is not done, and it matters

- **FPU/SSE/AVX state is not switched.** If the guest executes an SSE instruction, it corrupts the host's registers.
  The test guest does not use them. Needed in phase 3 (XSAVE/XRSTOR around `VMRUN`).
- **No protection against speculative attacks** (IBPB, buffer clearing, etc.). For running an untrusted guest
  this is unacceptable; for development it is tolerable.
- **One VM and one vCPU per client, root only, a fixed 1 MiB.** On purpose, to narrow the debugging surface.
- **No injection of interrupts and guest timers.** The guest cannot receive interrupts (phase 3).
- **The VMCB offsets and intercept bits were written down from memory** from AMD APM Vol. 2 (Appendix B, chapter 15) and checked
  for consistency only (`make test`). Check them against the manual when you read the code: this is where a silent mistake costs the most.
- **The host's `CR2`** is saved and restored around `VMRUN`, but other host state that is not swapped
  (for example `TSC_AUX`, `XCR0`) we do not touch.

## Acceptance criterion of the phase

`sudo rhv-guest-test` prints `PASS`, and `rhv-info stats` shows `vmInvalidExits 0`. After that
run it dozens of times in a row and check that the system is stable. Only then do we move on to phase 2.
