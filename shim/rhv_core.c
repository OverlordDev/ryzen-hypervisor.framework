/*
 * rhv_core.c - see rhv_core.h.
 */
#include "rhv_core.h"
#include "rhv_fields.h"

#include <pthread.h>
#include <string.h>

void (*rhv_core_trace)(const char *fmt, ...);

/* Interrupts/exceptions the client asked to inject (VM-entry interruption info written with its valid bit). */
unsigned long long rhv_core_injections;

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

/*
 * Every call into the kernel costs a few microseconds, and a guest I/O exit makes QEMU touch ~40 registers and VMCS
 * fields (the real framework reads shared memory). Guest timing loops (Linux calibrating the TSC against the PIT)
 * then fail, so the state is cached between exits:
 *   - the general registers, RIP, RSP and RFLAGS are fetched with one call, written back in one call before the next
 *     run (or before anything else that could see them); RIP/RSP/RFLAGS are served from here when asked as VMCS fields;
 *   - other VMCS fields are cached on read and dropped when written.
 * Everything is dropped when the guest runs (it changes state) and when an MSR is written (that can change a VMCS
 * field behind the scenes, EFER for one).
 *
 * All of it is per vCPU. A vCPU is only ever driven from the thread that created it, so there is no locking; the entry
 * point of each API call selects the vCPU's state in V (thread-local) and the code below reads it through the g* names.
 */
enum { kVcSlots = 64 };

typedef struct VState {
    int used;
    int creating;                       /* the slot is being set up by rhv_vcpu_create */
    void *ctx;                          /* what the backend's per-vCPU functions are given */
    unsigned long owner;
    uint64_t execTime;
    uint64_t apicBase;
    struct { uint32_t enc; uint64_t val; uint8_t valid; } vc[kVcSlots];
    RHVRegs regs;
    int regsValid, regsDirty;
    /*
     * An XSAVE image written while XCR0 had not enabled all the components it marks in use (Parallels writes XSTATE_BV=7
     * at XCR0=1 and enables AVX afterwards). The kernel cannot accept that (XRSTOR would fault), so it gets the image
     * without those bits and this copy keeps the whole thing until XCR0 is written or the guest runs.
     */
    RHVFpuState fpuWhole;
    int fpuWholeValid;
    /*
     * With the fast path (a backend with run_ex) VMCS writes are not sent at once: they wait here and travel with the
     * next run (or the next read that could depend on them). The run call also returns the registers and the fields in
     * RHV_RUN_FIELDS_LIST, so the usual "what happened?" reads after an exit are served from the cache.
     */
    RHVVmcsWrite pend[RHV_RUN_MAX_WRITES];
    unsigned nPend;
    RHVRunIn runIn;
    RHVRunOut runOut;
} VState;

static VState gV[RHV_MAX_VCPUS_PER_VM];
/* vCPUs are created (and destroyed) from their own threads, all at once at VM start: slot choice must be serialised. */
static pthread_mutex_t gSlotLock = PTHREAD_MUTEX_INITIALIZER;
static __thread VState *V;

#define gVc          (V->vc)
#define gRegs        (V->regs)
#define gRegsValid   (V->regsValid)
#define gRegsDirty   (V->regsDirty)
#define gFpuWhole    (V->fpuWhole)
#define gFpuWholeValid (V->fpuWholeValid)
#define gPend        (V->pend)
#define gNPend       (V->nPend)
#define gExecTime    (V->execTime)
#define gApicBase    (V->apicBase)

/* The guest's TSC as the client last synchronised it (hv_vm_sync_tsc): applied to every vCPU, also ones made later. */
static int      gTscOffsetValid;
static uint64_t gTscOffset;

static const uint32_t kRunFields[RHV_RUN_NFIELDS] = {
#define X(e) e,
    RHV_RUN_FIELDS_LIST(X)
#undef X
};

