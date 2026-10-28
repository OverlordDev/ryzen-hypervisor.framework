#include <IOKit/IOLib.h>

#include "KernelDecls.hpp"
#include "MsrMap.hpp"
#include "X86.hpp"
#include "Log.hpp"
#include "SVM.hpp"
#include "Vm.hpp"

#define super OSObject

OSDefineMetaClassAndStructors(com_overlorddev_RyzenHVVm, OSObject)
OSDefineMetaClassAndStructors(com_overlorddev_RyzenHVVmShared, OSObject)

using namespace rhv;
using namespace rhv::vmcb;

/* SvmRun.S addresses these by hand. */
static_assert(offsetof(GuestGprs, rbx) == 0, "GuestGprs layout");
static_assert(offsetof(GuestGprs, rcx) == 8, "GuestGprs layout");
static_assert(offsetof(GuestGprs, rdx) == 16, "GuestGprs layout");
static_assert(offsetof(GuestGprs, rsi) == 24, "GuestGprs layout");
static_assert(offsetof(GuestGprs, rdi) == 32, "GuestGprs layout");
static_assert(offsetof(GuestGprs, rbp) == 40, "GuestGprs layout");
static_assert(offsetof(GuestGprs, r8) == 48, "GuestGprs layout");
static_assert(offsetof(GuestGprs, r15) == 104, "GuestGprs layout");
static_assert(sizeof(GuestGprs) == 112, "GuestGprs layout");

namespace {

const uint64_t kTimesliceNs = 10ull * 1000 * 1000;   /* return to userland at least every ~10 ms */

const uint64_t kGuestPat = 0x0007040600070406ull;   /* power-on default PAT */

static_assert(RHV_GUEST_RAM_SIZE % 4096 == 0, "guest RAM must be page sized");
static_assert((int)RHV_MEM_READ == (int)rhv::npt::PERM_R && (int)RHV_MEM_WRITE == (int)rhv::npt::PERM_W &&
                  (int)RHV_MEM_EXEC == (int)rhv::npt::PERM_X,
              "RHV_MEM_* must match the NPT permission bits");

/* Table pages are wired physically contiguous kernel pages. Node bookkeeping is plain kernel heap. */
bool nptAllocPage(void *, rhv::npt::Page *out)
{
    rhv::PhysMem *m = (rhv::PhysMem *)IOMalloc(sizeof(rhv::PhysMem));
    if (!m)
        return false;
    if (!rhv::physAlloc(m, 4096)) {
        IOFree(m, sizeof(rhv::PhysMem));
        return false;
    }
    out->va     = m->va;
    out->pa     = m->pa;
    out->handle = m;
    return true;
}

void nptFreePage(void *, const rhv::npt::Page *p)
{
    rhv::PhysMem *m = (rhv::PhysMem *)p->handle;
    rhv::physFree(m);
    IOFree(m, sizeof(rhv::PhysMem));
}

void *nptAllocMem(void *, size_t n)
{
    void *p = IOMalloc(n);
    if (p)
        bzero(p, n);
    return p;
}

void nptFreeMem(void *, void *p, size_t n)
{
    IOFree(p, n);
}

const rhv::npt::Allocator kNptAllocator = { nullptr, nptAllocPage, nptFreePage, nptAllocMem,
                                            nptFreeMem };

void setSeg(Seg &s, uint16_t selector, uint64_t base, uint32_t limit, uint16_t attrib)
{
    s.selector = selector;
    s.base     = base;
    s.limit    = limit;
    s.attrib   = attrib;
}

} // namespace

/* ---- the part of a VM all its vCPUs share ---- */

namespace {
IOLock *gSharedLock;                       /* guards the registry below */
com_overlorddev_RyzenHVVmShared *gShared[16];
uint64_t gNextId = 1;

IOLock *sharedLock()
{
    if (!gSharedLock) {
        IOLock *l = IOLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, &gSharedLock))
            IOLockFree(l);
    }
    return gSharedLock;
}
} // namespace

com_overlorddev_RyzenHVVmShared *com_overlorddev_RyzenHVVmShared::create(uint32_t flags, task_t owner, IOReturn *err)
{
    IOLock *reg = sharedLock();
    if (!reg) {
        *err = kIOReturnNoMemory;
        return nullptr;
    }
    com_overlorddev_RyzenHVVmShared *sh = OSTypeAlloc(com_overlorddev_RyzenHVVmShared);
    if (!sh) {
        *err = kIOReturnNoMemory;
        return nullptr;
    }
    if (!sh->init() || !sh->setup(flags, owner)) {
        sh->release();
        *err = kIOReturnNoMemory;
        return nullptr;
    }
    IOLockLock(reg);
    int slot = -1;
    for (unsigned i = 0; i < sizeof(gShared) / sizeof(gShared[0]); i++) {
        if (!gShared[i]) {
            slot = (int)i;
            break;
        }
    }
    if (slot >= 0) {
        sh->fId = gNextId++;
        gShared[slot] = sh;            /* not retained: free() takes it out again */
    }
    IOLockUnlock(reg);
    if (slot < 0) {
        RHV_WARN("VmCreate: too many VMs");
        sh->release();
        *err = kIOReturnNoResources;
        return nullptr;
    }
    *err = kIOReturnSuccess;
    return sh;
}

com_overlorddev_RyzenHVVmShared *com_overlorddev_RyzenHVVmShared::find(uint64_t id, task_t owner)
{
    IOLock *reg = sharedLock();
    if (!reg)
        return nullptr;
    com_overlorddev_RyzenHVVmShared *found = nullptr;
    IOLockLock(reg);
    for (unsigned i = 0; i < sizeof(gShared) / sizeof(gShared[0]); i++) {
        if (gShared[i] && gShared[i]->fId == id && gShared[i]->fOwner == owner) {
            found = gShared[i];
            found->retain();          /* free() takes the lock before it unlinks, so it cannot be mid-destruction here */
            break;
        }
    }
    IOLockUnlock(reg);
    return found;
}

bool com_overlorddev_RyzenHVVmShared::init()
{
    if (!OSObject::init())
        return false;
    fId = 0;
    fOwner = nullptr;
    fRw = nullptr;
    fRegLock = nullptr;
    bzero(fVcpus, sizeof(fVcpus));
    fRam = nullptr;
    fPagesMapped = 0;
    fRw = IORWLockAlloc();
    fRegLock = IOLockAlloc();
    return fRw && fRegLock;
}

