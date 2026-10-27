/*
 * rhv_core.c - see rhv_core.h.
 */
#include "rhv_core.h"

#include <string.h>

void (*rhv_core_trace)(const char *fmt, ...);

#define TRACE(...)                                  \
    do {                                            \
        if (rhv_core_trace)                         \
            rhv_core_trace(__VA_ARGS__);            \
    } while (0)

/* Intel VMCS encodings used by the register mapping (the same numbers as VMCS_* in hv_arch_vmx.h). */
enum {
    F_TSC_OFFSET = 0x2010,
    F_GUEST_CR0 = 0x6800, F_GUEST_CR3 = 0x6802, F_GUEST_CR4 = 0x6804, F_GUEST_DR7 = 0x681a,
    F_GUEST_ES = 0x800, F_GUEST_CS = 0x802, F_GUEST_SS = 0x804, F_GUEST_DS = 0x806, F_GUEST_FS = 0x808,
    F_GUEST_GS = 0x80a, F_GUEST_LDTR = 0x80c, F_GUEST_TR = 0x80e,
    F_GUEST_LDTR_LIMIT = 0x480c, F_GUEST_TR_LIMIT = 0x480e, F_GUEST_GDTR_LIMIT = 0x4810, F_GUEST_IDTR_LIMIT = 0x4812,
    F_GUEST_LDTR_AR = 0x4820, F_GUEST_TR_AR = 0x4822,
    F_GUEST_LDTR_BASE = 0x6812, F_GUEST_TR_BASE = 0x6814, F_GUEST_GDTR_BASE = 0x6816, F_GUEST_IDTR_BASE = 0x6818,
    /* Our own, for state Intel's VMCS does not have (VmxMap.hpp). */
    F_X_CR2 = 0xF000, F_X_DR6 = 0xF002, F_X_TPR = 0xF004, F_X_DR0 = 0xF010
};

static RhvBackend B;
static int  gInit;
static int  gVm;
static int  gVcpu;
static unsigned long gOwner;
static uint64_t gExecTime;

void rhv_core_init(const RhvBackend *backend)
{
    B = *backend;
    gInit = 1;
    gVm = gVcpu = 0;
    gOwner = 0;
    gExecTime = 0;
}

int32_t rhv_hv_from_ioreturn(int ior)
{
    uint32_t r = (uint32_t)ior;
    if (r == 0)
        return RHV_HV_SUCCESS;
    switch (r) {
    case 0xe00002bd: case 0xe00002be: case 0xe00002db: case 0xe00002de:      /* NoMemory NoResources NoSpace CannotWire */
        return RHV_HV_NO_RESOURCES;
    case 0xe00002c0: case 0xe00002d8: case 0xe00002d9:                       /* NoDevice NotReady NotAttached */
        return RHV_HV_NO_DEVICE;
    case 0xe00002c1: case 0xe00002e2:                                         /* NotPrivileged NotPermitted */
        return RHV_HV_DENIED;
    case 0xe00002c2: case 0xe00002cf: case 0xe00002d0: case 0xe00002f0:      /* BadArgument NotWritable NotAligned NotFound */
    case 0xe00002f1: case 0xe00002c8:                                         /* Invalid VMError */
        return RHV_HV_BAD_ARGUMENT;
    case 0xe00002c5: case 0xe00002d5:                                         /* ExclusiveAccess Busy */
        return RHV_HV_BUSY;
    case 0xe00002c7:                                                          /* Unsupported */
        return RHV_HV_UNSUPPORTED;
    default:
        return RHV_HV_ERROR;
    }
}

/* ---- state checks ---- */

static int32_t needVm(void)
{
    return gInit && gVm ? RHV_HV_SUCCESS : RHV_HV_NO_DEVICE;
}

/* A vCPU call must name the vCPU we created and come from the thread that created it. */
static int32_t needVcpu(uint32_t vcpu)
{
    int32_t r = needVm();
    if (r)
        return r;
    if (!gVcpu || vcpu != 0)
        return RHV_HV_BAD_ARGUMENT;
    if (B.thread_id && B.thread_id(B.ctx) != gOwner)
        return RHV_HV_BAD_ARGUMENT;
    return RHV_HV_SUCCESS;
}

