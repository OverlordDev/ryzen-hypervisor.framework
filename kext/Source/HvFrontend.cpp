#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <string.h>
#include <mach/kern_return.h>
#include <pexpert/pexpert.h>
#include <libkern/OSAtomic.h>

#include "HvFrontend.hpp"
#include "SVM.hpp"
#include "HvPatch.hpp"
#include "HvMsrs.hpp"
#include "HvSlots.hpp"
#include "RyzenHV.hpp"
#include "KernelDecls.hpp"
#include "Vm.hpp"
#include "Log.hpp"

/*
 * The kernel side of this is public (xnu osfmk/kern/hv_support_kext.h), the declarations are repeated here so that the
 * build does not depend on which private headers the SDK ships.
 */
extern "C" {
typedef kern_return_t (*hv_trap_t)(void *target, uint64_t arg);

typedef struct {
    void (*dispatch)(void *vcpu);
    void (*preempt)(void *vcpu);
    void (*suspend)(void);
    void (*thread_destroy)(void *vcpu);
    void (*task_destroy)(void *vm);
    void (*volatile_state)(void *vcpu, int state);
    void (*resume)(void);
    void (*memory_pressure)(void);
} hv_callbacks_t;

extern int hv_get_support(void);                          /* returns hv_support_available: used to read the result back, and its code names the address */
extern "C" int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);   /* reads kern.hv_support through its OID */
extern kern_return_t hv_set_traps(int trap_type, const hv_trap_t *traps, unsigned trap_count);
extern void hv_release_traps(int trap_type);
extern kern_return_t hv_set_callbacks(hv_callbacks_t callbacks);
extern void hv_release_callbacks(void);
extern void hv_set_task_target(void *target);
extern void hv_set_thread_target(void *target);
extern void *hv_get_task_target(void);
extern void *hv_get_thread_target(void);
}

namespace {


enum { HV_TASK_TRAP = 0, HV_THREAD_TRAP = 1 };
constexpr unsigned kTraps = 16;                       /* the framework uses indices 0..13 */
constexpr kern_return_t kHvUnsupported = (kern_return_t)0xfae9400f;
constexpr kern_return_t kHvUnsupportedCode = kHvUnsupported;

bool gRegistered = false;
uint32_t gMode = 1;          /* 1: answer HV_UNSUPPORTED, 2: answer success so the client goes on and more traps show up */
volatile SInt32 gCount[2][kTraps];

const char *const kTaskNames[kTraps] = {
    "vm_create", "vm_destroy", "space_create", "space_destroy", "vm_map", "vm_unmap", "vm_protect", "vm_sync_tsc",
    "add_pio_notifier", "rem_pio_notifier", "intctl(atpic/ioapic/lapic)", "lapic_msi", "vcpu_create", "vcpu_interrupt", "?14", "?15"};
const char *const kThreadNames[kTraps] = {
    "vcpu_run", "vcpu_run_until", "invalidate_tlb", "disable_managed_msr", "enable_managed_msr", "vcpu_attach/destroy",
    "set_space", "read_vmcs(slot)", "?8", "set_apic_address", "set_msr_access", "sync_special_regs", "get_cap_write_vmcs",
    "apic(sub-op)", "?14", "?15"};

/*
 * Logs the call, with the argument as a number and, when it looks like a user pointer that can be read, up to eight
 * words behind it. copyin() fails cleanly for a value that is not a readable address (a deadline, a mask, a slot).
 */
void logTrap(unsigned type, unsigned index, uint64_t arg, SInt32 n)
{
    const char *name = (type == HV_TASK_TRAP ? kTaskNames : kThreadNames)[index];
    uint64_t w[8] = {0};
    unsigned words = 0;
    if (arg >= 0x1000 && arg < 0x00007fffffffffffULL && (arg & 7) == 0) {
        static const unsigned sizes[] = {64, 40, 16, 8};
        for (unsigned i = 0; i < 4 && !words; i++)
            if (copyin((user_addr_t)arg, w, sizes[i]) == 0)
                words = sizes[i] / 8;
    }
    if (words >= 5)
        RHV_INFO("trap %s %u %s #%d pid %d arg=%#llx [%#llx %#llx %#llx %#llx %#llx%s]", type == HV_TASK_TRAP ? "task" : "thread", index,
                 name, (int)n, proc_selfpid(), arg, w[0], w[1], w[2], w[3], w[4], words > 5 ? " ..." : "");
    else if (words >= 2)
        RHV_INFO("trap %s %u %s #%d pid %d arg=%#llx [%#llx %#llx]", type == HV_TASK_TRAP ? "task" : "thread", index, name, (int)n,
                 proc_selfpid(), arg, w[0], w[1]);
    else if (words == 1)
        RHV_INFO("trap %s %u %s #%d pid %d arg=%#llx [%#llx]", type == HV_TASK_TRAP ? "task" : "thread", index, name, (int)n,
                 proc_selfpid(), arg, w[0]);
    else
        RHV_INFO("trap %s %u %s #%d pid %d arg=%#llx", type == HV_TASK_TRAP ? "task" : "thread", index, name, (int)n, proc_selfpid(),
                 arg);
}

/* ------------------------------------------------------------------------------------------------------------------------
 * The VM and vCPU objects and the shared pages (layout: the framework's own, mirrored here). Nothing here runs a guest yet: the run traps
 * answer HV_UNSUPPORTED. What it does: gives the native framework the memory it expects, so that creating a VM and vCPUs works, the
 * register/VMCS cache protocol works, and the capabilities can be read.
 * ---------------------------------------------------------------------------------------------------------------------- */

constexpr kern_return_t kHvOk = 0;
constexpr kern_return_t kHvError = (kern_return_t)0xfae94001;
constexpr kern_return_t kHvBusy = (kern_return_t)0xfae94002;
constexpr kern_return_t kHvBadArg = (kern_return_t)0xfae94003;
constexpr kern_return_t kHvNoRes = (kern_return_t)0xfae94005;
constexpr kern_return_t kHvNoDev = (kern_return_t)0xfae94006;

constexpr unsigned kMaxVcpus = 64;
constexpr uint32_t kRwSlice = 0xe18, kRoSlice = 0x6d8;           /* per-vCPU slices, fixed in the framework */
constexpr uint32_t kRwFpOff = kMaxVcpus * kRwSlice;              /* 0x38600 */
constexpr uint32_t kRoFpOff = kMaxVcpus * kRoSlice;              /* 0x1b600 */
constexpr uint32_t kCacheSlots = 93;                             /* slots 0..92 live in the rw slice; the rest belongs to the kernel */
constexpr uint32_t kRwMsrVal = 0x7d0, kRwMsrValid = 0xc50;      /* cache of MSR values by index, and its valid bitmap (3 words) */
constexpr uint32_t kRoMsrPos = 0x58;                              /* ro: one byte per MSR index, position in the managed list, 0xff = not managed */
constexpr uint32_t kRwValid = 0x2f0, kRwDirty = 0x300, kRwRegs = 0xc80;   /* valid and dirty bitmaps (2 words each), the register array */
constexpr uint32_t kRoSynced = 0xe8, kRoCapMin = 0xf1, kRoCapMax = 0x3d9, kRoCapValid = 0x6c1;   /* ro slice (the arrays are unaligned) */
constexpr uint32_t kInfoBytes = 0x118;

/* VMX capability words offered to the client (the shim's values WITHOUT the TPR shadow and NMI-window offers: the native path implements neither yet), indexed by HV_VMX_CAP_*. */
const uint64_t kCaps[17] = {
    0x3f00000016ull, 0xf7d9fffe04006172ull, 0x10ea00000000ull, 0xd3ff000011ffull, 0x3fefff00036dffull, 0x98100000000001ull,
    0x3f00000016ull, 0xf7d9fffe04006172ull, 0xd3fb000011fbull, 0x3feffb00036dfbull, 0x40040ull, 0x20ull, 0xffffffffull, 0ull,
    0xffffffull, 0x5cull, 0xf0106114040ull};

/* VMs that exist. The kernel calls the callbacks WITHOUT a NULL check for every thread and task that has a target, so while this is above 0 the
 * callbacks must stay registered: unregistering is refused. */
volatile SInt32 gLiveVms = 0;

/* The TPR shadow (VMX control bit 21) is offered to every VM except HyperKit's: it lets Windows change its CR8 without a VM exit, which is most of the exits of
 * a Windows boot (Parallels boots in about 19 s instead of 90, VirtualBox's Windows Setup runs at all). On by default since 0.7.3; the boot-arg
 * rhv_tpr_shadow=0 or `rhv-info hv tpr 0` turns it off for the VMs created from then on. */
volatile int gTprShadow = 1;

volatile int *gSupportPtr = nullptr;                 /* the kernel's hv_support_available once located and written */
int gSupportSaved = 0;
bool gSupportSet = false;

com_overlorddev_RyzenHV *gSvc = nullptr;     /* for the CPU info a vCPU object needs; set by start(), lives as long as the kext */

struct HvVm;
struct HvVcpu {
    HvVm *vm;
    uint32_t id;
    uint8_t *rw;       /* kernel address of this vCPU's rw slice */
    uint8_t *ro;
    com_overlorddev_RyzenHVVm *cpu;   /* the vCPU object of the existing SVM core (VMX mode, joined to vm->shared) */
    RHVRunIn  in;      /* the run call's buffers live here, not on the kernel stack */
    RHVRunOut out;
    RHVFpuState fpu;
    uint8_t *msrPage;          /* this vCPU's 4 KiB page in the ro area: the managed MSR list, entries of 16 bytes {msr, value} */
    uint32_t nManaged;
    bool     cpuidPending;     /* the last exit was a CPUID: its answer is cleaned on the way back in (HyperKit only) */
    uint32_t cpuidLeaf;
    uint32_t cpuidSub;
    uint64_t tprPage;          /* the TPR word of the page as this driver last left it (every client but HyperKit: the client's write is applied when it differs) */
    uint64_t lastCtl[3];       /* the last pin-based, processor-based and secondary controls this client wrote, for the log */
    bool     tprLogged;
    uint8_t  managedIdx[256];  /* position in the list -> MSR cache index, 0 = a hole */
    uint64_t applied[144];     /* the last value handed to the core for each MSR cache index */
    uint8_t  appliedKnown[144];
};
struct HvVm {
    IOLock *lock;
    IOBufferMemoryDescriptor *md[3];   /* rw, ro, commpage */
    IOMemoryMap *map[3];
    uint8_t *kva[3];
    com_overlorddev_RyzenHVVmShared *shared;   /* the nested page tables and the guest memory pinned from the client */
    uint32_t fpStride;                         /* bytes per vCPU in the two FP-state arrays */
    uint32_t roArrOff;                         /* offset of the per-vCPU 4 KiB pages (managed MSR lists) in the ro area */
    uint64_t tscOffset;                        /* from hv_vm_sync_tsc, handed to vCPUs created later */
    bool     tscValid;
    bool     hyperkit;                         /* the creating process is Docker's HyperKit: its CPUID answers get the AMD-only features removed */
    bool     parallels;                        /* the creating process is Parallels' VM process: its LAPIC page (GPA 0xFEE00000) is not mapped */
    uint64_t caps[17];                         /* the VMX capability words this VM's client was shown (HV_VMX_CAP_*), also what thread trap 12 answers from */
    HvVcpu *vcpu[kMaxVcpus];
    uint32_t nVcpus;
};

inline uint32_t roundPage(uint32_t v) { return (v + 0xfffu) & ~0xfffu; }

uint32_t xsaveBytes()
{
    uint32_t a = 0xd, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if (c < 0x240 || c > 0x4000)
        c = 0x340;                                  /* a size every AVX-capable CPU fits in; the framework only reads it */
    return c;
}

void vmFree(HvVm *vm)
{
    if (!vm)
        return;
    if (vm->shared)
        vm->shared->release();                       /* unpins everything that is still mapped */
    for (unsigned i = 0; i < kMaxVcpus; i++) {
        if (vm->vcpu[i]) {
            if (vm->vcpu[i]->cpu)
                vm->vcpu[i]->cpu->release();
            IOFree(vm->vcpu[i], sizeof(HvVcpu));
        }
    }
    for (int i = 2; i >= 0; i--) {
        if (vm->map[i])
            vm->map[i]->release();
        if (vm->md[i])
            vm->md[i]->release();
    }
    if (vm->lock)
        IOLockFree(vm->lock);
    IOFree(vm, sizeof(HvVm));
    OSDecrementAtomic(&gLiveVms);
}

/* Allocates the three areas, maps them into the calling task, fills the interface block. */
HvVm *vmCreate(uint64_t *info)
{
    HvVm *vm = (HvVm *)IOMallocZero(sizeof(HvVm));
    if (!vm)
        return nullptr;
    OSIncrementAtomic(&gLiveVms);                    /* balanced by vmFree, which every failure path below also reaches */
    {
        char pname[32] = {0};
        proc_selfname(pname, sizeof(pname));
        vm->hyperkit = strncmp(pname, "com.docker.hyper", 16) == 0 || strncmp(pname, "hyperkit", 8) == 0;
        vm->parallels = strncmp(pname, "prl_vm_app", 10) == 0;
    }
    vm->lock = IOLockAlloc();
    if (!vm->lock) {
        vmFree(vm);
        return nullptr;
    }

    uint32_t fpStride = (xsaveBytes() + 63u) & ~63u;
    uint32_t roOffArr = roundPage(kRoFpOff + kMaxVcpus * fpStride);
    uint32_t sizes[3] = {roundPage(kRwFpOff + kMaxVcpus * fpStride), roOffArr + kMaxVcpus * 0x1000u, 0x1000u};

    task_t task = current_task();
    for (int i = 0; i < 3; i++) {
        vm->md[i] = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared, sizes[i], 4096);
        if (!vm->md[i]) {
            vmFree(vm);
            return nullptr;
        }
        vm->kva[i] = (uint8_t *)vm->md[i]->getBytesNoCopy();
        bzero(vm->kva[i], sizes[i]);
        if (i == 2) {
            /* hv_vmx_tsc_clock (Hypervisor.framework, called by Parallels when it starts a vCPU) is a seqlock over this page: it spins
             * until the 32-bit sequence at +0x10 is non-zero, then returns rdtsc + [+8] - [+0]. Zero in both is the host TSC itself,
             * which is what the shim's hv_tsc_clock returned and Parallels worked with. HyperKit never calls it. */
            *(volatile uint32_t *)(vm->kva[2] + 0x10) = 1;
        }
        /* The ro area and the commpage are read-only for the client. */
        vm->map[i] = vm->md[i]->createMappingInTask(task, 0, kIOMapAnywhere | (i == 0 ? 0 : kIOMapReadOnly));
        if (!vm->map[i]) {
            vmFree(vm);
            return nullptr;
        }
    }

