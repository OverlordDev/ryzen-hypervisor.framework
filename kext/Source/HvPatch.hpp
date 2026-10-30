/*
 * HvPatch.hpp - fixes the calling client's code in memory when it creates its VM, so that an unmodified application runs.
 *
 * Docker's HyperKit refuses a CPU whose CPUID vendor is not GenuineIntel and compares a guest linear address that AMD does not
 * report. Both are single branches in HyperKit's code. The kext finds them by the bytes around them (the same patterns as the
 * shim's shim/rhv_patch.c), in the executable regions of the CALLING task only, and changes them in that task's memory, once, when
 * the task creates its VM (hv_vm_create arrives before HyperKit's vendor check). Nothing on disk changes, nothing is signed again.
 *
 * Conditions: the calling process is on the list (by its name), the pattern is found exactly once in all executable regions below the
 * shared cache, and the machine runs with AMFI relaxed. A pattern that is not found (another build of the application) leaves the
 * application alone and the kext log says so. boot-arg rhv_clientpatch=0 turns the whole thing off.
 */
#ifndef RYZENHV_HVPATCH_HPP
#define RYZENHV_HVPATCH_HPP

namespace rhv {
namespace hv {
namespace patch {

/* Called in the context of the client's thread (a trap). Harmless for processes that are not on the list. */
void fixCurrentClient();

} // namespace patch
} // namespace hv
} // namespace rhv

#endif
