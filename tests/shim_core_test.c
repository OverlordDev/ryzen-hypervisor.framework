/*
 * shim_core_test.c - tests for shim/rhv_core.c with a fake kernel behind it. Also compares our copies of the API's
 * constants (error codes, register numbers, capability numbers) with the real SDK header.
 */
#include <Hypervisor/Hypervisor.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "../shim/rhv_core.h"
#include "../shim/rhv_fields.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ---- constants must equal Apple's ---- */
_Static_assert(RHV_HV_SUCCESS == HV_SUCCESS, "HV_SUCCESS");
_Static_assert(RHV_HV_ERROR == HV_ERROR, "HV_ERROR");
_Static_assert(RHV_HV_BUSY == HV_BUSY, "HV_BUSY");
_Static_assert(RHV_HV_BAD_ARGUMENT == HV_BAD_ARGUMENT, "HV_BAD_ARGUMENT");
_Static_assert(RHV_HV_NO_RESOURCES == HV_NO_RESOURCES, "HV_NO_RESOURCES");
_Static_assert(RHV_HV_NO_DEVICE == HV_NO_DEVICE, "HV_NO_DEVICE");
_Static_assert(RHV_HV_DENIED == HV_DENIED, "HV_DENIED");
_Static_assert(RHV_HV_UNSUPPORTED == HV_UNSUPPORTED, "HV_UNSUPPORTED");
#define SAME(a, b) _Static_assert((int)(a) == (int)(b), #a)
SAME(RHV_X86_RIP, HV_X86_RIP); SAME(RHV_X86_RFLAGS, HV_X86_RFLAGS); SAME(RHV_X86_RAX, HV_X86_RAX);
SAME(RHV_X86_RSP, HV_X86_RSP); SAME(RHV_X86_R15, HV_X86_R15); SAME(RHV_X86_CS, HV_X86_CS); SAME(RHV_X86_GS, HV_X86_GS);
SAME(RHV_X86_IDT_BASE, HV_X86_IDT_BASE); SAME(RHV_X86_GDT_LIMIT, HV_X86_GDT_LIMIT); SAME(RHV_X86_LDTR, HV_X86_LDTR);
SAME(RHV_X86_LDT_AR, HV_X86_LDT_AR); SAME(RHV_X86_TR, HV_X86_TR); SAME(RHV_X86_TSS_AR, HV_X86_TSS_AR);
SAME(RHV_X86_CR0, HV_X86_CR0); SAME(RHV_X86_CR1, HV_X86_CR1); SAME(RHV_X86_CR2, HV_X86_CR2); SAME(RHV_X86_CR3, HV_X86_CR3);
SAME(RHV_X86_CR4, HV_X86_CR4); SAME(RHV_X86_DR0, HV_X86_DR0); SAME(RHV_X86_DR3, HV_X86_DR3); SAME(RHV_X86_DR4, HV_X86_DR4);
SAME(RHV_X86_DR6, HV_X86_DR6); SAME(RHV_X86_DR7, HV_X86_DR7); SAME(RHV_X86_TPR, HV_X86_TPR); SAME(RHV_X86_XCR0, HV_X86_XCR0);
SAME(RHV_X86_REGISTERS_MAX, HV_X86_REGISTERS_MAX);
SAME(RHV_VMX_CAP_PINBASED, HV_VMX_CAP_PINBASED); SAME(RHV_VMX_CAP_PROCBASED, HV_VMX_CAP_PROCBASED);
SAME(RHV_VMX_CAP_PROCBASED2, HV_VMX_CAP_PROCBASED2); SAME(RHV_VMX_CAP_ENTRY, HV_VMX_CAP_ENTRY);
SAME(RHV_VMX_CAP_EXIT, HV_VMX_CAP_EXIT); SAME(RHV_VMX_CAP_BASIC, HV_VMX_CAP_BASIC);
SAME(RHV_VMX_CAP_TRUE_PROCBASED, HV_VMX_CAP_TRUE_PROCBASED); SAME(RHV_VMX_CAP_MISC, HV_VMX_CAP_MISC);
SAME(RHV_VMX_CAP_CR0_FIXED0, HV_VMX_CAP_CR0_FIXED0); SAME(RHV_VMX_CAP_CR4_FIXED1, HV_VMX_CAP_CR4_FIXED1);
SAME(RHV_HV_MEMORY_MASK, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);

/* ---- the fake kernel ---- */
#define IOR_BAD   0xe00002c2
#define IOR_BUSY  0xe00002d5
#define IOR_UNSUP 0xe00002c7

static struct {
    int vmCreated, vmDestroyed, kicks, runs;
    int getRegs, setRegs, vmcsReads, vmcsWrites;     /* calls that reached the "kernel" */
    int runEx, runExNoRun, readMany;                 /* fast path calls */
    uint32_t createFlags;
    struct { uint64_t uva, gpa, size; uint32_t flags; } map;
    struct { uint64_t gpa, size; } unmap;
    struct { uint64_t gpa, size; uint32_t flags; } prot;
    uint64_t vmcs[0x10000];                 /* indexed by field encoding */
    RHVRegs regs;
    RHVFpuState fpu;
    uint64_t msrs[16][2];                   /* msr number, value */
    int msrCount;
    int nativeMsr;                          /* last native op */
    uint32_t nativeMsrNum;
    uint64_t hostTsc, now;
    unsigned long thread;
    int failNext;
} K;

