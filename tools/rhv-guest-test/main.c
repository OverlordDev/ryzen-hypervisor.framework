/*
 * rhv-guest-test - the phase 1 acceptance test.
 *
 * Creates a VM, puts a few real-mode instructions into guest RAM and runs them:
 *
 *   0x1000  mov dx, 0x3F8          ; BA F8 03
 *   0x1003  mov si, 0x1100         ; BE 00 11
 *   0x1006  lodsb                  ; AC            <-+
 *   0x1007  test al, al            ; 84 C0           |
 *   0x1009  jz  0x100E             ; 74 03           |
 *   0x100B  out dx, al             ; EE   (exit: IO) |
 *   0x100C  jmp 0x1006             ; EB F8         --+
 *   0x100E  mov eax, 0x40000000    ; 66 B8 00 00 00 40
 *   0x1014  cpuid                  ; 0F A2         (exit: CPUID)
 *   0x1016  hlt                    ; F4            (exit: HLT)
 *   0x1017  jmp 0x1016             ; EB FD
 *
 * and the string at 0x1100. We play the part of the "hardware": every OUT to
 * port 0x3F8 prints a character, CPUID leaf 0x40000000 answers 0x52485631.
 *
 * Needs root and a kext booted with rhv_enable_svm=1:
 *     sudo ./build/rhv-guest-test
 */
#include <IOKit/IOKitLib.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "../../include/RyzenHVShared.h"

#define CODE_ADDR 0x1000
#define MSG_ADDR  0x1100
#define SERIAL    0x3F8
#define CPUID_MAGIC 0x52485631u   /* "RHV1" */
#define MAX_EXITS 100000

static const uint8_t kCode[] = {
    0xBA, 0xF8, 0x03,                   /* mov dx, 0x3F8       */
    0xBE, 0x00, 0x11,                   /* mov si, 0x1100      */
    0xAC,                               /* lodsb               */
    0x84, 0xC0,                         /* test al, al         */
    0x74, 0x03,                         /* jz +3               */
    0xEE,                               /* out dx, al          */
    0xEB, 0xF8,                         /* jmp -8              */
    0x66, 0xB8, 0x00, 0x00, 0x00, 0x40, /* mov eax, 0x40000000 */
    0x0F, 0xA2,                         /* cpuid               */
    0xF4,                               /* hlt                 */
    0xEB, 0xFD,                         /* jmp -3 (back to hlt)*/
};

static const char kMessage[] = "Hello from the guest!\n";

static const char *reason_name(uint32_t r)
{
    static const char *const kNames[] = {
        "NONE", "TIMESLICE", "HLT", "IO", "CPUID", "MSR", "NPF", "SHUTDOWN", "INVALID", "VMMCALL",
        "UNHANDLED",
    };
    return r < sizeof(kNames) / sizeof(kNames[0]) ? kNames[r] : "?";
}

static void print_regs(const RHVRegs *r)
{
    printf("  rax=%016" PRIx64 " rbx=%016" PRIx64 " rcx=%016" PRIx64 " rdx=%016" PRIx64 "\n", r->rax,
           r->rbx, r->rcx, r->rdx);
    printf("  rsi=%016" PRIx64 " rdi=%016" PRIx64 " rsp=%016" PRIx64 " rip=%016" PRIx64 "\n", r->rsi,
           r->rdi, r->rsp, r->rip);
}

static int call(io_connect_t conn, uint32_t sel, const char *name, const uint64_t *in,
                uint32_t inCnt, uint64_t *out, uint32_t *outCnt, const void *inStruct,
                size_t inStructSize, void *outStruct, size_t *outStructSize)
{
    kern_return_t kr = IOConnectCallMethod(conn, sel, in, inCnt, inStruct, inStructSize, out,
                                           outCnt, outStruct, outStructSize);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "%s failed: 0x%x\n", name, kr);
        return 1;
    }
    return 0;
}

