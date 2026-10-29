# Phase 3: VMX compatibility (Hypervisor.framework for applications)

Goal: an application written for Intel's `Hypervisor.framework` runs on a Ryzen unchanged. The first
test: QEMU with `-accel hvf` boots Linux. The goal after that: Parallels Desktop, VMware Fusion, Docker Desktop.

## What is really needed (from the SDK 26.2 headers)

The public x86 API is much wider than "VMCS + registers". Three layers:

| layer | functions | who uses it |
|---|---|---|
| **core API** | `hv_vm_create/destroy/map/unmap/protect`, `hv_vcpu_create/destroy/run`, `hv_vcpu_read/write_register`, `hv_vmx_vcpu_read/write_vmcs`, `hv_vmx_read_capability`, `hv_vcpu_read/write_msr`, `hv_vcpu_enable_native_msr`, `hv_vcpu_read/write_fpstate`, `hv_vcpu_interrupt`, `hv_vcpus_exit`, `hv_vm_sync_tsc` | QEMU (emulates the APIC/IOAPIC in userland), most simple VMMs |
| **built-in devices** | `hv_vcpu_apic_*`, `hv_vm_ioapic_*`, `hv_vm_atpic_*`, `hv_vm_lapic_msi`, `hv_vm_lapic_set_intr`, `hv_vm_add_pio_notifier`, `hv_vcpu_add_io_notifier` | newer commercial VMMs (probably Parallels/VMware), they save exits |
| **advanced** | `hv_vm_space_*`, `hv_vcpu_set_space`, `hv_vcpu_vmx_set_shadow_access`, `hv_vmx_vcpu_*_shadow_vmcs`, nested virtualization | special cases |

**Honestly about Parallels:** it is a closed binary. I do not know which subset it calls, whether it uses the built-in
APIC/IOAPIC (most likely yes, since the API exists) and whether it has checks of its own. You can only find out on it
itself (`nm -u` on the binary, tracing through the shim). That is why the first target is QEMU: it is open and uses only the core API.

## Stages

1. **Pure VMCS ⇄ VMCB translation (done and tested, `VmxMap.hpp`, `tests/vmx_map_test.cpp`).** A list of 166 VMCS fields
   (checked against the SDK), translation of segment access rights, translation of SVM exit codes into VMX exit reasons with qualification
   (I/O, MSR, EPT violation from NPF, exceptions, CR/DR).
2. **vCPU state in the kernel (done and verified on hardware: `rhv-vmx-test` prints `PASS`).** `VmcsState.hpp`:
   a VMCS shadow (a value for each of the 166 fields), writing and reading state directly in the VMCB, applying the control
   fields to intercepts (`CPU_BASED`, `EXC_BITMAP`, `CR0/CR4` masks, the interrupt window, `WBINVD`), `EFER` (SVME is hidden from
   the client, `LMA` follows the entry control), `Interruptibility`, one-shot event injection, recording the exit
   reason, qualification, instruction length, the EPT violation address. New kernel calls `VmcsRead`/`VmcsWrite`, the flag
   `RHV_VMCREATE_VMX`. **Not yet:** the `CR0/CR4` read shadow (a read returns the real value), NMI-window,
   MSR/IO bitmaps from the client (we intercept all MSRs and, with I/O exits enabled, all ports).
3. **Real guests in the kernel.**
   - **Done and verified on hardware (`rhv-long-test` PASS, 9 time slices in ~100 ms of guest):** swapping x87/SSE/AVX and `XCR0` around `VMRUN` (`FpuState.hpp`,
     `XSAVE`/`XRSTOR` handling `CR0.TS`), the `VmGetFpu`/`VmSetFpu` calls (in the format of `hv_vcpu_read_fpstate`),
     a self-check in the kernel (the `vmFpuMismatches` counter must be 0), a guest in long mode with paging. The guest's hardware `XCR0`
     always includes x87 and SSE, otherwise `XSAVE` would not save the guest's XMM until the first `XSETBV`.
   - **Not yet:** `TSC_AUX`, host and guest MSRs (`KERNEL_GS_BASE`, `STAR` etc. are switched by `VMSAVE`/`VMLOAD`, the others
     have to be checked), several vCPUs, `hv_vcpus_exit`/`hv_vcpu_interrupt` from another thread, speculative
     mitigations (IBPB and others), `hv_vcpu_enable_native_msr`.
