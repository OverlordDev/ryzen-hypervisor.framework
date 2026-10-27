/*
 * rhv-long-test - phase 3 stage 3 acceptance test: a 64-bit paged guest that uses x87, SSE and AVX.
 *
 * The guest runs in long mode (identity-mapped page tables the client builds in guest RAM, a flat GDT) and
 * executes three small programs, assembled into this file as blobs and copied into guest memory:
 *
 *   blob 1  paddd on XMM registers, an x87 add, STMXCSR, then CPUID (an exit) and HLT
 *   blob 2  the same with 256-bit AVX (only when the host has AVX and the client enables XCR0 itself)
 *   blob 3  a ~100 ms loop of paddd: host interrupts take the guest out many times, and every time its XMM state
 *           must come back unchanged
 *
 * What this proves:
 *   - guest x87/SSE/AVX state lives in the kext (VmGetFpu shows it) and is not lost between VMRUNs
 *   - XCR0 is the guest's own (VmSetFpu), the host's is restored
 *   - the host state is restored bit for bit: the kext compares it after every VMRUN (vmFpuMismatches must stay 0)
 *     and this process keeps computing correctly
 *
 * Needs root and a kext booted with rhv_enable_svm=1:
 *     sudo ./build/rhv-long-test
 */
#include <IOKit/IOKitLib.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

#include "../../include/RyzenHVShared.h"

/* ---- guest code ------------------------------------------------------------------------------------- */

__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl _rhv_blob1_start\n_rhv_blob1_start:\n"
    "    movdqu  Lb1_a(%rip), %xmm0\n"
    "    movdqu  Lb1_b(%rip), %xmm1\n"
    "    paddd   %xmm1, %xmm0\n"
    "    movdqu  %xmm0, _rhv_blob1_sse(%rip)\n"
    "    fld1\n"
    "    fld1\n"
    "    faddp\n"
    "    fstpl   _rhv_blob1_x87(%rip)\n"
    "    stmxcsr _rhv_blob1_mxcsr(%rip)\n"
    "    movl    $0x40000000, %eax\n"
    "    cpuid\n"
    "    hlt\n"
    ".p2align 4\n"
    "Lb1_a: .long 0x11111111, 0x22222222, 0x33333333, 0x44444444\n"
    "Lb1_b: .long 1, 2, 3, 4\n"
    ".globl _rhv_blob1_sse\n_rhv_blob1_sse: .space 16\n"
    ".globl _rhv_blob1_x87\n_rhv_blob1_x87: .space 8\n"
    ".globl _rhv_blob1_mxcsr\n_rhv_blob1_mxcsr: .space 8\n"
    ".globl _rhv_blob1_end\n_rhv_blob1_end:\n"

    ".p2align 4\n"
    ".globl _rhv_blob2_start\n_rhv_blob2_start:\n"
    "    vmovdqu Lb2_a(%rip), %ymm0\n"
    "    vpaddd  Lb2_b(%rip), %ymm0, %ymm0\n"
    "    vmovdqu %ymm0, _rhv_blob2_out(%rip)\n"
    "    movl    $0x40000000, %eax\n"
    "    cpuid\n"
    "    hlt\n"
    ".p2align 5\n"
    "Lb2_a: .long 1, 2, 3, 4, 5, 6, 7, 8\n"
    "Lb2_b: .long 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80\n"
    ".globl _rhv_blob2_out\n_rhv_blob2_out: .space 32\n"
    ".globl _rhv_blob2_end\n_rhv_blob2_end:\n"

    ".p2align 4\n"
    ".globl _rhv_blob3_start\n_rhv_blob3_start:\n"
    "    movdqu  Lb3_a(%rip), %xmm2\n"
    "    movdqu  Lb3_inc(%rip), %xmm3\n"
    "    movl    $400000000, %ecx\n"
    "1:  paddd   %xmm3, %xmm2\n"
    "    decl    %ecx\n"
    "    jnz     1b\n"
    "    movdqu  %xmm2, _rhv_blob3_out(%rip)\n"
    "    hlt\n"
    ".p2align 4\n"
    "Lb3_a: .long 0xA0000000, 0xB0000000, 0xC0000000, 0xD0000000\n"
    "Lb3_inc: .long 1, 3, 5, 7\n"
    ".globl _rhv_blob3_out\n_rhv_blob3_out: .space 16\n"
    ".globl _rhv_blob3_end\n_rhv_blob3_end:\n"
);