static int f_vm_create(void *c, uint32_t flags) { (void)c; K.vmCreated++; K.createFlags = flags; return 0; }
static int f_vm_destroy(void *c) { (void)c; K.vmDestroyed++; return 0; }
static int f_vm_map(void *c, uint64_t u, uint64_t g, uint64_t s, uint32_t f)
{ (void)c; K.map.uva = u; K.map.gpa = g; K.map.size = s; K.map.flags = f; return s == 0 ? IOR_BAD : (g == 0x7000 ? IOR_BUSY : 0); }
static int f_vm_unmap(void *c, uint64_t g, uint64_t s) { (void)c; K.unmap.gpa = g; K.unmap.size = s; return 0; }
static int f_vm_protect(void *c, uint64_t g, uint64_t s, uint32_t f) { (void)c; K.prot.gpa = g; K.prot.size = s; K.prot.flags = f; return 0xe00002f0; }
static int f_vmcs_read(void *c, uint32_t f, uint64_t *v) { (void)c; K.vmcsReads++; if (f >= 0x10000) return IOR_BAD; *v = K.vmcs[f]; return 0; }
static int f_vmcs_write(void *c, uint32_t f, uint64_t v) { (void)c; K.vmcsWrites++; if (f >= 0x10000) return IOR_BAD; K.vmcs[f] = v; return 0; }
static int f_get_regs(void *c, RHVRegs *r) { (void)c; K.getRegs++; *r = K.regs; return 0; }
static int f_set_regs(void *c, const RHVRegs *r) { (void)c; K.setRegs++; K.regs = *r; return 0; }
static int f_get_fpu(void *c, RHVFpuState *f) { (void)c; *f = K.fpu; return 0; }
static int f_set_fpu(void *c, const RHVFpuState *f) { (void)c; if (f->xcr0 == 0) return IOR_BAD; K.fpu = *f; return 0; }
static int f_msr(void *c, uint32_t op, uint32_t msr, uint64_t value, uint64_t *out)
{
    (void)c;
    if (msr == 0x80) return IOR_BAD;
    if (op == RHV_MSR_NATIVE_ENABLE || op == RHV_MSR_NATIVE_DISABLE) { K.nativeMsr = (int)op; K.nativeMsrNum = msr; return 0; }
    for (int i = 0; i < K.msrCount; i++)
        if (K.msrs[i][0] == msr) {
            if (op == RHV_MSR_READ) *out = K.msrs[i][1]; else K.msrs[i][1] = value;
            return 0;
        }
    if (op == RHV_MSR_WRITE) { K.msrs[K.msrCount][0] = msr; K.msrs[K.msrCount][1] = value; K.msrCount++; return 0; }
    *out = 0;
    return 0;
}
static int f_run(void *c, RHVExit *e) { (void)c; K.runs++; K.now += 1000; K.regs.rip += 2; K.vmcs[0x4402] = 12 + (uint64_t)K.runs; e->reason = RHV_EXIT_HLT; return K.failNext ? (int)0xe00002d8 : 0; }
static int f_kick(void *c) { (void)c; K.kicks++; return 0; }
static uint64_t f_now(void *c) { (void)c; return K.now; }
static uint64_t f_tsc(void *c) { (void)c; return K.hostTsc; }
static unsigned long f_thread(void *c) { (void)c; return K.thread; }

/* The fast path: the same fake kernel behind VmRunEx / VmcsReadMany. */
static const uint32_t kFastFields[RHV_RUN_NFIELDS] = {
#define X(e) e,
    RHV_RUN_FIELDS_LIST(X)
#undef X
};

static int f_run_ex(void *c, const RHVRunIn *in, RHVRunOut *out)
{
    (void)c;
    memset(out, 0, sizeof(*out));
    K.runEx++;
    if (in->nWrites > RHV_RUN_MAX_WRITES) return IOR_BAD;
    if (in->flags & RHV_RUN_REGS) K.regs = in->regs;
    for (uint32_t i = 0; i < in->nWrites; i++) {
        if (in->writes[i].field >= 0x10000) { out->failedWrite = i; return IOR_BAD; }
        K.vmcsWrites++;
        K.vmcs[in->writes[i].field] = in->writes[i].value;
    }
    if (in->flags & RHV_RUN_NO_RUN) K.runExNoRun++;
    else {
        RHVExit e;
        int rc = f_run(c, &e);
        if (rc) return rc;
    }
    out->regs = K.regs;
    for (unsigned i = 0; i < RHV_RUN_NFIELDS; i++) out->fields[i] = K.vmcs[kFastFields[i]];
    return 0;
}

static int f_read_many(void *c, const RHVReadMany *in, RHVReadManyOut *out)
{
    (void)c;
    K.readMany++;
    memset(out, 0, sizeof(*out));
    if (in->count > RHV_READMANY_MAX) return IOR_BAD;
    for (uint32_t i = 0; i < in->count; i++) out->values[i] = K.vmcs[in->fields[i]];
    return 0;
}

static void freshFast(void)
{
    memset(&K, 0, sizeof(K));
    K.fpu.xcr0 = 1;
    K.fpu.size = 832;
    K.thread = 1;
    RhvBackend b = { NULL, f_vm_create, f_vm_destroy, f_vm_map, f_vm_unmap, f_vm_protect, f_vmcs_read, f_vmcs_write,
                     f_get_regs, f_set_regs, f_get_fpu, f_set_fpu, f_msr, f_run, f_kick, f_now, f_tsc, f_thread,
                     f_run_ex, f_read_many };
    rhv_core_init(&b);
}