int main(void)
{
    io_service_t svc = IOServiceGetMatchingService(0, IOServiceMatching(RHV_SERVICE_NAME));
    if (!svc) {
        fprintf(stderr, "RyzenHV service not found. Is the kext loaded?\n");
        return 1;
    }
    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen failed: 0x%x\n", kr);
        return 1;
    }

    int rc = 1;
    mach_vm_address_t ram = 0;
    mach_vm_size_t ramSize = 0;
    int mapped = 0;

    /* 1. Create the VM. Fails unless SVM is active (rhv_enable_svm=1) and we are root. */
    uint64_t createFlags = 0;     /* 0 = with the built-in 1 MiB of guest RAM */
    uint64_t out[2] = { 0, 0 };
    uint32_t outCnt = 1;
    kr = IOConnectCallScalarMethod(conn, kRHVMethodVmCreate, &createFlags, 1, out, &outCnt);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "VmCreate failed: 0x%x (root? rhv_enable_svm=1? see `rhv-info log`)\n", kr);
        goto done;
    }
    printf("VM created, %" PRIu64 " KiB guest RAM\n", out[0] / 1024);

    /* 2. Map guest RAM and put the program in it. */
    kr = IOConnectMapMemory64(conn, kRHVMemGuestRam, mach_task_self(), &ram, &ramSize,
                              kIOMapAnywhere);
    if (kr != KERN_SUCCESS || ramSize < MSG_ADDR + sizeof(kMessage)) {
        fprintf(stderr, "map guest RAM failed: 0x%x (size %llu)\n", kr, (unsigned long long)ramSize);
        goto done;
    }
    mapped = 1;
    memcpy((void *)(uintptr_t)(ram + CODE_ADDR), kCode, sizeof(kCode));
    memcpy((void *)(uintptr_t)(ram + MSG_ADDR), kMessage, sizeof(kMessage));

    /* 3. Real-mode reset state, CS:IP = 0000:1000. */
    uint64_t resetArgs[2] = { 0, CODE_ADDR };
    if (call(conn, kRHVMethodVmReset, "VmReset", resetArgs, 2, NULL, NULL, NULL, 0, NULL, NULL))
        goto done;

    /* 4. Run until the guest halts, answering its I/O and CPUID. */
    uint64_t skip = 0;
    int sawCpuid = 0, halted = 0;
    unsigned exits = 0;

    printf("--- guest output ---\n");
    while (exits++ < MAX_EXITS) {
        RHVExit ex;
        size_t exSize = sizeof(ex);
        if (call(conn, kRHVMethodVmRun, "VmRun", &skip, 1, NULL, NULL, NULL, 0, &ex, &exSize))
            goto done;
        skip = 0;

        if (ex.reason == RHV_EXIT_TIMESLICE)
            continue;

        RHVRegs regs;
        size_t regsSize = sizeof(regs);
        if (call(conn, kRHVMethodVmGetRegs, "VmGetRegs", NULL, 0, NULL, NULL, NULL, 0, &regs,
                 &regsSize))
            goto done;

        if (ex.reason == RHV_EXIT_IO && !rhv_ioio_is_in(ex.info1) &&
            rhv_ioio_port(ex.info1) == SERIAL) {
            putchar((int)(regs.rax & 0xFF));
            fflush(stdout);
            skip = 1;
        } else if (ex.reason == RHV_EXIT_CPUID) {
            printf("--- guest executed CPUID leaf 0x%" PRIx64 " ---\n", regs.rax & 0xFFFFFFFF);
            regs.rax = CPUID_MAGIC;
            regs.rbx = regs.rcx = regs.rdx = 0;
            if (call(conn, kRHVMethodVmSetRegs, "VmSetRegs", NULL, 0, NULL, NULL, &regs,
                     sizeof(regs), NULL, NULL))
                goto done;
            sawCpuid = 1;
            skip = 1;
        } else if (ex.reason == RHV_EXIT_HLT) {
            halted = 1;
            printf("--- guest halted at rip 0x%" PRIx64 " ---\n", ex.rip);
            print_regs(&regs);
            if (sawCpuid && (regs.rax & 0xFFFFFFFF) == CPUID_MAGIC) {
                printf("PASS: output, CPUID and HLT all handled (%u exits)\n", exits);
                rc = 0;
            } else {
                printf("FAIL: halted, but CPUID result not seen (eax=0x%" PRIx64 ")\n", regs.rax);
            }
            break;
        } else {
            printf("--- unexpected exit: %s (code 0x%" PRIx64 " info1 0x%" PRIx64 " info2 0x%" PRIx64
                   " rip 0x%" PRIx64 ") ---\n",
                   reason_name(ex.reason), ex.exitCode, ex.info1, ex.info2, ex.rip);
            print_regs(&regs);
            printf("FAIL: see `rhv-info log` for the kernel side\n");
            break;
        }
    }
    if (!halted && exits > MAX_EXITS)
        printf("FAIL: gave up after %d exits\n", MAX_EXITS);

done:
    if (mapped)
        IOConnectUnmapMemory64(conn, kRHVMemGuestRam, mach_task_self(), ram);
    IOConnectCallScalarMethod(conn, kRHVMethodVmDestroy, NULL, 0, NULL, NULL);
    IOServiceClose(conn);
    return rc;
}
