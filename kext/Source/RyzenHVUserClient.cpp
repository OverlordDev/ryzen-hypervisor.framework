#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>

#include "Log.hpp"
#include "RyzenHV.hpp"
#include "RyzenHVUserClient.hpp"
#include "Vm.hpp"

#define super IOUserClient

OSDefineMetaClassAndStructors(com_overlorddev_RyzenHVUserClient, IOUserClient)

bool com_overlorddev_RyzenHVUserClient::initWithTask(task_t owningTask, void *securityToken,
                                                     UInt32 type)
{
    if (!super::initWithTask(owningTask, securityToken, type))
        return false;

    fTask    = owningTask;
    fOwner   = nullptr;
    fVm      = nullptr;
    fVmLock  = IOLockAlloc();
    fIsAdmin = clientHasPrivilege(securityToken, kIOClientPrivilegeAdministrator) ==
               kIOReturnSuccess;
    /*
     * Read-only methods (info, log, stats) are open to everyone. Methods that
     * change state or create VMs need root (fIsAdmin).
     * TODO(security): decide who may create VMs for real; Docker and friends
     * run unprivileged.
     */
    return fVmLock != nullptr;
}

bool com_overlorddev_RyzenHVUserClient::start(IOService *provider)
{
    if (!super::start(provider))
        return false;
    fOwner = OSDynamicCast(com_overlorddev_RyzenHV, provider);
    if (!fOwner)
        return false;

    RHV_STAT_INC(clientOpens);
    RHV_DBG("user client opened (admin=%d)", fIsAdmin);
    return true;
}

IOReturn com_overlorddev_RyzenHVUserClient::clientClose()
{
    RHV_DBG("user client closed");
    destroyVm();
    if (!isInactive())
        terminate();
    return kIOReturnSuccess;
}

void com_overlorddev_RyzenHVUserClient::free()
{
    destroyVm();
    if (fVmLock) {
        IOLockFree(fVmLock);
        fVmLock = nullptr;
    }
    super::free();
}

/* Drops our reference. A VmRun still in flight on another thread keeps the VM alive until it returns. */
void com_overlorddev_RyzenHVUserClient::destroyVm()
{
    if (!fVmLock)
        return;
    IOLockLock(fVmLock);
    com_overlorddev_RyzenHVVm *vm = fVm;
    fVm = nullptr;
    IOLockUnlock(fVmLock);

    if (vm) {
        RHV_INFO("VM destroyed");
        vm->release();
    }
}

IOReturn com_overlorddev_RyzenHVUserClient::externalMethod(uint32_t selector,
                                                           IOExternalMethodArguments *arguments,
                                                           IOExternalMethodDispatch *dispatch,
                                                           OSObject *target, void *reference)
{
    typedef com_overlorddev_RyzenHVUserClient C;

    static const IOExternalMethodDispatch kMethods[kRHVMethodCount] = {
        /* kRHVMethodGetInfo */
        { (IOExternalMethodAction)&C::sGetInfo, 0, 0, 0, sizeof(RHVInfo) },
        /* kRHVMethodReadLog: 1 scalar in (cursor), struct out */
        { (IOExternalMethodAction)&C::sReadLog, 1, 0, 0, sizeof(RHVLogChunk) },
        /* kRHVMethodSetLogLevel: 1 scalar in (level) */
        { (IOExternalMethodAction)&C::sSetLogLevel, 1, 0, 0, 0 },
        /* kRHVMethodGetStats */
        { (IOExternalMethodAction)&C::sGetStats, 0, 0, 0, sizeof(RHVStats) },
        /* kRHVMethodSelfTest: 2 scalars out (ok, total) */
        { (IOExternalMethodAction)&C::sSelfTest, 0, 0, 2, 0 },
        /* kRHVMethodVmCreate: 1 scalar in (flags), 1 scalar out (built-in RAM size) */
        { (IOExternalMethodAction)&C::sVmCreate, 1, 0, 1, 0 },
        /* kRHVMethodVmDestroy */
        { (IOExternalMethodAction)&C::sVmDestroy, 0, 0, 0, 0 },
        /* kRHVMethodVmReset: 2 scalars in (cs, ip) */
        { (IOExternalMethodAction)&C::sVmReset, 2, 0, 0, 0 },
        /* kRHVMethodVmSetRegs: struct in */
        { (IOExternalMethodAction)&C::sVmSetRegs, 0, sizeof(RHVRegs), 0, 0 },
        /* kRHVMethodVmGetRegs: struct out */
        { (IOExternalMethodAction)&C::sVmGetRegs, 0, 0, 0, sizeof(RHVRegs) },
        /* kRHVMethodVmRun: 1 scalar in (skip), struct out */
        { (IOExternalMethodAction)&C::sVmRun, 1, 0, 0, sizeof(RHVExit) },
        /* kRHVMethodVmMap: 4 scalars in (uva, gpa, size, flags) */
        { (IOExternalMethodAction)&C::sVmMap, 4, 0, 0, 0 },
        /* kRHVMethodVmUnmap: 2 scalars in (gpa, size) */
        { (IOExternalMethodAction)&C::sVmUnmap, 2, 0, 0, 0 },
        /* kRHVMethodVmProtect: 3 scalars in (gpa, size, flags) */
        { (IOExternalMethodAction)&C::sVmProtect, 3, 0, 0, 0 },
    };

    static_assert(sizeof(kMethods) / sizeof(kMethods[0]) == kRHVMethodCount,
                  "one dispatch entry per selector");

    if (selector >= kRHVMethodCount) {
        RHV_WARN("user client: unknown selector %u", selector);
        return kIOReturnUnsupported;
    }

    RHV_STAT_INC(clientCalls);
    RHV_TRACE("user client: selector %u", selector);

    dispatch = const_cast<IOExternalMethodDispatch *>(&kMethods[selector]);
    target   = this;
    return super::externalMethod(selector, arguments, dispatch, target, reference);
}