/* Segment state: when one of these is read, all of it is fetched in a single call (instruction emulation reads many). */
static const uint32_t kSegGroup[] = {
    0x0800, 0x0802, 0x0804, 0x0806, 0x0808, 0x080a, 0x080c, 0x080e,                 /* selectors */
    0x4800, 0x4802, 0x4804, 0x4806, 0x4808, 0x480a, 0x480c, 0x480e, 0x4810, 0x4812, /* limits, GDTR, IDTR */
    0x4814, 0x4816, 0x4818, 0x481a, 0x481c, 0x481e, 0x4820, 0x4822,                 /* access rights */
    0x6806, 0x6808, 0x680a, 0x680c, 0x680e, 0x6810, 0x6812, 0x6814, 0x6816, 0x6818  /* bases, GDTR, IDTR */
};
#define SEG_GROUP_N ((unsigned)(sizeof(kSegGroup) / sizeof(kSegGroup[0])))

static void cacheReset(void)
{
    memset(gVc, 0, sizeof(gVc));
    gRegsValid = gRegsDirty = 0;
    gNPend = 0;
    gFpuWholeValid = 0;
}

/* Drops the cache of every vCPU (the tests change the fake kernel's state behind the core's back). */
void rhv_core_cache_reset(void)
{
    VState *keep = V;
    for (unsigned i = 0; i < RHV_MAX_VCPUS_PER_VM; i++) {
        V = &gV[i];
        cacheReset();
    }
    V = keep;
}

static unsigned vcSlot(uint32_t enc) { return ((enc >> 1) ^ (enc >> 7)) & (kVcSlots - 1); }

static int vcGet(uint32_t enc, uint64_t *val)
{
    unsigned i = vcSlot(enc);
    for (unsigned n = 0; n < kVcSlots; n++, i = (i + 1) & (kVcSlots - 1)) {
        if (!gVc[i].valid)
            return 0;
        if (gVc[i].enc == enc) {
            *val = gVc[i].val;
            return 1;
        }
    }
    return 0;
}

static void vcPut(uint32_t enc, uint64_t val)
{
    unsigned i = vcSlot(enc);
    for (unsigned n = 0; n < kVcSlots; n++, i = (i + 1) & (kVcSlots - 1)) {
        if (!gVc[i].valid || gVc[i].enc == enc) {
            gVc[i].enc = enc;
            gVc[i].val = val;
            gVc[i].valid = 1;
            return;
        }
    }                                  /* full: just do not cache it */
}

static void vcDrop(uint32_t enc)
{
    /* Open addressing without tombstones: rebuild the table without the entry. */
    typeof(gVc) old;
    memcpy(old, gVc, sizeof(old));
    memset(gVc, 0, sizeof(gVc));
    for (unsigned i = 0; i < kVcSlots; i++)
        if (old[i].valid && old[i].enc != enc)
            vcPut(old[i].enc, old[i].val);
}

static int regsFetch(void)
{
    if (gRegsValid)
        return 0;
    if (!B.get_regs)
        return (int)0xe00002c7;
    int k = B.get_regs(V->ctx, &gRegs);
    if (!k)
        gRegsValid = 1;
    return k;
}

static int regsFlush(void)
{
    if (!gRegsDirty)
        return 0;
    int k = B.set_regs ? B.set_regs(V->ctx, &gRegs) : (int)0xe00002c7;
    if (!k)
        gRegsDirty = 0;
    return k;
}

enum { F_GUEST_RSP = 0x681c, F_GUEST_RIP = 0x681e, F_GUEST_RFLAGS = 0x6820 };

static uint64_t *regsField(uint32_t enc)
{
    switch (enc) {
    case F_GUEST_RSP: return &gRegs.rsp;
    case F_GUEST_RIP: return &gRegs.rip;
    case F_GUEST_RFLAGS: return &gRegs.rflags;
    default: return NULL;
    }
}

static void adoptReply(const RHVRunOut *o)
{
    gRegs = o->regs;
    gRegsValid = 1;
    gRegsDirty = 0;
    for (unsigned i = 0; i < RHV_RUN_NFIELDS; i++)
        vcPut(kRunFields[i], o->fields[i]);
}

/*
 * Turns the exit just reported into another one, for a client-side fix-up (shim/hv_shim.c: VMCALL on an AMD CPU is #UD,
 * and the exit the client has to see is VMX_REASON_VMCALL). The read-only fields come from the cache.
 */
