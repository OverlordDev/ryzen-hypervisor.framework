#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <pexpert/pexpert.h>

#include "Log.hpp"
#include "RyzenHV.hpp"
#include "SVM.hpp"

#define super IOService

#define RHV_VERSION "0.3.0"

OSDefineMetaClassAndStructors(com_overlorddev_RyzenHV, IOService)

namespace {

bool bootArgFlag(const char *name)
{
    uint32_t v = 0;
    return PE_parse_boot_argn(name, &v, sizeof(v));
}

bool bootArgValue(const char *name, uint32_t *out)
{
    return PE_parse_boot_argn(name, out, sizeof(*out));
}

/*
 * Bisecting a crash at boot: rhv_stage=N runs the start-up steps up to N and stops there.
 * Each step is announced with plain IOLog BEFORE it runs, so with -v the last line on screen
 * is the step that was running when the machine died. rhv_slow=N (seconds, max 10) waits after
 * each announcement so there is time to read or photograph it.
 */
enum {
    STAGE_LOADED   = 0,   /* start() entered, nothing else touched; returns false */
    STAGE_LOG      = 1,   /* + IOService::start, log ring, banner */
    STAGE_CPUID    = 2,   /* + CPUID probe on every CPU (no MSR reads) */
    STAGE_MSR      = 3,   /* + VM_CR / EFER reads (the full read-only probe) */
    STAGE_SERVICE  = 4,   /* + sleep/wake notifier, registerService, rhv-info works */
    STAGE_ENABLE   = 5    /* + honour rhv_enable_svm (default when rhv_stage is not given) */
};

uint32_t gStage = STAGE_ENABLE;
uint32_t gSlow  = 0;

void announce(uint32_t stage, const char *what)
{
    IOLog("RyzenHV: [stage %u] %s\n", stage, what);
    if (gSlow)
        IOSleep(gSlow * 1000);
}

} // namespace