void com_overlorddev_RyzenHVVmShared::free()
{
    IOLock *reg = gSharedLock;
    if (reg) {
        IOLockLock(reg);
        for (unsigned i = 0; i < sizeof(gShared) / sizeof(gShared[0]); i++)
            if (gShared[i] == this)
                gShared[i] = nullptr;
        IOLockUnlock(reg);
    }
    /* Unpins every buffer that is still mapped, then returns the tables. */
    fNpt.shutdown(&releaseOwner, this);
    if (fRam) {
        fRam->release();
        fRam = nullptr;
    }
    if (fRw) {
        IORWLockFree(fRw);
        fRw = nullptr;
    }
    if (fRegLock) {
        IOLockFree(fRegLock);
        fRegLock = nullptr;
    }
    OSObject::free();
}

bool com_overlorddev_RyzenHVVmShared::setup(uint32_t flags, task_t owner)
{
    fOwner = owner;
    if (!fNpt.init(kNptAllocator))
        return false;
    if (!(flags & RHV_VMCREATE_NO_RAM)) {
        /* Not contiguous on purpose: the NPT maps it page by page. kIOMemoryKernelUserShared lets userland map it. */
        fRam = IOBufferMemoryDescriptor::inTaskWithOptions(
            kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared, RHV_GUEST_RAM_SIZE, 4096);
        if (!fRam) {
            RHV_ERR("VM: cannot allocate %u bytes of guest RAM", RHV_GUEST_RAM_SIZE);
            return false;
        }
        bzero(fRam->getBytesNoCopy(), RHV_GUEST_RAM_SIZE);

        IOReturn rc = mapDescriptorLocked(fRam, 0, RHV_GUEST_RAM_SIZE, rhv::npt::PERM_MASK);
        if (rc != kIOReturnSuccess) {
            RHV_ERR("VM: cannot map the built-in RAM: 0x%x", rc);
            return false;
        }
    }
    return true;
}

IOReturn com_overlorddev_RyzenHVVmShared::attach(com_overlorddev_RyzenHVVm *vcpu, uint32_t *index)
{
    IOReturn rc = kIOReturnNoResources;
    IOLockLock(fRegLock);
    for (uint32_t i = 0; i < RHV_MAX_VCPUS_PER_VM; i++) {
        if (!fVcpus[i]) {
            fVcpus[i] = vcpu;
            *index = i;
            rc = kIOReturnSuccess;
            break;
        }
    }
    IOLockUnlock(fRegLock);
    return rc;
}

void com_overlorddev_RyzenHVVmShared::detach(com_overlorddev_RyzenHVVm *vcpu)
{
    IOLockLock(fRegLock);
    for (uint32_t i = 0; i < RHV_MAX_VCPUS_PER_VM; i++)
        if (fVcpus[i] == vcpu)
            fVcpus[i] = nullptr;
    IOLockUnlock(fRegLock);
}

/* Sends every vCPU that is inside the guest back to userland now, so a writer of the page tables need not wait a slice. */
void com_overlorddev_RyzenHVVmShared::kickAll()
{
    IOLockLock(fRegLock);
    for (uint32_t i = 0; i < RHV_MAX_VCPUS_PER_VM; i++)
        if (fVcpus[i])
            fVcpus[i]->kick();
    IOLockUnlock(fRegLock);
}

/* Called for every page that stops pointing into `b`. The last one unpins and frees it. */
void com_overlorddev_RyzenHVVmShared::dropPage(Backing *b)
{
    if (--b->live != 0)
        return;
    b->md->complete(kIODirectionInOut);
    b->md->release();
    IOFree(b, sizeof(*b));
}

/* Table::clear/shutdown callback: one call per page that is still mapped. */
void com_overlorddev_RyzenHVVmShared::releaseOwner(void *ctx, void *owner)
{
    com_overlorddev_RyzenHVVmShared *vm = static_cast<com_overlorddev_RyzenHVVmShared *>(ctx);
    vm->fPagesMapped--;
    RHV_STAT_ADD(vmPagesMapped, -1);
    dropPage(static_cast<Backing *>(owner));
}

/* Where byte `offset` of `ctx` (an IOMemoryDescriptor) lives in physical memory, see rhv::npt::SegmentFn. */
static bool mdSegment(void *ctx, uint64_t offset, uint64_t *pa, uint64_t *len)
{
    IOMemoryDescriptor *md = static_cast<IOMemoryDescriptor *>(ctx);
    IOByteCount seg = 0;
    addr64_t addr = md->getPhysicalSegment(offset, &seg, kIOMemoryMapperNone);
    if (!addr || !seg) {
        RHV_ERR("VM map: no physical segment at offset 0x%llx", (unsigned long long)offset);
        return false;
    }
    *pa  = addr;
    *len = seg;
    return true;
}

/*
 * Maps [0, size) of `md` at guest physical `gpa`, page by page, all or nothing.
 * Wires `md` (prepare) and holds a reference on it until the last page is unmapped.
 * Caller holds the lock or is still constructing the VM.
 */
IOReturn com_overlorddev_RyzenHVVmShared::mapDescriptorLocked(IOMemoryDescriptor *md, uint64_t gpa,
                                                        uint64_t size, uint32_t perm)
{
    for (uint64_t off = 0; off < size; off += 4096) {
        if (fNpt.isMapped(gpa + off)) {
            RHV_DBG("VM map: guest page 0x%llx is already mapped", (unsigned long long)(gpa + off));
            return kIOReturnBusy;
        }
    }

    IOReturn rc = md->prepare(kIODirectionInOut);
    if (rc != kIOReturnSuccess) {
        RHV_WARN("VM map: cannot pin the buffer: 0x%x", rc);
        return rc;
    }
    Backing *b = (Backing *)IOMalloc(sizeof(Backing));
    if (!b) {
        md->complete(kIODirectionInOut);
        return kIOReturnNoMemory;
    }
    md->retain();
    b->md   = md;
    b->live = 1;          /* the builder's own reference, dropped below */

    uint64_t done = 0;    /* bytes of the range that are mapped so far */
    rhv::npt::Status st = fNpt.mapRange(gpa, size, perm, b, &mdSegment, md, &done);
    if (st != rhv::npt::OK) {
        RHV_ERR("VM map: NPT mapRange failed (status %d) after %llu of %llu bytes at gpa 0x%llx", (int)st,
                (unsigned long long)done, (unsigned long long)size, (unsigned long long)gpa);
        switch (st) {
        case rhv::npt::NO_MEMORY:   rc = kIOReturnNoMemory; break;
        case rhv::npt::BUSY:        rc = kIOReturnBusy; break;
        case rhv::npt::BAD_SEGMENT: rc = kIOReturnVMError; break;
        default:                    rc = kIOReturnBadArgument; break;
        }
    }
    /* Count what was actually mapped. */
    fPagesMapped += done / 4096;
    RHV_STAT_ADD(vmPagesMapped, done / 4096);
    b->live += done / 4096;

    if (rc != kIOReturnSuccess) {
        /* Take back what was mapped so far. */
        for (uint64_t q = 0; q < done; q += 4096) {
            void *owner = fNpt.unmap(gpa + q);
            if (owner) {
                fPagesMapped--;
                RHV_STAT_ADD(vmPagesMapped, -1);
                dropPage(static_cast<Backing *>(owner));
            }
        }
    }
    dropPage(b);          /* frees the backing right away if nothing is mapped */
    return rc;
}