void rhv_vcpu_override_exit(uint32_t vcpu, uint64_t reason, uint64_t instrLen)
{
    if (vcpu >= RHV_MAX_VCPUS_PER_VM || !gV[vcpu].used)
        return;
    V = &gV[vcpu];
    vcPut(0x4402, reason);             /* VM-exit reason */
    vcPut(0x440c, instrLen);           /* VM-exit instruction length */
    vcPut(0x4404, 0);                  /* VM-exit interruption information: no exception any more */
}

static int pendingFor(uint32_t field)
{
    for (unsigned i = 0; i < gNPend; i++)
        if (gPend[i].field == field)
            return 1;
    return 0;
}

/* Sends buffered registers and VMCS writes to the kernel without running the guest. */
static int flushPending(void)
{
    if (!B.run_ex)
        return regsFlush();
    if (!gNPend && !gRegsDirty)
        return 0;
    RHVRunIn  *in  = &V->runIn;
    RHVRunOut *out = &V->runOut;
    in->flags = RHV_RUN_NO_RUN | (gRegsDirty ? RHV_RUN_REGS : 0);
    in->regs = gRegs;
    in->nWrites = gNPend;
    memcpy(in->writes, gPend, gNPend * sizeof(gPend[0]));
    int k = B.run_ex(V->ctx, in, out);
    gNPend = 0;
    if (k) {
        cacheReset();
        return k;
    }
    adoptReply(out);
    return 0;
}

static int segGroupHas(uint32_t field)
{
    for (unsigned i = 0; i < SEG_GROUP_N; i++)
        if (kSegGroup[i] == field)
            return 1;
    return 0;
}

/* VMCS field access through the cache. */
static int vmcsGet(uint32_t field, uint64_t *value)
{
    if (regsField(field)) {
        int k = regsFetch();
        if (k)
            return k;
        *value = *regsField(field);
        return 0;
    }
    if (B.run_ex && gNPend) {                  /* a pending write to this field (or to one fetched along) first */
        int dep = pendingFor(field);
        for (unsigned i = 0; !dep && i < SEG_GROUP_N && segGroupHas(field); i++)
            dep = pendingFor(kSegGroup[i]);
        if (dep) {
            int k = flushPending();
            if (k)
                return k;
        }
    }
    if (vcGet(field, value))
        return 0;
    if (B.vmcs_read_many && segGroupHas(field)) {
        {
            static unsigned noted;
            if (rhv_core_trace && noted++ < 3000 && (noted % 100) == 1)
                rhv_core_trace("segment group fetched because of field %#x", field);
        }
        RHVReadMany in;
        RHVReadManyOut out;
        in.count = SEG_GROUP_N;
        for (unsigned i = 0; i < SEG_GROUP_N; i++)
            in.fields[i] = kSegGroup[i];
        if (B.vmcs_read_many(V->ctx, &in, &out) == 0) {
            for (unsigned i = 0; i < SEG_GROUP_N; i++)
                vcPut(kSegGroup[i], out.values[i]);
            if (vcGet(field, value))
                return 0;
        }
    }
    if (!B.vmcs_read)
        return (int)0xe00002c7;
    int k = B.vmcs_read(V->ctx, field, value);
    if (!k)
        vcPut(field, *value);
    return k;
}

static int vmcsSet(uint32_t field, uint64_t value)
{
    uint64_t *slot = regsField(field);
    if (slot) {
        int k = regsFetch();
        if (k)
            return k;
        *slot = value;
        gRegsDirty = 1;
        return 0;
    }
    if (B.run_ex) {                            /* buffered; the kernel's verdict on the field is known in advance */
        switch (rhv_vmcs_write_status(field, value)) {
        case RHV_FIELD_OK: break;
        case RHV_FIELD_READ_ONLY: return (int)0xe00002cf;       /* kIOReturnNotWritable */
        default: return (int)0xe00002c2;                        /* kIOReturnBadArgument */
        }
        vcDrop(field);
        for (unsigned i = 0; i < gNPend; i++) {
            if (gPend[i].field == field) {
                gPend[i].value = value;
                return 0;
            }
        }
        if (gNPend == RHV_RUN_MAX_WRITES) {
            int k = flushPending();
            if (k)
                return k;
        }
        gPend[gNPend].field = field;
        gPend[gNPend].reserved = 0;
        gPend[gNPend].value = value;
        gNPend++;
        return 0;
    }
    if (!B.vmcs_write)
        return (int)0xe00002c7;
    int k = B.vmcs_write(V->ctx, field, value);
    vcDrop(field);
    return k;
}