static void fresh(void)
{
    memset(&K, 0, sizeof(K));
    K.fpu.xcr0 = 1;
    K.fpu.size = 832;
    K.thread = 1;
    RhvBackend b = { NULL, f_vm_create, f_vm_destroy, f_vm_map, f_vm_unmap, f_vm_protect, f_vmcs_read, f_vmcs_write,
                     f_get_regs, f_set_regs, f_get_fpu, f_set_fpu, f_msr, f_run, f_kick, f_now, f_tsc, f_thread,
                     NULL, NULL };               /* the one-call-per-field path */
    rhv_core_init(&b);
}

static uint32_t makeVcpu(void)
{
    uint32_t v = 99;
    CHECK(rhv_vm_create(0) == RHV_HV_SUCCESS);
    CHECK(rhv_vcpu_create(&v, 0) == RHV_HV_SUCCESS);
    return v;
}

static void testLifecycle(void)
{
    fresh();
    uint32_t v = 0;
    uint64_t x;
    /* Nothing works without a VM. */
    CHECK(rhv_vcpu_create(&v, 0) == RHV_HV_NO_DEVICE);
    CHECK(rhv_vm_map(0x1000, 0, 4096, 7) == RHV_HV_NO_DEVICE);
    CHECK(rhv_vm_destroy() == RHV_HV_NO_DEVICE);
    CHECK(rhv_vcpu_read_register(0, RHV_X86_RIP, &x) == RHV_HV_NO_DEVICE);

    CHECK(rhv_vm_create(0) == RHV_HV_SUCCESS);
    CHECK(K.vmCreated == 1 && (K.createFlags & RHV_VMCREATE_VMX) && (K.createFlags & RHV_VMCREATE_NO_RAM));
    CHECK(rhv_vm_create(0) == RHV_HV_BUSY && K.vmCreated == 1);                    /* one VM per process */

    CHECK(rhv_vcpu_create(NULL, 0) == RHV_HV_BAD_ARGUMENT);
    CHECK(rhv_vcpu_create(&v, 8) == RHV_HV_BAD_ARGUMENT);                           /* unknown option bits */
    CHECK(rhv_vcpu_create(&v, 0) == RHV_HV_SUCCESS && v == 0);
    uint32_t v2;
    CHECK(rhv_vcpu_create(&v2, 0) == RHV_HV_NO_RESOURCES);                          /* one vCPU */
    CHECK(rhv_vm_destroy() == RHV_HV_BUSY);                                         /* vCPU still there */
    CHECK(rhv_vcpu_read_register(5, RHV_X86_RIP, &x) == RHV_HV_BAD_ARGUMENT);       /* wrong vCPU id */
    CHECK(rhv_vcpu_destroy(0) == RHV_HV_SUCCESS);
    CHECK(rhv_vcpu_destroy(0) == RHV_HV_BAD_ARGUMENT);
    CHECK(rhv_vm_destroy() == RHV_HV_SUCCESS && K.vmDestroyed == 1);
    CHECK(rhv_vm_destroy() == RHV_HV_NO_DEVICE);
    CHECK(rhv_vm_create(0) == RHV_HV_SUCCESS);                                      /* and again */
}

static void testMemory(void)
{
    fresh();
    CHECK(rhv_vm_create(0) == RHV_HV_SUCCESS);
    CHECK(rhv_vm_map(0xabc000, 0x100000, 0x2000, 3) == RHV_HV_SUCCESS);
    CHECK(K.map.uva == 0xabc000 && K.map.gpa == 0x100000 && K.map.size == 0x2000 && K.map.flags == 3);
    CHECK(rhv_vm_map(0xabc000, 0, 4096, 8) == RHV_HV_BAD_ARGUMENT);                 /* HV_MEMORY_UEXEC et al. */
    CHECK(rhv_vm_map(0xabc000, 0, 0, 7) == RHV_HV_BAD_ARGUMENT);                    /* kernel BadArgument */
    CHECK(rhv_vm_map(0xabc000, 0x7000, 4096, 7) == RHV_HV_BUSY);                    /* kernel Busy */
    CHECK(rhv_vm_unmap(0x100000, 0x1000) == RHV_HV_SUCCESS && K.unmap.gpa == 0x100000 && K.unmap.size == 0x1000);
    CHECK(rhv_vm_protect(0x100000, 0x1000, 1) == RHV_HV_BAD_ARGUMENT && K.prot.flags == 1);   /* kernel NotFound */
    CHECK(rhv_vm_protect(0x100000, 0x1000, 16) == RHV_HV_BAD_ARGUMENT);
}

