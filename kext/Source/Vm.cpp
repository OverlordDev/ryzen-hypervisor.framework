#include <IOKit/IOLib.h>

#include "KernelDecls.hpp"
#include "Log.hpp"
#include "SVM.hpp"
#include "Vm.hpp"

#define super OSObject

OSDefineMetaClassAndStructors(com_overlorddev_RyzenHVVm, OSObject)

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

com_overlorddev_RyzenHVVm *com_overlorddev_RyzenHVVm::create(const RHVInfo &info, uint32_t flags,
                                                             IOReturn *err)
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

    com_overlorddev_RyzenHVVm *vm = OSTypeAlloc(com_overlorddev_RyzenHVVm);
    if (!vm) {
        *err = kIOReturnNoMemory;
        return nullptr;
    }
    if (!vm->init() || !vm->setup(flags)) {
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
    fRam = nullptr;
    bzero(&fVmcb, sizeof(fVmcb));
    bzero(&fHostVmcb, sizeof(fHostVmcb));
    bzero(&fIopm, sizeof(fIopm));
    bzero(&fMsrpm, sizeof(fMsrpm));
    fPagesMapped = 0;
    fVmx         = false;
    bzero(&fGprs, sizeof(fGprs));
    fFirstRun    = true;
    fHaveNextRip = false;

    fLock = IOLockAlloc();
    return fLock != nullptr;
}

void com_overlorddev_RyzenHVVm::free()
{
    /* Unpins every buffer that is still mapped, then returns the tables. */
    fNpt.shutdown(&releaseOwner, this);
    physFree(&fMsrpm);
    physFree(&fIopm);
    physFree(&fHostVmcb);
    physFree(&fVmcb);
    if (fRam) {
        fRam->release();
        fRam = nullptr;
    }
    if (fLock) {
        IOLockFree(fLock);
        fLock = nullptr;
    }
    super::free();
}

bool com_overlorddev_RyzenHVVm::setup(uint32_t flags)
{
    if (!physAlloc(&fVmcb, 4096) || !physAlloc(&fHostVmcb, 4096) || !physAlloc(&fIopm, 12288) ||
        !physAlloc(&fMsrpm, 8192) || !fNpt.init(kNptAllocator))
        return false;

    fVmx = (flags & RHV_VMCREATE_VMX) != 0;
    fVmcs.attach((rhv::vmcb::Vmcb *)fVmcb.va);

    /* Intercept every I/O port and every MSR: userland decides what the guest may touch. */
    memset(fIopm.va, 0xFF, fIopm.size);
    memset(fMsrpm.va, 0xFF, fMsrpm.size);

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

    fFirstRun = true;
    resetLocked(0, 0);
    RHV_INFO("VM created: %u KiB built-in RAM, vmcb pa 0x%llx, npt root pa 0x%llx",
             ramSize() / 1024, (unsigned long long)fVmcb.pa, (unsigned long long)fNpt.rootPa());
    return true;
}

/* Called for every page that stops pointing into `b`. The last one unpins and frees it. */
void com_overlorddev_RyzenHVVm::dropPage(Backing *b)
{
    if (--b->live != 0)
        return;
    b->md->complete(kIODirectionInOut);
    b->md->release();
    IOFree(b, sizeof(*b));
}