/*
 * IA32_APIC_BASE. The real framework answers this one itself (the guest's local APIC is the VMM's business, but the
 * MSR exists), and QEMU reads it without checking the result. Default: BSP, APIC enabled, base 0xFEE00000.
 */
#define MSR_IA32_APICBASE 0x1B
#define APICBASE_BSP      0xFEE00900ull      /* enabled, bootstrap processor */
#define APICBASE_AP       0xFEE00800ull      /* enabled */

void rhv_core_init(const RhvBackend *backend)
{
    B = *backend;
    gInit = 1;
    gVm = 0;
    gTscOffsetValid = 0;
    memset(gV, 0, sizeof(gV));
    rhv_core_cache_reset();
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

/* A vCPU call must name a vCPU we created and come from the thread that created it. Selects its state in V. */
static int32_t needVcpu(uint32_t vcpu)
{
    int32_t r = needVm();
    if (r)
        return r;
    if (vcpu >= RHV_MAX_VCPUS_PER_VM || !gV[vcpu].used)
        return RHV_HV_BAD_ARGUMENT;
    V = &gV[vcpu];
    if (B.thread_id && B.thread_id(B.ctx) != V->owner)
        return RHV_HV_BAD_ARGUMENT;
    return RHV_HV_SUCCESS;
}

/* How many vCPUs exist now (the shim uses it for the CPUID topology it reports). */
unsigned rhv_core_vcpu_count(void);

static unsigned vcpuCount(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < RHV_MAX_VCPUS_PER_VM; i++)
        n += gV[i].used;
    return n;
}

unsigned rhv_core_vcpu_count(void) { return vcpuCount(); }

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
    return RHV_HV_SUCCESS;
}

int32_t rhv_vm_destroy(void)
{
    int32_t r = needVm();
    if (r)
        return r;
    NEED(vm_destroy);
    if (vcpuCount())
        return RHV_HV_BUSY;       /* destroy the vCPUs first */
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

/* The guest sees the TSC `tsc` at this moment: guest TSC = host TSC + offset. Every vCPU gets the same offset. */
int32_t rhv_vm_sync_tsc(uint64_t tsc)
{
    int32_t r = needVm();
    if (r)
        return r;
    NEED(vmcs_write);
    NEED(tsc);
    gTscOffset = tsc - B.tsc(B.ctx);
    gTscOffsetValid = 1;
    int32_t result = RHV_HV_SUCCESS;
    unsigned n = 0;
    for (unsigned i = 0; i < RHV_MAX_VCPUS_PER_VM; i++) {
        if (!gV[i].used)
            continue;
        n++;
        V = &gV[i];
        if (flushPending() == 0) {}                   /* keep the order with buffered writes */
        vcDrop(F_TSC_OFFSET);
        int32_t k = rhv_hv_from_ioreturn(B.vmcs_write(V->ctx, F_TSC_OFFSET, gTscOffset));
        if (k)
            result = k;
    }
    if (!n)                                           /* no vCPU yet: the one the kernel made with the VM gets it now */
        result = rhv_hv_from_ioreturn(B.vmcs_write(B.ctx, F_TSC_OFFSET, gTscOffset));
    return result;
}

/* ---- vCPU ---- */

int32_t rhv_vcpu_create(uint32_t *vcpu, uint64_t flags)
{
    int32_t r = needVm();
    if (r)
        return r;
    if (!vcpu || flags > 3)       /* HV_VCPU_DEFAULT, ACCEL_RDPMC, TSC_RELATIVE */
        return RHV_HV_BAD_ARGUMENT;
    pthread_mutex_lock(&gSlotLock);
    unsigned i = 0;
    while (i < RHV_MAX_VCPUS_PER_VM && (gV[i].used || gV[i].creating))
        i++;
    if (i == RHV_MAX_VCPUS_PER_VM) {
        pthread_mutex_unlock(&gSlotLock);
        return RHV_HV_NO_RESOURCES;
    }
    void *vctx = B.ctx;
    if (B.vcpu_open) {
        gV[i].creating = 1;                /* the slot is ours while the kernel connection is made */
        int k = B.vcpu_open(B.ctx, i, &vctx);
        TRACE("vcpu_open(%u) -> %#x", i, k);
        if (k) {
            gV[i].creating = 0;
            pthread_mutex_unlock(&gSlotLock);
            return rhv_hv_from_ioreturn(k);
        }
    } else if (i != 0) {
        pthread_mutex_unlock(&gSlotLock);
        return RHV_HV_NO_RESOURCES;   /* the backend knows one vCPU only */
    }
    memset(&gV[i], 0, sizeof(gV[i]));
    V = &gV[i];
    V->ctx = vctx;
    V->used = 1;
    pthread_mutex_unlock(&gSlotLock);
    V->owner = B.thread_id ? B.thread_id(B.ctx) : 0;
    V->apicBase = i == 0 ? APICBASE_BSP : APICBASE_AP;
    if (gTscOffsetValid && B.vmcs_write)
        B.vmcs_write(V->ctx, F_TSC_OFFSET, gTscOffset);
    *vcpu = i;
    return RHV_HV_SUCCESS;
}

int32_t rhv_vcpu_destroy(uint32_t vcpu)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    if (B.vcpu_close)
        B.vcpu_close(B.ctx, vcpu, V->ctx);
    memset(V, 0, sizeof(*V));
    return RHV_HV_SUCCESS;
}