extern const uint8_t rhv_blob1_start[], rhv_blob1_end[], rhv_blob1_sse[], rhv_blob1_x87[], rhv_blob1_mxcsr[];
extern const uint8_t rhv_blob2_start[], rhv_blob2_end[], rhv_blob2_out[];
extern const uint8_t rhv_blob3_start[], rhv_blob3_end[], rhv_blob3_out[];

#define BLOB1 0x1000
#define BLOB2 0x1400
#define BLOB3 0x1800

/* ---- plumbing --------------------------------------------------------------------------------------- */

enum {
    VMCS_GUEST_ES = 0x800, VMCS_GUEST_ES_LIMIT = 0x4800, VMCS_GUEST_ES_AR = 0x4814, VMCS_GUEST_ES_BASE = 0x6806,
    VMCS_GUEST_GDTR_LIMIT = 0x4810, VMCS_GUEST_IDTR_LIMIT = 0x4812,
    VMCS_GUEST_GDTR_BASE = 0x6816, VMCS_GUEST_IDTR_BASE = 0x6818,
    VMCS_GUEST_CR0 = 0x6800, VMCS_GUEST_CR3 = 0x6802, VMCS_GUEST_CR4 = 0x6804,
    VMCS_GUEST_RSP = 0x681c, VMCS_GUEST_RIP = 0x681e, VMCS_GUEST_RFLAGS = 0x6820,
    VMCS_GUEST_IA32_EFER = 0x2806,
    VMCS_CTRL_CPU_BASED = 0x4002, VMCS_CTRL_VMENTRY_CONTROLS = 0x4012,
    VMCS_RO_EXIT_REASON = 0x4402, VMCS_RO_INSTR_LEN = 0x440c
};
#define CPU_HLT_EXIT (1u << 7)
#define ENTRY_IA32E_GUEST (1u << 9)
#define EXIT_CPUID 10
#define EXIT_HLT 12

static int failures;
#define EXPECT(cond, ...)                                                       \
    do {                                                                        \
        if (cond) {                                                             \
            printf("  ok: ");                                                   \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        } else {                                                                \
            printf("  FAIL (line %d): ", __LINE__);                             \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
            failures++;                                                         \
        }                                                                       \
    } while (0)

static io_connect_t conn;

static kern_return_t vmcs_write(uint32_t f, uint64_t v)
{
    uint64_t in[2] = { f, v };
    return IOConnectCallScalarMethod(conn, kRHVMethodVmcsWrite, in, 2, NULL, NULL);
}

static uint64_t vmcs_read(uint32_t f)
{
    uint64_t in = f, out = 0;
    uint32_t cnt = 1;
    IOConnectCallScalarMethod(conn, kRHVMethodVmcsRead, &in, 1, &out, &cnt);
    return out;
}

static int get_fpu(RHVFpuState *s)
{
    size_t sz = sizeof(*s);
    return IOConnectCallStructMethod(conn, kRHVMethodVmGetFpu, NULL, 0, s, &sz) != KERN_SUCCESS;
}

static kern_return_t set_fpu(const RHVFpuState *s)
{
    return IOConnectCallStructMethod(conn, kRHVMethodVmSetFpu, s, sizeof(*s), NULL, NULL);
}

static unsigned total_runs, total_timeslices;

/* Runs until the guest exits for a reason the client must handle. Counts how often it was taken out for the host. */
static int run_guest(void)
{
    for (int i = 0; i < 100000; i++) {
        uint64_t skip = 0;
        RHVExit ex;
        size_t sz = sizeof(ex);
        kern_return_t kr = IOConnectCallMethod(conn, kRHVMethodVmRun, &skip, 1, NULL, 0, NULL, NULL, &ex, &sz);
        total_runs++;
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "VmRun failed: 0x%x\n", kr);
            return -1;
        }
        if (ex.reason != RHV_EXIT_TIMESLICE)
            return 0;
        total_timeslices++;
    }
    return -1;
}