#define NEED(fn) do { if (!B.fn) return RHV_HV_UNSUPPORTED; } while (0)

/* ---- VM ---- */

int32_t rhv_vm_create(uint64_t flags)
{
    (void)flags;                  /* mitigation options have no meaning here */
    if (!gInit)
        return RHV_HV_ERROR;
    NEED(vm_create);
    if (gVm)
        return RHV_HV_BUSY;
    int r = B.vm_create(B.ctx, RHV_VMCREATE_VMX | RHV_VMCREATE_NO_RAM);
    TRACE("hv_vm_create -> kernel %#x", r);
    if (r)
        return rhv_hv_from_ioreturn(r);
    gVm = 1;
    gVcpu = 0;
    gExecTime = 0;
    return RHV_HV_SUCCESS;
}

int32_t rhv_vm_destroy(void)
{
    int32_t r = needVm();
    if (r)
        return r;
    NEED(vm_destroy);
    if (gVcpu)
        return RHV_HV_BUSY;       /* destroy the vCPU first */
    int k = B.vm_destroy(B.ctx);
    gVm = 0;
    return k ? rhv_hv_from_ioreturn(k) : RHV_HV_SUCCESS;
}

int32_t rhv_vm_map(uint64_t uva, uint64_t gpa, size_t size, uint64_t flags)
{
    int32_t r = needVm();
    if (r)
        return r;
    NEED(vm_map);
    if (flags & ~(uint64_t)RHV_HV_MEMORY_MASK)
        return RHV_HV_BAD_ARGUMENT;
    return rhv_hv_from_ioreturn(B.vm_map(B.ctx, uva, gpa, size, (uint32_t)flags));
}

int32_t rhv_vm_unmap(uint64_t gpa, size_t size)
{
    int32_t r = needVm();
    if (r)
        return r;
    NEED(vm_unmap);
    return rhv_hv_from_ioreturn(B.vm_unmap(B.ctx, gpa, size));
}

int32_t rhv_vm_protect(uint64_t gpa, size_t size, uint64_t flags)
{
    int32_t r = needVm();
    if (r)
        return r;
    NEED(vm_protect);
    if (flags & ~(uint64_t)RHV_HV_MEMORY_MASK)
        return RHV_HV_BAD_ARGUMENT;
    return rhv_hv_from_ioreturn(B.vm_protect(B.ctx, gpa, size, (uint32_t)flags));
}

/* The guest sees the TSC `tsc` at this moment: guest TSC = host TSC + offset. */
int32_t rhv_vm_sync_tsc(uint64_t tsc)
{
    int32_t r = needVm();
    if (r)
        return r;
    NEED(vmcs_write);
    NEED(tsc);
    return rhv_hv_from_ioreturn(B.vmcs_write(B.ctx, F_TSC_OFFSET, tsc - B.tsc(B.ctx)));
}

/* ---- vCPU ---- */

int32_t rhv_vcpu_create(uint32_t *vcpu, uint64_t flags)
{
    int32_t r = needVm();
    if (r)
        return r;
    if (!vcpu || flags > 3)       /* HV_VCPU_DEFAULT, ACCEL_RDPMC, TSC_RELATIVE */
        return RHV_HV_BAD_ARGUMENT;
    if (gVcpu)
        return RHV_HV_NO_RESOURCES;   /* one vCPU per VM for now */
    gVcpu = 1;
    gOwner = B.thread_id ? B.thread_id(B.ctx) : 0;
    *vcpu = 0;
    return RHV_HV_SUCCESS;
}

int32_t rhv_vcpu_destroy(uint32_t vcpu)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    gVcpu = 0;
    return RHV_HV_SUCCESS;
}

int32_t rhv_vcpu_run(uint32_t vcpu)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    NEED(run);
    RHVExit ex;
    memset(&ex, 0, sizeof(ex));
    uint64_t t0 = B.now ? B.now(B.ctx) : 0;
    int k = B.run(B.ctx, &ex);
    if (B.now)
        gExecTime += B.now(B.ctx) - t0;
    if (k)
        return rhv_hv_from_ioreturn(k);
    /* What happened is in the VMCS read-only fields (exit reason, qualification, ...): the kernel put it there. */
    return RHV_HV_SUCCESS;
}