    vm->fpStride = fpStride;
    vm->roArrOff = roOffArr;
    IOReturn err = kIOReturnSuccess;
    vm->shared = com_overlorddev_RyzenHVVmShared::create(RHV_VMCREATE_NO_RAM, task, &err);
    if (!vm->shared) {
        RHV_WARN("hv front end: cannot create the shared VM (0x%x)", err);
        vmFree(vm);
        return nullptr;
    }

    bzero(info, kInfoBytes);
    info[0x00 / 8] = kRwFpOff;
    info[0x08 / 8] = kRoFpOff;
    info[0x10 / 8] = fpStride;
    info[0x18 / 8] = roOffArr;
    info[0x20 / 8] = 0x1000;
    uint64_t caps[17];
    for (unsigned c = 0; c < 17; c++)
        caps[c] = kCaps[c];
    if (gTprShadow && !vm->hyperkit) {
        caps[1] |= 1ull << (32 + 21);                /* PROCBASED and TRUE_PROCBASED: allowed-1 for "use TPR shadow" (the framework does not strip it) */
        caps[7] |= 1ull << (32 + 21);
    }
    for (unsigned c = 0; c < 17; c++) {
        info[(0x28 + 8 * c) / 8] = caps[c];
        vm->caps[c] = caps[c];
    }
    info[0x100 / 8] = vm->map[0]->getAddress();
    info[0x108 / 8] = vm->map[1]->getAddress();
    info[0x110 / 8] = vm->map[2]->getAddress();
    return vm;
}

volatile SInt32 gTraceCount[2][16];

void note(unsigned type, unsigned index, uint64_t arg)
{
    SInt32 n = OSIncrementAtomic((volatile SInt32 *)&gTraceCount[type][index]) + 1;
    if (n <= 24 || (n % 1000) == 0)
        logTrap(type, index, arg, n);
}

kern_return_t hvFromIo(IOReturn r);
inline kern_return_t hvFromIoEarly(IOReturn r) { return hvFromIo(r); }

kern_return_t tVmCreate(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 0, arg);
    if (target)
        return kHvBusy;                              /* this task has a VM already */
    struct { uint64_t flags, info; } a = {0, 0};
    if (copyin((user_addr_t)arg, &a, sizeof(a)))
        return kHvError;
    if (a.flags & 0x400)
        return kHvUnsupportedCode;                   /* the in-kernel APIC variant */
    uint64_t info[kInfoBytes / 8];
    HvVm *vm = vmCreate(info);
    if (!vm)
        return kHvNoRes;
    if (copyout(info, (user_addr_t)a.info, kInfoBytes)) {
        vmFree(vm);
        return kHvError;
    }
    hv_set_task_target(vm);
    rhv::hv::patch::fixCurrentClient();              /* before the client's own CPU checks, which come after hv_vm_create */
    return kHvOk;
}