static uint64_t mismatches(void)
{
    RHVStats s;
    size_t sz = sizeof(s);
    if (IOConnectCallStructMethod(conn, kRHVMethodGetStats, NULL, 0, &s, &sz) != KERN_SUCCESS)
        return ~0ull;
    return s.vmFpuMismatches;
}

/* 64-bit guest: flat segments, paging on, identity map of the first 2 MiB, GDT at 0x9000, tables at 0x10000. */
static void program_long_mode(uint8_t *ram, uint64_t rip)
{
    static const uint64_t gdt[3] = { 0, 0x00AF9B000000FFFFull, 0x00CF93000000FFFFull };
    memcpy(ram + 0x9000, gdt, sizeof(gdt));
    uint64_t *pml4 = (uint64_t *)(ram + 0x10000), *pdpt = (uint64_t *)(ram + 0x11000), *pd = (uint64_t *)(ram + 0x12000);
    memset(pml4, 0, 4096); memset(pdpt, 0, 4096); memset(pd, 0, 4096);
    pml4[0] = 0x11000 | 3;
    pdpt[0] = 0x12000 | 3;
    pd[0]   = 0x83;                      /* present | write | 2 MiB page at 0 */

    static const uint32_t sel[8] = { 0x800, 0x802, 0x804, 0x806, 0x808, 0x80a, 0x80c, 0x80e };
    for (int i = 0; i < 8; i++) {
        uint16_t s = i == 1 ? 0x08 : (i == 6 || i == 7) ? 0 : 0x10;
        uint32_t ar = i == 1 ? 0xA09B : i == 6 ? 0x10000 : i == 7 ? 0x8B : 0xC093;
        vmcs_write(sel[i], s);
        vmcs_write(VMCS_GUEST_ES_LIMIT + 2 * i, i == 7 ? 0x67 : 0xFFFFFFFF);
        vmcs_write(VMCS_GUEST_ES_BASE + 2 * i, 0);
        vmcs_write(VMCS_GUEST_ES_AR + 2 * i, ar);
    }
    vmcs_write(VMCS_GUEST_GDTR_BASE, 0x9000);
    vmcs_write(VMCS_GUEST_GDTR_LIMIT, 0x17);
    vmcs_write(VMCS_GUEST_IDTR_BASE, 0);
    vmcs_write(VMCS_GUEST_IDTR_LIMIT, 0);
    vmcs_write(VMCS_GUEST_CR0, 0x80010033);              /* PG | WP | NE | ET | MP | PE */
    vmcs_write(VMCS_GUEST_CR3, 0x10000);
    vmcs_write(VMCS_GUEST_CR4, 0x620);                   /* PAE | OSFXSR | OSXMMEXCPT */
    vmcs_write(VMCS_GUEST_IA32_EFER, 0xD00);             /* LME | LMA | NXE */
    vmcs_write(VMCS_CTRL_VMENTRY_CONTROLS, ENTRY_IA32E_GUEST);
    vmcs_write(VMCS_GUEST_RSP, 0x8000);
    vmcs_write(VMCS_GUEST_RFLAGS, 2);
    vmcs_write(VMCS_GUEST_RIP, rip);
    vmcs_write(VMCS_CTRL_CPU_BASED, CPU_HLT_EXIT);
}

static int host_has_avx(void)
{
    int v = 0;
    size_t l = sizeof(v);
    return sysctlbyname("hw.optional.avx1_0", &v, &l, NULL, 0) == 0 && v;
}