/* The one call that may come from any thread. */
int32_t rhv_vcpu_interrupt(const uint32_t *vcpus, unsigned int count)
{
    int32_t r = needVm();
    if (r)
        return r;
    if (!vcpus || count == 0)
        return RHV_HV_BAD_ARGUMENT;
    for (unsigned int i = 0; i < count; i++) {
        if (!gVcpu || vcpus[i] != 0)
            return RHV_HV_BAD_ARGUMENT;
    }
    NEED(kick);
    return rhv_hv_from_ioreturn(B.kick(B.ctx));
}

int32_t rhv_vcpu_flush(uint32_t vcpu)
{
    return needVcpu(vcpu);        /* state is written straight through, nothing to flush */
}

int32_t rhv_vcpu_invalidate_tlb(uint32_t vcpu)
{
    return needVcpu(vcpu);        /* every VMRUN flushes the guest's TLB */
}

int32_t rhv_vcpu_get_exec_time(uint32_t vcpu, uint64_t *time)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    if (!time)
        return RHV_HV_BAD_ARGUMENT;
    *time = gExecTime;
    return RHV_HV_SUCCESS;
}

/* ---- registers ---- */

/* The VMCS (or extension) field behind a register that is not a general-purpose one, 0 if there is none. */
uint32_t rhv_core_register_field(uint32_t reg)
{
    switch (reg) {
    case RHV_X86_CS: return F_GUEST_CS;
    case RHV_X86_SS: return F_GUEST_SS;
    case RHV_X86_DS: return F_GUEST_DS;
    case RHV_X86_ES: return F_GUEST_ES;
    case RHV_X86_FS: return F_GUEST_FS;
    case RHV_X86_GS: return F_GUEST_GS;
    case RHV_X86_IDT_BASE: return F_GUEST_IDTR_BASE;
    case RHV_X86_IDT_LIMIT: return F_GUEST_IDTR_LIMIT;
    case RHV_X86_GDT_BASE: return F_GUEST_GDTR_BASE;
    case RHV_X86_GDT_LIMIT: return F_GUEST_GDTR_LIMIT;
    case RHV_X86_LDTR: return F_GUEST_LDTR;
    case RHV_X86_LDT_BASE: return F_GUEST_LDTR_BASE;
    case RHV_X86_LDT_LIMIT: return F_GUEST_LDTR_LIMIT;
    case RHV_X86_LDT_AR: return F_GUEST_LDTR_AR;
    case RHV_X86_TR: return F_GUEST_TR;
    case RHV_X86_TSS_BASE: return F_GUEST_TR_BASE;
    case RHV_X86_TSS_LIMIT: return F_GUEST_TR_LIMIT;
    case RHV_X86_TSS_AR: return F_GUEST_TR_AR;
    case RHV_X86_CR0: return F_GUEST_CR0;
    case RHV_X86_CR2: return F_X_CR2;
    case RHV_X86_CR3: return F_GUEST_CR3;
    case RHV_X86_CR4: return F_GUEST_CR4;
    case RHV_X86_DR0: case RHV_X86_DR1: case RHV_X86_DR2: case RHV_X86_DR3:
        return F_X_DR0 + 2 * (reg - RHV_X86_DR0);
    case RHV_X86_DR4: case RHV_X86_DR6: return F_X_DR6;      /* DR4 and DR5 alias DR6 and DR7 */
    case RHV_X86_DR5: case RHV_X86_DR7: return F_GUEST_DR7;
    case RHV_X86_TPR: return F_X_TPR;
    default: return 0;
    }
}