kern_return_t tVmDestroy(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 1, arg);
    HvVm *vm = static_cast<HvVm *>(target);
    if (!vm)
        return kHvNoDev;
    IOLockLock(vm->lock);
    unsigned n = vm->nVcpus;
    IOLockUnlock(vm->lock);
    if (n)
        return kHvBusy;
    const bool logIt = !vm->hyperkit;
    hv_set_task_target(nullptr);
    vmFree(vm);
    if (logIt) {                                   /* the entry is in the trap log; this shows that the teardown came back (a hung exit has no such line) */
        static volatile SInt32 doneLogged;
        if (OSIncrementAtomic(&doneLogged) < 40)
            RHV_INFO("hv front end: vm_destroy done (pid %d)", proc_selfpid());
    }
    return kHvOk;
}

void fpuToPage(HvVcpu *v);

kern_return_t tVcpuCreate(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 12, arg);
    HvVm *vm = static_cast<HvVm *>(target);
    if (!vm)
        return kHvNoDev;
    struct { uint64_t idPtr, flags; } a = {0, 0};
    if (copyin((user_addr_t)arg, &a, sizeof(a)))
        return kHvError;
    if (a.flags > 3)
        return kHvBadArg;
    if (hv_get_thread_target())
        return kHvBusy;                              /* this thread owns a vCPU already */

    if (!gSvc)
        return kHvNoDev;
    HvVcpu *v = (HvVcpu *)IOMallocZero(sizeof(HvVcpu));
    if (!v)
        return kHvNoRes;
    {
        rhv::svm::rescan();       /* CPUs that came online after the kext started */
        RHVInfo info;
        gSvc->getInfo(&info);
        IOReturn err = kIOReturnSuccess;
        v->cpu = com_overlorddev_RyzenHVVm::create(info, RHV_VMCREATE_VMX | RHV_VMCREATE_NO_RAM, current_task(), vm->shared, &err);
        if (!v->cpu) {
            RHV_WARN("hv front end: cannot create the vCPU object (0x%x)", err);
            IOFree(v, sizeof(HvVcpu));
            return hvFromIoEarly(err);
        }
    }
    IOLockLock(vm->lock);
    unsigned id = 0;
    while (id < kMaxVcpus && vm->vcpu[id])
        id++;
    if (id == kMaxVcpus) {
        IOLockUnlock(vm->lock);
        v->cpu->release();
        IOFree(v, sizeof(HvVcpu));
        return kHvNoDev;
    }
    v->vm = vm;
    v->id = id;
    v->rw = vm->kva[0] + id * kRwSlice;
    v->ro = vm->kva[1] + id * kRoSlice;
    bzero(v->rw, kRwSlice);
    bzero(v->ro, kRoSlice);
    memset(v->ro + kRoMsrPos, 0xff, rhv::hv::kMsrCacheMax + 1);               /* no MSR is managed yet */
    v->msrPage = vm->kva[1] + vm->roArrOff + id * 0x1000u;
    bzero(v->msrPage, 0x1000);
    if (vm->tscValid)
        v->cpu->vmcsWrite(0x2010, vm->tscOffset);      /* a vCPU created after hv_vm_sync_tsc gets the same offset */
    vm->vcpu[id] = v;
    vm->nVcpus++;
    IOLockUnlock(vm->lock);

    uint32_t outId = id;
    if (copyout(&outId, (user_addr_t)a.idPtr, sizeof(outId))) {
        IOLockLock(vm->lock);
        vm->vcpu[id] = nullptr;
        vm->nVcpus--;
        IOLockUnlock(vm->lock);
        v->cpu->release();
        IOFree(v, sizeof(HvVcpu));
        return kHvError;
    }
    fpuToPage(v);                                    /* initial FP state and XCR0 into the page */
    hv_set_thread_target(v);
    return kHvOk;
}

kern_return_t tVcpuDestroy(void *target, uint64_t arg)
{
    note(HV_THREAD_TRAP, 5, arg);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    if (!v)
        return kHvNoDev;
    HvVm *vm = v->vm;
    const bool logIt = !vm->hyperkit;
    const unsigned id = v->id;
    hv_set_thread_target(nullptr);
    IOLockLock(vm->lock);
    vm->vcpu[v->id] = nullptr;
    vm->nVcpus--;
    IOLockUnlock(vm->lock);
    v->cpu->release();
    IOFree(v, sizeof(HvVcpu));
    if (logIt) {
        static volatile SInt32 doneLogged;
        if (OSIncrementAtomic(&doneLogged) < 60)
            RHV_INFO("hv front end: vcpu%u destroy done (pid %d)", id, proc_selfpid());
    }
    return kHvOk;
}

/* The cache words of a vCPU's rw slice. */
inline uint64_t *slotWord(HvVcpu *v, unsigned slot) { return reinterpret_cast<uint64_t *>(v->rw + 8 + slot * 8); }
inline uint64_t *validWord(HvVcpu *v, unsigned w) { return reinterpret_cast<uint64_t *>(v->rw + kRwValid + w * 8); }
inline uint64_t *dirtyWord(HvVcpu *v, unsigned w) { return reinterpret_cast<uint64_t *>(v->rw + kRwDirty + w * 8); }
inline uint64_t *gprWord(HvVcpu *v, unsigned reg) { return reinterpret_cast<uint64_t *>(v->rw + kRwRegs + reg * 8); }
inline void markValid(HvVcpu *v, unsigned slot) { *validWord(v, slot >> 6) |= 1ull << (slot & 63); }
inline uint64_t *msrValWord(HvVcpu *v, unsigned idx) { return reinterpret_cast<uint64_t *>(v->rw + kRwMsrVal + idx * 8); }
inline uint64_t *msrValidWord(HvVcpu *v, unsigned w) { return reinterpret_cast<uint64_t *>(v->rw + kRwMsrValid + w * 8); }
inline bool msrIsValid(HvVcpu *v, unsigned idx) { return (*msrValidWord(v, idx >> 6) >> (idx & 63)) & 1; }
inline void msrSetValid(HvVcpu *v, unsigned idx, bool on)
{
    if (on)
        *msrValidWord(v, idx >> 6) |= 1ull << (idx & 63);
    else
        *msrValidWord(v, idx >> 6) &= ~(1ull << (idx & 63));
}
constexpr unsigned R_XCR0 = 50, R_DR0 = 41, R_DR4 = 45, R_DR5 = 46, R_DR6 = 47;   /* registers kept in the page, by the framework's numbers */

/* The core's FP state and XCR0 into the page: the ro FP array (what hv_vcpu_read_fpstate reads) and the XCR0 register. */
void fpuToPage(HvVcpu *v)
{
    v->cpu->getFpu(&v->fpu);
    uint8_t *dst = v->vm->kva[1] + kRoFpOff + v->id * v->vm->fpStride;
    uint32_t n = v->fpu.size < v->vm->fpStride ? v->fpu.size : v->vm->fpStride;
    bcopy(v->fpu.area, dst, n);
    *gprWord(v, R_XCR0) = v->fpu.xcr0;
}

/* Thread trap 7: the client read a slot that is not valid in its cache: ask the SVM core for the field and put it there. */
kern_return_t tReadSlot(void *target, uint64_t slot)
{
    note(HV_THREAD_TRAP, 7, slot);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    if (!v)
        return kHvNoDev;
    if (slot == 0 || slot >= kCacheSlots)
        return kHvBadArg;
    uint32_t field = rhv::hv::kSlotField[slot];
    uint64_t val = 0;
    IOReturn rc = v->cpu->vmcsRead(field, &val);
    if (rc != kIOReturnSuccess)
        return hvFromIo(rc);
    *slotWord(v, (unsigned)slot) = val;
    markValid(v, (unsigned)slot);
    return kHvOk;
}

/* Thread trap 11: the client wants DR0..DR7 and the page does not have them current: read them from the core (DR7 is a VMCS slot, the client
 * gets it with the other slots; DR4 and DR5 alias DR6 and DR7). */
kern_return_t tSyncSpecial(void *target, uint64_t arg)
{
    note(HV_THREAD_TRAP, 11, arg);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    if (!v)
        return kHvNoDev;
    uint64_t val = 0;
    for (unsigned n = 0; n < 4; n++) {
        val = 0;
        v->cpu->vmcsRead(0xF010 + 2 * n, &val);
        *gprWord(v, R_DR0 + n) = val;
    }
    val = 0;
    v->cpu->vmcsRead(0xF002, &val);
    *gprWord(v, R_DR6) = val;
    *gprWord(v, R_DR4) = val;
    val = 0;
    v->cpu->vmcsRead(0x681a, &val);
    *gprWord(v, R_DR5) = val;
    v->ro[kRoSynced] |= 1;
    return kHvOk;
}