static void testRegisters(void)
{
    fresh();
    uint32_t v = makeVcpu();
    uint64_t x;

    /* General purpose registers go through RHVRegs, each to its own slot. */
    for (uint32_t r = RHV_X86_RIP; r <= RHV_X86_R15; r++)
        CHECK(rhv_vcpu_write_register(v, r, 0x1000 + r) == RHV_HV_SUCCESS);
    CHECK(rhv_vcpu_flush(v) == RHV_HV_SUCCESS);                  /* register writes are buffered until flush or run */
    CHECK(K.regs.rip == 0x1000 && K.regs.rflags == 0x1001 && K.regs.rax == 0x1002 && K.regs.rcx == 0x1003 &&
          K.regs.rdx == 0x1004 && K.regs.rbx == 0x1005 && K.regs.rsi == 0x1006 && K.regs.rdi == 0x1007 &&
          K.regs.rsp == 0x1008 && K.regs.rbp == 0x1009 && K.regs.r8 == 0x100a && K.regs.r15 == 0x1011);
    for (uint32_t r = RHV_X86_RIP; r <= RHV_X86_R15; r++)
        CHECK(rhv_vcpu_read_register(v, r, &x) == RHV_HV_SUCCESS && x == 0x1000 + r);

    /* Writing one register must not disturb the others. */
    CHECK(rhv_vcpu_write_register(v, RHV_X86_RAX, 0xAA) == RHV_HV_SUCCESS && rhv_vcpu_flush(v) == RHV_HV_SUCCESS);
    CHECK(K.regs.rbx == 0x1005 && K.regs.rax == 0xAA);

    /* Everything else is a VMCS (or extension) field, each to its own encoding. */
    struct { uint32_t reg, field; } map[] = {
        { RHV_X86_CS, 0x802 }, { RHV_X86_SS, 0x804 }, { RHV_X86_DS, 0x806 }, { RHV_X86_ES, 0x800 }, { RHV_X86_FS, 0x808 },
        { RHV_X86_GS, 0x80a }, { RHV_X86_IDT_BASE, 0x6818 }, { RHV_X86_IDT_LIMIT, 0x4812 }, { RHV_X86_GDT_BASE, 0x6816 },
        { RHV_X86_GDT_LIMIT, 0x4810 }, { RHV_X86_LDTR, 0x80c }, { RHV_X86_LDT_BASE, 0x6812 }, { RHV_X86_LDT_LIMIT, 0x480c },
        { RHV_X86_LDT_AR, 0x4820 }, { RHV_X86_TR, 0x80e }, { RHV_X86_TSS_BASE, 0x6814 }, { RHV_X86_TSS_LIMIT, 0x480e },
        { RHV_X86_TSS_AR, 0x4822 }, { RHV_X86_CR0, 0x6800 }, { RHV_X86_CR3, 0x6802 }, { RHV_X86_CR4, 0x6804 },
        { RHV_X86_CR2, 0xF000 }, { RHV_X86_DR0, 0xF010 }, { RHV_X86_DR1, 0xF012 }, { RHV_X86_DR2, 0xF014 },
        { RHV_X86_DR3, 0xF016 }, { RHV_X86_DR6, 0xF002 }, { RHV_X86_DR7, 0x681a }, { RHV_X86_TPR, 0xF004 },
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        CHECK(rhv_core_register_field(map[i].reg) == map[i].field);
        CHECK(rhv_vcpu_write_register(v, map[i].reg, 0x5000 + i) == RHV_HV_SUCCESS);
        CHECK(K.vmcs[map[i].field] == 0x5000 + i);
        CHECK(rhv_vcpu_read_register(v, map[i].reg, &x) == RHV_HV_SUCCESS && x == 0x5000 + i);
    }
    /* DR4/DR5 alias DR6/DR7. */
    CHECK(rhv_vcpu_write_register(v, RHV_X86_DR4, 0xD6) == RHV_HV_SUCCESS && K.vmcs[0xF002] == 0xD6);
    CHECK(rhv_vcpu_read_register(v, RHV_X86_DR5, &x) == RHV_HV_SUCCESS && x == K.vmcs[0x681a]);
    /* Registers the hardware does not have. */
    CHECK(rhv_vcpu_read_register(v, RHV_X86_CR1, &x) == RHV_HV_BAD_ARGUMENT);
    CHECK(rhv_vcpu_write_register(v, RHV_X86_CR1, 0) == RHV_HV_BAD_ARGUMENT);
    CHECK(rhv_vcpu_read_register(v, RHV_X86_REGISTERS_MAX, &x) == RHV_HV_BAD_ARGUMENT);
    CHECK(rhv_vcpu_read_register(v, 9999, &x) == RHV_HV_BAD_ARGUMENT);
    CHECK(rhv_vcpu_read_register(v, RHV_X86_RIP, NULL) == RHV_HV_BAD_ARGUMENT);

    /* XCR0 is in the FPU state. */
    CHECK(rhv_vcpu_read_register(v, RHV_X86_XCR0, &x) == RHV_HV_SUCCESS && x == 1);
    uint64_t bv = 7;
    memcpy(K.fpu.area + 512, &bv, 8);                      /* the saved image says AVX state is in use */
    CHECK(rhv_vcpu_write_register(v, RHV_X86_XCR0, 7) == RHV_HV_SUCCESS && K.fpu.xcr0 == 7);
    memcpy(&bv, K.fpu.area + 512, 8);
    CHECK(bv == 7);
    CHECK(rhv_vcpu_write_register(v, RHV_X86_XCR0, 3) == RHV_HV_SUCCESS);
    memcpy(&bv, K.fpu.area + 512, 8);
    CHECK(bv == 3);                                        /* AVX no longer marked in use */
    CHECK(rhv_vcpu_write_register(v, RHV_X86_XCR0, 0) == RHV_HV_BAD_ARGUMENT);   /* kernel refuses */
}