IOReturn com_overlorddev_RyzenHVVmShared::map(task_t task, uint64_t uva, uint64_t gpa, uint64_t size,
                                        uint32_t flags)
{
    uint32_t perm = flags;
    if (!rhv::npt::normalizePerm(&perm) || size == 0 || size > RHV_MAP_MAX_SIZE ||
        ((uva | gpa | size) & 0xFFF) || gpa >= rhv::npt::kGpaLimit ||
        size > rhv::npt::kGpaLimit - gpa || uva + size < uva)
        return kIOReturnBadArgument;

    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(uva, size, kIODirectionInOut, task);
    if (!md)
        return kIOReturnVMError;   /* typically an unmapped or non-writable range */

    kickAll();
    IORWLockWrite(fRw);            /* no vCPU is inside the guest while the tables change */
    IOReturn rc = mapDescriptorLocked(md, gpa, size, perm);
    IORWLockUnlock(fRw);

    md->release();                 /* the backing took its own reference */
    if (rc == kIOReturnSuccess) {
        RHV_STAT_INC(vmMaps);
        RHV_DBG("VM map: uva 0x%llx -> gpa 0x%llx, %llu KiB, perm %u", (unsigned long long)uva,
                (unsigned long long)gpa, (unsigned long long)(size / 1024), perm);
    } else {
        RHV_STAT_INC(vmMapFailures);
        RHV_WARN("VM map failed (0x%x): uva 0x%llx gpa 0x%llx size 0x%llx", rc,
                 (unsigned long long)uva, (unsigned long long)gpa, (unsigned long long)size);
    }
    return rc;
}