/* Table::clear/shutdown callback: one call per page that is still mapped. */
void com_overlorddev_RyzenHVVm::releaseOwner(void *ctx, void *owner)
{
    com_overlorddev_RyzenHVVm *vm = static_cast<com_overlorddev_RyzenHVVm *>(ctx);
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
IOReturn com_overlorddev_RyzenHVVm::mapDescriptorLocked(IOMemoryDescriptor *md, uint64_t gpa,
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

IOReturn com_overlorddev_RyzenHVVm::map(task_t task, uint64_t uva, uint64_t gpa, uint64_t size,
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

    IOLockLock(fLock);
    IOReturn rc = mapDescriptorLocked(md, gpa, size, perm);
    IOLockUnlock(fLock);

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

IOReturn com_overlorddev_RyzenHVVm::unmap(uint64_t gpa, uint64_t size)
{
    if (size == 0 || ((gpa | size) & 0xFFF) || gpa >= rhv::npt::kGpaLimit ||
        size > rhv::npt::kGpaLimit - gpa)
        return kIOReturnBadArgument;

    IOLockLock(fLock);
    for (uint64_t off = 0; off < size; off += 4096) {
        void *owner = fNpt.unmap(gpa + off);
        if (owner) {
            fPagesMapped--;
            RHV_STAT_ADD(vmPagesMapped, -1);
            dropPage(static_cast<Backing *>(owner));
        }
    }
    IOLockUnlock(fLock);   /* the TLB is flushed on every VMRUN, see run() */

    RHV_STAT_INC(vmUnmaps);
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVVm::protect(uint64_t gpa, uint64_t size, uint32_t flags)
{
    uint32_t perm = flags;
    if (!rhv::npt::normalizePerm(&perm) || size == 0 || ((gpa | size) & 0xFFF) ||
        gpa >= rhv::npt::kGpaLimit || size > rhv::npt::kGpaLimit - gpa)
        return kIOReturnBadArgument;

    IOLockLock(fLock);
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
    IOLockUnlock(fLock);
    return rc;
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
    c.nestedCr3     = fNpt.rootPa();

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
        RHV_SYNC("VM: first VMRUN, rip=0x%llx cs.base=0x%llx efer=0x%llx cr0=0x%llx nCr3=0x%llx",
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

    for (;;) {
        v->control.exitCode = 0;
        v->control.tlbCtl   = TLB_FLUSH_ALL;
        v->control.clean    = 0;      /* every field is dirty, we never use clean bits yet */

        uint64_t hostCr2;
        __asm__ volatile("movq %%cr2, %0" : "=r"(hostCr2));   /* guest page faults can overwrite it */

        /* Interrupts off from here until the exit has been looked at. See activeOnAllUsable(). */
        int ints = ml_set_interrupts_enabled(0);
        if (!svm::activeOnAllUsable()) {
            ml_set_interrupts_enabled(ints);
            RHV_ERR("VmRun: SVM was turned off under us (sleep?), not entering the guest");
            rc = kIOReturnNotReady;
            break;
        }

        rhv_svm_vmrun(&fGprs, fVmcb.pa, fHostVmcb.pa);

        __asm__ volatile("movq %0, %%cr2" : : "r"(hostCr2));
        ml_set_interrupts_enabled(ints);   /* a pending host interrupt is taken right here */

        code = v->control.exitCode;
        RHV_STAT_INC(vmExits);

        if (code == EXIT_INTR || code == EXIT_NMI) {
            /* An event that was being delivered when the host interrupt came in must be injected again. */
            if (v->control.exitIntInfo & (1u << 31)) {
                v->control.eventInj    = v->control.exitIntInfo;
                v->control.eventInjErr = v->control.exitIntInfoErr;
            }
            /* The host needed the CPU. It has it now; run on unless the slice is over. */
            if (mach_absolute_time() >= deadline) {
                timeslice = true;
                break;
            }
            continue;
        }
        break;
    }

    if (rc == kIOReturnSuccess) {
        bzero(out, sizeof(*out));
        if (timeslice) {
            out->reason = RHV_EXIT_TIMESLICE;
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
                if (code == EXIT_NPF)
                    fNpt.lookup(v->control.exitInfo2 & ~(uint64_t)0xFFF, nullptr, &perm);
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

IOReturn com_overlorddev_RyzenHVVm::vmcsRead(uint32_t field, uint64_t *value)
{
    if (!fVmx)
        return kIOReturnUnsupported;
    IOLockLock(fLock);
    rhv::vmx::Result r = fVmcs.read(field, value);
    IOLockUnlock(fLock);
    return r == rhv::vmx::R_OK ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn com_overlorddev_RyzenHVVm::vmcsWrite(uint32_t field, uint64_t value)
{
    if (!fVmx)
        return kIOReturnUnsupported;
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
