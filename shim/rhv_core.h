/*
 * rhv_core.h - the semantics of Intel's x86 Hypervisor.framework API, on top of the RyzenHV kext.
 *
 * This is the part of the shim that decides what hv_vcpu_read_register(HV_X86_CR2) means, which error code a call
 * returns, what hv_vmx_read_capability reports, and so on. It does not talk to the kernel itself: it goes through
 * an RhvBackend (rhv_iokit.c in the real shim, a fake in tests/shim_core_test.c), and it does not include
 * <Hypervisor/Hypervisor.h>, so it builds and is tested on any machine. hv_shim.c adapts it to the real prototypes.
 *
 * Limits of this first version, all reported as errors rather than faked:
 *   - one VM per process (several vCPUs per VM; each runs on the thread that created it)
 *   - no in-kernel APIC / IOAPIC / ATPIC (hv_vcpu_apic_*, hv_vm_ioapic_*, ...): HV_UNSUPPORTED; QEMU emulates those
 *   - no VM address spaces (hv_vm_space_*)
 */
#ifndef RHV_CORE_H
#define RHV_CORE_H

#include <stdint.h>
#include <stddef.h>

#include "../include/RyzenHVShared.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- hv_return_t values (the same numbers as Apple's, checked against the SDK header in the tests) ---- */
enum {
    RHV_HV_SUCCESS      = 0,
    RHV_HV_ERROR        = (int32_t)0xfae94001,
    RHV_HV_BUSY         = (int32_t)0xfae94002,
    RHV_HV_BAD_ARGUMENT = (int32_t)0xfae94003,
    RHV_HV_NO_RESOURCES = (int32_t)0xfae94005,
    RHV_HV_NO_DEVICE    = (int32_t)0xfae94006,
    RHV_HV_DENIED       = (int32_t)0xfae94007,
    RHV_HV_UNSUPPORTED  = (int32_t)0xfae9400f
};

/* ---- hv_x86_reg_t (same numbering as the SDK) ---- */
enum {
    RHV_X86_RIP = 0, RHV_X86_RFLAGS, RHV_X86_RAX, RHV_X86_RCX, RHV_X86_RDX, RHV_X86_RBX, RHV_X86_RSI, RHV_X86_RDI,
    RHV_X86_RSP, RHV_X86_RBP, RHV_X86_R8, RHV_X86_R9, RHV_X86_R10, RHV_X86_R11, RHV_X86_R12, RHV_X86_R13,
    RHV_X86_R14, RHV_X86_R15, RHV_X86_CS, RHV_X86_SS, RHV_X86_DS, RHV_X86_ES, RHV_X86_FS, RHV_X86_GS,
    RHV_X86_IDT_BASE, RHV_X86_IDT_LIMIT, RHV_X86_GDT_BASE, RHV_X86_GDT_LIMIT, RHV_X86_LDTR, RHV_X86_LDT_BASE,
    RHV_X86_LDT_LIMIT, RHV_X86_LDT_AR, RHV_X86_TR, RHV_X86_TSS_BASE, RHV_X86_TSS_LIMIT, RHV_X86_TSS_AR,
    RHV_X86_CR0, RHV_X86_CR1, RHV_X86_CR2, RHV_X86_CR3, RHV_X86_CR4, RHV_X86_DR0, RHV_X86_DR1, RHV_X86_DR2,
    RHV_X86_DR3, RHV_X86_DR4, RHV_X86_DR5, RHV_X86_DR6, RHV_X86_DR7, RHV_X86_TPR, RHV_X86_XCR0, RHV_X86_REGISTERS_MAX
};

/* ---- hv_vmx_capability_t ---- */
enum {
    RHV_VMX_CAP_PINBASED = 0, RHV_VMX_CAP_PROCBASED, RHV_VMX_CAP_PROCBASED2, RHV_VMX_CAP_ENTRY, RHV_VMX_CAP_EXIT,
    RHV_VMX_CAP_BASIC, RHV_VMX_CAP_TRUE_PINBASED, RHV_VMX_CAP_TRUE_PROCBASED, RHV_VMX_CAP_TRUE_ENTRY,
    RHV_VMX_CAP_TRUE_EXIT, RHV_VMX_CAP_MISC, RHV_VMX_CAP_CR0_FIXED0, RHV_VMX_CAP_CR0_FIXED1,
    RHV_VMX_CAP_CR4_FIXED0, RHV_VMX_CAP_CR4_FIXED1, RHV_VMX_CAP_VMCS_ENUM, RHV_VMX_CAP_EPT_VPID_CAP
};

/* What the real thing accepts for hv_vm_map and friends. */
#define RHV_HV_MEMORY_MASK 0x7u

/*
 * The kernel side. Every function returns an IOReturn-style code (0 = success) and is given `ctx`. Functions the
 * backend does not need can be NULL; the core then answers HV_UNSUPPORTED.
 */