IOReturn com_overlorddev_RyzenHVVmShared::unmap(uint64_t gpa, uint64_t size)
{
    if (size == 0 || ((gpa | size) & 0xFFF) || gpa >= rhv::npt::kGpaLimit ||
        size > rhv::npt::kGpaLimit - gpa)
        return kIOReturnBadArgument;

    kickAll();
    IORWLockWrite(fRw);
    for (uint64_t off = 0; off < size; off += 4096) {
        void *owner = fNpt.unmap(gpa + off);
        if (owner) {
            fPagesMapped--;
            RHV_STAT_ADD(vmPagesMapped, -1);
            dropPage(static_cast<Backing *>(owner));
        }
    }
    IORWLockUnlock(fRw);   /* the TLB is flushed on every VMRUN, see run() */

    RHV_STAT_INC(vmUnmaps);
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVVmShared::protect(uint64_t gpa, uint64_t size, uint32_t flags)
{
    uint32_t perm = flags;
    if (!rhv::npt::normalizePerm(&perm) || size == 0 || ((gpa | size) & 0xFFF) ||
        gpa >= rhv::npt::kGpaLimit || size > rhv::npt::kGpaLimit - gpa)
        return kIOReturnBadArgument;

    kickAll();
    IORWLockWrite(fRw);
    IOReturn rc = kIOReturnSuccess;
    for (uint64_t off = 0; off < size; off += 4096) {
        if (!fNpt.isMapped(gpa + off)) {
            rc = kIOReturnNotFound;   /* check everything first so a failure changes nothing */
            break;
        }
    }
    if (rc == kIOReturnSuccess) {
        for (uint64_t off = 0; off < size; off += 4096)
            fNpt.protect(gpa + off, perm);
        RHV_STAT_INC(vmProtects);
    }
    IORWLockUnlock(fRw);
    return rc;
}


/* ---- one vCPU ---- */

com_overlorddev_RyzenHVVm *com_overlorddev_RyzenHVVm::create(const RHVInfo &info, uint32_t flags, task_t owner,
                                                             com_overlorddev_RyzenHVVmShared *shared, IOReturn *err)
{
    if (flags & ~(uint32_t)(RHV_VMCREATE_NO_RAM | RHV_VMCREATE_VMX)) {
        *err = kIOReturnBadArgument;
        return nullptr;
    }

    if (info.nCpusSvmActive == 0 || info.nCpusSvmActive != info.nCpusSvmUsable) {
        RHV_WARN("VmCreate: SVM is not active on all CPUs (%u of %u), boot with rhv_enable_svm=1",
                 info.nCpusSvmActive, info.nCpusSvmUsable);
        *err = kIOReturnNotReady;
        return nullptr;
    }
    if (!(info.svmFeatures & RHV_SVM_FEAT_NP) || !(info.svmFeatures & RHV_SVM_FEAT_NRIPS)) {
        RHV_ERR("VmCreate: CPU needs NPT and NRIPS (features=0x%x)", info.svmFeatures);
        *err = kIOReturnUnsupported;
        return nullptr;
    }
    if (info.numAsids < 2) {
        /* ASID 0 is the host's; the guest uses 1. */
        RHV_ERR("VmCreate: CPU offers %u ASIDs, need at least 2", info.numAsids);
        *err = kIOReturnUnsupported;
        return nullptr;
    }

    com_overlorddev_RyzenHVVmShared *sh = shared;
    if (sh)
        sh->retain();
    else
        sh = com_overlorddev_RyzenHVVmShared::create(flags, owner, err);
    if (!sh)
        return nullptr;

    com_overlorddev_RyzenHVVm *vm = OSTypeAlloc(com_overlorddev_RyzenHVVm);
    if (!vm) {
        sh->release();
        *err = kIOReturnNoMemory;
        return nullptr;
    }
    vm->fShared = sh;                  /* the vCPU owns this reference from now on */
    if (!vm->init()) {
        vm->release();
        *err = kIOReturnNoMemory;
        return nullptr;
    }
    IOReturn arc = sh->attach(vm, &vm->fIndex);
    if (arc != kIOReturnSuccess) {
        RHV_WARN("VmCreate: VM %llu has no room for another vCPU", (unsigned long long)sh->id());
        vm->release();
        *err = arc;
        return nullptr;
    }
    if (!vm->setup(flags)) {
        vm->release();
        *err = kIOReturnNoMemory;
        return nullptr;
    }

    RHV_STAT_INC(vmCreates);
    *err = kIOReturnSuccess;
    return vm;
}

bool com_overlorddev_RyzenHVVm::init()
{
    if (!super::init())
        return false;

    /* free() must be safe after any partial failure in setup(), so do not rely on zeroed allocations. */
    com_overlorddev_RyzenHVVmShared *keep = fShared;       /* set by create() before init() */
    bzero(&fVmcb, sizeof(fVmcb));
    bzero(&fHostVmcb, sizeof(fHostVmcb));
    bzero(&fIopm, sizeof(fIopm));
    bzero(&fMsrpm, sizeof(fMsrpm));
    fShared      = keep;
    fIndex       = ~0u;
    fVmx         = false;
    bzero(&fGprs, sizeof(fGprs));
    bzero(&fHostFpu, sizeof(fHostFpu));
    bzero(&fGuestFpu, sizeof(fGuestFpu));
    bzero(&fScratchFpu, sizeof(fScratchFpu));
    fGuestXcr0     = rhv::fpu::XCR0_X87;
    fXcr0Supported = 0;
    fFpuSize       = 0;
    fKick          = 0;
    fHasTscAux     = false;
    fRunCpu        = -1;
    fFirstRun    = true;
    fHaveNextRip = false;

    fLock = IOLockAlloc();
    return fLock != nullptr;
}

void com_overlorddev_RyzenHVVm::free()
{
    if (fShared) {
        if (fIndex != ~0u)
            fShared->detach(this);
    }
    physFree(&fScratchFpu);
    physFree(&fGuestFpu);
    physFree(&fHostFpu);
    physFree(&fMsrpm);
    physFree(&fIopm);
    physFree(&fHostVmcb);
    physFree(&fVmcb);
    if (fLock) {
        IOLockFree(fLock);
        fLock = nullptr;
    }
    if (fShared) {                    /* last: the page tables go when the last vCPU is gone */
        fShared->release();
        fShared = nullptr;
    }
    super::free();
}

bool com_overlorddev_RyzenHVVm::setup(uint32_t flags)
{
    if (!physAlloc(&fVmcb, 4096) || !physAlloc(&fHostVmcb, 4096) || !physAlloc(&fIopm, 12288) ||
        !physAlloc(&fMsrpm, 8192))
        return false;

    {
        uint32_t r[4];
        rhv_cpuid(0x80000001u, 0, r);
        fHasTscAux = (r[3] >> 27) & 1;           /* RDTSCP */
    }

    /* Extended state areas. The size is what XSAVE needs for the XCR0 this thread has now. */
    {
        uint32_t r[4];
        rhv_cpuid(0xD, 0, r);
        fXcr0Supported = ((uint64_t)r[3] << 32) | r[0];
        fFpuSize       = r[1];
        if (fFpuSize < rhv::fpu::kHeaderEnd || fFpuSize > RHV_FPU_AREA_MAX) {
            RHV_ERR("VM: XSAVE area of %u bytes is not supported (limit %u)", fFpuSize, RHV_FPU_AREA_MAX);
            return false;
        }
        if (!physAlloc(&fHostFpu, 4096) || !physAlloc(&fGuestFpu, 4096) || !physAlloc(&fScratchFpu, 4096))
            return false;
        rhv::fpu::initArea((uint8_t *)fGuestFpu.va, 4096);
        fGuestXcr0 = rhv::fpu::XCR0_X87;
    }

    fVmx = (flags & RHV_VMCREATE_VMX) != 0;
    fVmcs.attach((rhv::vmcb::Vmcb *)fVmcb.va);

    /* Intercept every I/O port and every MSR: userland decides what the guest may touch. */
    memset(fIopm.va, 0xFF, fIopm.size);
    memset(fMsrpm.va, 0xFF, fMsrpm.size);

    fFirstRun = true;
    resetLocked(0, 0);
    RHV_INFO("vCPU %u of VM %llu created: %u KiB built-in RAM, vmcb pa 0x%llx, npt root pa 0x%llx", fIndex,
             (unsigned long long)fShared->id(), ramSize() / 1024, (unsigned long long)fVmcb.pa,
             (unsigned long long)fShared->npt().rootPa());
    return true;
}

IOReturn com_overlorddev_RyzenHVVm::map(task_t task, uint64_t uva, uint64_t gpa, uint64_t size, uint32_t flags)
{
    return fShared->map(task, uva, gpa, size, flags);
}

IOReturn com_overlorddev_RyzenHVVm::unmap(uint64_t gpa, uint64_t size)
{
    return fShared->unmap(gpa, size);
}

IOReturn com_overlorddev_RyzenHVVm::protect(uint64_t gpa, uint64_t size, uint32_t flags)
{
    return fShared->protect(gpa, size, flags);
}

/* Real-mode reset state. Caller holds the lock (or is still constructing). */
void com_overlorddev_RyzenHVVm::resetLocked(uint16_t cs, uint16_t ip)
{
    Vmcb *v = (Vmcb *)fVmcb.va;
    bzero(v, sizeof(*v));
    bzero(&fGprs, sizeof(fGprs));

    Control &c = v->control;
    c.interceptDr   = 0xFFFFFFFFu;   /* DR0-3 are not swapped by VMRUN, never let the guest touch them */
    c.interceptVec3 = V3_INTR | V3_NMI | V3_SMI | V3_INIT | V3_CPUID | V3_INVD | V3_HLT | V3_IOIO |
                      V3_MSR | V3_SHUTDOWN;
    c.interceptVec4 = V4_VMRUN | V4_VMMCALL | V4_VMLOAD | V4_VMSAVE | V4_STGI | V4_CLGI |
                      V4_SKINIT | V4_WBINVD | V4_MONITOR | V4_MWAIT;
    c.iopmBasePa    = fIopm.pa;
    c.msrpmBasePa   = fMsrpm.pa;
    c.asid          = 1;                       /* 0 is the host */
    c.tlbCtl        = TLB_FLUSH_ALL;
    c.intCtl        = INT_CTL_V_INTR_MASKING;  /* guest EFLAGS.IF does not gate host interrupts */
    c.nestedCtl     = NESTED_CTL_NP_ENABLE;
    c.nestedCr3     = fShared->npt().rootPa();

    Save &s = v->save;
    setSeg(s.es, 0, 0, 0xFFFF, ATTR_REAL_DATA);
    setSeg(s.ss, 0, 0, 0xFFFF, ATTR_REAL_DATA);
    setSeg(s.ds, 0, 0, 0xFFFF, ATTR_REAL_DATA);
    setSeg(s.fs, 0, 0, 0xFFFF, ATTR_REAL_DATA);
    setSeg(s.gs, 0, 0, 0xFFFF, ATTR_REAL_DATA);
    setSeg(s.cs, cs, (uint64_t)cs << 4, 0xFFFF, ATTR_REAL_CODE);
    setSeg(s.gdtr, 0, 0, 0xFFFF, 0);
    setSeg(s.idtr, 0, 0, 0xFFFF, 0);
    setSeg(s.ldtr, 0, 0, 0xFFFF, ATTR_LDT);
    setSeg(s.tr, 0, 0, 0xFFFF, ATTR_TSS_BUSY);

    s.cpl    = 0;
    s.efer   = EFER_SVME;      /* mandatory */
    s.cr0    = 0x10;           /* ET, real mode, caches on */
    s.cr3    = 0;
    s.cr4    = 0;
    s.dr6    = 0xFFFF0FF0ull;
    s.dr7    = 0x400;
    s.rflags = 0x2;
    s.rip    = ip;
    s.rsp    = 0;
    s.rax    = 0;
    s.gPat   = kGuestPat;

    fHaveNextRip = false;
    fVmcs.reset();
    if (fGuestFpu.va) {
        rhv::fpu::initArea((uint8_t *)fGuestFpu.va, 4096);
        fGuestXcr0 = rhv::fpu::XCR0_X87;
    }
}

IOReturn com_overlorddev_RyzenHVVm::reset(uint16_t cs, uint16_t ip)
{
    IOLockLock(fLock);
    resetLocked(cs, ip);
    IOLockUnlock(fLock);
    RHV_DBG("VM reset: cs=0x%x ip=0x%x", cs, ip);
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVVm::getRegs(RHVRegs *out)
{
    IOLockLock(fLock);
    const Vmcb *v = (const Vmcb *)fVmcb.va;
    out->rax = v->save.rax;
    out->rbx = fGprs.rbx;
    out->rcx = fGprs.rcx;
    out->rdx = fGprs.rdx;
    out->rsi = fGprs.rsi;
    out->rdi = fGprs.rdi;
    out->rbp = fGprs.rbp;
    out->rsp = v->save.rsp;
    out->r8  = fGprs.r8;
    out->r9  = fGprs.r9;
    out->r10 = fGprs.r10;
    out->r11 = fGprs.r11;
    out->r12 = fGprs.r12;
    out->r13 = fGprs.r13;
    out->r14 = fGprs.r14;
    out->r15 = fGprs.r15;
    out->rip    = v->save.rip;
    out->rflags = v->save.rflags;
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVVm::setRegs(const RHVRegs *in)
{
    IOLockLock(fLock);
    Vmcb *v = (Vmcb *)fVmcb.va;
    v->save.rax = in->rax;
    fGprs.rbx = in->rbx;
    fGprs.rcx = in->rcx;
    fGprs.rdx = in->rdx;
    fGprs.rsi = in->rsi;
    fGprs.rdi = in->rdi;
    fGprs.rbp = in->rbp;
    v->save.rsp = in->rsp;
    fGprs.r8  = in->r8;
    fGprs.r9  = in->r9;
    fGprs.r10 = in->r10;
    fGprs.r11 = in->r11;
    fGprs.r12 = in->r12;
    fGprs.r13 = in->r13;
    fGprs.r14 = in->r14;
    fGprs.r15 = in->r15;
    /*
     * Writing back the registers you just read (the usual "answer CPUID" dance)
     * must not cancel a pending skip. Only a changed RIP means userland moved
     * the guest itself.
     */
    if (v->save.rip != in->rip)
        fHaveNextRip = false;
    v->save.rip    = in->rip;
    v->save.rflags = in->rflags | 0x2;   /* bit 1 is always set */
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

static uint32_t classify(uint64_t code)
{
    switch (code) {
    case EXIT_HLT:      return RHV_EXIT_HLT;
    case EXIT_IOIO:     return RHV_EXIT_IO;
    case EXIT_CPUID:    return RHV_EXIT_CPUID;
    case EXIT_MSR:      return RHV_EXIT_MSR;
    case EXIT_NPF:      return RHV_EXIT_NPF;
    case EXIT_SHUTDOWN: return RHV_EXIT_SHUTDOWN;
    case EXIT_VMMCALL:  return RHV_EXIT_VMMCALL;
    case EXIT_INVALID:  return RHV_EXIT_INVALID;
    default:            return RHV_EXIT_UNHANDLED;
    }
}

/*
 * At most one kick IPI per CPU is in flight: mp_cpus_call draws from a small pool in the kernel, and the handler may run
 * after the VM that asked for it is gone, so it only touches this array, never the VM.
 */
static volatile uint8_t gKickInFlight[64];

static void kickAction(void *)
{
    int cpu = cpu_number();
    if (cpu >= 0 && cpu < 64)
        gKickInFlight[cpu] = 0;
}

void com_overlorddev_RyzenHVVm::kick()
{
    fKick = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    int cpu = fRunCpu;
    if (cpu >= 0 && cpu < 64 && __sync_bool_compare_and_swap(&gKickInFlight[cpu], 0, 1))
        mp_cpus_call(1ull << cpu, 2 /* NOSYNC */, kickAction, nullptr);
}

IOReturn com_overlorddev_RyzenHVVm::run(bool skipInstruction, RHVExit *out)
{
    IOReturn rc = kIOReturnSuccess;
    Vmcb *v = (Vmcb *)fVmcb.va;

    IOLockLock(fLock);
    RHV_STAT_INC(vmRuns);

    if (skipInstruction) {
        if (fHaveNextRip && v->control.nextRip)
            v->save.rip = v->control.nextRip;
        else
            RHV_WARN("VmRun: skip requested but the last exit has no next_rip, RIP unchanged");
    }
    fHaveNextRip = false;

    if (fFirstRun) {
        fFirstRun = false;
        /* A wrong VMCB here shows up as VMEXIT_INVALID, anything worse as a panic: leave a trail. */
        RHV_SYNC("vCPU %u: first VMRUN, rip=0x%llx cs.base=0x%llx efer=0x%llx cr0=0x%llx nCr3=0x%llx", fIndex,
                 (unsigned long long)v->save.rip, (unsigned long long)v->save.cs.base,
                 (unsigned long long)v->save.efer, (unsigned long long)v->save.cr0,
                 (unsigned long long)v->control.nestedCr3);
    }

    if (fVmx)
        fVmcs.applyControls();      /* controls and event injection the client set through the VMCS */

    uint64_t sliceAbs = 0;
    nanoseconds_to_absolutetime(kTimesliceNs, &sliceAbs);
    uint64_t deadline = mach_absolute_time() + sliceAbs;
    uint64_t code = 0;
    bool timeslice = false;
    uint32_t syntheticReason = 0;       /* VMX exit reason to report instead of an external interrupt (TPR below threshold) */
    /*
     * An event waiting to go in (the client's injection, or one whose delivery a host interrupt cut short) when we
     * leave without having run the guest long enough to deliver it. It goes back to the client as IDT-vectoring
     * information; dropping it loses the interrupt (the client believes it was delivered).
     */
    uint32_t carryInfo = 0, carryErr = 0;
    auto takePendingEvent = [&]() {
        if (v->control.eventInj & (1u << 31)) {
            carryInfo = v->control.eventInj;
            carryErr  = v->control.eventInjErr;
            v->control.eventInj    = 0;
            v->control.eventInjErr = 0;
        }
    };

    /*
     * An event the client injected must be delivered before the run is cut short, as on Intel: the entry delivers it,
     * and only then can an external-interrupt exit (hv_vcpu_interrupt, the time slice) happen. A client may not look at
     * IDT-vectoring information on such an exit (Parallels does not) and would take the event for delivered, so
     * handing it back is the last resort. `deferrals` bounds how often the exit is put off for it.
     */
    unsigned deferrals = 0;
    auto injectionWaiting = [&]() { return (v->control.eventInj & (1u << 31)) != 0 && deferrals < 16; };

    for (;;) {
        if (fKick) {                  /* hv_vcpu_interrupt from another thread */
            if (injectionWaiting()) {
                deferrals++;          /* enter first; the flag stays set and ends the run right after the delivery */
            } else {
                fKick = 0;
                timeslice = true;
                takePendingEvent();
                break;
            }
        }
        /*
         * The page tables must not change while this vCPU is inside the guest: hold them for reading from here until the
         * exit has been looked at (a VmMap/VmUnmap/VmProtect on another thread waits for this, after kicking us out).
         */
        struct NptReadGuard {
            IORWLock *l;
            explicit NptReadGuard(IORWLock *x) : l(x) { IORWLockRead(l); }
            ~NptReadGuard() { IORWLockUnlock(l); }
        } nptGuard(fShared->rw());
        if (fKick && !injectionWaiting()) {     /* a kick that came in while we waited for the page tables */
            fKick = 0;
            timeslice = true;
            takePendingEvent();
            break;
        }

        v->control.exitCode = 0;
        v->control.tlbCtl   = TLB_FLUSH_ALL;
        v->control.clean    = 0;      /* every field is dirty, we never use clean bits yet */

        uint64_t hostCr2;
        __asm__ volatile("movq %%cr2, %0" : "=r"(hostCr2));   /* guest page faults can overwrite it */

        /* Interrupts off from here until the exit has been looked at. See activeOnAllUsable(). */
        int ints = ml_set_interrupts_enabled(0);
        fRunCpu = cpu_number();               /* interrupts are off: the thread cannot move before VMRUN */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (fKick && !injectionWaiting()) {   /* a kick that arrived before fRunCpu was visible */
            fRunCpu = -1;
            ml_set_interrupts_enabled(ints);
            continue;
        }
        if (!svm::activeOnAllUsable()) {
            fRunCpu = -1;
            ml_set_interrupts_enabled(ints);
            RHV_ERR("VmRun: SVM was turned off under us (sleep?), not entering the guest");
            rc = kIOReturnNotReady;
            break;
        }

        /*
         * VMRUN does not touch x87/SSE/AVX state or XCR0. Swap them by hand, still with interrupts off, so
         * the calling process gets its registers back exactly as it left them.
         */
        /*
         * XSAVE only manages the components XCR0 enables, and an OS that has not run XSETBV yet (XCR0 = 1) can
         * still use SSE. So while the guest runs, the hardware XCR0 always includes x87 and SSE; the value the
         * client sees (and a later XSETBV sets) is fGuestXcr0. The only visible difference is that XGETBV in
         * such a guest reads 3 instead of 1.
         */
        const uint64_t guestX = fGuestXcr0 | rhv::fpu::XCR0_X87 | rhv::fpu::XCR0_SSE;
        const bool wasTs = rhv::fpu::clearTs();
        const uint64_t hostX = rhv::fpu::xgetbv0();
        if (guestX & ~hostX) {
            fRunCpu = -1;
            rhv::fpu::restoreTs(wasTs);
            ml_set_interrupts_enabled(ints);
            RHV_ERR("VmRun: guest XCR0 0x%llx needs state this thread has not enabled (0x%llx)",
                    (unsigned long long)guestX, (unsigned long long)hostX);
            rc = kIOReturnUnsupported;
            break;
        }
        rhv::fpu::xsave(fHostFpu.va, hostX);
        if (guestX != hostX)
            rhv::fpu::xsetbv0(guestX);
        rhv::fpu::xrstor(fGuestFpu.va, guestX);

        /*
         * Debug registers: VMRUN swaps the guest's DR6/DR7 through the VMCB but neither DR0-DR3 nor the host's own
         * values (a debugger may have set breakpoints on the calling process). Swap them by hand in VMX mode, where
         * the client lets the guest use them; in the raw modes every DR access is intercepted.
         */
        uint64_t hostDr[6] = { 0, 0, 0, 0, 0, 0 };
        if (fVmx) {
            hostDr[0] = rhv_read_dr(0); hostDr[1] = rhv_read_dr(1); hostDr[2] = rhv_read_dr(2);
            hostDr[3] = rhv_read_dr(3); hostDr[4] = rhv_read_dr(6); hostDr[5] = rhv_read_dr(7);
            rhv_write_dr(7, 0);                       /* no breakpoint can fire while the registers change */
            for (unsigned i = 0; i < 4; i++)
                rhv_write_dr(i, fVmcs.dr(i));
        }

        /* RDTSCP reads IA32_TSC_AUX, which VMRUN does not switch: put the guest's value in for the run. */
        uint64_t hostTscAux = 0;
        if (fVmx && fHasTscAux) {
            hostTscAux = rhv_rdmsr(rhv::msr::MSR_TSC_AUX);
            rhv_wrmsr(rhv::msr::MSR_TSC_AUX, fVmcs.tscAux());
        }

        rhv_svm_vmrun(&fGprs, fVmcb.pa, fHostVmcb.pa);
        fRunCpu = -1;

        if (fVmx && fHasTscAux) {
            fVmcs.setTscAux(rhv_rdmsr(rhv::msr::MSR_TSC_AUX));
            rhv_wrmsr(rhv::msr::MSR_TSC_AUX, hostTscAux);
        }

        if (fVmx) {
            for (unsigned i = 0; i < 4; i++)
                fVmcs.setDr(i, rhv_read_dr(i));
            rhv_write_dr(7, 0);
            rhv_write_dr(0, hostDr[0]); rhv_write_dr(1, hostDr[1]); rhv_write_dr(2, hostDr[2]);
            rhv_write_dr(3, hostDr[3]); rhv_write_dr(6, hostDr[4]); rhv_write_dr(7, hostDr[5]);
        }

        rhv::fpu::xsave(fGuestFpu.va, guestX);
        if (guestX != hostX)
            rhv::fpu::xsetbv0(hostX);
        rhv::fpu::xrstor(fHostFpu.va, hostX);
        /* Self-check: the host state must be bit for bit what it was (this code uses no SSE of its own). */
        rhv::fpu::xsave(fScratchFpu.va, hostX);
        if (!rhv::fpu::legacyEqual((const uint8_t *)fHostFpu.va, (const uint8_t *)fScratchFpu.va))
            RHV_STAT_INC(vmFpuMismatches);
        rhv::fpu::restoreTs(wasTs);

        __asm__ volatile("movq %0, %%cr2" : : "r"(hostCr2));
        ml_set_interrupts_enabled(ints);   /* a pending host interrupt is taken right here */

        code = v->control.exitCode;
        RHV_STAT_INC(vmExits);
        /* The injection was either delivered or is reported in exitIntInfo; do not let it fire a second time. */
        v->control.eventInj    = 0;
        v->control.eventInjErr = 0;

        /*
         * The guest executed an SVM instruction (VMRUN, VMLOAD, VMSAVE, STGI, CLGI, SKINIT). A Hypervisor.framework
         * client's guest has no such instructions, so what it would see on Intel with VMX off is #UD: inject that
         * and let the guest handle it.
         */
        if (fVmx && (code == 0x80 || (code >= 0x82 && code <= 0x86))) {
            v->control.eventInj = (1u << 31) | (3u << 8) | 6;
            continue;
        }

        if (code == EXIT_INTR || code == EXIT_NMI) {
            /* An event that was being delivered when the host interrupt came in must be injected again. */
            if (v->control.exitIntInfo & (1u << 31)) {
                v->control.eventInj    = v->control.exitIntInfo;
                v->control.eventInjErr = v->control.exitIntInfoErr;
            }
            /* The host needed the CPU. It has it now; run on unless the slice is over. */
            if (mach_absolute_time() >= deadline && !injectionWaiting()) {
                timeslice = true;
                takePendingEvent();
                break;
            }
            continue;
        }

        /*
         * TPR shadow, see VmcsState::applyControls: a MOV to CR8 is intercepted only while the client waits for the TPR
         * to drop below its threshold. Handle it here, without a trip to userland: set V_TPR, step over the instruction,
         * and report "TPR below threshold" only when the new value really is below the threshold.
         */
        if (fVmx && code == 0x18 && fVmcs.tprShadowActive() && (v->control.exitInfo1 >> 63) && v->control.nextRip) {
            unsigned gpr = (unsigned)(v->control.exitInfo1 & 0xF);
            uint64_t val;
            switch (gpr) {
            case 0:  val = v->save.rax; break;
            case 1:  val = fGprs.rcx; break;
            case 2:  val = fGprs.rdx; break;
            case 3:  val = fGprs.rbx; break;
            case 4:  val = v->save.rsp; break;
            case 5:  val = fGprs.rbp; break;
            case 6:  val = fGprs.rsi; break;
            case 7:  val = fGprs.rdi; break;
            case 8:  val = fGprs.r8; break;
            case 9:  val = fGprs.r9; break;
            case 10: val = fGprs.r10; break;
            case 11: val = fGprs.r11; break;
            case 12: val = fGprs.r12; break;
            case 13: val = fGprs.r13; break;
            case 14: val = fGprs.r14; break;
            default: val = fGprs.r15; break;
            }
            if (val <= 0xF) {                               /* anything else is a #GP the client emulates */
                v->control.intCtl = (v->control.intCtl & ~0xFFu) | (uint32_t)val;
                v->save.rip       = v->control.nextRip;
                if (v->control.exitIntInfo & (1u << 31)) {  /* an event that was being delivered goes in again */
                    v->control.eventInj    = v->control.exitIntInfo;
                    v->control.eventInjErr = v->control.exitIntInfoErr;
                }
                if (val < fVmcs.tprThreshold()) {
                    timeslice = true;
                    syntheticReason = rhv::vmx::VMX_TPR_BELOW_THRESHOLD;
                    takePendingEvent();
                    break;
                }
                if (mach_absolute_time() >= deadline && !injectionWaiting()) {
                    timeslice = true;
                    takePendingEvent();
                    break;
                }
                continue;
            }
        }
        break;
    }

    if (rc == kIOReturnSuccess) {
        bzero(out, sizeof(*out));
        if (timeslice) {
            out->reason = RHV_EXIT_TIMESLICE;
            if (fVmx)
                fVmcs.recordSynthetic(syntheticReason ? syntheticReason : rhv::vmx::VMX_EXT_INTR, carryInfo, carryErr);
        } else {
            out->reason   = classify(code);
            if (out->reason == RHV_EXIT_NPF)
                RHV_STAT_INC(vmNptFaults);
            out->exitCode = code;
            out->info1    = v->control.exitInfo1;
            out->info2    = v->control.exitInfo2;
            out->nextRip  = v->control.nextRip;
            fHaveNextRip  = v->control.nextRip != 0;
            if (fVmx) {
                uint32_t perm = 0;
                if (code == EXIT_NPF) {
                    IORWLockRead(fShared->rw());
                    fShared->npt().lookup(v->control.exitInfo2 & ~(uint64_t)0xFFF, nullptr, &perm);
                    IORWLockUnlock(fShared->rw());
                }
                if (!fVmcs.recordExit(perm))
                    out->reason = RHV_EXIT_UNHANDLED;      /* e.g. the guest executed an SVM instruction */
            }
            RHV_TRACE("VM exit: code=0x%llx info1=0x%llx info2=0x%llx rip=0x%llx",
                      (unsigned long long)code, (unsigned long long)out->info1,
                      (unsigned long long)out->info2, (unsigned long long)v->save.rip);
        }
        out->rip = v->save.rip;

        if (out->reason == RHV_EXIT_INVALID) {
            RHV_STAT_INC(vmInvalidExits);
            dumpVmcb("VMRUN rejected the VMCB (VMEXIT_INVALID)");
        } else if (out->reason == RHV_EXIT_UNHANDLED) {
            RHV_WARN("VM exit with unhandled code 0x%llx at rip 0x%llx", (unsigned long long)code,
                     (unsigned long long)v->save.rip);
        }
    }

    IOLockUnlock(fLock);
    return rc;
}

IOReturn com_overlorddev_RyzenHVVm::msrOp(uint32_t op, uint32_t msrNum, uint64_t value, uint64_t *out)
{
    using namespace rhv::msr;
    if (!fVmx)
        return kIOReturnUnsupported;
    Info mi = info(msrNum);
    if (mi.kind == K_UNKNOWN)
        return kIOReturnBadArgument;

    IOReturn rc = kIOReturnSuccess;
    IOLockLock(fLock);
    switch (op) {
    case RHV_MSR_READ: {
        uint64_t tsc = 0;
        if (mi.kind == K_TSC) {
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            tsc = ((uint64_t)hi << 32) | lo;
        }
        *out = 0;
        if (fVmcs.readMsr(msrNum, tsc, out) != rhv::vmx::R_OK)
            rc = kIOReturnBadArgument;
        break;
    }
    case RHV_MSR_WRITE: {
        uint64_t tsc = 0;
        if (mi.kind == K_TSC) {
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            tsc = ((uint64_t)hi << 32) | lo;
        }
        if (fVmcs.writeMsr(msrNum, tsc, value) != rhv::vmx::R_OK)
            rc = kIOReturnBadArgument;
        break;
    }
    case RHV_MSR_NATIVE_ENABLE:
    case RHV_MSR_NATIVE_DISABLE:
        /* The guest EFER must keep SVME set, so it can never be written without an exit. */
        if (mi.kind == K_EFER || !setNative((uint8_t *)fMsrpm.va, msrNum, op == RHV_MSR_NATIVE_ENABLE))
            rc = kIOReturnBadArgument;
        break;
    default:
        rc = kIOReturnBadArgument;
    }
    IOLockUnlock(fLock);
    return rc;
}

IOReturn com_overlorddev_RyzenHVVm::getFpu(RHVFpuState *out)
{
    bzero(out, sizeof(*out));
    IOLockLock(fLock);
    out->xcr0 = fGuestXcr0;
    out->size = fFpuSize;
    bcopy(fGuestFpu.va, out->area, fFpuSize);
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVVm::setFpu(const RHVFpuState *in)
{
    /* Everything XRSTOR or XSETBV would fault on is refused here: a fault in the kernel is a panic. */
    const uint64_t hostX = rhv::fpu::xgetbv0();
    if (in->size != fFpuSize || !rhv::fpu::validXcr0(in->xcr0, fXcr0Supported) || (in->xcr0 & ~hostX) ||
        !rhv::fpu::validArea(in->area, fFpuSize, in->xcr0 | rhv::fpu::XCR0_X87 | rhv::fpu::XCR0_SSE))
        return kIOReturnBadArgument;
    IOLockLock(fLock);
    fGuestXcr0 = in->xcr0;
    bcopy(in->area, fGuestFpu.va, fFpuSize);
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVVm::vmcsRead(uint32_t field, uint64_t *value)
{
    if (!fVmx)
        return kIOReturnUnsupported;
    IOLockLock(fLock);
    rhv::vmx::Result r = fVmcs.read(field, value);
    IOLockUnlock(fLock);
    return r == rhv::vmx::R_OK ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn com_overlorddev_RyzenHVVm::vmcsReadMany(const RHVReadMany *in, RHVReadManyOut *out)
{
    bzero(out, sizeof(*out));
    if (in->count > RHV_READMANY_MAX)
        return kIOReturnBadArgument;
    for (uint32_t i = 0; i < in->count; i++) {
        IOReturn rc = vmcsRead(in->fields[i], &out->values[i]);
        if (rc != kIOReturnSuccess)
            return rc;
    }
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVVm::runEx(const RHVRunIn *in, RHVRunOut *out)
{
    bzero(out, sizeof(*out));
    if (in->nWrites > RHV_RUN_MAX_WRITES)
        return kIOReturnBadArgument;

    IOReturn rc;
    if (in->flags & RHV_RUN_REGS) {
        rc = setRegs(&in->regs);
        if (rc != kIOReturnSuccess)
            return rc;
    }
    for (uint32_t i = 0; i < in->nWrites; i++) {
        rc = vmcsWrite(in->writes[i].field, in->writes[i].value);
        if (rc != kIOReturnSuccess) {
            out->failedWrite = i;
            return rc;
        }
    }
    if (!(in->flags & RHV_RUN_NO_RUN)) {
        rc = run((in->flags & RHV_RUN_SKIP) != 0, &out->exit);
        if (rc != kIOReturnSuccess)
            return rc;
    }

    getRegs(&out->regs);
    unsigned n = 0;
    IOLockLock(fLock);                                    /* one lock for all the fields, not one per field */
#define RHV_X(enc) do { uint64_t v_ = 0; if (fVmx) (void)fVmcs.read(enc, &v_); out->fields[n++] = v_; } while (0);
    RHV_RUN_FIELDS_LIST(RHV_X)
#undef RHV_X
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVVm::vmcsWrite(uint32_t field, uint64_t value)
{
    if (!fVmx)
        return kIOReturnUnsupported;
    /* MOV to DR0-DR3 faults on a non-canonical address, and the kext loads these in kernel mode. */
    if (field >= rhv::vmx::X_DR0 && field <= rhv::vmx::X_DR3 && (uint64_t)(((int64_t)(value << 16)) >> 16) != value)
        return kIOReturnBadArgument;
    IOLockLock(fLock);
    rhv::vmx::Result r = fVmcs.write(field, value);
    if (r == rhv::vmx::R_OK && field == rhv::vmx::F_GUEST_RIP)
        fHaveNextRip = false;           /* the client moved RIP itself */
    IOLockUnlock(fLock);
    switch (r) {
    case rhv::vmx::R_OK:         return kIOReturnSuccess;
    case rhv::vmx::R_READ_ONLY:  return kIOReturnNotWritable;
    default:                     return kIOReturnBadArgument;
    }
}

void com_overlorddev_RyzenHVVm::dumpVmcb(const char *why)
{
    const Vmcb *v = (const Vmcb *)fVmcb.va;
    RHV_ERR("VMCB dump: %s", why);
    RHV_ERR("  efer=0x%llx cr0=0x%llx cr3=0x%llx cr4=0x%llx rflags=0x%llx rip=0x%llx",
            (unsigned long long)v->save.efer, (unsigned long long)v->save.cr0,
            (unsigned long long)v->save.cr3, (unsigned long long)v->save.cr4,
            (unsigned long long)v->save.rflags, (unsigned long long)v->save.rip);
    RHV_ERR("  cs sel=0x%x base=0x%llx lim=0x%x attr=0x%x  dr6=0x%llx dr7=0x%llx",
            v->save.cs.selector, (unsigned long long)v->save.cs.base, v->save.cs.limit,
            v->save.cs.attrib, (unsigned long long)v->save.dr6, (unsigned long long)v->save.dr7);
    RHV_ERR("  asid=%u nested_ctl=0x%llx n_cr3=0x%llx v3=0x%x v4=0x%x int_ctl=0x%x",
            v->control.asid, (unsigned long long)v->control.nestedCtl,
            (unsigned long long)v->control.nestedCr3, v->control.interceptVec3,
            v->control.interceptVec4, v->control.intCtl);
    RHV_ERR("  iopm=0x%llx msrpm=0x%llx exit=0x%llx info1=0x%llx info2=0x%llx",
            (unsigned long long)v->control.iopmBasePa, (unsigned long long)v->control.msrpmBasePa,
            (unsigned long long)v->control.exitCode, (unsigned long long)v->control.exitInfo1,
            (unsigned long long)v->control.exitInfo2);
}