/* Thread trap 12: capability of a writable field ("may be 0" / "may be 1" words). Everything is allowed for now. */
kern_return_t tCapWrite(void *target, uint64_t slot)
{
    note(HV_THREAD_TRAP, 12, slot);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    if (!v)
        return kHvNoDev;
    if (slot >= kCacheSlots)
        return kHvBadArg;
    uint64_t lo = 0, hi = ~0ull, valid;
    if (!v->vm->hyperkit) {
        /* The control words of VMX have limits: the bits that must be 1 (allowed 0-settings, the low half of the capability word) and the bits that may be 1
         * (allowed 1-settings, the high half), the same numbers hv_vmx_read_capability gives. VirtualBox's log shows it takes the two arrays as
         * allowed_0 and allowed_1. Everything else (guest state, 64-bit fields) can be written freely. HyperKit keeps the answer it always got. */
        int ci = -1;
        switch (rhv::hv::kSlotField[slot]) {
        case 0x4000: ci = 0; break;      /* pin-based */
        case 0x4002: ci = 1; break;      /* processor-based */
        case 0x401e: ci = 2; break;      /* secondary processor-based */
        case 0x4012: ci = 3; break;      /* VM entry */
        case 0x400c: ci = 4; break;      /* VM exit */
        default: break;
        }
        if (ci >= 0) {
            lo = v->vm->caps[ci] & 0xFFFFFFFFull;
            hi = v->vm->caps[ci] >> 32;
        }
    }
    __builtin_memcpy(v->ro + kRoCapMin + slot * 8, &lo, 8);
    __builtin_memcpy(v->ro + kRoCapMax + slot * 8, &hi, 8);
    __builtin_memcpy(&valid, v->ro + kRoCapValid + (slot >> 6) * 8, 8);
    valid |= 1ull << (slot & 63);
    __builtin_memcpy(v->ro + kRoCapValid + (slot >> 6) * 8, &valid, 8);
    return kHvOk;
}

kern_return_t tInvalidateTlb(void *target, uint64_t arg)
{
    note(HV_THREAD_TRAP, 2, arg);
    return target ? kHvOk : kHvNoDev;
}

/* Managed MSRs (thread traps 3 enable, 4 disable, 10 set access): the guest uses the MSR natively; the client reads its value from the
 * per-vCPU list in the ro area. */
/* MSRs the framework keeps in VMCS slots (indices 145..150: SYSENTER_CS/ESP/EIP, FS_BASE, GS_BASE, DEBUGCTL): their values already travel
 * through the slot cache, only the guest's native access is switched here. Their bytes in the ro position table do not exist (they would
 * land on the synced flag and the mode byte), so nothing is written there. */
uint32_t slotMsr(unsigned idx)
{
    switch (idx) {
    case 145: return 0x174;
    case 146: return 0x175;
    case 147: return 0x176;
    case 148: return 0xc0000100;
    case 149: return 0xc0000101;
    case 150: return 0x1d9;
    case 144: return 0x10;          /* the TSC: the guest reads it natively, the offset is the VMCB's */
    }
    return 0;
}

kern_return_t managedOn(HvVcpu *v, unsigned idx)
{
    if (uint32_t sm = slotMsr(idx)) {
        uint64_t unused = 0;
        return hvFromIo(v->cpu->msrOp(RHV_MSR_NATIVE_ENABLE, sm, 0, &unused));
    }
    if (idx < 1 || idx > rhv::hv::kMsrCacheMax || !rhv::hv::kMsrOfIdx[idx])
        return kHvBadArg;
    if (v->ro[kRoMsrPos + idx] != 0xff)
        return kHvOk;                                     /* already managed */
    if (v->nManaged >= 255)
        return kHvNoRes;
    uint32_t msr = rhv::hv::kMsrOfIdx[idx];
    uint64_t val = 0;
    IOReturn rc = v->cpu->msrOp(RHV_MSR_NATIVE_ENABLE, msr, 0, &val);
    if (rc != kIOReturnSuccess)
        return hvFromIo(rc);
    v->cpu->msrOp(RHV_MSR_READ, msr, 0, &val);
    unsigned pos = v->nManaged++;
    v->managedIdx[pos] = (uint8_t)idx;
    __builtin_memcpy(v->msrPage + pos * 16, &msr, 4);
    __builtin_memcpy(v->msrPage + pos * 16 + 8, &val, 8);
    v->ro[kRoMsrPos + idx] = (uint8_t)pos;
    msrSetValid(v, idx, false);
    return kHvOk;
}

kern_return_t managedOff(HvVcpu *v, unsigned idx)
{
    if (uint32_t sm = slotMsr(idx)) {
        uint64_t unused = 0;
        return hvFromIo(v->cpu->msrOp(RHV_MSR_NATIVE_DISABLE, sm, 0, &unused));
    }
    if (idx < 1 || idx > rhv::hv::kMsrCacheMax || !rhv::hv::kMsrOfIdx[idx])
        return kHvBadArg;
    uint8_t pos = v->ro[kRoMsrPos + idx];
    if (pos == 0xff)
        return kHvOk;
    uint64_t val = 0;
    __builtin_memcpy(&val, v->msrPage + pos * 16 + 8, 8);
    IOReturn rc = v->cpu->msrOp(RHV_MSR_NATIVE_DISABLE, rhv::hv::kMsrOfIdx[idx], 0, &val);
    v->managedIdx[pos] = 0;                                /* the list keeps a hole, positions never move */
    v->ro[kRoMsrPos + idx] = 0xff;
    __builtin_memcpy(&val, v->msrPage + pos * 16 + 8, 8);
    *msrValWord(v, idx) = val;                             /* the last value stays readable from the cache */
    msrSetValid(v, idx, true);
    v->applied[idx] = val;
    v->appliedKnown[idx] = 1;
    return rc == kIOReturnSuccess ? kHvOk : hvFromIo(rc);
}

kern_return_t tEnableManaged(void *target, uint64_t idx)
{
    note(HV_THREAD_TRAP, 3, idx);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    return v ? managedOn(v, (unsigned)idx) : kHvNoDev;
}

kern_return_t tDisableManaged(void *target, uint64_t idx)
{
    note(HV_THREAD_TRAP, 4, idx);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    return v ? managedOff(v, (unsigned)idx) : kHvNoDev;
}

/* arg = (access << 32) | cache index; any access means "the guest uses it without exits". */
kern_return_t tSetMsrAccess(void *target, uint64_t arg)
{
    note(HV_THREAD_TRAP, 10, arg);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    if (!v)
        return kHvNoDev;
    return (arg >> 32) ? managedOn(v, (unsigned)arg) : managedOff(v, (unsigned)arg);
}

/* hv_vm_sync_tsc: the guest sees `tsc` now; every vCPU gets the same offset. */
kern_return_t tVmSyncTsc(void *target, uint64_t tsc)
{
    note(HV_TASK_TRAP, 7, tsc);
    HvVm *vm = static_cast<HvVm *>(target);
    if (!vm)
        return kHvNoDev;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t offset = tsc - (((uint64_t)hi << 32) | lo);
    IOLockLock(vm->lock);
    vm->tscOffset = offset;
    vm->tscValid = true;
    for (unsigned id = 0; id < kMaxVcpus; id++)
        if (vm->vcpu[id] && vm->vcpu[id]->cpu)
            vm->vcpu[id]->cpu->vmcsWrite(0x2010, offset);
    IOLockUnlock(vm->lock);
    return kHvOk;
}

/* The general registers live in the page, indexed by the framework's register number. */
enum { R_RAX = 2, R_RCX = 3, R_RDX = 4, R_RBX = 5, R_RSI = 6, R_RDI = 7, R_RSP = 8, R_RBP = 9, R_R8 = 10, R_CR2 = 38, R_TPR = 49 };
constexpr unsigned kSlotRsp = 75, kSlotRip = 76, kSlotRflags = 77;

const uint32_t kRunFields[RHV_RUN_NFIELDS] = {
#define RHV_X(enc) enc,
    RHV_RUN_FIELDS_LIST(RHV_X)
#undef RHV_X
};

