/*
 * KernelDecls.hpp - kernel symbols that are exported but not in the public
 * headers. All of them resolve through com.apple.kpi.unsupported / .mach.
 * If the link step reports one of these as undefined, tell me which.
 */
#ifndef RYZENHV_KERNELDECLS_HPP
#define RYZENHV_KERNELDECLS_HPP

#include <stdint.h>

extern "C" {
void         mp_rendezvous_no_intrs(void (*action)(void *), void *arg);
int          cpu_number(void);
uint64_t     mach_absolute_time(void);
void         nanoseconds_to_absolutetime(uint64_t nanoseconds, uint64_t *result);
struct task *current_task(void);
int          ml_set_interrupts_enabled(int enable);   /* returns the previous state */
void         rhv_svm_vmrun(void *guestGprs, uint64_t guestVmcbPa, uint64_t hostVmcbPa);  /* SvmRun.S */
}

#endif /* RYZENHV_KERNELDECLS_HPP */
