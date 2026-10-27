/*
 * fpu_state_test.cpp - host tests for kext/Source/FpuState.hpp, including the real XSAVE/XRSTOR wrappers
 * (they work in user mode; only XSETBV needs the kernel).
 */
#include <cpuid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kext/Source/FpuState.hpp"

using namespace rhv::fpu;

namespace {

int fails = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
            fails++;                                                      \
        }                                                                 \
    } while (0)

void testXcr0()
{
    const uint64_t avx2cpu = XCR0_X87 | XCR0_SSE | XCR0_AVX;          /* what a Zen 3 offers */
    const uint64_t avx512cpu = avx2cpu | XCR0_AVX512 | XCR0_PKRU;
    CHECK(validXcr0(1, avx2cpu));                                       /* x87 only */
    CHECK(validXcr0(3, avx2cpu));                                       /* + SSE */
    CHECK(validXcr0(7, avx2cpu));                                       /* + AVX */
    CHECK(!validXcr0(0, avx2cpu));                                      /* x87 cannot be off */
    CHECK(!validXcr0(2, avx2cpu));                                      /* x87 cannot be off */
    CHECK(!validXcr0(5, avx2cpu));                                      /* AVX without SSE */
    CHECK(!validXcr0(7 | XCR0_OPMASK, avx2cpu));                        /* beyond what the CPU supports */
    CHECK(!validXcr0(1ull << 63, avx2cpu));
    CHECK(validXcr0(7 | XCR0_AVX512, avx512cpu));
    CHECK(!validXcr0(7 | XCR0_OPMASK, avx512cpu));                      /* AVX-512 components come as a set */
    CHECK(!validXcr0(7 | XCR0_OPMASK | XCR0_ZMM_HI256, avx512cpu));
    CHECK(!validXcr0(3 | XCR0_AVX512, avx512cpu));                      /* AVX-512 needs AVX */
    CHECK(validXcr0(7 | XCR0_PKRU, avx512cpu));
}

void testLegacyEqual()
{
    alignas(64) uint8_t a[512], b[512];
    memset(a, 0x5A, sizeof(a));
    memcpy(b, a, sizeof(a));
    CHECK(legacyEqual(a, b));
    b[28] ^= 0xFF;                                  /* MXCSR_MASK is a read-only report: not compared */
    CHECK(legacyEqual(a, b));
    b[28] = a[28];
    b[24] ^= 1;                                     /* MXCSR */
    CHECK(!legacyEqual(a, b));
    b[24] = a[24];
    b[160] ^= 1;                                    /* XMM0 */
    CHECK(!legacyEqual(a, b));
    b[160] = a[160];
    b[0] ^= 1;                                      /* x87 FCW */
    CHECK(!legacyEqual(a, b));
    b[0] = a[0];
    b[511] ^= 1;                                    /* XMM15 */
    CHECK(!legacyEqual(a, b));
}

void testArea()
{
    alignas(64) uint8_t a[2688];
    initArea(a, sizeof(a));
    uint32_t mxcsr;
    memcpy(&mxcsr, a + 24, 4);
    CHECK(mxcsr == 0x1F80);
    CHECK(validArea(a, sizeof(a), 7));
    CHECK(!validArea(a, 500, 7));                                       /* too small to have a header */

    uint64_t bv = 7;
    memcpy(a + 512, &bv, 8);
    CHECK(validArea(a, sizeof(a), 7));
    CHECK(!validArea(a, sizeof(a), 3));                                 /* sets AVX, XCR0 has none */
    bv = 7; memcpy(a + 512, &bv, 8);
    uint64_t comp = 1ull << 63;                                         /* compacted format */
    memcpy(a + 520, &comp, 8);
    CHECK(!validArea(a, sizeof(a), 7));
    comp = 0; memcpy(a + 520, &comp, 8);
    a[540] = 1;                                                         /* reserved header byte */
    CHECK(!validArea(a, sizeof(a), 7));
    a[540] = 0;
    mxcsr = 0x1F80 | (1u << 20);                                        /* reserved MXCSR bit */
    memcpy(a + 24, &mxcsr, 4);
    CHECK(!validArea(a, sizeof(a), 7));
}