/* Parallels' monitor answers CPUID from the AMD host. The same post-processing the shim does (shim/hv_shim.c, cpuid_hide_hypervisor), applied to the
 * registers the monitor wrote, on the way back into the guest:
 *  - the hypervisor bit and the Hyper-V leaves are hidden: Windows then never sets up the hypercall page, which Parallels fills with the Intel
 *    VMCALL (a #UD on AMD, and a triple fault at the very start of the Windows kernel);
 *  - an Intel-style cache/core topology (leaf 4, AMD's 0x80000005/6/8 and CmpLegacy): the AMD host's own answer makes Windows see every vCPU as a
 *    package of its own, and Windows Home runs only one package, so it would start one CPU. */
void parallelsCpuidFixup(HvVcpu *v)
{
    const uint32_t leaf = v->cpuidLeaf, sub = v->cpuidSub;
    unsigned n = v->vm->nVcpus;
    if (n < 1)
        n = 1;
    uint64_t *rax = gprWord(v, R_RAX), *rcx = gprWord(v, R_RCX), *rdx = gprWord(v, R_RDX), *rbx = gprWord(v, R_RBX);
    if (leaf == 4) {
        const uint32_t cores = (uint32_t)(n - 1) << 26;
        uint32_t a = 0, b = 0, c = 0;
        switch (sub) {
        case 0: a = cores | (1u << 8) | (1u << 5) | 1; b = 63 | (7u << 22); c = 63; break;                                    /* L1 data    */
        case 1: a = cores | (1u << 8) | (1u << 5) | 2; b = 63 | (7u << 22); c = 63; break;                                    /* L1 inst    */
        case 2: a = cores | (1u << 8) | (2u << 5) | 3; b = 63 | (7u << 22); c = 1023; break;                                  /* L2 unified */
        case 3: a = cores | ((uint32_t)(n - 1) << 14) | (1u << 8) | (3u << 5) | 3; b = 63 | (15u << 22); c = 16383; break;   /* L3         */
        default: break;
        }
        *rax = a;
        *rbx = b;
        *rcx = c;
        *rdx = 0;
    } else if (leaf == 0x80000008u) {
        if (n > 1) {                                    /* ECX[7:0] = cores per package - 1, ECX[15:12] = bits of the APIC id that name the core */
            unsigned bits = 0;
            while ((1u << bits) < n)
                bits++;
            *rcx = (*rcx & ~0xF0FFull) | ((uint64_t)bits << 12) | (n - 1);
        }
    } else if (leaf == 0x80000005u) {                   /* L1: 32 KiB data and instruction, 8-way, 64-byte lines */
        *rax = 0xFF40FF40u;
        *rbx = 0xFF40FF40u;
        *rcx = (32u << 24) | (8u << 16) | (1u << 8) | 64;
        *rdx = (32u << 24) | (8u << 16) | (1u << 8) | 64;
    } else if (leaf == 0x80000006u) {                   /* L3: 4 MiB per core (Windows adds them up), 16-way */
        *rdx = (8u << 18) | (8u << 12) | (1u << 8) | 64;
    } else if (leaf == 0x80000001u) {
        if (n > 1)                                      /* CmpLegacy: the logical-processor count in leaf 1 is valid */
            *rcx |= 2;
    } else if (leaf == 1) {
        *rcx &= ~(1ull << 31);                          /* no hypervisor */
    } else if (leaf >= 0x40000000u && leaf <= 0x400000FFu) {
        *rax = 0;
        *rbx = 0;
        *rcx = 0;
        *rdx = 0;
    }
}