/* The fast path: one kernel call per guest exit, writes buffered, the usual reads served from the reply. */
static void testFastPath(void)
{
    freshFast();
    uint32_t v = makeVcpu();
    uint64_t x;
    K.regs.rax = 0x11; K.regs.rip = 0x2000; K.regs.rflags = 0x202;
    K.vmcs[0x4402] = 30; K.vmcs[0x6400] = 0x80;

    /* Writes wait for the run, in one call, together with the dirty registers. */
    CHECK(rhv_vmx_vcpu_write_vmcs(v, 0x4002, 0x840065fe) == RHV_HV_SUCCESS);
    CHECK(rhv_vmx_vcpu_write_vmcs(v, 0x4016, 0x80000030) == RHV_HV_SUCCESS);
    CHECK(rhv_vmx_vcpu_write_vmcs(v, 0x4002, 0x840065fa) == RHV_HV_SUCCESS);          /* same field again: last wins */
    CHECK(rhv_vcpu_write_register(v, RHV_X86_RAX, 0x99) == RHV_HV_SUCCESS);
    CHECK(K.vmcsWrites == 0 && K.runEx == 0 && K.getRegs == 1);                       /* only the register fetch */
    CHECK(rhv_vcpu_run(v) == RHV_HV_SUCCESS);
    CHECK(K.runEx == 1 && K.vmcsWrites == 2 && K.vmcs[0x4002] == 0x840065fa && K.vmcs[0x4016] == 0x80000030);
    CHECK(K.regs.rax == 0x99 && K.runs == 1 && K.setRegs == 0);

    /* After the exit nothing more is asked of the kernel for the usual reads. */
    int calls = K.getRegs + K.vmcsReads + K.readMany + K.runEx;
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x4402, &x) == RHV_HV_SUCCESS && x == 13);
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x6400, &x) == RHV_HV_SUCCESS && x == 0x80);
    CHECK(rhv_vcpu_read_register(v, RHV_X86_RIP, &x) == RHV_HV_SUCCESS && x == 0x2002);
    CHECK(rhv_vcpu_read_register(v, RHV_X86_RAX, &x) == RHV_HV_SUCCESS && x == 0x99);
    CHECK(rhv_vcpu_read_register(v, RHV_X86_CR4, &x) == RHV_HV_SUCCESS);
    CHECK(rhv_vcpu_read_register(v, RHV_X86_TPR, &x) == RHV_HV_SUCCESS);
    CHECK(K.getRegs + K.vmcsReads + K.readMany + K.runEx == calls);

    /* A field the kernel refuses is refused at once, and nothing is buffered for it. */
    CHECK(rhv_vmx_vcpu_write_vmcs(v, 0x4402, 1) == RHV_HV_BAD_ARGUMENT);              /* read-only exit field */
    CHECK(rhv_vmx_vcpu_write_vmcs(v, 0xFFFFF, 1) == RHV_HV_BAD_ARGUMENT);             /* no such field */
    CHECK(rhv_vmx_vcpu_write_vmcs(v, 0xF010, 0x0000800000000000ull) == RHV_HV_BAD_ARGUMENT);   /* non-canonical DR0 */
    CHECK(rhv_vcpu_flush(v) == RHV_HV_SUCCESS && K.runEx == 1);

    /* Reading a field with a write pending sends the write first. */
    CHECK(rhv_vmx_vcpu_write_vmcs(v, 0x6804, 0x26b0) == RHV_HV_SUCCESS && K.vmcs[0x6804] == 0);
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x6804, &x) == RHV_HV_SUCCESS && x == 0x26b0);
    CHECK(K.runEx == 2 && K.runExNoRun == 1 && K.vmcs[0x6804] == 0x26b0 && K.runs == 1);

    /* An MSR access sees the buffered VMCS writes (EFER is both). */
    CHECK(rhv_vmx_vcpu_write_vmcs(v, 0x2806, 0xd01) == RHV_HV_SUCCESS);
    CHECK(rhv_vcpu_read_msr(v, 0xC0000082, &x) == RHV_HV_SUCCESS);
    CHECK(K.vmcs[0x2806] == 0xd01);
    CHECK(rhv_vcpu_write_msr(v, 0xC0000082, 5) == RHV_HV_SUCCESS);

    /* Segment state comes in one call. */
    for (uint32_t i = 0; i < 8; i++) { K.vmcs[0x800 + 2 * i] = 0x10 + i; K.vmcs[0x6806 + 2 * i] = 0x1000 * i; K.vmcs[0x4814 + 2 * i] = 0x93 + i; }
    CHECK(rhv_vcpu_run(v) == RHV_HV_SUCCESS);                                         /* drops the cache */
    int rm = K.readMany, rd = K.vmcsReads;
    CHECK(rhv_vcpu_read_register(v, RHV_X86_CS, &x) == RHV_HV_SUCCESS && x == 0x11);
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x6808, &x) == RHV_HV_SUCCESS && x == 0x1000);    /* CS base */
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x4816, &x) == RHV_HV_SUCCESS && x == 0x94);      /* CS access rights */
    CHECK(rhv_vcpu_read_register(v, RHV_X86_TR, &x) == RHV_HV_SUCCESS && x == 0x17);
    CHECK(K.readMany == rm + 1 && K.vmcsReads == rd);
    /* ... and not twice for the same exit, but again after the next run. */
    CHECK(rhv_vcpu_run(v) == RHV_HV_SUCCESS);
    CHECK(rhv_vcpu_read_register(v, RHV_X86_DS, &x) == RHV_HV_SUCCESS && K.readMany == rm + 2);

    /* More writes than fit in one call: flushed on the way, none lost, order kept. */
    uint32_t many[100];
    unsigned nMany = 0;
    for (unsigned i = 0; i < sizeof(kRhvVmcsEncodings) / sizeof(kRhvVmcsEncodings[0]) && nMany < 100; i++)
        if (rhv_vmcs_write_status(kRhvVmcsEncodings[i], 0) == RHV_FIELD_OK && kRhvVmcsEncodings[i] != 0x4002 &&
            kRhvVmcsEncodings[i] != 0x6804)
            many[nMany++] = kRhvVmcsEncodings[i];
    CHECK(nMany == 100);
    for (uint32_t i = 0; i < 150; i++)
        CHECK(rhv_vmx_vcpu_write_vmcs(v, many[i % 100], 1000 + i) == RHV_HV_SUCCESS);       /* the first 50 twice */
    CHECK(rhv_vcpu_run(v) == RHV_HV_SUCCESS);
    for (uint32_t f = 0; f < 100; f++)
        CHECK(K.vmcs[many[f]] == (f < 50 ? 1100 + f : 1000 + f));

    /* A failed run call is an error, and the buffered state does not linger. */
    CHECK(rhv_vmx_vcpu_write_vmcs(v, 0x4002, 7) == RHV_HV_SUCCESS);
    K.failNext = 1;
    CHECK(rhv_vcpu_run(v) != RHV_HV_SUCCESS);
    K.failNext = 0;
    CHECK(rhv_vcpu_flush(v) == RHV_HV_SUCCESS);
}