typedef struct RhvBackend {
    void *ctx;
    int (*vm_create)(void *ctx, uint32_t flags);                 /* RHV_VMCREATE_* */
    int (*vm_destroy)(void *ctx);
    int (*vm_map)(void *ctx, uint64_t uva, uint64_t gpa, uint64_t size, uint32_t flags);
    int (*vm_unmap)(void *ctx, uint64_t gpa, uint64_t size);
    int (*vm_protect)(void *ctx, uint64_t gpa, uint64_t size, uint32_t flags);
    int (*vmcs_read)(void *ctx, uint32_t field, uint64_t *value);
    int (*vmcs_write)(void *ctx, uint32_t field, uint64_t value);
    int (*get_regs)(void *ctx, RHVRegs *regs);
    int (*set_regs)(void *ctx, const RHVRegs *regs);
    int (*get_fpu)(void *ctx, RHVFpuState *fpu);
    int (*set_fpu)(void *ctx, const RHVFpuState *fpu);
    int (*msr)(void *ctx, uint32_t op, uint32_t msr, uint64_t value, uint64_t *out);
    int (*run)(void *ctx, RHVExit *exit);
    int (*kick)(void *ctx);
    uint64_t (*now)(void *ctx);                                  /* mach_absolute_time units, for exec time */
    uint64_t (*tsc)(void *ctx);                                  /* host TSC */
    unsigned long (*thread_id)(void *ctx);                       /* identifies the calling thread */
    /* Fast path (ABI 8); without them the core falls back to one call per register / field. */
    int (*run_ex)(void *ctx, const RHVRunIn *in, RHVRunOut *out);
    int (*vmcs_read_many)(void *ctx, const RHVReadMany *in, RHVReadManyOut *out);
    /*
     * More than one vCPU (ABI 9). With these the per-vCPU functions above (vmcs_*, *_regs, *_fpu, msr, run, run_ex, kick,
     * vmcs_read_many) get the pointer vcpu_open returned instead of `ctx`; the VM-wide ones keep using `ctx`.
     * vcpu_open(index 0) returns the vCPU that vm_create made. Without vcpu_open there is one vCPU and its ctx is `ctx`.
     */
    int (*vcpu_open)(void *ctx, uint32_t index, void **vctx);
    int (*vcpu_close)(void *ctx, uint32_t index, void *vctx);
} RhvBackend;

/* Installs the backend. The core copies it. Must be called before the first API call. */
void rhv_core_init(const RhvBackend *backend);

/* Whether hv_vmx_read_capability offers the TPR shadow (CPU-based control bit 21); default no. */
extern int rhv_core_offer_tpr_shadow;

/* Optional trace sink, printf style; NULL (the default) disables tracing. */
extern void (*rhv_core_trace)(const char *fmt, ...);

/* ---- the API (return values are the hv_return_t numbers above) ---- */
int32_t rhv_vm_create(uint64_t flags);
int32_t rhv_vm_destroy(void);
int32_t rhv_vm_map(uint64_t uva, uint64_t gpa, size_t size, uint64_t flags);
int32_t rhv_vm_unmap(uint64_t gpa, size_t size);
int32_t rhv_vm_protect(uint64_t gpa, size_t size, uint64_t flags);
int32_t rhv_vm_sync_tsc(uint64_t tsc);

int32_t rhv_vcpu_create(uint32_t *vcpu, uint64_t flags);
int32_t rhv_vcpu_destroy(uint32_t vcpu);
int32_t rhv_vcpu_run(uint32_t vcpu);
void rhv_vcpu_override_exit(uint32_t vcpu, uint64_t reason, uint64_t instrLen);
int32_t rhv_vcpu_interrupt(const uint32_t *vcpus, unsigned int count);
int32_t rhv_vcpu_read_register(uint32_t vcpu, uint32_t reg, uint64_t *value);
int32_t rhv_vcpu_write_register(uint32_t vcpu, uint32_t reg, uint64_t value);
int32_t rhv_vcpu_read_fpstate(uint32_t vcpu, void *buffer, size_t size);
int32_t rhv_vcpu_write_fpstate(uint32_t vcpu, const void *buffer, size_t size);
int32_t rhv_vcpu_read_msr(uint32_t vcpu, uint32_t msr, uint64_t *value);
int32_t rhv_vcpu_write_msr(uint32_t vcpu, uint32_t msr, uint64_t value);
int32_t rhv_vcpu_enable_native_msr(uint32_t vcpu, uint32_t msr, int enable);
int32_t rhv_vcpu_flush(uint32_t vcpu);
int32_t rhv_vcpu_invalidate_tlb(uint32_t vcpu);
int32_t rhv_vcpu_get_exec_time(uint32_t vcpu, uint64_t *time);
uint64_t rhv_tsc_clock(void);
int32_t rhv_vcpu_set_tsc_relative(uint32_t vcpu, int64_t offset);
int32_t rhv_vcpu_set_msr_access(uint32_t vcpu, uint32_t msr, uint32_t flags);
int32_t rhv_vmx_get_msr_info(uint32_t field, uint64_t *value);
int32_t rhv_vmx_vcpu_set_apic_address(uint32_t vcpu, uint64_t gpa);
int32_t rhv_vmx_vcpu_set_apic_address_space(uint32_t vcpu, uint32_t asid, uint64_t gpa);

int32_t rhv_vmx_vcpu_read_vmcs(uint32_t vcpu, uint32_t field, uint64_t *value);
int32_t rhv_vmx_vcpu_write_vmcs(uint32_t vcpu, uint32_t field, uint64_t value);
int32_t rhv_vmx_read_capability(uint32_t cap, uint64_t *value);

/* Drops the register/VMCS cache (the tests change the fake kernel's state behind the core's back). */
void rhv_core_cache_reset(void);

/* Number of vCPUs that exist. */
unsigned rhv_core_vcpu_count(void);

/* Maps a kernel return code (IOReturn) to an hv_return_t. */
int32_t rhv_hv_from_ioreturn(int ior);

/* Which VMCS field holds the x86 register `reg`, or 0 when it is not a plain VMCS/extension field. For tests. */
uint32_t rhv_core_register_field(uint32_t reg);

#ifdef __cplusplus
}
#endif

#endif /* RHV_CORE_H */