kern_return_t tVcpuRun(void *target, uint64_t arg)
{
    note(HV_THREAD_TRAP, 0, arg);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    if (!v)
        return kHvNoDev;
    com_overlorddev_RyzenHVVm *cpu = v->cpu;

    /* 1. What the client changed since the last run: registers from the page, VMCS fields from the dirty slots. */
    /* HyperKit answers CPUID from the host's values, which on a Ryzen are AMD's; a Linux guest then trusts features HyperKit does not emulate
     * (MWAITX in its delay loop, SVM, extended topology, AMD counters, IBPB/IBRS/SSBD enumeration). Clean the answer it just wrote. */
    if (v->cpuidPending && v->vm->parallels) {
        v->cpuidPending = false;
        parallelsCpuidFixup(v);
    } else if (v->cpuidPending) {
        v->cpuidPending = false;
        if (v->cpuidLeaf == 0x80000001u)
            *gprWord(v, R_RCX) &= ~((1ull << 2) | (1ull << 22) | (1ull << 23) | (1ull << 24) | (1ull << 26) | (1ull << 27) | (1ull << 29));
        else if (v->cpuidLeaf == 0x80000008u)
            *gprWord(v, R_RBX) = 0;
    }
    RHVRegs regs;
    cpu->getRegs(&regs);
    regs.rax = *gprWord(v, R_RAX);
    regs.rcx = *gprWord(v, R_RCX);
    regs.rdx = *gprWord(v, R_RDX);
    regs.rbx = *gprWord(v, R_RBX);
    regs.rsi = *gprWord(v, R_RSI);
    regs.rdi = *gprWord(v, R_RDI);
    regs.rbp = *gprWord(v, R_RBP);
    uint64_t *r8_15[8] = {&regs.r8, &regs.r9, &regs.r10, &regs.r11, &regs.r12, &regs.r13, &regs.r14, &regs.r15};
    for (unsigned i = 0; i < 8; i++)
        *r8_15[i] = *gprWord(v, R_R8 + i);
    const uint64_t flags = *reinterpret_cast<uint64_t *>(v->rw);   /* 0x01 slots, 0x04 MSRs, 0x08 misc; 0x20 XCR0, 0x40 DRs, 0x80 FP state, 0x100 TSC */
    uint64_t dirty[2] = {*dirtyWord(v, 0), *dirtyWord(v, 1)};
    auto isDirty = [&](unsigned slot) { return (dirty[slot >> 6] >> (slot & 63)) & 1; };
    if (isDirty(kSlotRsp))
        regs.rsp = *slotWord(v, kSlotRsp);
    if (isDirty(kSlotRip))
        regs.rip = *slotWord(v, kSlotRip);
    if (isDirty(kSlotRflags))
        regs.rflags = *slotWord(v, kSlotRflags);
    unsigned rejected = 0;
    for (unsigned slot = 1; slot < kCacheSlots; slot++) {
        if (!isDirty(slot) || slot == kSlotRsp || slot == kSlotRip || slot == kSlotRflags)
            continue;
        uint32_t field = rhv::hv::kSlotField[slot];
        /* A field the SVM core does not model is skipped, not fatal: the framework accepts any write to a slot. */
        if (field) {
            if (!v->vm->hyperkit && (field == 0x4000 || field == 0x4002 || field == 0x401e)) {      /* which controls does this client switch on? */
                const unsigned which = field == 0x4000 ? 0 : field == 0x4002 ? 1 : 2;
                const uint64_t val = *slotWord(v, slot);
                static volatile SInt32 ctlLogged;
                if (v->lastCtl[which] != val && OSIncrementAtomic(&ctlLogged) < 80) {
                    char pn[32] = {0};
                    proc_selfname(pn, sizeof(pn));
                    RHV_INFO("hv front end: %s vcpu%u writes control %#x = %#llx (was %#llx)", pn, v->id, field, (unsigned long long)val,
                             (unsigned long long)v->lastCtl[which]);
                }
                v->lastCtl[which] = val;
            }
            if (field == 0x4016 && v->vm->parallels && (*slotWord(v, slot) >> 31)) {        /* the monitor injects an event: what and at which TPR */
                static volatile SInt32 injected;
                SInt32 ni = OSIncrementAtomic(&injected) + 1;
                if (ni <= 60 || (ni % 500) == 0)
                    RHV_INFO("hv front end: inject %#llx (#%d) at tpr %#llx rflags %#llx", (unsigned long long)*slotWord(v, slot), (int)ni,
                             (unsigned long long)*gprWord(v, R_TPR), (unsigned long long)regs.rflags);
            }
            IOReturn wr = cpu->vmcsWrite(field, *slotWord(v, slot));
            if (wr != kIOReturnSuccess) {
                rejected++;
                static volatile SInt32 logged;
                if (OSIncrementAtomic(&logged) < 20)    /* the shim told the client at write time; here the write is dropped, so say so */
                    RHV_INFO("hv front end: VMCS write refused by the core: field %#x slot %u value %#llx (0x%x)", field, slot,
                             *slotWord(v, slot), wr);
            }
        }
    }
    if (rejected && v->id == 0)
        RHV_DBG("hv front end: %u VMCS writes were not accepted by the core", rejected);
    *dirtyWord(v, 0) = 0;
    *dirtyWord(v, 1) = 0;
    *reinterpret_cast<uint64_t *>(v->rw) = 0;

    /* XCR0 and the FP state: the client's copy (rw array) when it wrote one, the core's own otherwise. */
    if (flags & (0x80 | 0x20)) {
        RHVFpuState *f = &v->fpu;
        cpu->getFpu(f);
        if (flags & 0x80) {
            uint32_t n = f->size < v->vm->fpStride ? f->size : v->vm->fpStride;
            bcopy(v->vm->kva[0] + kRwFpOff + v->id * v->vm->fpStride, f->area, n);
        }
        f->xcr0 = *gprWord(v, R_XCR0);
        if (flags & 0x80) {
            /* Parallels writes XSTATE_BV=7 while XCR0 is still 1 (it enables AVX afterwards). XRSTOR with a component that XCR0 has not enabled
             * faults, so the core refuses such an image; the shim cut those bits too. They are components still in their initial state. */
            if (f->size >= 520) {
                uint64_t bv;
                __builtin_memcpy(&bv, f->area + 512, sizeof(bv));
                const uint64_t keep = bv & (f->xcr0 | 3);          /* x87 and SSE are always allowed */
                if (keep != bv)
                    __builtin_memcpy(f->area + 512, &keep, sizeof(keep));
            }
        }
        IOReturn fr = cpu->setFpu(f);
        if (fr != kIOReturnSuccess)
            RHV_INFO("hv front end: FP state/XCR0 refused by the core (0x%x), xcr0 %#llx", fr, (unsigned long long)f->xcr0);
    }
    if (flags & 0x100) {
        static volatile SInt32 tscLogged;
        if (OSIncrementAtomic(&tscLogged) < 3)
            RHV_INFO("hv front end: a TSC write through hv_vcpu_write_msr(0x10) is not applied yet (hv_vm_sync_tsc is)");
    }
    /* Debug registers (DR7 is a slot and went with the others). */
    if (flags & 0x40) {
        for (unsigned n = 0; n < 4; n++)
            cpu->vmcsWrite(0xF010 + 2 * n, *gprWord(v, R_DR0 + n));
        cpu->vmcsWrite(0xF002, *gprWord(v, R_DR6));
    }
    /* MSRs of the cache: every valid entry whose value is not what the core was last given. */
    if (flags & 0x4) {
        for (unsigned idx = 1; idx <= rhv::hv::kMsrCacheMax; idx++) {
            uint32_t msr = rhv::hv::kMsrOfIdx[idx];
            if (!msr || !msrIsValid(v, idx))
                continue;
            uint64_t val = *msrValWord(v, idx);
            if (v->appliedKnown[idx] && v->applied[idx] == val)
                continue;
            uint64_t unused = 0;
            IOReturn mr = cpu->msrOp(RHV_MSR_WRITE, msr, val, &unused);
            v->applied[idx] = val;
            v->appliedKnown[idx] = 1;
            if (mr != kIOReturnSuccess) {
                static volatile SInt32 logged;
                if (OSIncrementAtomic(&logged) < 20)
                    RHV_INFO("hv front end: MSR %#x write refused by the core (0x%x), value %#llx", msr, mr, (unsigned long long)val);
            }
        }
    }

    /* NMI-window exiting is answered here: NMI blocking is not tracked, so the window is open at once (a client that sets the control wants
     * to inject an NMI and clears the control when the exit arrives). Exit reason 8, qualification 0, no guest instruction runs. */
    {
        uint64_t procCtls = 0;
        const bool haveCtls = cpu->vmcsRead(0x4002, &procCtls) == kIOReturnSuccess;
        if (haveCtls && !v->tprLogged && !v->vm->hyperkit && (procCtls & (1u << 21)) && !(procCtls & (1u << 19))) {
            v->tprLogged = true;
            RHV_INFO("hv front end: vcpu%u of this client uses the TPR shadow (processor controls %#llx)", v->id, (unsigned long long)procCtls);
        }
        if (haveCtls && (procCtls & (1u << 22))) {
            const unsigned slotReason = rhv::hv::kFieldSlot[0x4402 >> 1], slotQual = rhv::hv::kFieldSlot[0x6400 >> 1];
            if (slotReason && slotReason < kCacheSlots) {
                *slotWord(v, slotReason) = 8;
                markValid(v, slotReason);
            }
            if (slotQual && slotQual < kCacheSlots) {
                *slotWord(v, slotQual) = 0;
                markValid(v, slotQual);
            }
            /* The VMCS fields of this batch were applied above; the registers (RIP, RSP, RFLAGS, the GPRs) are applied by runEx, which this path skips,
             * and the dirty bits are already cleared. VMware's SMM entry arrives like this (NMI window first, then CS, RIP... in one batch): the guest
             * got the new CS and kept the old RIP and ran off into zeros (a black screen). */
            cpu->setRegs(&regs);
            return kHvOk;
        }
    }

    /* 2. Run. */
    *validWord(v, 0) = 0;                              /* the guest changes things: nothing in the cache is trusted until refilled */
    *validWord(v, 1) = 0;
    v->in.flags = RHV_RUN_REGS;
    v->in.nWrites = 0;
    v->in.regs = regs;
    if (!v->vm->hyperkit) {
        /* HV_X86_TPR is the 8-bit APIC TPR (class in bits 7:4); the core's V_TPR is the 4-bit class. Parallels' monitor (and VirtualBox's NEM) decide from it
         * whether they may inject an interrupt and answer the guest's CR8 reads from it, so a write has to reach the core and read back (the shim's
         * read<<4, write>>4). HyperKit never needed it and keeps the raw value. */
        const uint64_t tpr = *gprWord(v, R_TPR);
        if (tpr != v->tprPage) {
            cpu->vmcsWrite(0xF004, (tpr >> 4) & 0xF);
            v->tprPage = tpr;
        }
    }
    const uint64_t inRax = regs.rax, inRcx = regs.rcx, inRdx = regs.rdx;       /* what the client set before this entry */
    IOReturn rc = cpu->runEx(&v->in, &v->out);
    if (rc != kIOReturnSuccess) {
        static volatile SInt32 failed;
        if (!v->vm->hyperkit && OSIncrementAtomic(&failed) < 30)
            RHV_INFO("hv front end: run vcpu%u failed: 0x%x (rip %#llx)", v->id, rc, (unsigned long long)regs.rip);
        return hvFromIo(rc);
    }

    if (!v->vm->hyperkit) {
        /* What a client that is new to the native path does: the first exits, then one in 5000. Lets a stalled guest be read from rhv-info log. */
        static volatile SInt32 runs;
        SInt32 n = OSIncrementAtomic(&runs) + 1;
        if (n <= 120 || (n % 5000) == 0) {
            uint64_t reason = 0, qual = 0, ilen = 0, intr = 0, cr0 = 0;
            for (unsigned i = 0; i < RHV_RUN_NFIELDS; i++) {
                const uint64_t f = v->out.fields[i];
                switch (kRunFields[i]) {
                case 0x4402: reason = f; break;
                case 0x6400: qual = f; break;
                case 0x440c: ilen = f; break;
                case 0x4824: intr = f; break;
                case 0x6800: cr0 = f; break;
                default: break;
                }
            }
            RHV_INFO("hv front end: run %d vcpu%u exit reason %llu qual %#llx ilen %llu rip %#llx rflags %#llx rax %#llx rcx %#llx rdx %#llx in rax %#llx rcx %#llx rdx %#llx intr %#llx cr0 %#llx",
                     (int)n, v->id, (unsigned long long)(reason & 0xffff), (unsigned long long)qual, (unsigned long long)ilen,
                     (unsigned long long)v->out.regs.rip, (unsigned long long)v->out.regs.rflags, (unsigned long long)v->out.regs.rax,
                     (unsigned long long)v->out.regs.rcx, (unsigned long long)v->out.regs.rdx, (unsigned long long)inRax, (unsigned long long)inRcx,
                     (unsigned long long)inRdx, (unsigned long long)intr, (unsigned long long)cr0);
        }
    }
    if (v->vm->hyperkit || v->vm->parallels) {
        uint64_t exitReason = 0;
        for (unsigned i = 0; i < RHV_RUN_NFIELDS; i++)
            if (kRunFields[i] == 0x4402)
                exitReason = v->out.fields[i];
        if ((exitReason & 0xFFFF) == 10) {
            v->cpuidPending = true;
            v->cpuidLeaf = (uint32_t)v->out.regs.rax;
            v->cpuidSub = (uint32_t)v->out.regs.rcx;
        }
    }
    fpuToPage(v);                                       /* the core's FP state and XCR0 after the run */
    v->ro[kRoSynced] &= ~1;                             /* the DRs in the page are stale: the next read of one asks (thread trap 11) */
    for (unsigned pos = 0; pos < v->nManaged; pos++) {  /* managed MSRs live in the guest: their values are read from the core after each run */
        unsigned idx = v->managedIdx[pos];
        if (!idx)
            continue;
        uint64_t mv = 0;
        if (cpu->msrOp(RHV_MSR_READ, rhv::hv::kMsrOfIdx[idx], 0, &mv) == kIOReturnSuccess) {
            __builtin_memcpy(v->msrPage + pos * 16 + 8, &mv, 8);
            v->applied[idx] = mv;                       /* what the core holds now: a client write of the old value must not be skipped */
            v->appliedKnown[idx] = 1;
        }
        msrSetValid(v, idx, false);                     /* so that a read goes to the list */
    }

    /* 3. What happened: the registers into the page, the exit information into its slots, exactly those valid. */
    const RHVRegs &o = v->out.regs;
    *gprWord(v, R_RAX) = o.rax;
    *gprWord(v, R_RCX) = o.rcx;
    *gprWord(v, R_RDX) = o.rdx;
    *gprWord(v, R_RBX) = o.rbx;
    *gprWord(v, R_RSI) = o.rsi;
    *gprWord(v, R_RDI) = o.rdi;
    *gprWord(v, R_RBP) = o.rbp;
    const uint64_t *or8_15[8] = {&o.r8, &o.r9, &o.r10, &o.r11, &o.r12, &o.r13, &o.r14, &o.r15};
    for (unsigned i = 0; i < 8; i++)
        *gprWord(v, R_R8 + i) = *or8_15[i];
    *slotWord(v, kSlotRsp) = o.rsp;
    markValid(v, kSlotRsp);
    *slotWord(v, kSlotRip) = o.rip;
    markValid(v, kSlotRip);
    *slotWord(v, kSlotRflags) = o.rflags;
    markValid(v, kSlotRflags);
    for (unsigned i = 0; i < RHV_RUN_NFIELDS; i++) {
        uint32_t enc = kRunFields[i];
        uint64_t val = v->out.fields[i];
        if (enc == 0xF000) {
            *gprWord(v, R_CR2) = val;                  /* CR2 and TPR are registers in the page, not VMCS slots */
        } else if (enc == 0xF004) {
            if (!v->vm->hyperkit) {
                *gprWord(v, R_TPR) = (val & 0xF) << 4;
                v->tprPage = (val & 0xF) << 4;
            } else {
                *gprWord(v, R_TPR) = val;
            }
        } else if ((enc >> 1) < rhv::hv::kFieldSlotCount) {
            unsigned slot = rhv::hv::kFieldSlot[enc >> 1];
            if (slot && slot < kCacheSlots) {
                *slotWord(v, slot) = val;
                markValid(v, slot);
            }
        }
    }
    return kHvOk;
}