/* The state cache: how many calls reach the kernel, and that nothing stale is ever returned. */
static void testCache(void)
{
    fresh();
    uint32_t v = makeVcpu();
    uint64_t x;
    K.regs.rax = 0x11; K.regs.rip = 0x2000; K.regs.rsp = 0x3000; K.regs.rflags = 0x202;
    K.vmcs[0x4402] = 30; K.vmcs[0x6400] = 0x80;

    /* Sixteen registers, RIP and the VMCS twins of RIP/RSP/RFLAGS: one call. */
    for (uint32_t r = RHV_X86_RIP; r <= RHV_X86_R15; r++)
        CHECK(rhv_vcpu_read_register(v, r, &x) == RHV_HV_SUCCESS);
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x681e, &x) == RHV_HV_SUCCESS && x == 0x2000);
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x681c, &x) == RHV_HV_SUCCESS && x == 0x3000);
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x6820, &x) == RHV_HV_SUCCESS && x == 0x202);
    CHECK(K.getRegs == 1 && K.vmcsReads == 0);

    /* Other fields are read once. */
    for (int i = 0; i < 5; i++)
        CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x4402, &x) == RHV_HV_SUCCESS && x == 30);
    CHECK(K.vmcsReads == 1);

    /* Register writes are buffered and visible to reads, including as VMCS fields; one write-back. */
    CHECK(rhv_vcpu_write_register(v, RHV_X86_RAX, 0x99) == RHV_HV_SUCCESS);
    CHECK(rhv_vcpu_write_register(v, RHV_X86_RIP, 0x2004) == RHV_HV_SUCCESS);
    CHECK(rhv_vmx_vcpu_write_vmcs(v, 0x6820, 0x246) == RHV_HV_SUCCESS);
    CHECK(rhv_vcpu_read_register(v, RHV_X86_RAX, &x) == RHV_HV_SUCCESS && x == 0x99);
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x681e, &x) == RHV_HV_SUCCESS && x == 0x2004);
    CHECK(rhv_vcpu_read_register(v, RHV_X86_RFLAGS, &x) == RHV_HV_SUCCESS && x == 0x246);
    CHECK(K.setRegs == 0 && K.regs.rax == 0x11 && K.getRegs == 1);
    CHECK(rhv_vcpu_run(v) == RHV_HV_SUCCESS);
    CHECK(K.setRegs == 1 && K.runs == 1);                          /* flushed by the run, before it */

    /* The guest ran: the cache is gone, reads see what the run changed (our fake run moves RIP by 2 and the reason). */
    CHECK(rhv_vcpu_read_register(v, RHV_X86_RIP, &x) == RHV_HV_SUCCESS && x == 0x2006);
    CHECK(rhv_vcpu_read_register(v, RHV_X86_RAX, &x) == RHV_HV_SUCCESS && x == 0x99);
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x4402, &x) == RHV_HV_SUCCESS && x == 13);
    CHECK(K.getRegs == 2);

    /* A VMCS write is passed through at once, and the old value is not served afterwards. */
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x4002, &x) == RHV_HV_SUCCESS && x == 0);
    int w = K.vmcsWrites;
    CHECK(rhv_vmx_vcpu_write_vmcs(v, 0x4002, 0x84000004) == RHV_HV_SUCCESS && K.vmcsWrites == w + 1 && K.vmcs[0x4002] == 0x84000004);
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x4002, &x) == RHV_HV_SUCCESS && x == 0x84000004);
    /* An unknown field is an error every time, not a cached zero. */
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0xFFFFF, &x) == RHV_HV_BAD_ARGUMENT);
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0xFFFFF, &x) == RHV_HV_BAD_ARGUMENT);

    /* Writing an MSR can change VMCS fields (EFER): the VMCS cache is dropped. */
    K.vmcs[0x2806] = 0x500;
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x2806, &x) == RHV_HV_SUCCESS && x == 0x500);
    K.vmcs[0x2806] = 0xd01;
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x2806, &x) == RHV_HV_SUCCESS && x == 0x500);       /* still cached */
    CHECK(rhv_vcpu_write_msr(v, 0xC0000080, 0xd01) == RHV_HV_SUCCESS);
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x2806, &x) == RHV_HV_SUCCESS && x == 0xd01);

    /* Many different fields at once must all stay correct (the cache is a small hash table). */
    for (uint32_t f = 0; f < 100; f++)
        K.vmcs[0x800 + 2 * f] = 1000 + f;
    rhv_core_cache_reset();
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t f = 0; f < 100; f++)
            CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x800 + 2 * f, &x) == RHV_HV_SUCCESS && x == 1000 + f);
    CHECK(rhv_vmx_vcpu_write_vmcs(v, 0x800 + 2 * 7, 5) == RHV_HV_SUCCESS);
    for (uint32_t f = 0; f < 100; f++)
        CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x800 + 2 * f, &x) == RHV_HV_SUCCESS && x == (f == 7 ? 5u : 1000 + f));

    /* A dead vCPU leaves nothing behind. */
    CHECK(rhv_vcpu_write_register(v, RHV_X86_RBX, 0x77) == RHV_HV_SUCCESS);
    CHECK(rhv_vcpu_destroy(v) == RHV_HV_SUCCESS);
    uint32_t v2 = 9;
    CHECK(rhv_vcpu_create(&v2, 0) == RHV_HV_SUCCESS);
    K.regs.rbx = 0x55;
    CHECK(rhv_vcpu_read_register(v2, RHV_X86_RBX, &x) == RHV_HV_SUCCESS && x == 0x55);
}