bool com_overlorddev_RyzenHV::start(IOService *provider)
{
    /* Kill switch first, before we touch anything. Plain IOLog: our own log is not up yet. */
    if (bootArgFlag("-rhv_off")) {
        IOLog("RyzenHV: boot-arg -rhv_off present, not starting\n");
        return false;
    }

    if (bootArgValue("rhv_stage", &gStage) && gStage > STAGE_ENABLE)
        gStage = STAGE_ENABLE;
    if (bootArgValue("rhv_slow", &gSlow) && gSlow > 10)
        gSlow = 10;

    announce(STAGE_LOADED, "start() reached");
    if (gStage < STAGE_LOG)
        return false;

    announce(STAGE_LOG, "IOService::start and log ring");
    if (!super::start(provider))
        return false;

    rhv::log::init();
    RHV_INFO("RyzenHV " RHV_VERSION " (ABI %d) built " __DATE__ " " __TIME__, RHV_ABI_VERSION);

    if (gStage < STAGE_CPUID) {
        RHV_INFO("rhv_stage=%u: stopping after the banner", gStage);
        return true;
    }
    bool readMsrs = gStage >= STAGE_MSR;
    announce(STAGE_CPUID, "CPUID probe on every CPU");
    if (readMsrs)
        announce(STAGE_MSR, "reading VM_CR and EFER on every CPU");
    rhv::svm::probe(&fInfo, readMsrs);
    if (!readMsrs)
        RHV_INFO("rhv_stage=%u: MSRs were not read, BIOS-disabled/locked flags are unknown", gStage);

    RHV_INFO("cpus=%u amd=%u family=0x%x model=0x%x step=%u", fInfo.nCpus, fInfo.vendorIsAmd,
             fInfo.cpuFamily, fInfo.cpuModel, fInfo.cpuStepping);
    RHV_INFO("svm=%u disabledByBios=%u locked=%u rev=%u asids=%u features=0x%x usableCpus=%u",
             fInfo.svmSupported, fInfo.svmDisabledByBios, fInfo.svmLocked, fInfo.svmRevision,
             fInfo.numAsids, fInfo.svmFeatures, fInfo.nCpusSvmUsable);

    uint32_t selftest = 0;
    bootArgValue("rhv_selftest", &selftest);
    fSelfTest = selftest != 0;

    if (gStage < STAGE_SERVICE) {
        RHV_INFO("rhv_stage=%u: not registering the service", gStage);
        return true;
    }
    announce(STAGE_SERVICE, "sleep/wake notifier and registerService");

    /* IOPMrootDomain is only forward-declared in the public SDK headers. It derives from
     * IOService with single inheritance, so a reinterpret_cast is safe. */
    IOService *root = reinterpret_cast<IOService *>(getPMRootDomain());
    fPMNotifier = root ? root->registerInterest(gIOPriorityPowerStateInterest,
                                                &com_overlorddev_RyzenHV::sleepWakeHandler, this)
                       : nullptr;
    if (!fPMNotifier)
        RHV_WARN("could not register for sleep/wake notifications");

    registerService();

    if (gStage < STAGE_ENABLE)
        return true;

    if (!fInfo.vendorIsAmd || !fInfo.svmSupported) {
        RHV_WARN("no SVM on this CPU, diagnostics only");
    } else if (fInfo.svmDisabledByBios) {
        RHV_WARN("SVM is disabled in firmware (VM_CR.SVMDIS), enable 'SVM Mode' in BIOS");
    } else if (fInfo.nCpusSvmUsable != fInfo.nCpus) {
        RHV_WARN("SVM is usable on only %u of %u CPUs, refusing to enable", fInfo.nCpusSvmUsable,
                 fInfo.nCpus);
    } else {
        /* Enabling SVM changes CPU state, so it is opt-in. */
        uint32_t enable = 0;
        bootArgValue("rhv_enable_svm", &enable);
        if (enable) {
            announce(STAGE_ENABLE, "enabling SVM (EFER.SVME and VM_HSAVE_PA on every CPU)");
            uint32_t n = rhv::svm::enableAll();
            if (n && fSelfTest) {
                uint32_t ok, total;
                rhv::svm::verifyAll(&ok, &total);
            }
        } else {
            RHV_INFO("probe only; add boot-arg rhv_enable_svm=1 to enable SVM");
        }
    }
    return true;
}

void com_overlorddev_RyzenHV::stop(IOService *provider)
{
    RHV_INFO("stopping");
    if (fPMNotifier) {
        fPMNotifier->remove();
        fPMNotifier = nullptr;
    }
    rhv::svm::disableAll();
    super::stop(provider);
}

IOReturn com_overlorddev_RyzenHV::sleepWakeHandler(void *target, void *, UInt32 messageType,
                                                   IOService *, void *, vm_size_t)
{
    com_overlorddev_RyzenHV *me = static_cast<com_overlorddev_RyzenHV *>(target);

    switch (messageType) {
    case kIOMessageSystemWillSleep:
        RHV_STAT_INC(sleepEvents);
        rhv::svm::suspend();
        break;
    case kIOMessageSystemHasPoweredOn:
        RHV_STAT_INC(wakeEvents);
        if (rhv::svm::resume() && me->fSelfTest) {
            uint32_t ok, total;
            rhv::svm::verifyAll(&ok, &total);
        }
        break;
    default:
        break;
    }

    /* Returning success acknowledges the notification. */
    return kIOReturnSuccess;
}

void com_overlorddev_RyzenHV::getInfo(RHVInfo *out)
{
    fInfo.nCpusSvmActive = rhv::svm::activeCpuCount();
    *out = fInfo;
}

void com_overlorddev_RyzenHV::getStats(RHVStats *out)
{
    rhv::gStats.logLevel = rhv::log::level();
    *out = rhv::gStats;
}

bool com_overlorddev_RyzenHV::runSelfTest(uint32_t *ok, uint32_t *total)
{
    return rhv::svm::verifyAll(ok, total);
}