/* Real instructions: save the registers, wreck them, restore, compare. */
void testRoundTrip()
{
    uint32_t eax, ebx, ecx, edx;
    CHECK(__get_cpuid_count(0xD, 0, &eax, &ebx, &ecx, &edx));
    size_t size = ebx;                                                  /* for the currently enabled XCR0 */
    CHECK(size >= 576);
    uint64_t xcr0 = xgetbv0();
    CHECK(validXcr0(xcr0, ((uint64_t)edx << 32) | eax));
    printf("  (host XCR0 = 0x%llx, XSAVE area %zu bytes)\n", (unsigned long long)xcr0, size);

    uint8_t *area = (uint8_t *)aligned_alloc(64, (size + 63) & ~(size_t)63);
    uint8_t *again = (uint8_t *)aligned_alloc(64, (size + 63) & ~(size_t)63);
    memset(area, 0, size);
    memset(again, 0, size);

    alignas(32) uint8_t pat[32], got[32], ypat[32], ygot[32];
    for (int i = 0; i < 32; i++) { pat[i] = (uint8_t)(0xA0 + i); ypat[i] = (uint8_t)(0x30 + 3 * i); }
    uint32_t mx = 0x1FA0;

    __asm__ volatile("movdqu %0, %%xmm3" : : "m"(*(uint8_t(*)[16])pat) : "xmm3");
    __asm__ volatile("ldmxcsr %0" : : "m"(mx));
    bool avx = (xcr0 & XCR0_AVX) != 0;
    if (avx)
        __asm__ volatile("vmovdqu %0, %%ymm5" : : "m"(*(uint8_t(*)[32])ypat) : "xmm5");

    xsave(area, xcr0);
    CHECK(validArea(area, size, xcr0));                                 /* what hardware writes must pass our validation */

    /* Wreck the state. */
    __asm__ volatile("pxor %%xmm3, %%xmm3" : : : "xmm3");
    if (avx)
        __asm__ volatile("vpxor %%ymm5, %%ymm5, %%ymm5" : : : "xmm5");
    uint32_t wreck = 0x1F80;
    __asm__ volatile("ldmxcsr %0" : : "m"(wreck));

    xrstor(area, xcr0);
    __asm__ volatile("movdqu %%xmm3, %0" : "=m"(*(uint8_t(*)[16])got));
    if (avx)
        __asm__ volatile("vmovdqu %%ymm5, %0" : "=m"(*(uint8_t(*)[32])ygot));
    uint32_t mxback;
    __asm__ volatile("stmxcsr %0" : "=m"(mxback));

    CHECK(memcmp(got, pat, 16) == 0);
    if (avx)
        CHECK(memcmp(ygot, ypat, 32) == 0);
    CHECK(mxback == mx);

    /* A second save shows the same XMM3 and MXCSR. (Not the whole legacy region: this test is compiled with SSE,
     * so the compiler may use other XMM registers in between. The kext is built with -mkernel and does not.) */
    xsave(again, xcr0);
    CHECK(memcmp(area + 160 + 16 * 3, again + 160 + 16 * 3, 16) == 0);
    CHECK(memcmp(area + 24, again + 24, 4) == 0);

    /* The init-state area is accepted by XRSTOR with the host's XCR0 and resets the registers. */
    initArea(area, size);
    CHECK(validArea(area, size, xcr0));
    xrstor(area, xcr0);
    __asm__ volatile("movdqu %%xmm3, %0" : "=m"(*(uint8_t(*)[16])got));
    uint8_t zero[16] = {};
    CHECK(memcmp(got, zero, 16) == 0);
    __asm__ volatile("stmxcsr %0" : "=m"(mxback));
    CHECK(mxback == 0x1F80);

    free(area);
    free(again);
}

} // namespace

int main()
{
    testXcr0();
    testLegacyEqual();
    testArea();
    testRoundTrip();
    if (fails) {
        printf("fpu_state_test: %d FAILED\n", fails);
        return 1;
    }
    printf("fpu_state_test: OK\n");
    return 0;
}
