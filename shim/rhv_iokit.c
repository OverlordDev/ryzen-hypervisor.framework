/*
 * rhv_iokit.c - the RhvBackend that talks to the RyzenHV kext through IOKit.
 *
 * The connection is opened on hv_vm_create and closed on hv_vm_destroy. It is made with the calling task, which is
 * what VmMap needs: the kext pins pages of the task that owns the connection.
 */
#include "rhv_iokit.h"

#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define IOR_NO_DEVICE 0xe00002c0
#define IOR_BAD_ARG   0xe00002c2

/* One connection (one kext user client) per vCPU: the first is the one that created the VM, the others join it. */
typedef struct Conn {
    io_connect_t c;
} Conn;

static Conn     gPrimary;
static uint64_t gVmId;
static uint32_t gVmFlags;
#define gConn (gPrimary.c)

/* Kernel round trips made so far; the shim's statistics report it (RYZENHV_STATS). */
unsigned long long rhv_iokit_calls;
unsigned long long rhv_iokit_by_selector[32];
struct { uint32_t field; unsigned long long n; } rhv_iokit_reads[24];     /* which VMCS fields miss the cache */                /* per selector, for the statistics */
#define IOConnectCallScalarMethod(c, sel, ...) (rhv_iokit_calls++, rhv_iokit_by_selector[(sel) & 31]++, IOConnectCallScalarMethod(c, sel, __VA_ARGS__))
#define IOConnectCallStructMethod(c, sel, ...) (rhv_iokit_calls++, rhv_iokit_by_selector[(sel) & 31]++, IOConnectCallStructMethod(c, sel, __VA_ARGS__))
#define IOConnectCallMethod(c, sel, ...)       (rhv_iokit_calls++, rhv_iokit_by_selector[(sel) & 31]++, IOConnectCallMethod(c, sel, __VA_ARGS__))

static int vm_create(void *ctx, uint32_t flags)
{
    (void)ctx;
    if (gConn)
        return (int)IOR_BAD_ARG;
    io_service_t svc = IOServiceGetMatchingService(0, IOServiceMatching(RHV_SERVICE_NAME));
    if (!svc)
        return (int)IOR_NO_DEVICE;                       /* the kext is not loaded */
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &gConn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        gConn = 0;
        return kr;
    }
    /* The shim and the kext share struct layouts and selectors; a stale kext (not rebooted after an update) would
     * misread them. */
    RHVInfo info;
    size_t infoSize = sizeof(info);
    kr = IOConnectCallStructMethod(gConn, kRHVMethodGetInfo, NULL, 0, &info, &infoSize);
    if (kr == KERN_SUCCESS && info.abiVersion != RHV_ABI_VERSION) {
        fprintf(stderr, "ryzenhv: the loaded kext speaks ABI %u, this shim ABI %u: install the matching RyzenHV.kext "
                        "and reboot\n", info.abiVersion, (unsigned)RHV_ABI_VERSION);
        kr = (kern_return_t)0xe00002c7;
    }
    if (kr != KERN_SUCCESS) {
        IOServiceClose(gConn);
        gConn = 0;
        return kr;
    }
    gVmFlags = flags;
    uint64_t in = flags, out[2] = { 0, 0 };
    uint32_t outCnt = 2;
    kr = IOConnectCallScalarMethod(gConn, kRHVMethodVmCreate, &in, 1, out, &outCnt);
    if (kr != KERN_SUCCESS) {
        IOServiceClose(gConn);
        gConn = 0;
    } else {
        gVmId = out[1];
    }
    return kr;
}


/* vCPU `index`: 0 is the one vm_create made; another one is a new connection that joins the VM. */
static int vcpu_open(void *ctx, uint32_t index, void **vctx)
{
    (void)ctx;
    if (index == 0) {
        *vctx = &gPrimary;
        return 0;
    }
    Conn *c = calloc(1, sizeof(*c));
    if (!c)
        return (int)0xe00002bd;                          /* kIOReturnNoMemory */
    io_service_t svc = IOServiceGetMatchingService(0, IOServiceMatching(RHV_SERVICE_NAME));
    if (!svc) {
        free(c);
        return (int)IOR_NO_DEVICE;
    }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &c->c);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        free(c);
        return kr;
    }
    uint64_t in[2] = { gVmFlags, gVmId }, out = 0;
    uint32_t outCnt = 1;
    kr = IOConnectCallScalarMethod(c->c, kRHVMethodVmJoin, in, 2, &out, &outCnt);
    if (kr != KERN_SUCCESS) {
        IOServiceClose(c->c);
        free(c);
        return kr;
    }
    *vctx = c;
    return 0;
}

static int vcpu_close(void *ctx, uint32_t index, void *vctx)
{
    (void)ctx;
    if (index == 0 || !vctx)
        return 0;                                        /* the first one goes with the VM, see vm_destroy */
    Conn *c = vctx;
    IOConnectCallScalarMethod(c->c, kRHVMethodVmDestroy, NULL, 0, NULL, NULL);
    IOServiceClose(c->c);
    free(c);
    return 0;
}

static int vm_destroy(void *ctx)
{
    (void)ctx;
    if (!gConn)
        return (int)IOR_NO_DEVICE;
    IOConnectCallScalarMethod(gConn, kRHVMethodVmDestroy, NULL, 0, NULL, NULL);
    IOServiceClose(gConn);
    gConn = 0;
    return 0;
}