static void testFpuMsrVmcs(void)
{
    fresh();
    uint32_t v = makeVcpu();
    uint8_t buf[1024];
    for (int i = 0; i < 832; i++) K.fpu.area[i] = (uint8_t)i;
    CHECK(rhv_vcpu_read_fpstate(v, buf, 831) == RHV_HV_BAD_ARGUMENT);              /* buffer too small */
    CHECK(rhv_vcpu_read_fpstate(v, NULL, 1024) == RHV_HV_BAD_ARGUMENT);
    memset(buf, 0, sizeof(buf));
    CHECK(rhv_vcpu_read_fpstate(v, buf, sizeof(buf)) == RHV_HV_SUCCESS);
    CHECK(memcmp(buf, K.fpu.area, 832) == 0 && buf[832] == 0);                      /* exactly size bytes */
    for (int i = 0; i < 832; i++) buf[i] = (uint8_t)(255 - (i & 0xFF));
    memset(buf + 512, 0, 64);                                                       /* a valid, empty header */
    CHECK(rhv_vcpu_write_fpstate(v, buf, 100) == RHV_HV_BAD_ARGUMENT);
    CHECK(rhv_vcpu_write_fpstate(v, buf, 832) == RHV_HV_SUCCESS && memcmp(K.fpu.area, buf, 832) == 0);

    uint64_t x = 0;
    CHECK(rhv_vcpu_write_msr(v, 0xC0000082, 0xdead) == RHV_HV_SUCCESS);
    CHECK(rhv_vcpu_read_msr(v, 0xC0000082, &x) == RHV_HV_SUCCESS && x == 0xdead);
    CHECK(rhv_vcpu_read_msr(v, 0x1B, &x) == RHV_HV_SUCCESS && x == 0xFEE00900ull);  /* APIC base: answered by the shim */
    CHECK(rhv_vcpu_write_msr(v, 0x1B, 0xFEE00800ull) == RHV_HV_SUCCESS);
    CHECK(rhv_vcpu_read_msr(v, 0x1B, &x) == RHV_HV_SUCCESS && x == 0xFEE00800ull && K.msrCount == 1);   /* never reached the kernel */
    CHECK(rhv_vcpu_read_msr(v, 0x80, &x) == RHV_HV_BAD_ARGUMENT);                   /* other unknown MSRs: the kernel refuses */
    CHECK(rhv_vcpu_enable_native_msr(v, 0xC0000081, 1) == RHV_HV_SUCCESS && K.nativeMsr == RHV_MSR_NATIVE_ENABLE && K.nativeMsrNum == 0xC0000081);
    CHECK(rhv_vcpu_enable_native_msr(v, 0xC0000081, 0) == RHV_HV_SUCCESS && K.nativeMsr == RHV_MSR_NATIVE_DISABLE);

    CHECK(rhv_vmx_vcpu_write_vmcs(v, 0x4002, 0x8401E172) == RHV_HV_SUCCESS);
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x4002, &x) == RHV_HV_SUCCESS && x == 0x8401E172);
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0xFFFFF, &x) == RHV_HV_BAD_ARGUMENT);          /* kernel refuses */
    CHECK(rhv_vmx_vcpu_read_vmcs(v, 0x4002, NULL) == RHV_HV_BAD_ARGUMENT);

    CHECK(rhv_vcpu_flush(v) == RHV_HV_SUCCESS && rhv_vcpu_invalidate_tlb(v) == RHV_HV_SUCCESS);
    K.hostTsc = 1000;
    CHECK(rhv_vm_sync_tsc(5000) == RHV_HV_SUCCESS && K.vmcs[0x2010] == 4000);       /* guest TSC = host TSC + 4000 */
}

static void *otherThread(void *arg)
{
    int *results = arg;
    uint64_t x;
    K.thread = 2;                                          /* now the "calling thread" is a different one */
    results[0] = rhv_vcpu_run(0);
    results[1] = rhv_vcpu_read_register(0, RHV_X86_RIP, &x);
    uint32_t v = 0;
    results[2] = rhv_vcpu_interrupt(&v, 1);                /* this one is allowed from anywhere */
    return NULL;
}