int32_t rhv_vcpu_run(uint32_t vcpu)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    NEED(run);

    if (B.run_ex) {
        /* One call: send what is buffered, run, get the registers and the exit information back. */
        RHVRunIn  *in  = &V->runIn;
        RHVRunOut *out = &V->runOut;
        in->flags = gRegsDirty ? RHV_RUN_REGS : 0;
        in->regs = gRegs;
        in->nWrites = gNPend;
        memcpy(in->writes, gPend, gNPend * sizeof(gPend[0]));
        uint64_t t0 = B.now ? B.now(B.ctx) : 0;
        int k = B.run_ex(V->ctx, in, out);
        if (B.now)
            gExecTime += B.now(B.ctx) - t0;
        cacheReset();                 /* the guest ran: all of it may have changed */
        if (k)
            return rhv_hv_from_ioreturn(k);
        adoptReply(out);
        return RHV_HV_SUCCESS;
    }

    int fk = regsFlush();
    if (fk)
        return rhv_hv_from_ioreturn(fk);
    RHVExit ex;
    memset(&ex, 0, sizeof(ex));
    uint64_t t0 = B.now ? B.now(B.ctx) : 0;
    int k = B.run(V->ctx, &ex);
    if (B.now)
        gExecTime += B.now(B.ctx) - t0;
    cacheReset();                     /* the guest ran: all of it may have changed */
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
        if (vcpus[i] >= RHV_MAX_VCPUS_PER_VM || !gV[vcpus[i]].used)
            return RHV_HV_BAD_ARGUMENT;
    }
    NEED(kick);
    int32_t result = RHV_HV_SUCCESS;
    for (unsigned int i = 0; i < count; i++) {
        int32_t k = rhv_hv_from_ioreturn(B.kick(gV[vcpus[i]].ctx));
        if (k)
            result = k;
    }
    return result;
}

int32_t rhv_vcpu_flush(uint32_t vcpu)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    return rhv_hv_from_ioreturn(flushPending());
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

/*
 * hv_tsc_clock is the host TSC here (the abstract clock "ticks at the same rate as the host TSC"), so the offset the
 * client wants relative to it is the VMCB TSC offset itself.
 */
uint64_t rhv_tsc_clock(void)
{
    return B.tsc ? B.tsc(B.ctx) : 0;
}

int32_t rhv_vcpu_set_tsc_relative(uint32_t vcpu, int64_t offset)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    NEED(vmcs_write);
    return rhv_hv_from_ioreturn(vmcsSet(F_TSC_OFFSET, (uint64_t)offset));
}

/* hv_vcpu_set_msr_access: a managed MSR the guest may use without exits (any access) or not at all. */
int32_t rhv_vcpu_set_msr_access(uint32_t vcpu, uint32_t msr, uint32_t flags)
{
    if (flags & ~3u)
        return RHV_HV_BAD_ARGUMENT;
    return rhv_vcpu_enable_native_msr(vcpu, msr, flags != 0);
}