/* General-purpose registers live in RHVRegs. Returns a pointer to the field, or NULL. */
static uint64_t *gprSlot(RHVRegs *r, uint32_t reg)
{
    switch (reg) {
    case RHV_X86_RIP: return &r->rip;
    case RHV_X86_RFLAGS: return &r->rflags;
    case RHV_X86_RAX: return &r->rax;
    case RHV_X86_RCX: return &r->rcx;
    case RHV_X86_RDX: return &r->rdx;
    case RHV_X86_RBX: return &r->rbx;
    case RHV_X86_RSI: return &r->rsi;
    case RHV_X86_RDI: return &r->rdi;
    case RHV_X86_RSP: return &r->rsp;
    case RHV_X86_RBP: return &r->rbp;
    case RHV_X86_R8: return &r->r8;
    case RHV_X86_R9: return &r->r9;
    case RHV_X86_R10: return &r->r10;
    case RHV_X86_R11: return &r->r11;
    case RHV_X86_R12: return &r->r12;
    case RHV_X86_R13: return &r->r13;
    case RHV_X86_R14: return &r->r14;
    case RHV_X86_R15: return &r->r15;
    default: return NULL;
    }
}

int32_t rhv_vcpu_read_register(uint32_t vcpu, uint32_t reg, uint64_t *value)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    if (!value || reg >= RHV_X86_REGISTERS_MAX)
        return RHV_HV_BAD_ARGUMENT;

    if (reg == RHV_X86_XCR0) {
        NEED(get_fpu);
        RHVFpuState f;
        int k = B.get_fpu(B.ctx, &f);
        if (k)
            return rhv_hv_from_ioreturn(k);
        *value = f.xcr0;
        return RHV_HV_SUCCESS;
    }
    RHVRegs regs;
    uint64_t *slot = NULL;
    if (reg <= RHV_X86_R15) {
        NEED(get_regs);
        int k = B.get_regs(B.ctx, &regs);
        if (k)
            return rhv_hv_from_ioreturn(k);
        slot = gprSlot(&regs, reg);
        *value = *slot;
        return RHV_HV_SUCCESS;
    }
    uint32_t field = rhv_core_register_field(reg);
    if (!field)
        return RHV_HV_BAD_ARGUMENT;         /* CR1 */
    NEED(vmcs_read);
    return rhv_hv_from_ioreturn(B.vmcs_read(B.ctx, field, value));
}

int32_t rhv_vcpu_write_register(uint32_t vcpu, uint32_t reg, uint64_t value)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    if (reg >= RHV_X86_REGISTERS_MAX)
        return RHV_HV_BAD_ARGUMENT;

    if (reg == RHV_X86_XCR0) {
        NEED(get_fpu);
        NEED(set_fpu);
        RHVFpuState f;
        int k = B.get_fpu(B.ctx, &f);
        if (k)
            return rhv_hv_from_ioreturn(k);
        /* Components the new XCR0 turns off must not stay marked as in use in the saved image. */
        uint64_t bv;
        memcpy(&bv, f.area + 512, 8);
        bv &= value | 3;
        memcpy(f.area + 512, &bv, 8);
        f.xcr0 = value;
        return rhv_hv_from_ioreturn(B.set_fpu(B.ctx, &f));
    }
    if (reg <= RHV_X86_R15) {
        NEED(get_regs);
        NEED(set_regs);
        RHVRegs regs;
        int k = B.get_regs(B.ctx, &regs);
        if (k)
            return rhv_hv_from_ioreturn(k);
        *gprSlot(&regs, reg) = value;
        return rhv_hv_from_ioreturn(B.set_regs(B.ctx, &regs));
    }
    uint32_t field = rhv_core_register_field(reg);
    if (!field)
        return RHV_HV_BAD_ARGUMENT;
    NEED(vmcs_write);
    return rhv_hv_from_ioreturn(B.vmcs_write(B.ctx, field, value));
}

/* ---- extended state, MSRs ---- */

int32_t rhv_vcpu_read_fpstate(uint32_t vcpu, void *buffer, size_t size)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    NEED(get_fpu);
    if (!buffer)
        return RHV_HV_BAD_ARGUMENT;
    RHVFpuState f;
    int k = B.get_fpu(B.ctx, &f);
    if (k)
        return rhv_hv_from_ioreturn(k);
    if (size < f.size)
        return RHV_HV_BAD_ARGUMENT;
    memcpy(buffer, f.area, f.size);
    return RHV_HV_SUCCESS;
}

