/*
 * Vm.hpp - a virtual machine: one shared part and one object per vCPU.
 *
 * com_overlorddev_RyzenHVVmShared is what all vCPUs of a VM have in common: the nested page tables (guest physical ->
 * pinned memory, built by rhv::npt::Table), the optional built-in RAM and the list of the vCPUs. com_overlorddev_RyzenHVVm
 * is one vCPU: it owns the VMCB, the host VMCB, the I/O and MSR permission bitmaps and the guest's general purpose
 * registers, and has its own lock, so vCPUs run in parallel on different host threads.
 *
 * Guest memory comes from two places: the optional built-in 1 MiB at guest
 * physical 0 (kernel memory userland can map), and VmMap, which pins pages of the
 * calling process. Both end up as 4 KiB NPT entries pointing into a Backing.
 *
 * A vCPU's own state is serialised by its lock: VmRun holds it while the guest runs, which is bounded by the time slice
 * (see Vm.cpp). The page tables have a reader/writer lock: a vCPU holds it for reading while it is inside the guest, and
 * VmMap/VmUnmap/VmProtect take it for writing after kicking every vCPU out, so when they return no vCPU still runs with
 * the old mappings or permissions (every VMRUN flushes the TLB).
 */
#ifndef RYZENHV_VM_HPP
#define RYZENHV_VM_HPP

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSObject.h>

#include "../../include/RyzenHVShared.h"
#include "FpuState.hpp"
#include "Npt.hpp"
#include "VmcsState.hpp"
#include "PhysMem.hpp"
#include "VMCB.hpp"

namespace rhv {

/* Guest GPRs that VMRUN does not keep in the VMCB (RAX and RSP are in the VMCB). Layout is used by SvmRun.S. */
struct GuestGprs {
    uint64_t rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
};

} // namespace rhv


class com_overlorddev_RyzenHVVm;

class com_overlorddev_RyzenHVVmShared : public OSObject {
    OSDeclareDefaultStructors(com_overlorddev_RyzenHVVmShared)

public:
    /* flags: RHV_VMCREATE_*. `owner` is the task that created it (a vCPU may only join from the same task). */
    static com_overlorddev_RyzenHVVmShared *create(uint32_t flags, task_t owner, IOReturn *err);
    /* Looks a VM up by id; takes a reference, nullptr when there is none or it belongs to another task. */
    static com_overlorddev_RyzenHVVmShared *find(uint64_t id, task_t owner);

    bool init() override;
    void free() override;

    uint64_t            id() const { return fId; }
    rhv::npt::Table    &npt() { return fNpt; }
    IORWLock           *rw() const { return fRw; }
    IOMemoryDescriptor *ramDescriptor() const { return fRam; }
    uint32_t            ramSize() const { return fRam ? RHV_GUEST_RAM_SIZE : 0; }

    /* hv_vm_map / unmap / protect. See RyzenHVShared.h for the exact rules. */
    IOReturn map(task_t task, uint64_t uva, uint64_t gpa, uint64_t size, uint32_t flags);
    IOReturn unmap(uint64_t gpa, uint64_t size);
    IOReturn protect(uint64_t gpa, uint64_t size, uint32_t flags);

    /* The vCPUs, for kicking them out of the guest. attach() fails with kIOReturnNoResources when the VM is full. */
    IOReturn attach(com_overlorddev_RyzenHVVm *vcpu, uint32_t *index);
    void     detach(com_overlorddev_RyzenHVVm *vcpu);
    void     kickAll();

    /* A pinned buffer (user or kernel) and how many NPT pages still point into it. */
    struct Backing {
        IOMemoryDescriptor *md;
        uint64_t            live;
    };

private:
    bool     setup(uint32_t flags, task_t owner);
    IOReturn mapDescriptorLocked(IOMemoryDescriptor *md, uint64_t gpa, uint64_t size, uint32_t perm);
    static void dropPage(Backing *b);
    static void releaseOwner(void *ctx, void *owner);

    uint64_t                  fId;
    task_t                    fOwner;
    IORWLock                 *fRw;
    IOLock                   *fRegLock;
    com_overlorddev_RyzenHVVm *fVcpus[RHV_MAX_VCPUS_PER_VM];
    IOBufferMemoryDescriptor *fRam;
    rhv::npt::Table           fNpt;
    uint64_t                  fPagesMapped;
};

