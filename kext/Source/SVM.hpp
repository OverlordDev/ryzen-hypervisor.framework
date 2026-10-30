/*
 * SVM.hpp - AMD-V (SVM) capability probing and per-CPU enable/disable.
 *
 * Phase 0 only: no VMCB, no VMRUN yet. See docs/ROADMAP.md.
 */
#ifndef RYZENHV_SVM_HPP
#define RYZENHV_SVM_HPP

#include "../../include/RyzenHVShared.h"

namespace rhv {
namespace svm {

/*
 * Reads CPUID (and, with readMsrs, VM_CR and EFER) on every logical CPU and fills `out`.
 * Read-only. Without readMsrs the BIOS-disabled/locked flags are unknown and reported as 0:
 * that mode exists to bisect a crash (see boot-arg rhv_stage).
 */
void probe(RHVInfo *out, bool readMsrs);

/*
 * Sets EFER.SVME and VM_HSAVE_PA on every CPU where SVM is supported and not
 * disabled by the BIOS. Returns the number of CPUs that were enabled.
 * Must be preceded by probe().
 */
uint32_t enableAll();

/* Undoes enableAll() and frees the host save areas. Safe to call when nothing is enabled. */
void disableAll();

/*
 * Sleep support. suspend() turns SVM off on all CPUs but keeps the host save
 * areas; resume() turns it back on. Both are no-ops when SVM was never enabled.
 */
void     suspend();
uint32_t resume();

/*
 * Reads EFER and VM_HSAVE_PA back on every CPU and checks they hold what
 * enableAll() wrote. Mismatches are logged. Returns false when SVM is not
 * active. Fills *ok with the number of CPUs that match and *total with the
 * number of CPUs checked.
 */
bool verifyAll(uint32_t *ok, uint32_t *total);

uint32_t activeCpuCount();

/*
 * True while SVM is on in every CPU that supports it (not suspended, not
 * disabled). Cheap and lock-free: VmRun calls it with interrupts disabled,
 * right before VMRUN. Interrupts being off is what makes it race-free: the
 * suspend/disable rendezvous cannot complete without this CPU.
 */
bool activeOnAllUsable();

/*
 * For VmRun, on the CPU it is about to VMRUN on, with interrupts off. VMRUN's first instruction here is CLGI, which is #UD (a
 * kernel panic) when EFER.SVME is clear on this CPU; activeOnAllUsable() only counts what enableAll() did and cannot see a
 * CPU whose EFER changed afterwards. This reads EFER and VM_HSAVE_PA on this CPU and, if either is not what enableAll() set,
 * writes it back (the same writes as enableAll(), from the same save area).
 *   0: both were right    1: something was wrong and has been restored    -1: this CPU cannot run SVM, do not VMRUN
 *   -2: this CPU was not known to the SVM code yet: turn interrupts on, rescan() and try again
 * No logging (interrupts are off); the caller logs a 1 after it has turned them on again.
 */
int ensureThisCpu();      /* also -2: this CPU was not known when SVM was enabled (rescan() and look again) */

/* How many times ensureThisCpu() had to restore something. */
uint32_t healedCount();

/*
 * The CPUs are counted once, when the kext starts. On macOS 26 the other cores are not online yet at that moment (a Ryzen 9 5900X showed "cpus=1"), so
 * SVM was turned on for CPU 0 only and a VM thread on any other CPU stopped with "SVM cannot be used on cpu N". rescan() asks every CPU that is online now,
 * probes the ones it has not seen and, if enableAll() turned SVM on before, gives them a host save area and turns it on there too.
 * Thread context only, no spinlock and no page-table lock held (it allocates and runs a rendezvous, every call). Do not call it on a hot path. Returns how many CPUs were newly enabled.
 */
uint32_t rescan();

/* The CPUs the SVM code knows about, and how many of them can use SVM (what rescan() keeps up to date). */
uint32_t cpuCount();
uint32_t usableCpuCount();

/*
 * Debugging only (`rhv-info hv svmlimit N`): forgets every CPU from index n up and turns SVM off on them, which puts the SVM code in the state of a kext that started
 * before those CPUs were online, so rescan() and the guard in VmRun can be tried on a machine where the cores come up in time. Returns false for a bad n.
 */
bool debugLimit(uint32_t n);

} // namespace svm
} // namespace rhv

#endif /* RYZENHV_SVM_HPP */
