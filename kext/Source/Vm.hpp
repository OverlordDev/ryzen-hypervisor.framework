/*
 * Vm.hpp - one virtual machine with one vCPU (phase 1).
 *
 * Owns the VMCB, the host VMCB, the I/O and MSR permission bitmaps, the nested
 * page tables (guest physical -> pinned memory, built by rhv::npt::Table) and
 * the guest's general purpose registers.
 *
 * Guest memory comes from two places: the optional built-in 1 MiB at guest
 * physical 0 (kernel memory userland can map), and VmMap, which pins pages of the
 * calling process. Both end up as 4 KiB NPT entries pointing into a Backing.
 *
 * Everything is serialised by one lock: VmRun holds it while the guest runs,
 * which is bounded by the time slice (see Vm.cpp).
 */
#ifndef RYZENHV_VM_HPP
#define RYZENHV_VM_HPP

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSObject.h>

#include "../../include/RyzenHVShared.h"
#include "Npt.hpp"
#include "PhysMem.hpp"
#include "VMCB.hpp"

namespace rhv {

/* Guest GPRs that VMRUN does not keep in the VMCB (RAX and RSP are in the VMCB). Layout is used by SvmRun.S. */
struct GuestGprs {
    uint64_t rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
};

} // namespace rhv

class com_overlorddev_RyzenHVVm : public OSObject {
    OSDeclareDefaultStructors(com_overlorddev_RyzenHVVm)

public:
    /*
     * Fails with kIOReturnNotReady if SVM is not active on all CPUs and with
     * kIOReturnUnsupported if the CPU lacks NPT or NRIPS.
     */
    static com_overlorddev_RyzenHVVm *create(const RHVInfo &info, uint32_t flags, IOReturn *err);

    bool init() override;
    void free() override;

    /* nullptr when created with RHV_VMCREATE_NO_RAM. */
    IOMemoryDescriptor *ramDescriptor() const { return fRam; }
    uint32_t            ramSize() const { return fRam ? RHV_GUEST_RAM_SIZE : 0; }

    /* hv_vm_map / unmap / protect. See RyzenHVShared.h for the exact rules. */
    IOReturn map(task_t task, uint64_t uva, uint64_t gpa, uint64_t size, uint32_t flags);
    IOReturn unmap(uint64_t gpa, uint64_t size);
    IOReturn protect(uint64_t gpa, uint64_t size, uint32_t flags);

    IOReturn reset(uint16_t cs, uint16_t ip);
    IOReturn getRegs(RHVRegs *out);
    IOReturn setRegs(const RHVRegs *in);
    IOReturn run(bool skipInstruction, RHVExit *out);

    /* A pinned buffer (user or kernel) and how many NPT pages still point into it. */
    struct Backing {
        IOMemoryDescriptor *md;
        uint64_t            live;
    };

private:
    bool     setup(uint32_t flags);
    IOReturn mapDescriptorLocked(IOMemoryDescriptor *md, uint64_t gpa, uint64_t size,
                                 uint32_t perm);
    static void dropPage(Backing *b);
    static void releaseOwner(void *ctx, void *owner);
    void     resetLocked(uint16_t cs, uint16_t ip);
    void     dumpVmcb(const char *why);

    IOLock                   *fLock;
    IOBufferMemoryDescriptor *fRam;
    rhv::PhysMem              fVmcb;
    rhv::PhysMem              fHostVmcb;
    rhv::PhysMem              fIopm;     /* 12 KiB, all ones: intercept every port */
    rhv::PhysMem              fMsrpm;    /*  8 KiB, all ones: intercept every MSR  */
    rhv::npt::Table           fNpt;
    uint64_t                  fPagesMapped;
    rhv::GuestGprs            fGprs;
    bool                      fFirstRun;
    bool                      fHaveNextRip;
};

#endif /* RYZENHV_VM_HPP */