class com_overlorddev_RyzenHVVm : public OSObject {
    OSDeclareDefaultStructors(com_overlorddev_RyzenHVVm)

public:
    /*
     * Fails with kIOReturnNotReady if SVM is not active on all CPUs and with
     * kIOReturnUnsupported if the CPU lacks NPT or NRIPS. With `shared` null a new VM is made (this is its first vCPU),
     * otherwise the vCPU joins that VM (kIOReturnNoResources when it is full).
     */
    static com_overlorddev_RyzenHVVm *create(const RHVInfo &info, uint32_t flags, task_t owner,
                                             com_overlorddev_RyzenHVVmShared *shared, IOReturn *err);

    com_overlorddev_RyzenHVVmShared *shared() const { return fShared; }
    uint32_t                         index() const { return fIndex; }

    bool init() override;
    void free() override;

    /* nullptr when created with RHV_VMCREATE_NO_RAM (and for a vCPU that joined). */
    IOMemoryDescriptor *ramDescriptor() const { return fShared->ramDescriptor(); }
    uint32_t            ramSize() const { return fShared->ramSize(); }

    /* hv_vm_map / unmap / protect. See RyzenHVShared.h for the exact rules. */
    IOReturn map(task_t task, uint64_t uva, uint64_t gpa, uint64_t size, uint32_t flags);
    IOReturn unmap(uint64_t gpa, uint64_t size);
    IOReturn protect(uint64_t gpa, uint64_t size, uint32_t flags);

    IOReturn reset(uint16_t cs, uint16_t ip);
    IOReturn getRegs(RHVRegs *out);
    IOReturn setRegs(const RHVRegs *in);
    IOReturn run(bool skipInstruction, RHVExit *out);
    IOReturn runEx(const RHVRunIn *in, RHVRunOut *out);            /* VmRunEx: apply, run, report in one call */
    IOReturn vmcsReadMany(const RHVReadMany *in, RHVReadManyOut *out);

    /* VMX mode (RHV_VMCREATE_VMX): the Hypervisor.framework view of the guest. */
    bool     isVmx() const { return fVmx; }
    IOReturn vmcsRead(uint32_t field, uint64_t *value);
    IOReturn vmcsWrite(uint32_t field, uint64_t value);

    /* Guest x87/SSE/AVX state and XCR0. */
    IOReturn msrOp(uint32_t op, uint32_t msr, uint64_t value, uint64_t *out);
    /*
     * hv_vcpu_interrupt: no lock, called while another thread sits in run(). Besides raising the flag it sends an IPI to
     * the CPU the guest runs on: the physical interrupt makes VMRUN exit at once, instead of when the host happens to
     * take its next interrupt (up to 10 ms later, too slow for a guest that expects a 250-1000 Hz timer).
     */
    void     kick();

    IOReturn getFpu(RHVFpuState *out);
    IOReturn setFpu(const RHVFpuState *in);

private:
    bool     setup(uint32_t flags);
    void     resetLocked(uint16_t cs, uint16_t ip);
    void     dumpVmcb(const char *why);

    IOLock                   *fLock;
    com_overlorddev_RyzenHVVmShared *fShared;
    uint32_t                  fIndex;
    rhv::PhysMem              fVmcb;
    rhv::PhysMem              fHostVmcb;
    rhv::PhysMem              fIopm;     /* 12 KiB, all ones: intercept every port */
    rhv::PhysMem              fMsrpm;    /*  8 KiB, all ones: intercept every MSR  */
    rhv::vmx::VmcsState       fVmcs;
    rhv::PhysMem              fHostFpu, fGuestFpu, fScratchFpu;   /* XSAVE areas, 4 KiB aligned */
    uint64_t                  fGuestXcr0;
    uint64_t                  fXcr0Supported;    /* CPUID.0xD.0 EDX:EAX */
    bool                      fHasTscAux;        /* the CPU has RDTSCP, so IA32_TSC_AUX */
    uint32_t                  fFpuSize;
    volatile uint32_t         fKick;
    volatile int              fRunCpu;           /* CPU inside VMRUN, -1 when none; for kick() */
    bool                      fVmx;
    rhv::GuestGprs            fGprs;
    bool                      fFirstRun;
    bool                      fHaveNextRip;
};

#endif /* RYZENHV_VM_HPP */