4. **The shim (done and verified on hardware: `hvf-smoke` PASS through `scripts/run-with-shim.sh`).** `shim/rhv_core.c` is the API semantics on top of an abstract back end
   (tested on the host with a fake kernel: `tests/shim_core_test.c`, plus a check of all constants against the SDK); `rhv_iokit.c` is
   the back end to the kext; `hv_shim.c` does the replacement through dyld interpose; `hv_stubs.inc` holds generated
   `HV_UNSUPPORTED` stubs for all the rest of the x86 API, so that a call does not go to the real framework.
   Implemented: `hv_vm_{create,destroy,map,unmap,protect,sync_tsc}`, `hv_vcpu_{create,destroy,run,run_until,interrupt,
   read/write_register,read/write_fpstate,read/write_msr,enable_native_msr,enable_managed_msr,flush,invalidate_tlb,
   get_exec_time}`, `hv_vmx_vcpu_{read,write}_vmcs`, `hv_vmx_read_capability`. Limits: one VM per process, one
   vCPU; the kernel lets in only root for now.
   To run: `scripts/run-with-shim.sh ./build/hvf-smoke`.
5. **Our own smoke test on the public API** (`tools/hvf-smoke`, written): the same binary works on a real Intel
   Mac and on a Ryzen through the shim. It is also a regression test for the future.
6. **QEMU (done, verified on hardware).** QEMU 11.1.2 built from source with `--enable-hvf`, the Alpine 6.6 kernel with an
   initramfs boots through the shim to a `~ #` shell and idles quietly (`scripts/qemu-hvf-test.sh`). What had to be
   fixed on the way (each item was found by a real run, not by tests):
   - `CR4.VMXE`: QEMU always writes it into the guest CR4, but on AMD the bit is reserved and `VMRUN` answers `VMEXIT_INVALID`
     (QEMU saw `unhandled exit 80000021`). Now the bit does not get into the VMCB and is read back (`VmcsState.hpp`).
   - The EPT violation qualification without bits 7/8 (the linear address is valid, the access is to the final address): QEMU considered
     the violation "not its own" and restarted the guest forever. Now we take them from `EXITINFO1` bits 32/33 (`VmxMap.hpp`).
   - `hv_vcpu_interrupt` only raised a flag and waited for the next host interrupt (up to 10 ms); now an IPI goes to the CPU where
     the guest runs (`mp_cpus_call`, at most one in flight per CPU).
   - **Lost interrupts**: an injected event that had not managed to enter (or an interrupted delivery) was lost on an exit
     by time slice/kick, and `IDT_VECTORING_INFO` stayed from the previous exit. QEMU considered the interrupt delivered,
     the vector stayed "in service" forever in its LAPIC, and the Linux kernel panicked in the timer check
     ("IO-APIC + timer doesn't work"). Now an undelivered event is returned to the client and the old value is cleared.
   - The shim: a cache of registers and VMCS fields between exits (QEMU makes dozens of accesses per exit; in the real framework
     that is shared memory, for us a system call), `IA32_APIC_BASE` is answered by the shim itself.
   - Tools: `RYZENHV_STATS=1` (exits per second, kernel calls per exit, µs per exit, injections, kicks),
     `RYZENHV_TRACE=2` (a trace without routine exits), `RHV_QEMU_EXTRA` for QEMU options (for example a monitor over TCP:
     `-monitor tcp:127.0.0.1:4444,server=on,wait=off`, then `info lapic`).
   **ABI 8: one kernel call per exit (done and verified on hardware).** An exit cost ~40 µs and ~12 kernel calls (in
   the real framework ~3 µs), and Linux could not calibrate the TSC against the PIT/HPET (the tolerance of one port read is about 16 µs),
   so the tests ran with `tsc_early_khz=3200000 lpj=16000000 tsc=reliable`. Now `VmRunEx` accepts dirty registers and
   up to 96 VMCS writes, runs the guest and returns the exit, the registers and 18 fields that the client reads after every
   exit (`RHV_RUN_FIELDS_LIST`); the shim buffers writes (an error about a wrong or read-only field is returned at once, using
   the same table as the kernel: `shim/rhv_fields.h`, checked against all 65536 encodings), and reads the segment fields
   with a single `VmcsReadMany`. The shim no longer accepts a kernel with ABI 7 (checked in `hv_vm_create`, with a clear message).
   Result on a Ryzen 7 5800H: 1.2-2 kernel calls per exit instead of 12, ~17 µs per exit under load (57 thousand exits/s)
   instead of ~38 µs; Linux with no parameters at all calibrates the TSC by itself (`Detected 3194.185 MHz processor`) and reaches `~ #`.
