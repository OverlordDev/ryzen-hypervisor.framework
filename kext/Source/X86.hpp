/*
 * X86.hpp - raw CPUID / MSR access and the AMD64 constants the kext needs.
 *
 * WARNING: RDMSR/WRMSR on an MSR the CPU does not implement raises #GP,
 * which panics the kernel. Only touch MSRs after the matching CPUID check.
 */
#ifndef RYZENHV_X86_HPP
#define RYZENHV_X86_HPP

#include <stdint.h>

#define RHV_MSR_EFER        0xC0000080u
#define RHV_MSR_VM_CR       0xC0010114u
#define RHV_MSR_VM_HSAVE_PA 0xC0010117u

#define RHV_EFER_SVME       (1ull << 12)
#define RHV_VM_CR_LOCK      (1ull << 3)
#define RHV_VM_CR_SVMDIS    (1ull << 4)

static inline void rhv_cpuid(uint32_t leaf, uint32_t sub, uint32_t out[4])
{
    __asm__ volatile("cpuid"
                     : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3])
                     : "a"(leaf), "c"(sub));
}

static inline uint64_t rhv_rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void rhv_wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

/* Debug registers. VMRUN loads and stores the guest's DR6 and DR7 but not DR0-DR3, and it does not keep the host's. */
static inline uint64_t rhv_read_dr(unsigned n)
{
    uint64_t v = 0;
    switch (n) {
    case 0: __asm__ volatile("movq %%dr0, %0" : "=r"(v)); break;
    case 1: __asm__ volatile("movq %%dr1, %0" : "=r"(v)); break;
    case 2: __asm__ volatile("movq %%dr2, %0" : "=r"(v)); break;
    case 3: __asm__ volatile("movq %%dr3, %0" : "=r"(v)); break;
    case 6: __asm__ volatile("movq %%dr6, %0" : "=r"(v)); break;
    case 7: __asm__ volatile("movq %%dr7, %0" : "=r"(v)); break;
    default: break;
    }
    return v;
}

static inline void rhv_write_dr(unsigned n, uint64_t v)
{
    switch (n) {
    case 0: __asm__ volatile("movq %0, %%dr0" : : "r"(v)); break;
    case 1: __asm__ volatile("movq %0, %%dr1" : : "r"(v)); break;
    case 2: __asm__ volatile("movq %0, %%dr2" : : "r"(v)); break;
    case 3: __asm__ volatile("movq %0, %%dr3" : : "r"(v)); break;
    case 6: __asm__ volatile("movq %0, %%dr6" : : "r"(v)); break;
    case 7: __asm__ volatile("movq %0, %%dr7" : : "r"(v)); break;
    default: break;
    }
}

#endif /* RYZENHV_X86_HPP */
