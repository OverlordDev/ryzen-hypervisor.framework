/*
 * HvFrontend.hpp - native mode: the kext registers itself with xnu as the hypervisor provider, in the place of Apple's
 * AppleHV, so that the unmodified Hypervisor.framework talks to this kext directly (no shim, no DYLD_INSERT_LIBRARIES).
 * See docs/NATIVE.md.
 *
 *   rhv_hv=1        register the trap tables and callbacks: clients can create VMs and vCPUs and run them
 *   rhv_hv=2        debugging only: answer success to the traps that are not implemented
 *   rhv_hv_sysctl=1 also write 1 into the variable behind kern.hv_support at boot (untested at boot, `rhv-info hv sysctl 1` works)
 *
 * Without rhv_hv nothing here runs.
 */
#ifndef RYZENHV_HVFRONTEND_HPP
#define RYZENHV_HVFRONTEND_HPP

class com_overlorddev_RyzenHV;

namespace rhv {
namespace hv {

/* Registers when boot-arg rhv_hv is given (and the sysctl with rhv_hv_sysctl=1); harmless otherwise. */
void start(com_overlorddev_RyzenHV *service);
/* Undoes everything (kext stop). */
void stop();

/* Run-time control, used by the user client (rhv-info hv ...). Mode 0 unregisters, 1 and 2 register (or just change the mode). */
bool setMode(uint32_t mode);
bool setSysctl(bool on);
bool setTprShadow(bool on);      /* offer the TPR shadow to VMs created afterwards (not HyperKit's) */
bool tprShadow();
uint32_t mode();
bool registered();
bool sysctlTaken();

} // namespace hv
} // namespace rhv

#endif