static void testRunAndThreads(void)
{
    fresh();
    uint32_t v = makeVcpu();
    CHECK(rhv_vcpu_run(v) == RHV_HV_SUCCESS && K.runs == 1);
    uint64_t t = 0;
    CHECK(rhv_vcpu_get_exec_time(v, &t) == RHV_HV_SUCCESS && t == 1000);
    CHECK(rhv_vcpu_run(v) == RHV_HV_SUCCESS && rhv_vcpu_get_exec_time(v, &t) == RHV_HV_SUCCESS && t == 2000);
    K.failNext = 1;
    CHECK(rhv_vcpu_run(v) == RHV_HV_NO_DEVICE);            /* kernel NotReady */
    K.failNext = 0;

    /* hv_vcpu_interrupt arguments. */
    uint32_t ids[2] = { 0, 0 }, bad = 3;
    CHECK(rhv_vcpu_interrupt(ids, 2) == RHV_HV_SUCCESS && K.kicks == 1);
    CHECK(rhv_vcpu_interrupt(&bad, 1) == RHV_HV_BAD_ARGUMENT);
    CHECK(rhv_vcpu_interrupt(NULL, 1) == RHV_HV_BAD_ARGUMENT && rhv_vcpu_interrupt(ids, 0) == RHV_HV_BAD_ARGUMENT);

    /* vCPU calls belong to the creating thread; hv_vcpu_interrupt does not. */
    int res[3] = { 0, 0, 0 };
    pthread_t th;
    pthread_create(&th, NULL, otherThread, res);
    pthread_join(th, NULL);
    CHECK(res[0] == RHV_HV_BAD_ARGUMENT);
    CHECK(res[1] == RHV_HV_BAD_ARGUMENT);
    CHECK(res[2] == RHV_HV_SUCCESS);
    K.thread = 1;
    CHECK(rhv_vcpu_run(v) == RHV_HV_SUCCESS);
}

static void testErrorMapping(void)
{
    CHECK(rhv_hv_from_ioreturn(0) == RHV_HV_SUCCESS);
    CHECK(rhv_hv_from_ioreturn((int)0xe00002bd) == RHV_HV_NO_RESOURCES);
    CHECK(rhv_hv_from_ioreturn((int)0xe00002c1) == RHV_HV_DENIED);
    CHECK(rhv_hv_from_ioreturn((int)0xe00002c2) == RHV_HV_BAD_ARGUMENT);
    CHECK(rhv_hv_from_ioreturn((int)0xe00002c7) == RHV_HV_UNSUPPORTED);
    CHECK(rhv_hv_from_ioreturn((int)0xe00002d5) == RHV_HV_BUSY);
    CHECK(rhv_hv_from_ioreturn((int)0xe00002c0) == RHV_HV_NO_DEVICE);
    CHECK(rhv_hv_from_ioreturn((int)0xe0000999) == RHV_HV_ERROR);
    CHECK(rhv_hv_from_ioreturn(-1) == RHV_HV_ERROR);
}

static void testCapabilities(void)
{
    uint64_t v;
    CHECK(rhv_vmx_read_capability(999, &v) == RHV_HV_BAD_ARGUMENT);
    CHECK(rhv_vmx_read_capability(RHV_VMX_CAP_PINBASED, NULL) == RHV_HV_BAD_ARGUMENT);

    /* Every control capability: the bits that must be 1 must also be allowed to be 1. */
    for (uint32_t c = RHV_VMX_CAP_PINBASED; c <= RHV_VMX_CAP_TRUE_EXIT; c++) {
        if (c == RHV_VMX_CAP_BASIC)
            continue;
        CHECK(rhv_vmx_read_capability(c, &v) == RHV_HV_SUCCESS);
        uint32_t must1 = (uint32_t)v, may1 = (uint32_t)(v >> 32);
        CHECK((must1 & ~may1) == 0);
    }

    /* The way QEMU's cap2ctrl builds controls from them: (wanted | must1) & may1. */
    CHECK(rhv_vmx_read_capability(RHV_VMX_CAP_PROCBASED, &v) == RHV_HV_SUCCESS);
    uint32_t wanted = (1u << 7) | (1u << 10) | (1u << 3) | (1u << 21) | (1u << 22);      /* HLT MWAIT TSC_OFFSET TPR_SHADOW NMI_WINDOW */
    uint32_t ctl = (wanted | (uint32_t)v) & (uint32_t)(v >> 32);
    CHECK((ctl & (1u << 7)) && (ctl & (1u << 10)) && (ctl & (1u << 3)));                  /* what we implement: kept */
    CHECK(!(ctl & (1u << 21)) && !(ctl & (1u << 22)));                                    /* TPR shadow, NMI window: dropped */
    CHECK(!(ctl & (1u << 15)) && !(ctl & (1u << 16)));                                    /* CR3 exiting is not forced */
    CHECK(rhv_vmx_read_capability(RHV_VMX_CAP_PROCBASED2, &v) == RHV_HV_SUCCESS);
    CHECK(!(((uint32_t)(v >> 32)) & (1u << 0)));                                          /* no APIC virtualization */
    CHECK(((uint32_t)(v >> 32)) & (1u << 7));                                             /* unrestricted guest */
    CHECK(rhv_vmx_read_capability(RHV_VMX_CAP_PINBASED, &v) == RHV_HV_SUCCESS);
    CHECK(!(((uint32_t)(v >> 32)) & (1u << 6)));                                          /* no preemption timer */
    CHECK(rhv_vmx_read_capability(RHV_VMX_CAP_CR4_FIXED1, &v) == RHV_HV_SUCCESS && (v & (1u << 13)));    /* VMXE is accepted (QEMU always sets it) */
    CHECK(rhv_vmx_read_capability(RHV_VMX_CAP_CR4_FIXED0, &v) == RHV_HV_SUCCESS && v == 0);
}

int main(void)
{
    testLifecycle();
    testMemory();
    testRegisters();
    testCache();
    testFastPath();
    testFpuMsrVmcs();
    testRunAndThreads();
    testErrorMapping();
    testCapabilities();
    if (fails) { printf("shim_core_test: %d FAILED\n", fails); return 1; }
    printf("shim_core_test: OK\n");
    return 0;
}
