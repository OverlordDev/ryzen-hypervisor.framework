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

} // namespace

bool com_overlorddev_RyzenHV::start(IOService *provider)
{
    /* Kill switch first, before we touch anything. Plain IOLog: our own log is not up yet. */
    if (bootArgFlag("-rhv_off")) {
        IOLog("RyzenHV: boot-arg -rhv_off present, not starting\n");
        return false;
    }

    if (!super::start(provider))
        return false;

    rhv::log::init();
    RHV_INFO("RyzenHV " RHV_VERSION " (ABI %d) built " __DATE__ " " __TIME__, RHV_ABI_VERSION);

    rhv::svm::probe(&fInfo);

    RHV_INFO("cpus=%u amd=%u family=0x%x model=0x%x step=%u", fInfo.nCpus, fInfo.vendorIsAmd,
             fInfo.cpuFamily, fInfo.cpuModel, fInfo.cpuStepping);
    RHV_INFO("svm=%u disabledByBios=%u locked=%u rev=%u asids=%u features=0x%x usableCpus=%u",
             fInfo.svmSupported, fInfo.svmDisabledByBios, fInfo.svmLocked, fInfo.svmRevision,
             fInfo.numAsids, fInfo.svmFeatures, fInfo.nCpusSvmUsable);

    uint32_t selftest = 0;
    bootArgValue("rhv_selftest", &selftest);
    fSelfTest = selftest != 0;

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
            uint32_t n = rhv::svm::enableAll();
            if (n && fSelfTest) {
                uint32_t ok, total;
                rhv::svm::verifyAll(&ok, &total);
            }
        } else {
            RHV_INFO("probe only; add boot-arg rhv_enable_svm=1 to enable SVM");
        }
    }

    /* IOPMrootDomain is only forward-declared in the public SDK headers. It derives from
     * IOService with single inheritance, so a reinterpret_cast is safe. */
    IOService *root = reinterpret_cast<IOService *>(getPMRootDomain());
    fPMNotifier = root ? root->registerInterest(gIOPriorityPowerStateInterest,
                                                &com_overlorddev_RyzenHV::sleepWakeHandler, this)
                       : nullptr;
    if (!fPMNotifier)
        RHV_WARN("could not register for sleep/wake notifications");

    registerService();
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