int main(void)
{
    io_service_t svc = IOServiceGetMatchingService(0, IOServiceMatching(RHV_SERVICE_NAME));
    if (!svc) {
        fprintf(stderr, "RyzenHV service not found. Is the kext loaded?\n");
        return 1;
    }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen failed: 0x%x\n", kr);
        return 1;
    }

    uint64_t flags = RHV_VMCREATE_VMX, ramSize = 0;
    uint32_t cnt = 1;
    kr = IOConnectCallScalarMethod(conn, kRHVMethodVmCreate, &flags, 1, &ramSize, &cnt);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "VmCreate failed: 0x%x (ABI %d: is the kext new enough?)\n", kr, RHV_ABI_VERSION);
        return 1;
    }
    mach_vm_address_t ramAddr = 0;
    mach_vm_size_t sz = 0;
    if (IOConnectMapMemory64(conn, kRHVMemGuestRam, mach_task_self(), &ramAddr, &sz, kIOMapAnywhere) != KERN_SUCCESS) {
        fprintf(stderr, "map guest RAM failed\n");
        return 1;
    }
    uint8_t *ram = (uint8_t *)(uintptr_t)ramAddr;

    memcpy(ram + BLOB1, rhv_blob1_start, (size_t)(rhv_blob1_end - rhv_blob1_start));
    memcpy(ram + BLOB2, rhv_blob2_start, (size_t)(rhv_blob2_end - rhv_blob2_start));
    memcpy(ram + BLOB3, rhv_blob3_start, (size_t)(rhv_blob3_end - rhv_blob3_start));

    /* A computation of our own that depends on the FPU registers, so we notice if the kext ever corrupts them. */
    volatile double hostAcc = 1.0;
    for (int i = 0; i < 2000; i++)
        hostAcc = hostAcc * 1.0001 + 0.25;
    double hostExpected = hostAcc;

    RHVFpuState fs;
    printf("stage 1: long mode, x87 and SSE\n");
    program_long_mode(ram, BLOB1);
    if (get_fpu(&fs))
        goto done;
    printf("  (XSAVE area %u bytes, guest XCR0 starts at 0x%" PRIx64 ")\n", fs.size, fs.xcr0);
    EXPECT(fs.xcr0 == 1, "guest XCR0 starts as x87 only");
    if (run_guest())
        goto done;
    EXPECT(vmcs_read(VMCS_RO_EXIT_REASON) == EXIT_CPUID, "the guest reached CPUID in 64-bit mode (reason %" PRIu64 ")", vmcs_read(VMCS_RO_EXIT_REASON));
    uint32_t sse[4], expectSse[4] = { 0x11111112, 0x22222224, 0x33333336, 0x44444448 };
    memcpy(sse, ram + BLOB1 + (rhv_blob1_sse - rhv_blob1_start), 16);
    EXPECT(memcmp(sse, expectSse, 16) == 0, "SSE paddd result %08x %08x %08x %08x", sse[0], sse[1], sse[2], sse[3]);
    double x87;
    memcpy(&x87, ram + BLOB1 + (rhv_blob1_x87 - rhv_blob1_start), 8);
    EXPECT(x87 == 2.0, "x87 fld1+fld1+faddp = %g", x87);
    uint32_t mx;
    memcpy(&mx, ram + BLOB1 + (rhv_blob1_mxcsr - rhv_blob1_start), 4);
    EXPECT(mx == 0x1F80, "MXCSR has its power-on value (0x%x)", mx);

    /* The state sits in the kext now: XMM0 and XMM1 are in the XSAVE image. */
    if (get_fpu(&fs))
        goto done;
    EXPECT(memcmp(fs.area + 160, expectSse, 16) == 0, "VmGetFpu: XMM0 holds the sum");
    uint32_t b[4] = { 1, 2, 3, 4 };
    EXPECT(memcmp(fs.area + 160 + 16, b, 16) == 0, "VmGetFpu: XMM1 holds the addend");
    EXPECT(fs.xcr0 == 1, "XCR0 is still the guest's own value");
    EXPECT(mismatches() == 0, "host x87/SSE state was restored bit for bit (mismatches: %" PRIu64 ")", mismatches());

    /* A bad state is refused instead of crashing the kernel in XRSTOR. */
    RHVFpuState bad = fs;
    bad.xcr0 = 0;
    EXPECT(set_fpu(&bad) != KERN_SUCCESS, "XCR0 = 0 is refused");
    bad = fs; bad.xcr0 = 5;
    EXPECT(set_fpu(&bad) != KERN_SUCCESS, "XCR0 with AVX but no SSE is refused");
    bad = fs; bad.area[528] = 1;
    EXPECT(set_fpu(&bad) != KERN_SUCCESS, "a reserved bit in the XSAVE header is refused");
    bad = fs; bad.size = fs.size + 64;
    EXPECT(set_fpu(&bad) != KERN_SUCCESS, "a wrong size is refused");
    bad = fs; bad.area[24 + 2] = 1;                  /* MXCSR bit 17 */
    EXPECT(set_fpu(&bad) != KERN_SUCCESS, "a reserved MXCSR bit is refused");

    /* ---- AVX ---- */
    printf("stage 2: AVX with a client-emulated XSETBV\n");
    if (!host_has_avx()) {
        printf("  (this host has no AVX, skipped)\n");
    } else {
        fs.xcr0 = 7;                                              /* what the guest OS would set with XSETBV */
        EXPECT(set_fpu(&fs) == KERN_SUCCESS, "VmSetFpu accepted XCR0 = 7");
        vmcs_write(VMCS_GUEST_CR4, vmcs_read(VMCS_GUEST_CR4) | (1u << 18));      /* OSXSAVE */
        vmcs_write(VMCS_GUEST_RIP, BLOB2);
        if (run_guest())
            goto done;
        EXPECT(vmcs_read(VMCS_RO_EXIT_REASON) == EXIT_CPUID, "the AVX program reached CPUID (reason %" PRIu64 ")", vmcs_read(VMCS_RO_EXIT_REASON));
        uint32_t avx[8], expectAvx[8];
        for (int i = 0; i < 8; i++)
            expectAvx[i] = (uint32_t)(i + 1) + 0x10u * (uint32_t)(i + 1);
        memcpy(avx, ram + BLOB2 + (rhv_blob2_out - rhv_blob2_start), 32);
        EXPECT(memcmp(avx, expectAvx, 32) == 0, "256-bit vpaddd result is right");
        if (get_fpu(&fs))
            goto done;
        EXPECT(fs.xcr0 == 7, "XCR0 stayed 7");
        EXPECT(memcmp(fs.area + 576, expectAvx + 4, 16) == 0, "VmGetFpu: the upper half of YMM0 is in the AVX component");
        EXPECT(memcmp(fs.area + 160, expectAvx, 16) == 0, "VmGetFpu: the lower half of YMM0 is in XMM0");
        EXPECT(mismatches() == 0, "host state still intact (mismatches: %" PRIu64 ")", mismatches());
    }

    /* ---- Host interrupts in the middle of guest SSE ---- */
    printf("stage 3: ~100 ms of guest SSE with host interrupts\n");
    vmcs_write(VMCS_GUEST_RIP, BLOB3);
    total_runs = total_timeslices = 0;
    if (run_guest())
        goto done;
    EXPECT(vmcs_read(VMCS_RO_EXIT_REASON) == EXIT_HLT, "the loop finished (reason %" PRIu64 ")", vmcs_read(VMCS_RO_EXIT_REASON));
    uint32_t loop[4], expectLoop[4], a[4] = { 0xA0000000u, 0xB0000000u, 0xC0000000u, 0xD0000000u }, inc[4] = { 1, 3, 5, 7 };
    for (int i = 0; i < 4; i++)
        expectLoop[i] = a[i] + 400000000u * inc[i];
    memcpy(loop, ram + BLOB3 + (rhv_blob3_out - rhv_blob3_start), 16);
    EXPECT(memcmp(loop, expectLoop, 16) == 0, "XMM2 survived every interruption: %08x %08x %08x %08x", loop[0], loop[1], loop[2], loop[3]);
    printf("  (VmRun was called %u times, %u of them returned for a time slice)\n", total_runs, total_timeslices);
    EXPECT(total_timeslices >= 2, "host interrupts really took the guest out (time slices: %u)", total_timeslices);
    EXPECT(mismatches() == 0, "host state intact after thousands of VMRUNs (mismatches: %" PRIu64 ")", mismatches());

    /* Our own floating point still gives the same answer. */
    volatile double again = 1.0;
    for (int i = 0; i < 2000; i++)
        again = again * 1.0001 + 0.25;
    EXPECT(again == hostExpected && fabs(hostAcc - hostExpected) == 0.0, "this process's own floating point is unaffected");

done:
    IOConnectUnmapMemory64(conn, kRHVMemGuestRam, mach_task_self(), ramAddr);
    IOConnectCallScalarMethod(conn, kRHVMethodVmDestroy, NULL, 0, NULL, NULL);
    IOServiceClose(conn);
    if (failures) {
        printf("FAIL: %d check(s) failed, see `rhv-info log`\n", failures);
        return 1;
    }
    printf("PASS: a 64-bit paged guest with x87/SSE/AVX state runs and the host state survives\n");
    return 0;
}