/*
 * Facts about MSRs the framework would otherwise get from the Intel CPU (ARCH_CAPABILITIES, which PERFEVTSEL bits are
 * valid, ...). None apply to an AMD CPU: report "nothing", which a client takes as "do not touch these MSRs".
 */
int32_t rhv_vmx_get_msr_info(uint32_t field, uint64_t *value)
{
    if (!value || field > 8)
        return RHV_HV_BAD_ARGUMENT;
    *value = 0;
    return RHV_HV_SUCCESS;
}

/*
 * The APIC-access page belongs to Intel's "virtualize APIC accesses" control, which this shim does not offer; a
 * client that sees it missing from the capabilities leaves it off, and the guest's APIC accesses then arrive as
 * ordinary EPT violations at the APIC address. Accept and remember nothing.
 */
int32_t rhv_vmx_vcpu_set_apic_address(uint32_t vcpu, uint64_t gpa)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    return (gpa & 0xFFF) ? RHV_HV_BAD_ARGUMENT : RHV_HV_SUCCESS;
}

int32_t rhv_vmx_vcpu_set_apic_address_space(uint32_t vcpu, uint32_t asid, uint64_t gpa)
{
    if (asid != 0)
        return needVcpu(vcpu) ? needVcpu(vcpu) : RHV_HV_BAD_ARGUMENT;      /* only the default space exists */
    return rhv_vmx_vcpu_set_apic_address(vcpu, gpa);
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
        int k = B.get_fpu(V->ctx, &f);
        if (k)
            return rhv_hv_from_ioreturn(k);
        *value = f.xcr0;
        return RHV_HV_SUCCESS;
    }
    if (reg <= RHV_X86_R15) {
        NEED(get_regs);
        int k = regsFetch();
        if (k)
            return rhv_hv_from_ioreturn(k);
        *value = *gprSlot(&gRegs, reg);
        return RHV_HV_SUCCESS;
    }
    uint32_t field = rhv_core_register_field(reg);
    if (!field)
        return RHV_HV_BAD_ARGUMENT;         /* CR1 */
    NEED(vmcs_read);
    {
        int k = vmcsGet(field, value);
        /* HV_X86_TPR is the 8-bit APIC task priority (class in bits 7:4); the kernel's V_TPR holds the 4-bit class. */
        if (!k && reg == RHV_X86_TPR)
            *value = (*value & 0xF) << 4;
        return rhv_hv_from_ioreturn(k);
    }
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
        int k = B.get_fpu(V->ctx, &f);
        if (k)
            return rhv_hv_from_ioreturn(k);
        if (gFpuWholeValid)                           /* the image written earlier, whole: components may be on now */
            memcpy(f.area, gFpuWhole.area, f.size);
        /* Components the new XCR0 turns off must not stay marked as in use in the saved image. */
        uint64_t bv;
        memcpy(&bv, f.area + 512, 8);
        uint64_t whole = bv;
        bv &= value | 3;
        memcpy(f.area + 512, &bv, 8);
        f.xcr0 = value;
        int rc = B.set_fpu(V->ctx, &f);
        if (rc == 0 && gFpuWholeValid && !(whole & ~(value | 3)))
            gFpuWholeValid = 0;                       /* everything it marked is enabled now */
        return rhv_hv_from_ioreturn(rc);
    }
    if (reg <= RHV_X86_R15) {
        NEED(get_regs);
        NEED(set_regs);
        int k = regsFetch();
        if (k)
            return rhv_hv_from_ioreturn(k);
        *gprSlot(&gRegs, reg) = value;
        gRegsDirty = 1;                       /* written back before the next run */
        return RHV_HV_SUCCESS;
    }
    uint32_t field = rhv_core_register_field(reg);
    if (!field)
        return RHV_HV_BAD_ARGUMENT;
    NEED(vmcs_write);
    if (reg == RHV_X86_TPR)
        value = (value >> 4) & 0xF;                    /* 8-bit APIC TPR -> the kernel's 4-bit class */
    return rhv_hv_from_ioreturn(vmcsSet(field, value));
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
    int k = B.get_fpu(V->ctx, &f);
    if (k)
        return rhv_hv_from_ioreturn(k);
    TRACE("read_fpstate: buffer %zu, area %u, xcr0 %#llx", size, f.size, (unsigned long long)f.xcr0);
    if (size < f.size)
        return RHV_HV_BAD_ARGUMENT;
    memcpy(buffer, gFpuWholeValid ? gFpuWhole.area : f.area, f.size);
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
    int k = B.get_fpu(V->ctx, &f);
    if (k)
        return rhv_hv_from_ioreturn(k);
    TRACE("write_fpstate: buffer %zu, area %u, xcr0 %#llx", size, f.size, (unsigned long long)f.xcr0);
    if (size < f.size)
        return RHV_HV_BAD_ARGUMENT;
    memcpy(f.area, buffer, f.size);
    {
        uint64_t bv;
        memcpy(&bv, f.area + 512, 8);
        gFpuWholeValid = 0;
        if (bv & ~(f.xcr0 | 3)) {                     /* marks components XCR0 has not enabled yet */
            gFpuWhole = f;
            gFpuWholeValid = 1;
            bv &= f.xcr0 | 3;
            memcpy(f.area + 512, &bv, 8);
        }
    }
    if (rhv_core_trace) {
        uint64_t bv, comp;
        uint32_t mxcsr;
        memcpy(&bv, f.area + 512, 8);
        memcpy(&comp, f.area + 520, 8);
        memcpy(&mxcsr, f.area + 24, 4);
        int junk = 0;
        for (unsigned i = 528; i < 576; i++)
            junk |= f.area[i];
        rhv_core_trace("write_fpstate: xstate_bv %#llx xcomp_bv %#llx mxcsr %#x header-junk %d", (unsigned long long)bv,
                       (unsigned long long)comp, mxcsr, junk);
    }
    return rhv_hv_from_ioreturn(B.set_fpu(V->ctx, &f));
}