IOReturn com_overlorddev_RyzenHVUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
                                                                IOMemoryDescriptor **memory)
{
    if (type != kRHVMemGuestRam || !fIsAdmin)
        return kIOReturnUnsupported;

    IOLockLock(fVmLock);
    com_overlorddev_RyzenHVVm *vm = fVm;
    if (vm)
        vm->retain();
    IOLockUnlock(fVmLock);
    if (!vm)
        return kIOReturnNotReady;

    IOMemoryDescriptor *md = vm->ramDescriptor();
    if (!md) {                /* created with RHV_VMCREATE_NO_RAM */
        vm->release();
        return kIOReturnNotFound;
    }
    md->retain();             /* the caller releases it */
    vm->release();

    *options = 0;
    *memory  = md;
    return kIOReturnSuccess;
}

/* Common prologue of the methods below. */
#define RHV_ME()                                                                         \
    auto *me = OSDynamicCast(com_overlorddev_RyzenHVUserClient, target);                 \
    if (!me || !me->fOwner)                                                              \
        return kIOReturnNotAttached

#define RHV_ADMIN()                                                                      \
    if (!me->fIsAdmin)                                                                   \
        return kIOReturnNotPrivileged

/* Takes a reference on the client's VM, or returns kIOReturnNotReady. */
#define RHV_RETAIN_VM(vm)                                                                \
    com_overlorddev_RyzenHVVm *vm;                                                       \
    IOLockLock(me->fVmLock);                                                             \
    vm = me->fVm;                                                                        \
    if (vm)                                                                              \
        vm->retain();                                                                    \
    IOLockUnlock(me->fVmLock);                                                           \
    if (!vm)                                                                             \
        return kIOReturnNotReady

