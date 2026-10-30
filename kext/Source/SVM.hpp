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
 * No logging (interrupts are off); the caller logs a 1 after it has turned them on again.
 */
int ensureThisCpu();

/* How many times ensureThisCpu() had to restore something. */
uint32_t healedCount();

} // namespace svm
} // namespace rhv

#endif /* RYZENHV_SVM_HPP */