int32_t rhv_vcpu_read_msr(uint32_t vcpu, uint32_t msr, uint64_t *value)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    if (!value)
        return RHV_HV_BAD_ARGUMENT;
    if (msr == MSR_IA32_APICBASE) {
        *value = gApicBase;
        return RHV_HV_SUCCESS;
    }
    NEED(msr);
    int fk = flushPending();                  /* an MSR and a VMCS field can be the same state (EFER) */
    if (fk)
        return rhv_hv_from_ioreturn(fk);
    return rhv_hv_from_ioreturn(B.msr(V->ctx, RHV_MSR_READ, msr, 0, value));
}

int32_t rhv_vcpu_write_msr(uint32_t vcpu, uint32_t msr, uint64_t value)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    if (msr == MSR_IA32_APICBASE) {
        gApicBase = value;
        return RHV_HV_SUCCESS;
    }
    NEED(msr);
    uint64_t out;
    int fk = flushPending();
    if (fk)
        return rhv_hv_from_ioreturn(fk);
    int k = B.msr(V->ctx, RHV_MSR_WRITE, msr, value, &out);
    for (unsigned i = 0; i < kVcSlots; i++) gVc[i].valid = 0;       /* EFER and friends are VMCS fields too */
    return rhv_hv_from_ioreturn(k);
}

int32_t rhv_vcpu_enable_native_msr(uint32_t vcpu, uint32_t msr, int enable)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    NEED(msr);
    uint64_t out;
    int fk = flushPending();
    if (fk)
        return rhv_hv_from_ioreturn(fk);
    return rhv_hv_from_ioreturn(B.msr(V->ctx, enable ? RHV_MSR_NATIVE_ENABLE : RHV_MSR_NATIVE_DISABLE, msr, 0, &out));
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
    return rhv_hv_from_ioreturn(vmcsGet(field, value));
}

int32_t rhv_vmx_vcpu_write_vmcs(uint32_t vcpu, uint32_t field, uint64_t value)
{
    int32_t r = needVcpu(vcpu);
    if (r)
        return r;
    NEED(vmcs_write);
    if (field == 0x4016 && (value & (1u << 31)))
        rhv_core_injections++;
    if (rhv_core_trace && (field == 0x4016 || field == 0x4002))      /* VM-entry interruption info, CPU-based controls */
        rhv_core_trace("write_vmcs(%#x) = %#llx", field, (unsigned long long)value);
    return rhv_hv_from_ioreturn(vmcsSet(field, value));
}