/* hv_vcpu_interrupt: `arg` is a mask of vCPU ids. */
kern_return_t tVcpuInterrupt(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 13, arg);
    HvVm *vm = static_cast<HvVm *>(target);
    if (!vm)
        return kHvNoDev;
    IOLockLock(vm->lock);                              /* a vCPU cannot be freed while the lock is held */
    for (unsigned id = 0; id < kMaxVcpus; id++)
        if (((arg >> id) & 1) && vm->vcpu[id] && vm->vcpu[id]->cpu)
            vm->vcpu[id]->cpu->kick();
    IOLockUnlock(vm->lock);
    return kHvOk;
}

/* Callbacks the kernel makes. They must not log: `suspend` can run with interrupts off. A thread that dies leaves its vCPU to the
 * task; the task's VM is freed with the task. */
void cbNoopVcpu(void *) {}
void cbNoopVolatile(void *, int) {}
void cbNoop(void) {}
void cbTaskDestroy(void *vm) { vmFree(static_cast<HvVm *>(vm)); }

/* IOReturn of the existing VM code -> the codes the framework expects (same table as the shim). */
kern_return_t hvFromIo(IOReturn r)
{
    switch ((uint32_t)r) {
    case 0:
        return kHvOk;
    case 0xe00002bd: case 0xe00002be: case 0xe00002db: case 0xe00002de:      /* NoMemory NoResources NoSpace CannotWire */
        return kHvNoRes;
    case 0xe00002c0: case 0xe00002d8: case 0xe00002d9:                       /* NoDevice NotReady NotAttached */
        return kHvNoDev;
    case 0xe00002c1: case 0xe00002e2:                                         /* NotPrivileged NotPermitted */
        return (kern_return_t)0xfae94007;
    case 0xe00002c2: case 0xe00002cf: case 0xe00002d0: case 0xe00002f0:      /* BadArgument NotWritable NotAligned NotFound */
    case 0xe00002f1: case 0xe00002c8:                                         /* Invalid VMError */
        return kHvBadArg;
    case 0xe00002c5: case 0xe00002d5:                                         /* ExclusiveAccess Busy */
        return kHvBusy;
    case 0xe00002c7:                                                          /* Unsupported */
        return kHvUnsupportedCode;
    default:
        return kHvError;
    }
}

/* hv_vm_map_item_t: { space, uva, gpa, size, flags }, 40 bytes. Only the default address space (0) exists. */
struct MapItem { uint64_t space, uva, gpa, size, flags; };

kern_return_t mapItem(HvVm *vm, uint64_t arg, MapItem *it)
{
    if (!vm)
        return kHvNoDev;
    if (copyin((user_addr_t)arg, it, sizeof(*it)))
        return kHvError;
    if (it->space != 0 || (it->flags >> 32))
        return kHvBadArg;
    return kHvOk;
}

/* Parallels maps one page at GPA 0xFEE00000 as the "APIC page". Intel's hardware APIC virtualization intercepts it; here it would be plain
 * guest RAM and the guest's LAPIC (timer, EOI) would do nothing. Left unmapped, the accesses fault to the client as exit 48, which its
 * monitor emulates as LAPIC MMIO (the same fix the shim has: docs/PARALLELS.md). unmap/protect of that page are answered with success. */
inline bool apicPageSkipped(const HvVm *vm, const MapItem &it)
{
    return vm->parallels && it.gpa == 0xFEE00000ull && it.size == 0x1000;
}

kern_return_t tVmMap(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 4, arg);
    MapItem it;
    kern_return_t k = mapItem(static_cast<HvVm *>(target), arg, &it);
    if (k)
        return k;
    if (apicPageSkipped(static_cast<HvVm *>(target), it))
        return kHvOk;
    return hvFromIo(static_cast<HvVm *>(target)->shared->map(current_task(), it.uva, it.gpa, it.size, (uint32_t)it.flags));
}

kern_return_t tVmUnmap(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 5, arg);
    MapItem it;
    kern_return_t k = mapItem(static_cast<HvVm *>(target), arg, &it);
    if (k)
        return k;
    if (apicPageSkipped(static_cast<HvVm *>(target), it))
        return kHvOk;
    return hvFromIo(static_cast<HvVm *>(target)->shared->unmap(it.gpa, it.size));
}

kern_return_t tVmProtect(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 6, arg);
    MapItem it;
    kern_return_t k = mapItem(static_cast<HvVm *>(target), arg, &it);
    if (k)
        return k;
    if (apicPageSkipped(static_cast<HvVm *>(target), it))
        return kHvOk;
    return hvFromIo(static_cast<HvVm *>(target)->shared->protect(it.gpa, it.size, (uint32_t)it.flags));
}

/* hv_vmx_vcpu_set_apic_address: the client tells where its virtual-APIC page is when it uses the TPR shadow. The core keeps the TPR in V_TPR and never
 * reads that page, so there is nothing to remember (the shim did the same); the argument block is not read, so a layout we do not know cannot hurt. */
kern_return_t tSetApicAddress(void *target, uint64_t arg)
{
    note(HV_THREAD_TRAP, 9, arg);
    return target ? kHvOk : kHvNoDev;
}

template <unsigned Type, unsigned Index>
kern_return_t stub(void *target, uint64_t arg)
{
    (void)target;
    SInt32 n = OSIncrementAtomic((volatile SInt32 *)&gCount[Type][Index]) + 1;
    if (n <= 24 || (n % 1000) == 0)
        logTrap(Type, Index, arg, n);
    return gMode >= 2 ? KERN_SUCCESS : kHvUnsupported;
}

#define S(T, I) stub<T, I>
/* Task traps: 0 vm_create, 1 vm_destroy, 4 vm_map, 5 vm_unmap, 6 vm_protect, 7 vm_sync_tsc, 12 vcpu_create, 13 vcpu_interrupt are real; the rest is logged and answered per mode. */
const hv_trap_t kTaskTable[kTraps] = {tVmCreate, tVmDestroy, S(0, 2), S(0, 3), tVmMap, tVmUnmap, tVmProtect, tVmSyncTsc, S(0, 8), S(0, 9),
                                      S(0, 10), S(0, 11), tVcpuCreate, tVcpuInterrupt, S(0, 14), S(0, 15)};