int32_t rhv_vcpu_write_fpstate(uint32_t vcpu, const void *buffer, size_t size)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    NEED(get_fpu);
    NEED(set_fpu);
    if (!buffer)
        return RHV_HV_BAD_ARGUMENT;
    RHVFpuState f;
    int k = B.get_fpu(B.ctx, &f);
    if (k)
        return rhv_hv_from_ioreturn(k);
    if (size < f.size)
        return RHV_HV_BAD_ARGUMENT;
    memcpy(f.area, buffer, f.size);
    return rhv_hv_from_ioreturn(B.set_fpu(B.ctx, &f));
}

int32_t rhv_vcpu_read_msr(uint32_t vcpu, uint32_t msr, uint64_t *value)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    NEED(msr);
    if (!value)
        return RHV_HV_BAD_ARGUMENT;
    return rhv_hv_from_ioreturn(B.msr(B.ctx, RHV_MSR_READ, msr, 0, value));
}

int32_t rhv_vcpu_write_msr(uint32_t vcpu, uint32_t msr, uint64_t value)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    NEED(msr);
    uint64_t out;
    return rhv_hv_from_ioreturn(B.msr(B.ctx, RHV_MSR_WRITE, msr, value, &out));
}

int32_t rhv_vcpu_enable_native_msr(uint32_t vcpu, uint32_t msr, int enable)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    NEED(msr);
    uint64_t out;
    return rhv_hv_from_ioreturn(B.msr(B.ctx, enable ? RHV_MSR_NATIVE_ENABLE : RHV_MSR_NATIVE_DISABLE, msr, 0, &out));
}

/* ---- VMX ---- */

int32_t rhv_vmx_vcpu_read_vmcs(uint32_t vcpu, uint32_t field, uint64_t *value)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    NEED(vmcs_read);
    if (!value)
        return RHV_HV_BAD_ARGUMENT;
    return rhv_hv_from_ioreturn(B.vmcs_read(B.ctx, field, value));
}

int32_t rhv_vmx_vcpu_write_vmcs(uint32_t vcpu, uint32_t field, uint64_t value)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    NEED(vmcs_write);
    return rhv_hv_from_ioreturn(B.vmcs_write(B.ctx, field, value));
}

/*
 * What a VMX client may ask for. Each value has the layout of the matching IA32_VMX_* MSR: low 32 bits = controls that
 * must be 1, high 32 bits = controls that may be 1. Clients compute their controls as (wanted | low) & high, so a bit
 * we leave out of "high" is silently not requested. Everything claimed here is either implemented by VmcsState
 * (kext/Source/VmcsState.hpp) or harmless to ignore (the VM-entry/exit "load" controls: the state fields are
 * authoritative). APIC virtualization, posted interrupts, the preemption timer and NMI-window exiting are NOT offered.
 */
static uint64_t cap(uint32_t must1, uint32_t may1)
{
    return ((uint64_t)(may1 | must1) << 32) | must1;
}

enum {
    PIN_EXTINT = 1u << 0, PIN_NMI = 1u << 3,
    CPU_INTR_WINDOW = 1u << 2, CPU_TSC_OFFSETING = 1u << 3, CPU_HLT = 1u << 7, CPU_INVLPG = 1u << 9,
    CPU_MWAIT = 1u << 10, CPU_RDPMC = 1u << 11, CPU_RDTSC = 1u << 12, CPU_CR3_LOAD = 1u << 15, CPU_CR3_STORE = 1u << 16,
    CPU_CR8_LOAD = 1u << 19, CPU_CR8_STORE = 1u << 20, CPU_MOV_DR = 1u << 23, CPU_IO = 1u << 24,
    CPU_IO_BITMAPS = 1u << 25, CPU_MSR_BITMAPS = 1u << 28, CPU_MONITOR = 1u << 29, CPU_PAUSE = 1u << 30,
    CPU_SECONDARY = 1u << 31,
    CPU2_EPT = 1u << 1, CPU2_RDTSCP = 1u << 3, CPU2_VPID = 1u << 5, CPU2_WBINVD = 1u << 6,
    CPU2_UNRESTRICTED = 1u << 7, CPU2_INVPCID = 1u << 12,
    ENTRY_IA32E = 1u << 9, ENTRY_LOAD_PAT = 1u << 14, ENTRY_LOAD_EFER = 1u << 15,
    EXIT_HOST_ADDR_SPACE = 1u << 9, EXIT_ACK_INTR = 1u << 15, EXIT_SAVE_PAT = 1u << 18, EXIT_LOAD_PAT = 1u << 19,
    EXIT_SAVE_EFER = 1u << 20, EXIT_LOAD_EFER = 1u << 21
};