/*
 * What a VMX client may ask for. Each value has the layout of the matching IA32_VMX_* MSR: low 32 bits = controls that
 * must be 1, high 32 bits = controls that may be 1. Clients compute their controls as (wanted | low) & high, so a bit
 * we leave out of "high" is silently not requested. Everything claimed here is either implemented by VmcsState
 * (kext/Source/VmcsState.hpp) or harmless to ignore (the VM-entry/exit "load" controls: the state fields are
 * authoritative). The TPR shadow is implemented in the kext (CR8 writes only exit below the TPR threshold). APIC
 * virtualization, posted interrupts, the preemption timer and NMI-window exiting are NOT offered.
 */
static uint64_t cap(uint32_t must1, uint32_t may1)
{
    return ((uint64_t)(may1 | must1) << 32) | must1;
}

enum {
    PIN_EXTINT = 1u << 0, PIN_NMI = 1u << 3,
    CPU_INTR_WINDOW = 1u << 2, CPU_TSC_OFFSETING = 1u << 3, CPU_HLT = 1u << 7, CPU_INVLPG = 1u << 9,
    CPU_MWAIT = 1u << 10, CPU_RDPMC = 1u << 11, CPU_RDTSC = 1u << 12, CPU_CR3_LOAD = 1u << 15, CPU_CR3_STORE = 1u << 16,
    CPU_CR8_LOAD = 1u << 19, CPU_CR8_STORE = 1u << 20, CPU_TPR_SHADOW = 1u << 21, CPU_MOV_DR = 1u << 23, CPU_IO = 1u << 24,
    CPU_IO_BITMAPS = 1u << 25, CPU_MSR_BITMAPS = 1u << 28, CPU_MONITOR = 1u << 29, CPU_PAUSE = 1u << 30,
    CPU_SECONDARY = 1u << 31,
    CPU2_EPT = 1u << 1, CPU2_RDTSCP = 1u << 3, CPU2_VPID = 1u << 5, CPU2_WBINVD = 1u << 6,
    CPU2_UNRESTRICTED = 1u << 7, CPU2_INVPCID = 1u << 12,
    ENTRY_IA32E = 1u << 9, ENTRY_LOAD_PAT = 1u << 14, ENTRY_LOAD_EFER = 1u << 15,
    EXIT_HOST_ADDR_SPACE = 1u << 9, EXIT_ACK_INTR = 1u << 15, EXIT_SAVE_PAT = 1u << 18, EXIT_LOAD_PAT = 1u << 19,
    EXIT_SAVE_EFER = 1u << 20, EXIT_LOAD_EFER = 1u << 21
};

/* Set by the shim for a client that knows what to do with the TPR shadow (Parallels); QEMU keeps CR8 exiting. */
int rhv_core_offer_tpr_shadow;

int32_t rhv_vmx_read_capability(uint32_t c, uint64_t *value)
{
    if (!value)
        return RHV_HV_BAD_ARGUMENT;
    const uint32_t proc = CPU_INTR_WINDOW | CPU_TSC_OFFSETING | CPU_HLT | CPU_INVLPG | CPU_MWAIT | CPU_RDPMC | CPU_RDTSC |
                          CPU_CR3_LOAD | CPU_CR3_STORE | CPU_CR8_LOAD | CPU_CR8_STORE | (rhv_core_offer_tpr_shadow ? CPU_TPR_SHADOW : 0) | CPU_MOV_DR | CPU_IO |
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
    case RHV_VMX_CAP_VMCS_ENUM: *value = 0x2E << 1; break;                /* highest VMCS field index: bits 9:1 */
    case RHV_VMX_CAP_EPT_VPID_CAP:
        /* 4-level walks, write-back, 2 MiB pages, INVEPT (single/all context); INVVPID (all four types) */
        *value = (1ull << 6) | (1ull << 14) | (1ull << 16) | (1ull << 20) | (1ull << 25) | (1ull << 26) |
                 (1ull << 32) | (1ull << 40) | (1ull << 41) | (1ull << 42) | (1ull << 43);
        break;
    case RHV_VMX_CAP_CR4_FIXED1: *value = 0x00FFFFFFull; break;    /* bit 13 (VMXE) is accepted and kept out of the VMCB */
    default:
        return RHV_HV_BAD_ARGUMENT;
    }
    return RHV_HV_SUCCESS;
}
