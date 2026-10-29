# Architecture

## Layers

```
 application (QEMU, Docker, ...)
        │  hv_vm_create, hv_vcpu_run, hv_vmx_vcpu_read_vmcs, ...
        ▼
 ┌─────────────────────────────── front end (replaceable) ─────────────────────┐
 │ B. shim/hv_shim.c        dyld interpose, talks to the kext via IOUserClient │
 │ A. kernel interception   replacing the private traps of the native framework│
 └──────────────────────────────────────┬──────────────────────────────────────┘
                                        ▼
 ┌─────────────────────────────── core (kext) ─────────────────────────────────┐
 │ translation of VMCS fields and VMX exit reasons  ⇄  VMCB and SVM exit codes │
 │ VM / vCPU: life cycle, registers, interrupts                                │
 │ memory: hv_vm_map  →  nested page tables (NPT)                              │
 │ SVM primitives: EFER.SVME, VM_HSAVE_PA, VMRUN/VMSAVE/VMLOAD, ASID           │
 └─────────────────────────────────────────────────────────────────────────────┘
```

The main idea: **the core is written once**, the front end can be swapped. That way we do not depend on how
easy the private kernel interface is to decipher.

## Why two front ends

| | A. kernel interception | B. userland shim |
|---|---|---|
| Compatibility | any application unchanged | only what loads our dylib |
| Hardened runtime / SIP | does not get in the way | gets in the way (SIP/AMFI must be relaxed) |
| Needs the private kernel ABI | yes, reverse engineering | no |
| Risk | higher (we patch the kernel) | lower |
| First result | later | fast |

The plan: B first (to test the core quickly), then A (so that Docker and the rest work as they are).
For A we need to work out which calls the native framework makes into the kernel: `docs/REVERSING.md`.
The interception itself is probably done with Lilu-style routing of kernel functions, but that has to be checked
on the disassembled kernel, not from memory.

## The hardest part: VMX → SVM

The old x86 `Hypervisor.framework` API is VMX-shaped: applications read and write VMCS fields by
numeric codes and expect VMX exit codes. So the core has to **emulate a VMCS on top of a VMCB**.

What has to be translated (check the codes against the manuals, the ones below are from memory):

| Event | VMX exit reason | SVM exit code |
|---|---|---|
| external interrupt | 1 | 0x60 (INTR) |
| CPUID | 10 | 0x72 |
| HLT | 12 | 0x78 |
| I/O instruction | 30 | 0x7B |
| RDMSR / WRMSR | 31 / 32 | 0x7C (one event, told apart by EXITINFO1) |
| EPT violation / NPT fault | 48 | 0x400 (NPF) |

Differences we will stumble over:

- The VMCS lives behind `vmread/vmwrite`, the VMCB is an ordinary structure in memory. The segment attribute formats differ.
- SVM has no analogue of the VMX preemption timer: the timer has to be done on the host.
- EPT vs NPT: different entry formats and permission bits, different violation qualification.
- What to intercept and how (MSR/IO bitmaps, intercept bits in the VMCB) is up to us.
- The virtual capability MSRs (`hv_vmx_read_capability`) have to be synthesized.

## Decisions for now

- A kext, not DriverKit: VMRUN and MSRs are needed in ring 0.
- Loaded through OpenCore (Kernel → Add), unsigned.
- SVM is enabled only by `boot-arg rhv_enable_svm=1`. By default the kext only reads.
- The kext ⇄ userland ABI is described in `include/RyzenHVShared.h` and changes only together with a bump of `RHV_ABI_VERSION`.

## References

- AMD64 Architecture Programmer's Manual, Vol. 2, chapter 15 (Secure Virtual Machine)
- Intel SDM, Vol. 3, chapters 23-28 (VMX): to know exactly what is being emulated
- bhyve, `sys/amd64/vmm/amd/` (BSD): a working SVM back end, can be read and ported with the copyright kept
- Linux KVM, `arch/x86/kvm/svm/` (GPL): read only