IOReturn com_overlorddev_RyzenHVUserClient::sGetInfo(OSObject *target, void *,
                                                     IOExternalMethodArguments *args)
{
    RHV_ME();
    RHVInfo info;
    me->fOwner->getInfo(&info);
    bcopy(&info, args->structureOutput, sizeof(info));
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVUserClient::sReadLog(OSObject *target, void *,
                                                     IOExternalMethodArguments *args)
{
    RHV_ME();
    /* Fill IOKit's own output buffer directly, 4 KiB is too much for the kernel stack. */
    rhv::log::read(args->scalarInput[0], (RHVLogChunk *)args->structureOutput);
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVUserClient::sSetLogLevel(OSObject *target, void *,
                                                         IOExternalMethodArguments *args)
{
    RHV_ME();
    RHV_ADMIN();
    if (args->scalarInput[0] > RHV_LOG_TRACE)
        return kIOReturnBadArgument;

    rhv::log::setLevel((uint32_t)args->scalarInput[0]);
    RHV_SYNC("log level set to %u by user client", (uint32_t)args->scalarInput[0]);
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVUserClient::sGetStats(OSObject *target, void *,
                                                      IOExternalMethodArguments *args)
{
    RHV_ME();
    RHVStats stats;
    me->fOwner->getStats(&stats);
    bcopy(&stats, args->structureOutput, sizeof(stats));
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVUserClient::sSelfTest(OSObject *target, void *,
                                                      IOExternalMethodArguments *args)
{
    RHV_ME();
    RHV_ADMIN();
    uint32_t ok = 0, total = 0;
    me->fOwner->runSelfTest(&ok, &total);
    args->scalarOutput[0] = ok;
    args->scalarOutput[1] = total;
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVUserClient::sVmCreate(OSObject *target, void *,
                                                      IOExternalMethodArguments *args)
{
    RHV_ME();
    RHV_ADMIN();

    RHVInfo info;
    me->fOwner->getInfo(&info);

    IOReturn err = kIOReturnSuccess;
    if (args->scalarInput[0] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    com_overlorddev_RyzenHVVm *vm =
        com_overlorddev_RyzenHVVm::create(info, (uint32_t)args->scalarInput[0], &err);
    if (!vm)
        return err;

    IOLockLock(me->fVmLock);
    bool taken = me->fVm != nullptr;
    if (!taken)
        me->fVm = vm;
    IOLockUnlock(me->fVmLock);

    if (taken) {
        vm->release();
        return kIOReturnExclusiveAccess;   /* one VM per client in phase 1 */
    }

    args->scalarOutput[0] = vm->ramSize();
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVUserClient::sVmDestroy(OSObject *target, void *,
                                                       IOExternalMethodArguments *)
{
    RHV_ME();
    RHV_ADMIN();
    me->destroyVm();
    return kIOReturnSuccess;
}

IOReturn com_overlorddev_RyzenHVUserClient::sVmReset(OSObject *target, void *,
                                                     IOExternalMethodArguments *args)
{
    RHV_ME();
    RHV_ADMIN();
    if (args->scalarInput[0] > 0xFFFF || args->scalarInput[1] > 0xFFFF)
        return kIOReturnBadArgument;

    RHV_RETAIN_VM(vm);
    IOReturn rc = vm->reset((uint16_t)args->scalarInput[0], (uint16_t)args->scalarInput[1]);
    vm->release();
    return rc;
}

IOReturn com_overlorddev_RyzenHVUserClient::sVmSetRegs(OSObject *target, void *,
                                                       IOExternalMethodArguments *args)
{
    RHV_ME();
    RHV_ADMIN();
    RHV_RETAIN_VM(vm);
    IOReturn rc = vm->setRegs((const RHVRegs *)args->structureInput);
    vm->release();
    return rc;
}

IOReturn com_overlorddev_RyzenHVUserClient::sVmGetRegs(OSObject *target, void *,
                                                       IOExternalMethodArguments *args)
{
    RHV_ME();
    RHV_ADMIN();
    RHV_RETAIN_VM(vm);
    IOReturn rc = vm->getRegs((RHVRegs *)args->structureOutput);
    vm->release();
    return rc;
}

IOReturn com_overlorddev_RyzenHVUserClient::sVmRun(OSObject *target, void *,
                                                   IOExternalMethodArguments *args)
{
    RHV_ME();
    RHV_ADMIN();
    RHV_RETAIN_VM(vm);
    IOReturn rc = vm->run(args->scalarInput[0] != 0, (RHVExit *)args->structureOutput);
    vm->release();
    return rc;
}

/*
 * Memory methods. VmMap pins pages of the process that owns this connection, so
 * a connection handed to another process (mach port passing) must not be able
 * to name addresses in its own address space.
 */
IOReturn com_overlorddev_RyzenHVUserClient::sVmMap(OSObject *target, void *,
                                                   IOExternalMethodArguments *args)
{
    RHV_ME();
    RHV_ADMIN();
    if (current_task() != me->fTask)
        return kIOReturnNotPermitted;
    if (args->scalarInput[3] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;

    RHV_RETAIN_VM(vm);
    IOReturn rc = vm->map(me->fTask, args->scalarInput[0], args->scalarInput[1],
                          args->scalarInput[2], (uint32_t)args->scalarInput[3]);
    vm->release();
    return rc;
}

IOReturn com_overlorddev_RyzenHVUserClient::sVmUnmap(OSObject *target, void *,
                                                     IOExternalMethodArguments *args)
{
    RHV_ME();
    RHV_ADMIN();
    RHV_RETAIN_VM(vm);
    IOReturn rc = vm->unmap(args->scalarInput[0], args->scalarInput[1]);
    vm->release();
    return rc;
}

IOReturn com_overlorddev_RyzenHVUserClient::sVmProtect(OSObject *target, void *,
                                                       IOExternalMethodArguments *args)
{
    RHV_ME();
    RHV_ADMIN();
    if (args->scalarInput[2] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    RHV_RETAIN_VM(vm);
    IOReturn rc = vm->protect(args->scalarInput[0], args->scalarInput[1],
                              (uint32_t)args->scalarInput[2]);
    vm->release();
    return rc;
}
