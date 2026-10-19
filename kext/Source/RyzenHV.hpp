#ifndef RYZENHV_HPP
#define RYZENHV_HPP

#include <IOKit/IOService.h>

#include "../../include/RyzenHVShared.h"

/*
 * Root service of the kext. Matches on IOResources (see Info.plist), probes
 * SVM on start() and hands out user clients.
 *
 * Boot-args:
 *   -rhv_off          kill switch: the kext loads but does nothing
 *   rhv_enable_svm=1  turn SVM on (default is probe only)
 *   rhv_selftest=1    verify EFER/VM_HSAVE_PA on every CPU after enabling
 *   rhv_loglevel=N    see Log.hpp
 *   rhv_iolog=N       see Log.hpp
 */
class com_overlorddev_RyzenHV : public IOService {
    OSDeclareDefaultStructors(com_overlorddev_RyzenHV)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    /* Refreshes the dynamic fields and copies the current state out. */
    void getInfo(RHVInfo *out);
    void getStats(RHVStats *out);

    /* Returns false when SVM is not active. */
    bool runSelfTest(uint32_t *ok, uint32_t *total);

private:
    static IOReturn sleepWakeHandler(void *target, void *refCon, UInt32 messageType,
                                     IOService *provider, void *messageArgument,
                                     vm_size_t argSize);

    RHVInfo     fInfo;
    IONotifier *fPMNotifier;
    bool        fSelfTest;
};

#endif /* RYZENHV_HPP */