int32_t rhv_vmx_read_capability(uint32_t c, uint64_t *value)
{
    if (!value)
        return RHV_HV_BAD_ARGUMENT;
    const uint32_t proc = CPU_INTR_WINDOW | CPU_TSC_OFFSETING | CPU_HLT | CPU_INVLPG | CPU_MWAIT | CPU_RDPMC | CPU_RDTSC |
                          CPU_CR3_LOAD | CPU_CR3_STORE | CPU_CR8_LOAD | CPU_CR8_STORE | CPU_MOV_DR | CPU_IO |
                          CPU_IO_BITMAPS | CPU_MSR_BITMAPS | CPU_MONITOR | CPU_PAUSE | CPU_SECONDARY;
    switch (c) {
    case RHV_VMX_CAP_PINBASED:
    case RHV_VMX_CAP_TRUE_PINBASED:
        *value = cap(0x16, PIN_EXTINT | PIN_NMI);
        break;
    case RHV_VMX_CAP_PROCBASED:
    case RHV_VMX_CAP_TRUE_PROCBASED:
        /* The "true" must-be-one set: CR3 load/store exiting is not forced, so the guest changes CR3 without exits. */
        *value = cap(0x04006172, proc);
        break;
    case RHV_VMX_CAP_PROCBASED2:
        *value = cap(0, CPU2_EPT | CPU2_RDTSCP | CPU2_VPID | CPU2_WBINVD | CPU2_UNRESTRICTED | CPU2_INVPCID);
        break;
    case RHV_VMX_CAP_ENTRY:
        *value = cap(0x000011ff, ENTRY_IA32E | ENTRY_LOAD_PAT | ENTRY_LOAD_EFER);
        break;
    case RHV_VMX_CAP_TRUE_ENTRY:
        *value = cap(0x000011fb, ENTRY_IA32E | ENTRY_LOAD_PAT | ENTRY_LOAD_EFER);
        break;
    case RHV_VMX_CAP_EXIT:
        *value = cap(0x00036dff, EXIT_HOST_ADDR_SPACE | EXIT_ACK_INTR | EXIT_SAVE_PAT | EXIT_LOAD_PAT | EXIT_SAVE_EFER | EXIT_LOAD_EFER);
        break;
    case RHV_VMX_CAP_TRUE_EXIT:
        *value = cap(0x00036dfb, EXIT_HOST_ADDR_SPACE | EXIT_ACK_INTR | EXIT_SAVE_PAT | EXIT_LOAD_PAT | EXIT_SAVE_EFER | EXIT_LOAD_EFER);
        break;
    case RHV_VMX_CAP_BASIC:
        /* revision 1, 4 KiB region, write-back, "true" control MSRs exist (bit 55) */
        *value = 1ull | (0x1000ull << 32) | (6ull << 50) | (1ull << 55);
        break;
    case RHV_VMX_CAP_MISC:
        *value = (1u << 6) | (4u << 16);          /* HLT activity state; 4 CR3-target values */
        break;
    case RHV_VMX_CAP_CR0_FIXED0: *value = 0x20; break;             /* NE; PE and PG are free (unrestricted guest) */
    case RHV_VMX_CAP_CR0_FIXED1: *value = 0xFFFFFFFFull; break;
    case RHV_VMX_CAP_CR4_FIXED0: *value = 0; break;                /* no VMXE: the guest is not run inside VMX */
    case RHV_VMX_CAP_CR4_FIXED1: *value = 0x00FFDFFFull; break;    /* every CR4 bit AMD has; bit 13 (VMXE) is reserved */
    default:
        return RHV_HV_BAD_ARGUMENT;
    }
    return RHV_HV_SUCCESS;
}
