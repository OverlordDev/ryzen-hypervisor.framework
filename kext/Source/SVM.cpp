#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/OSAtomic.h>

#include "KernelDecls.hpp"
#include "Log.hpp"
#include "SVM.hpp"
#include "X86.hpp"

namespace {

struct CpuProbe {
    uint32_t vendorAmd;
    uint32_t family, model, stepping;
    uint32_t svmSupported;
    uint32_t svmRevision;
    uint32_t numAsids;
    uint32_t features;
    uint32_t svmDisabled;
    uint32_t svmLocked;
    uint32_t eferSvme;
};

struct CpuVerify {
    uint64_t efer;
    uint64_t hsave;
    uint32_t checked;
};

CpuProbe                  gProbe[RHV_MAX_CPUS];
CpuVerify                 gVerify[RHV_MAX_CPUS];
IOBufferMemoryDescriptor *gHostSave[RHV_MAX_CPUS];
uint64_t                  gHostSavePa[RHV_MAX_CPUS];
uint32_t                  gNCpus;
volatile SInt32           gActive;
volatile SInt32           gSeen;      /* CPUs that ran probeAction */
volatile SInt32           gMaxIndex;  /* highest cpu_number() seen */
volatile bool             gReadMsrs = true;   /* set before the probe rendezvous */
bool                      gSuspended;

bool cpuUsable(const CpuProbe &p)
{
    return p.vendorAmd && p.svmSupported && !p.svmDisabled;
}

/*
 * The *Action functions run on every CPU with interrupts disabled:
 * no allocation, no logging (not even RHV_*), no locks.
 */
void probeAction(void *)
{
    int cpu = cpu_number();
    OSIncrementAtomic(&gSeen);
    if (cpu > gMaxIndex)
        gMaxIndex = cpu;      /* every CPU runs this with interrupts off, so a plain store is enough */
    if (cpu < 0 || cpu >= RHV_MAX_CPUS)
        return;

    CpuProbe &p = gProbe[cpu];
    uint32_t r[4];

    rhv_cpuid(0, 0, r);
    p.vendorAmd = rhv_is_amd_vendor(r[1], r[2], r[3]);
    if (!p.vendorAmd)
        return;

    rhv_cpuid(1, 0, r);
    p.family   = rhv_amd_family(r[0]);
    p.model    = rhv_amd_model(r[0]);
    p.stepping = rhv_amd_stepping(r[0]);

    rhv_cpuid(0x80000000u, 0, r);
    uint32_t maxExt = r[0];

    if (maxExt >= 0x80000001u) {
        rhv_cpuid(0x80000001u, 0, r);
        p.svmSupported = (r[2] >> 2) & 1;
    }

    if (p.svmSupported && maxExt >= 0x8000000Au) {
        rhv_cpuid(0x8000000Au, 0, r);
        p.svmRevision = r[0] & 0xFF;
        p.numAsids    = r[1];
        p.features    = r[3];
    }

    if (!gReadMsrs)
        return;

    if (p.svmSupported) {
        uint64_t vmcr = rhv_rdmsr(RHV_MSR_VM_CR);
        p.svmDisabled = (vmcr & RHV_VM_CR_SVMDIS) ? 1 : 0;
        p.svmLocked   = (vmcr & RHV_VM_CR_LOCK) ? 1 : 0;
    }

    p.eferSvme = (rhv_rdmsr(RHV_MSR_EFER) & RHV_EFER_SVME) ? 1 : 0;
}

void enableAction(void *)
{
    int cpu = cpu_number();
    if (cpu < 0 || cpu >= RHV_MAX_CPUS)
        return;
    if (!cpuUsable(gProbe[cpu]) || !gHostSavePa[cpu])
        return;

    /* Read-modify-write: keep every EFER bit the kernel already relies on. */
    uint64_t efer = rhv_rdmsr(RHV_MSR_EFER);
    if (!(efer & RHV_EFER_SVME))
        rhv_wrmsr(RHV_MSR_EFER, efer | RHV_EFER_SVME);
    rhv_wrmsr(RHV_MSR_VM_HSAVE_PA, gHostSavePa[cpu]);

    OSIncrementAtomic(&gActive);
}

void disableAction(void *)
{
    int cpu = cpu_number();
    if (cpu < 0 || cpu >= RHV_MAX_CPUS)
        return;
    if (!cpuUsable(gProbe[cpu]))
        return;

    uint64_t efer = rhv_rdmsr(RHV_MSR_EFER);
    if (efer & RHV_EFER_SVME) {
        rhv_wrmsr(RHV_MSR_VM_HSAVE_PA, 0);
        rhv_wrmsr(RHV_MSR_EFER, efer & ~RHV_EFER_SVME);
    }
}

void verifyAction(void *)
{
    int cpu = cpu_number();
    if (cpu < 0 || cpu >= RHV_MAX_CPUS)
        return;
    if (!cpuUsable(gProbe[cpu]))
        return;

    CpuVerify &v = gVerify[cpu];
    v.efer    = rhv_rdmsr(RHV_MSR_EFER);
    v.hsave   = rhv_rdmsr(RHV_MSR_VM_HSAVE_PA);
    v.checked = 1;
}

bool allocHostSave(uint32_t cpu)
{
    /* One 4K page, physically contiguous, 4K aligned, below 2^52. */
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 4096,
        0x000FFFFFFFFFF000ULL);
    if (!md) {
        RHV_ERR("cpu %u: host save area: allocation failed", cpu);
        return false;
    }

