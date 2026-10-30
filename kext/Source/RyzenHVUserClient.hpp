#ifndef RYZENHV_USERCLIENT_HPP
#define RYZENHV_USERCLIENT_HPP

#include <IOKit/IOUserClient.h>

class com_overlorddev_RyzenHV;
class com_overlorddev_RyzenHVVm;

/*
 * One instance per IOServiceOpen() from userland (the shim, rhv-info, ...).
 * Each client owns one vCPU (the first one also created its VM, the others joined it); it is destroyed when the
 * client goes away.
 */
class com_overlorddev_RyzenHVUserClient : public IOUserClient {
    OSDeclareDefaultStructors(com_overlorddev_RyzenHVUserClient)

public:
    bool initWithTask(task_t owningTask, void *securityToken, UInt32 type) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    void free() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                            IOExternalMethodDispatch *dispatch, OSObject *target,
                            void *reference) override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
                                 IOMemoryDescriptor **memory) override;

private:
    static IOReturn sGetInfo(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sReadLog(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sSetLogLevel(OSObject *target, void *reference,
                                 IOExternalMethodArguments *args);
    static IOReturn sGetStats(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sSelfTest(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sHvControl(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmCreate(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmDestroy(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmReset(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmSetRegs(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmGetRegs(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmRun(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmMap(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmUnmap(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmProtect(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmcsRead(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmcsWrite(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmGetFpu(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmSetFpu(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmMsr(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmKick(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmRunEx(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmcsReadMany(OSObject *target, void *reference, IOExternalMethodArguments *args);
    static IOReturn sVmJoin(OSObject *target, void *reference, IOExternalMethodArguments *args);

    void destroyVm();

    com_overlorddev_RyzenHV   *fOwner;
    com_overlorddev_RyzenHVVm *fVm;
    IOLock                    *fVmLock;   /* guards the fVm pointer, not the VM itself */
    task_t                     fTask;
    bool                       fIsAdmin;
};

#endif /* RYZENHV_USERCLIENT_HPP */