7. **Parallels (in progress).** Parallels Desktop 26.5 for Intel checks `sysctl kern.hv_support` itself (0 on AMD) and falls back
   to its own engine with a kext that we do not have (`AppleHV does not support this Mac, defaulting to own engine` in
   `~/Parallels/<VM>.pvm/parallels.log`). Its Hypervisor.framework back end lives in `libMonitor.dylib` (x86_64) and
   uses 35 functions (`nm -arch x86_64 -u libMonitor.dylib | grep _hv_`). Worked through on real runs:
   - **How to get into the process.** `prl_vm_app` is a hardened binary: dyld ignores `DYLD_INSERT_LIBRARIES`, and the Parallels
     dispatcher checks the process signature (`anchor apple generic and certificate leaf[subject.OU] = "4C6364ACXT"`), so
     neither an environment variable nor re-signing works. `scripts/parallels-shim.sh prepare` makes a mirror of the application in `~/.ryzenhv/prl`
     (Parallels' signature intact) where the library `libPrlXmlModel.1.dylib`, loaded at launch, is the shim
     re-exporting the real one (`shim/hijack.c`); `install` (sudo) replaces only `prl_vm_app` in `/Applications` with a
     redirecting wrapper, `uninstall` brings the original back. The replacing of functions is done by the shim itself (rewriting import pointers
     by name, like fishhook): `dyld_dynamic_interpose` does not touch already loaded images on this system.
   - **kern.hv_support**: the shim returns 1, and Parallels picks the Apple HV back end.
   - **The CPUID vendor check** (`GenuineIntel`, an instruction, cannot be intercepted) is bypassed by patching one conditional
     jump: in the memory of `prl_vm_app` (the file cannot be edited, the signature) and in the copy of `libMonitor.dylib` in the mirror.
   - Implemented: `hv_vm_allocate/deallocate`, `hv_tsc_clock`, `hv_vcpu_set_tsc_relative`, `hv_vcpu_set_msr_access`,
     `hv_vmx_get_msr_info`, `hv_vmx_vcpu_set_apic_address[_space]`, the VMX capabilities `VMCS_ENUM`/`EPT_VPID_CAP`, an XSAVE image with
     `XSTATE_BV` wider than the current `XCR0` (Parallels writes it before AVX is enabled), `IA32_TSC_AUX` as a "native" MSR.
   - Kernel: the boot-arg `rhv_allow_users=1` lets in a local user; the Parallels VM runs as a user.
   **State (stage 8, ABI 11): Windows 11 Home installs and runs in Parallels 26.5.0 on 1–4 vCPUs**, the Setup window
   after ~45–50 s, Parallels Tools, network. Done on top of the list above (details in `docs/PARALLELS.md`):
   - several vCPUs per VM (a shared part + an object per vCPU, `VmJoin`, a kernel connection per vCPU, locking of the NPT tables);
   - the keyboard (the monitor drops keys until 0 is written to port 0x80), the APIC page left unmapped, hiding the hypervisor,
     the Hyper-V `VMCALL` through #UD interception, handling of the monitor's reset (the address of `libMonitor` is looked up again);
   - TPR shadow in the kernel (CR8 exits are gone), the 8-bit `HV_X86_TPR`, `RDTSCP` together with `RDTSC exiting`,
     one kernel call per exit (55 fields in `VmRunEx`);
   - the CPUID topology for the guest (otherwise Windows Home runs a single processor);
   - debugging: a dump on triple fault and of the Windows crash dump, a trace of MSR/MMIO/ICR/CPUID, the MADT table, statistics.
   **Not yet:** address spaces (`hv_vm_space_*`, `hv_vm_{map,unmap,protect}_space`, `hv_vcpu_set_space`), sleep/wake
   and snapshots with several vCPUs, 3D acceleration (NVIDIA's Metal driver crashes on the author's machine), other guests, other
   Parallels builds (the address-based patches are tied to 26.5.0 (57613)).
8. Everything else that Parallels will need.

## Risks

- Stage 3 is the most dangerous code in the project (FPU and interrupts in the kernel): every mistake is a panic.
- The semantics of VMX and SVM differ in small things (the `CR0/CR4` shadow, NMI blocking, `Activity state`, `preemption timer`).
  Applications sometimes rely on them. Everything we cannot emulate exactly has to be documented explicitly.
- There is no guarantee that Parallels does not use private calls or checks that the replacement breaks.