    bzero(md->getBytesNoCopy(), 4096);

    IOByteCount len = 0;
    addr64_t pa = md->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
    if (!pa || len < 4096 || (pa & 0xFFF)) {
        RHV_ERR("cpu %u: host save area: bad physical segment pa=0x%llx len=%llu", cpu,
                (unsigned long long)pa, (unsigned long long)len);
        md->release();
        return false;
    }

    gHostSave[cpu]   = md;
    gHostSavePa[cpu] = pa;
    RHV_DBG("cpu %u: host save area at pa 0x%llx", cpu, (unsigned long long)pa);
    return true;
}

void freeHostSave()
{
    for (uint32_t i = 0; i < RHV_MAX_CPUS; i++) {
        if (gHostSave[i]) {
            gHostSave[i]->release();
            gHostSave[i] = nullptr;
        }
        gHostSavePa[i] = 0;
    }
}

uint32_t usableCount()
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < gNCpus; i++) {
        if (cpuUsable(gProbe[i]))
            n++;
    }
    return n;
}

} // namespace

namespace rhv {
namespace svm {

void probe(RHVInfo *out, bool readMsrs)
{
    bzero(gProbe, sizeof(gProbe));
    gReadMsrs = readMsrs;

    /*
     * The CPU count is not exported to kexts (ml_get_max_cpus does not exist on Sequoia), so count
     * the CPUs that take part in the rendezvous. Indices are contiguous from 0.
     */
    gSeen     = 0;
    gMaxIndex = 0;
    mp_rendezvous_no_intrs(probeAction, nullptr);
    RHV_STAT_INC(probeRuns);

    gNCpus = (uint32_t)gMaxIndex + 1;
    if ((uint32_t)gSeen != gNCpus)
        RHV_WARN("rendezvous ran on %d CPUs but the highest cpu_number is %d", (int)gSeen, (int)gMaxIndex);
    if (gNCpus > RHV_MAX_CPUS) {
        RHV_WARN("machine reports %u CPUs, only the first %u are handled", gNCpus, RHV_MAX_CPUS);
        gNCpus = RHV_MAX_CPUS;
    }

    bzero(out, sizeof(*out));
    out->abiVersion = RHV_ABI_VERSION;
    out->nCpus      = gNCpus;

    /* CPU 0 is the reference; the per-CPU usable count catches asymmetric setups. */
    const CpuProbe &p = gProbe[0];
    out->vendorIsAmd       = p.vendorAmd;
    out->cpuFamily         = p.family;
    out->cpuModel          = p.model;
    out->cpuStepping       = p.stepping;
    out->svmSupported      = p.svmSupported;
    out->svmDisabledByBios = p.svmDisabled;
    out->svmLocked         = p.svmLocked;
    out->svmRevision       = p.svmRevision;
    out->numAsids          = p.numAsids;
    out->svmFeatures       = p.features;
    out->nCpusSvmUsable    = usableCount();
    out->nCpusSvmActive    = (uint32_t)gActive;

    for (uint32_t i = 0; i < gNCpus; i++) {
        const CpuProbe &c = gProbe[i];
        RHV_DBG("cpu %u: amd=%u fam=0x%x model=0x%x svm=%u dis=%u lock=%u rev=%u asids=%u "
                "feat=0x%x efer.svme=%u",
                i, c.vendorAmd, c.family, c.model, c.svmSupported, c.svmDisabled, c.svmLocked,
                c.svmRevision, c.numAsids, c.features, c.eferSvme);
    }
}

uint32_t enableAll()
{
    RHV_STAT_INC(enableCalls);

    if (gActive) {
        RHV_WARN("enableAll: already active on %d CPUs", (int)gActive);
        return (uint32_t)gActive;
    }

    for (uint32_t i = 0; i < gNCpus; i++) {
        if (cpuUsable(gProbe[i]) && !allocHostSave(i)) {
            RHV_ERR("enableAll: giving up, SVM stays off");
            RHV_STAT_INC(enableFailures);
            freeHostSave();
            return 0;
        }
    }

    /* If the next step panics, this is the last line you will see. */
    RHV_SYNC("enabling SVM on %u CPUs now (writing EFER and VM_HSAVE_PA)", usableCount());

    mp_rendezvous_no_intrs(enableAction, nullptr);

    uint32_t active = (uint32_t)gActive;
    RHV_STAT_ADD(enableCpusOk, active);
    if (active != usableCount()) {
        RHV_ERR("enableAll: only %u of %u CPUs took the enable path", active, usableCount());
        RHV_STAT_INC(enableFailures);
    } else {
        RHV_INFO("SVM enabled on %u CPUs", active);
    }
    return active;
}

void disableAll()
{
    RHV_STAT_INC(disableCalls);

    if (gActive) {
        RHV_SYNC("disabling SVM on all CPUs");
        mp_rendezvous_no_intrs(disableAction, nullptr);
        gActive = 0;
    }
    gSuspended = false;
    freeHostSave();
}

void suspend()
{
    if (!gActive)
        return;

    RHV_SYNC("sleep: turning SVM off, host save areas are kept");
    mp_rendezvous_no_intrs(disableAction, nullptr);
    gActive    = 0;
    gSuspended = true;
}

uint32_t resume()
{
    if (!gSuspended)
        return 0;

    RHV_SYNC("wake: turning SVM back on");
    gSuspended = false;
    gActive    = 0;
    mp_rendezvous_no_intrs(enableAction, nullptr);

    uint32_t active = (uint32_t)gActive;
    if (active != usableCount())
        RHV_ERR("wake: only %u of %u CPUs re-enabled", active, usableCount());
    else
        RHV_INFO("wake: SVM re-enabled on %u CPUs", active);
    return active;
}

bool verifyAll(uint32_t *ok, uint32_t *total)
{
    *ok = 0;
    *total = 0;
    if (!gActive) {
        RHV_WARN("selftest: SVM is not active, nothing to verify");
        return false;
    }

    bzero(gVerify, sizeof(gVerify));
    mp_rendezvous_no_intrs(verifyAction, nullptr);

    for (uint32_t i = 0; i < gNCpus; i++) {
        const CpuVerify &v = gVerify[i];
        if (!v.checked)
            continue;
        (*total)++;

        bool svme  = (v.efer & RHV_EFER_SVME) != 0;
        bool hsave = v.hsave == gHostSavePa[i];
        if (svme && hsave) {
            (*ok)++;
            RHV_DBG("selftest cpu %u: ok (efer=0x%llx hsave=0x%llx)", i,
                    (unsigned long long)v.efer, (unsigned long long)v.hsave);
        } else {
            RHV_ERR("selftest cpu %u: MISMATCH efer=0x%llx (svme=%d) hsave=0x%llx expected 0x%llx",
                    i, (unsigned long long)v.efer, svme, (unsigned long long)v.hsave,
                    (unsigned long long)gHostSavePa[i]);
        }
    }

    RHV_STAT_INC(selftestRuns);
    RHV_STAT_ADD(selftestCpusOk, *ok);
    RHV_INFO("selftest: %u of %u CPUs ok", *ok, *total);
    return true;
}

uint32_t activeCpuCount()
{
    return (uint32_t)gActive;
}

bool activeOnAllUsable()
{
    return !gSuspended && gActive > 0 && (uint32_t)gActive == usableCount();
}

} // namespace svm
} // namespace rhv