/* Thread traps: 0 run, 1 run_until (the deadline is not honoured yet), 2 invalidate_tlb, 3/4 managed MSRs on/off, 10 set_msr_access, 5 vcpu_destroy, 7 read slot, 11 sync special regs, 12 capability of a writable slot are real. */
const hv_trap_t kThreadTable[kTraps] = {tVcpuRun, tVcpuRun, tInvalidateTlb, tEnableManaged, tDisableManaged, tVcpuDestroy, S(1, 6), tReadSlot, S(1, 8),
                                        tSetApicAddress, tSetMsrAccess, tSyncSpecial, tCapWrite, S(1, 13), S(1, 14), S(1, 15)};
#undef S

} // namespace

namespace rhv {
namespace hv {

bool registered() { return gRegistered; }
uint32_t mode() { return gRegistered ? gMode : 0; }

bool setTprShadow(bool on)
{
    gTprShadow = on ? 1 : 0;
    RHV_SYNC("hv front end: TPR shadow is %s for the VMs created from now on (not HyperKit's)", on ? "offered" : "not offered");
    return true;
}

bool tprShadow() { return gTprShadow != 0; }
bool sysctlTaken() { return gSupportSet; }

static bool registerTraps()
{
    /*
     * On this Mac Apple's own AppleHV is loaded and has registered its tables (a native hv_vm_create answers with AppleHV's own
     * error). The kernel accepts only one registration, so Apple's is released first. Its callbacks are left alone: they are only
     * called for tasks and threads that have a target, and AppleHV never set one on a machine where its VM creation fails.
     */
    hv_release_traps(HV_THREAD_TRAP);
    hv_release_traps(HV_TASK_TRAP);
    if (hv_set_traps(HV_TASK_TRAP, kTaskTable, kTraps) != KERN_SUCCESS) {
        RHV_WARN("hv front end: hv_set_traps(task) refused");
        return false;
    }
    if (hv_set_traps(HV_THREAD_TRAP, kThreadTable, kTraps) != KERN_SUCCESS) {
        RHV_WARN("hv front end: hv_set_traps(thread) refused");
        hv_release_traps(HV_TASK_TRAP);
        return false;
    }
    /* The callbacks free a task's VM when the task ends, and keep the scheduler's calls (dispatch, preempt, suspend, ...) harmless.
     * Apple's callbacks are released first, as with the traps. */
    hv_release_callbacks();
    hv_callbacks_t cb = {};
    cb.dispatch = cbNoopVcpu;
    cb.preempt = cbNoopVcpu;
    cb.suspend = cbNoop;
    cb.thread_destroy = cbNoopVcpu;
    cb.task_destroy = cbTaskDestroy;
    cb.volatile_state = cbNoopVolatile;
    cb.resume = cbNoop;
    if (hv_set_callbacks(cb) != KERN_SUCCESS) {
        RHV_WARN("hv front end: hv_set_callbacks refused");
        hv_release_traps(HV_THREAD_TRAP);
        hv_release_traps(HV_TASK_TRAP);
        return false;
    }
    gRegistered = true;
    return true;
}

bool setMode(uint32_t m)
{
    if (m == 0) {
        if (gRegistered && gLiveVms > 0) {
            RHV_WARN("hv front end: %d VM(s) exist, not unregistering (the kernel calls the callbacks without a NULL check)", (int)gLiveVms);
            return false;
        }
        if (gRegistered) {
            setSysctl(false);
            hv_release_traps(HV_THREAD_TRAP);
            hv_release_traps(HV_TASK_TRAP);
            hv_release_callbacks();
            gRegistered = false;
            RHV_SYNC("RyzenHV hv front end: unregistered (Apple's tables are not put back)");
        }
        return true;
    }
    if (m > 2)
        m = 2;
    if (!gRegistered) {
        RHV_SYNC("RyzenHV hv front end: registering in place of AppleHV");
        if (!registerTraps())
            return false;
    }
    gMode = m;
    RHV_INFO("hv front end: registered (mode %u: traps are logged and answered %s), kern.hv_support is untouched", gMode,
             gMode >= 2 ? "success" : "HV_UNSUPPORTED");
    return true;
}

/*
 * kern.hv_support is SYSCTL_INT(... &hv_support_available ...): an int in the kernel's data that is not exported to kexts, and its OID is permanent
 * (replacing the OID panicked the kernel, see the notes). What a kext can do is write the int. Its address is the one the kernel's own hv_get_support()
 * uses, decoded from its code (`mov eax,[rip+disp32]`, matched byte for byte): that is the layout of the RUNNING kernel. Distances taken from the kernel
 * FILE do not hold for data (the first run showed the data segment sits elsewhere than in the file), so they are no longer used. The independent check is
 * the OID: after the write, sysctlbyname("kern.hv_support") must read 1 as well; if it does not, the old value is put back and the address is refused.
 */
bool addressFromCode(volatile int **out)
{
    const uint8_t *f = reinterpret_cast<const uint8_t *>(&hv_get_support);
    static const uint8_t expect[6] = {0x55, 0x48, 0x89, 0xe5, 0x8b, 0x05};
    if (__builtin_memcmp(f, expect, sizeof(expect)) != 0 || f[10] != 0x5d || f[11] != 0xc3)
        return false;
    int32_t disp;
    __builtin_memcpy(&disp, f + 6, sizeof(disp));
    *out = reinterpret_cast<volatile int *>(const_cast<uint8_t *>(f) + 10 + disp);        /* rip after the mov is f + 10 */
    return true;
}

bool oidValue(int *v)
{
    int x = -1;
    size_t len = sizeof(x);
    if (sysctlbyname("kern.hv_support", &x, &len, nullptr, 0) != 0 || len != sizeof(x))
        return false;
    *v = x;
    return true;
}

bool locateHvSupport(volatile int **out)
{
    volatile int *fromCode = nullptr;
    bool codeOk = addressFromCode(&fromCode);
    RHV_INFO("hv front end: hv_support_available from hv_get_support's code %#llx (%s)", (unsigned long long)(uintptr_t)fromCode,
             codeOk ? "decoded" : "code not as expected");
    if (!codeOk || ((uintptr_t)fromCode & 3))
        return false;
    *out = fromCode;
    return true;
}

bool setSysctl(bool on)
{
    if (!on) {
        if (gSupportSet && gSupportPtr) {
            *gSupportPtr = gSupportSaved;
            gSupportSet = false;
            RHV_INFO("hv front end: kern.hv_support back to %d", gSupportSaved);
        }
        return true;
    }
    if (!gRegistered)
        return false;
    if (gSupportSet)
        return true;
    volatile int *p = nullptr;
    if (!locateHvSupport(&p)) {
        RHV_WARN("hv front end: hv_get_support() is not the code expected (another kernel build); kern.hv_support is left alone");
        return false;
    }
    int cur = *p;
    if (cur != 0 && cur != 1) {
        RHV_WARN("hv front end: the int at the located address is %d, not 0 or 1; kern.hv_support is left alone", cur);
        return false;
    }
    int viaOid = -1;
    if (!oidValue(&viaOid) || viaOid != cur) {          /* before writing: the OID must show the same value as the located int */
        RHV_WARN("hv front end: kern.hv_support reads %d through its OID but the located int is %d; refusing, nothing written", viaOid, cur);
        return false;
    }
    gSupportPtr = p;
    gSupportSaved = cur;
    *p = 1;
    if (hv_get_support() != 1 || !oidValue(&viaOid) || viaOid != 1) {      /* the kernel's accessor and the OID must both see it */
        *p = cur;
        RHV_WARN("hv front end: after the write hv_get_support() gives %d and the OID %d, put back %d, refusing", hv_get_support(), viaOid, cur);
        return false;
    }
    gSupportSet = true;
    RHV_INFO("hv front end: kern.hv_support was %d, now 1 (read back through hv_get_support and the OID)", cur);
    return true;
}

void start(com_overlorddev_RyzenHV *service)
{
    gSvc = service;
    uint32_t v = 0;
    uint32_t tprArg = 1;
    if (PE_parse_boot_argn("rhv_tpr_shadow", &tprArg, sizeof(tprArg)))
        gTprShadow = tprArg ? 1 : 0;
    if (!PE_parse_boot_argn("rhv_hv", &v, sizeof(v)) || v == 0)
        return;
    if (!setMode(v))
        return;
    uint32_t sc = 0;
    if (PE_parse_boot_argn("rhv_hv_sysctl", &sc, sizeof(sc)) && sc)
        setSysctl(true);
}

void stop()
{
    setSysctl(false);
    setMode(0);
}

} // namespace hv
} // namespace rhv