static int vm_map(void *ctx, uint64_t uva, uint64_t gpa, uint64_t size, uint32_t flags)
{
    (void)ctx;
    uint64_t in[4] = { uva, gpa, size, flags };
    return IOConnectCallScalarMethod(gConn, kRHVMethodVmMap, in, 4, NULL, NULL);
}

static int vm_unmap(void *ctx, uint64_t gpa, uint64_t size)
{
    (void)ctx;
    uint64_t in[2] = { gpa, size };
    return IOConnectCallScalarMethod(gConn, kRHVMethodVmUnmap, in, 2, NULL, NULL);
}

static int vm_protect(void *ctx, uint64_t gpa, uint64_t size, uint32_t flags)
{
    (void)ctx;
    uint64_t in[3] = { gpa, size, flags };
    return IOConnectCallScalarMethod(gConn, kRHVMethodVmProtect, in, 3, NULL, NULL);
}

static int vmcs_read(void *ctx, uint32_t field, uint64_t *value)
{
    for (int i = 0; i < 24; i++) {
        if (rhv_iokit_reads[i].n && rhv_iokit_reads[i].field != field)
            continue;
        rhv_iokit_reads[i].field = field;
        rhv_iokit_reads[i].n++;
        break;
    }
    uint64_t in = field, out = 0;
    uint32_t cnt = 1;
    kern_return_t kr = IOConnectCallScalarMethod(((Conn *)ctx)->c, kRHVMethodVmcsRead, &in, 1, &out, &cnt);
    *value = out;
    return kr;
}

static int vmcs_write(void *ctx, uint32_t field, uint64_t value)
{
    uint64_t in[2] = { field, value };
    return IOConnectCallScalarMethod(((Conn *)ctx)->c, kRHVMethodVmcsWrite, in, 2, NULL, NULL);
}

static int get_regs(void *ctx, RHVRegs *regs)
{
    size_t sz = sizeof(*regs);
    return IOConnectCallStructMethod(((Conn *)ctx)->c, kRHVMethodVmGetRegs, NULL, 0, regs, &sz);
}

static int set_regs(void *ctx, const RHVRegs *regs)
{
    return IOConnectCallStructMethod(((Conn *)ctx)->c, kRHVMethodVmSetRegs, regs, sizeof(*regs), NULL, NULL);
}

static int get_fpu(void *ctx, RHVFpuState *fpu)
{
    size_t sz = sizeof(*fpu);
    return IOConnectCallStructMethod(((Conn *)ctx)->c, kRHVMethodVmGetFpu, NULL, 0, fpu, &sz);
}

static int set_fpu(void *ctx, const RHVFpuState *fpu)
{
    return IOConnectCallStructMethod(((Conn *)ctx)->c, kRHVMethodVmSetFpu, fpu, sizeof(*fpu), NULL, NULL);
}

static int msr_op(void *ctx, uint32_t op, uint32_t msr, uint64_t value, uint64_t *out)
{
    uint64_t in[3] = { op, msr, value }, o = 0;
    uint32_t cnt = 1;
    kern_return_t kr = IOConnectCallScalarMethod(((Conn *)ctx)->c, kRHVMethodVmMsr, in, 3, &o, &cnt);
    *out = o;
    return kr;
}

static int run(void *ctx, RHVExit *ex)
{
    uint64_t skip = 0;                  /* a VMX client advances RIP itself */
    size_t sz = sizeof(*ex);
    return IOConnectCallMethod(((Conn *)ctx)->c, kRHVMethodVmRun, &skip, 1, NULL, 0, NULL, NULL, ex, &sz);
}

static int run_ex(void *ctx, const RHVRunIn *in, RHVRunOut *out)
{
    size_t sz = sizeof(*out);
    return IOConnectCallStructMethod(((Conn *)ctx)->c, kRHVMethodVmRunEx, in, sizeof(*in), out, &sz);
}

static int vmcs_read_many(void *ctx, const RHVReadMany *in, RHVReadManyOut *out)
{
    size_t sz = sizeof(*out);
    return IOConnectCallStructMethod(((Conn *)ctx)->c, kRHVMethodVmcsReadMany, in, sizeof(*in), out, &sz);
}

static int kick(void *ctx)
{
    return IOConnectCallScalarMethod(((Conn *)ctx)->c, kRHVMethodVmKick, NULL, 0, NULL, NULL);
}

static uint64_t now(void *ctx)
{
    (void)ctx;
    return mach_absolute_time();
}

static uint64_t tsc(void *ctx)
{
    (void)ctx;
    return __builtin_ia32_rdtsc();
}

static unsigned long thread_id(void *ctx)
{
    (void)ctx;
    return (unsigned long)pthread_mach_thread_np(pthread_self());
}

void rhv_iokit_backend(RhvBackend *b)
{
    b->ctx = NULL;
    b->vm_create = vm_create;
    b->vm_destroy = vm_destroy;
    b->vm_map = vm_map;
    b->vm_unmap = vm_unmap;
    b->vm_protect = vm_protect;
    b->vmcs_read = vmcs_read;
    b->vmcs_write = vmcs_write;
    b->get_regs = get_regs;
    b->set_regs = set_regs;
    b->get_fpu = get_fpu;
    b->set_fpu = set_fpu;
    b->msr = msr_op;
    b->run = run;
    b->kick = kick;
    b->now = now;
    b->tsc = tsc;
    b->thread_id = thread_id;
    b->run_ex = run_ex;
    b->vmcs_read_many = vmcs_read_many;
    b->vcpu_open = vcpu_open;
    b->vcpu_close = vcpu_close;
}
