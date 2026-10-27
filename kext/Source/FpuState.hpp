/*
 * FpuState.hpp - extended processor state (x87, SSE, AVX) around VMRUN.
 *
 * VMRUN saves and restores none of it. If the kernel does not do it, a guest that executes a single SSE
 * instruction silently corrupts the registers of whatever user process called VmRun. The kext therefore does,
 * with interrupts off right around VMRUN:
 *
 *      XSAVE host state -> XSETBV (guest XCR0) -> XRSTOR guest state -> VMRUN
 *      -> XSAVE guest state -> XSETBV (host XCR0) -> XRSTOR host state
 *
 * The validation and layout helpers are pure and tested on the host (tests/fpu_state_test.cpp); the wrappers
 * around XGETBV/XSETBV/XSAVE/XRSTOR are inline assembly that is only executed in the kext.
 *
 * Layout (Intel SDM Vol. 1, 13.4, standard non-compacted format): legacy region 0..511 (x87 + MXCSR + XMM0-15),
 * XSAVE header 512..575 (XSTATE_BV at 512, XCOMP_BV at 520, rest reserved), then the components by CPUID.0xD.
 */
#ifndef RYZENHV_FPUSTATE_HPP
#define RYZENHV_FPUSTATE_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace rhv {
namespace fpu {

/* XCR0 state-component bits. */
enum : uint64_t {
    XCR0_X87       = 1ull << 0,
    XCR0_SSE       = 1ull << 1,
    XCR0_AVX       = 1ull << 2,
    XCR0_OPMASK    = 1ull << 5,
    XCR0_ZMM_HI256 = 1ull << 6,
    XCR0_HI16_ZMM  = 1ull << 7,
    XCR0_PKRU      = 1ull << 9
};
static const uint64_t XCR0_AVX512 = XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM;

static const size_t   kLegacySize = 512;
static const size_t   kHeaderEnd  = 576;
static const uint32_t kMxcsrDefault = 0x1F80;
static const uint32_t kMxcsrOffset  = 24;

/* Is `value` a legal XCR0 for a CPU that supports `supported`? The rules XSETBV itself enforces. */
static inline bool validXcr0(uint64_t value, uint64_t supported)
{
    if (value & ~supported)
        return false;
    if (!(value & XCR0_X87))
        return false;                              /* x87 can never be off */
    if ((value & XCR0_AVX) && !(value & XCR0_SSE))
        return false;                              /* AVX needs SSE */
    uint64_t k = value & XCR0_AVX512;
    if (k && k != XCR0_AVX512)
        return false;                              /* the AVX-512 components come as a set */
    if (k && !(value & XCR0_AVX))
        return false;                              /* ...and need AVX */
    return true;
}

/* A fresh state: everything in its init state, MXCSR at its power-on value. */
static inline void initArea(uint8_t *area, size_t size)
{
    memset(area, 0, size);
    uint32_t mxcsr = kMxcsrDefault;
    memcpy(area + kMxcsrOffset, &mxcsr, sizeof(mxcsr));
}

/*
 * Is `area` safe to hand to XRSTOR under `xcr0`? XRSTOR raises #GP, which is a kernel panic here, for a header
 * that sets unsupported components, a compacted-format header, reserved bits, or MXCSR reserved bits.
 */
static inline bool validArea(const uint8_t *area, size_t size, uint64_t xcr0)
{
    if (size < kHeaderEnd)
        return false;
    uint64_t xstateBv, xcompBv;
    memcpy(&xstateBv, area + 512, 8);
    memcpy(&xcompBv, area + 520, 8);
    if (xstateBv & ~xcr0)
        return false;
    if (xcompBv != 0)
        return false;                              /* we use the standard format only */
    for (size_t i = 528; i < kHeaderEnd; i++) {
        if (area[i])
            return false;
    }
    uint32_t mxcsr;
    memcpy(&mxcsr, area + kMxcsrOffset, sizeof(mxcsr));
    if (mxcsr & ~0xFFFFu)
        return false;                              /* bits 31:16 are reserved on every CPU */
    return true;
}

/* Number of bytes of the legacy region that must be identical after a save/restore round trip. */
static inline bool legacyEqual(const uint8_t *a, const uint8_t *b)
{
    /* x87 + MXCSR + XMM0-15, except MXCSR_MASK (bytes 28..31), which is a read-only report. */
    return memcmp(a, b, 28) == 0 && memcmp(a + 32, b + 32, kLegacySize - 32) == 0;
}

/* ---- inline assembly, kernel only ---------------------------------------------------------------- */

static inline uint64_t xgetbv0()
{
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((uint64_t)hi << 32) | lo;
}

static inline void xsetbv0(uint64_t value)
{
    __asm__ volatile("xsetbv" : : "a"((uint32_t)value), "d"((uint32_t)(value >> 32)), "c"(0) : "memory");
}

/*
 * CR0.TS ("task switched") makes XSAVE/XRSTOR/SSE raise #NM. XNU sets it for a thread that has not used the FPU
 * yet, so before touching extended state the kext clears it, and puts it back afterwards. Interrupts are off, so
 * the CPU and the TS bit cannot change under us. Returns whether TS was set.
 */
static inline bool clearTs()
{
    uint64_t cr0;
    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
    if (!(cr0 & 8))
        return false;
    __asm__ volatile("clts" : : : "memory");
    return true;
}

static inline void restoreTs(bool wasSet)
{
    if (!wasSet)
        return;
    uint64_t cr0;
    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
    cr0 |= 8;
    __asm__ volatile("movq %0, %%cr0" : : "r"(cr0) : "memory");
}

/* `area` must be 64-byte aligned and large enough for the components in `mask` (a subset of the current XCR0). */
static inline void xsave(void *area, uint64_t mask)
{
    __asm__ volatile("xsave64 (%0)" : : "r"(area), "a"((uint32_t)mask), "d"((uint32_t)(mask >> 32)) : "memory");
}

static inline void xrstor(const void *area, uint64_t mask)
{
    __asm__ volatile("xrstor64 (%0)" : : "r"(area), "a"((uint32_t)mask), "d"((uint32_t)(mask >> 32)) : "memory");
}

} // namespace fpu
} // namespace rhv

#endif /* RYZENHV_FPUSTATE_HPP */
