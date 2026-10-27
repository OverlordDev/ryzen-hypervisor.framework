/*
 * rhv-map-test - the phase 2 acceptance test: guest memory that is the
 * process's own memory (hv_vm_map / unmap / protect semantics).
 *
 * Stages (each prints PASS/FAIL, exit status is the number of failures):
 *   1  guest writes to pages we VmMap'ed at 0x100000 and the bytes show up in OUR buffer
 *   2  VmProtect read-only -> guest write faults (NPF, present+write), VmProtect back, retry works
 *   3  guest touches an unmapped page -> NPF (not present), we VmMap it from the handler, retry works
 *   4  VmUnmap -> guest read faults; VmMap a different buffer there -> guest reads OUR bytes
 *   5  mapping over existing memory is refused (kIOReturnBusy), bad arguments are refused
 *   6  a VM created with NO_RAM runs code that lives only in a VmMap'ed buffer
 *   7  after VmDestroy the kernel's mapped-page counter is back where it started (no pinned pages leak)
 *
 * Guest layout: built-in RAM holds the code at 0x1000/0x1100. Real mode with DS=0xFFFF has segment
 * base 0xFFFF0, so offset 0x20 is linear 0x100010 (the HMA, above 1 MiB), where the mapped buffers live:
 *   DS:0x0020 -> 0x100010,  DS:0x1020 -> 0x101010,  DS:0x2020 -> 0x102010.
 * (Careful: base + offset, not offset + 0x100000. DS:0x1010 would be 0x101000, the very start of the page.)
 *
 * Needs root and a kext booted with rhv_enable_svm=1:
 *     sudo ./build/rhv-map-test
 */
#include <IOKit/IOKitLib.h>
#include <emmintrin.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../../include/RyzenHVShared.h"

#define PAGE      4096u
#define HMA_BASE  0x100000ull

#ifndef kIOReturnBusy
#define kIOReturnBusy 0xe00002d5
#endif

static int failures;

#define EXPECT(cond, ...)                                                       \
    do {                                                                        \
        if (cond) {                                                             \
            printf("  ok: ");                                                   \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        } else {                                                                \
            printf("  FAIL (line %d): ", __LINE__);                              \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
            failures++;                                                         \
        }                                                                       \
    } while (0)

/* Program A at 0x1000. See the file comment for the addresses. */
static const uint8_t kProgA[] = {
    0xB8, 0xFF, 0xFF,                   /* 1000 mov ax, 0xFFFF           */
    0x8E, 0xD8,                         /* 1003 mov ds, ax               */
    0xC6, 0x06, 0x20, 0x00, 0xAB,       /* 1005 mov byte [0x0020], 0xAB  */
    0xA0, 0x20, 0x00,                   /* 100A mov al, [0x0020]         */
    0xC6, 0x06, 0x20, 0x10, 0xCD,       /* 100D mov byte [0x1020], 0xCD  */
    0xF4,                               /* 1012 hlt                      */
    0xC6, 0x06, 0x20, 0x10, 0xEF,       /* 1013 mov byte [0x1020], 0xEF  */
    0xC6, 0x06, 0x20, 0x20, 0x77,       /* 1018 mov byte [0x2020], 0x77  */
    0xF4,                               /* 101D hlt                      */
    0xEB, 0xFE,                         /* 101E jmp $                    */
};

/* Program B at 0x1100: read DS:0x0020 (= 0x100010) into AL and stop. */
static const uint8_t kProgB[] = {
    0xB8, 0xFF, 0xFF,                   /* 1100 mov ax, 0xFFFF   */
    0x8E, 0xD8,                         /* 1103 mov ds, ax       */
    0xA0, 0x20, 0x00,                   /* 1105 mov al, [0x0020] */
    0xF4,                               /* 1108 hlt              */
};

/* Program C at guest physical 0 of the NO_RAM VM. */
static const uint8_t kProgC[] = {
    0xB8, 0x34, 0x12,                   /* 0000 mov ax, 0x1234 */
    0xF4,                               /* 0003 hlt            */
};

static const char *reason_name(uint32_t r)
{
    static const char *const kNames[] = {
        "NONE", "TIMESLICE", "HLT", "IO", "CPUID", "MSR", "NPF", "SHUTDOWN", "INVALID", "VMMCALL",
        "UNHANDLED",
    };
    return r < sizeof(kNames) / sizeof(kNames[0]) ? kNames[r] : "?";
}

static kern_return_t vm_map_call(io_connect_t c, const void *uva, uint64_t gpa, uint64_t size,
                                 uint32_t flags)
{
    uint64_t in[4] = { (uint64_t)(uintptr_t)uva, gpa, size, flags };
    return IOConnectCallScalarMethod(c, kRHVMethodVmMap, in, 4, NULL, NULL);
}

static kern_return_t vm_unmap_call(io_connect_t c, uint64_t gpa, uint64_t size)
{
    uint64_t in[2] = { gpa, size };
    return IOConnectCallScalarMethod(c, kRHVMethodVmUnmap, in, 2, NULL, NULL);
}

static kern_return_t vm_protect_call(io_connect_t c, uint64_t gpa, uint64_t size, uint32_t flags)
{
    uint64_t in[3] = { gpa, size, flags };
    return IOConnectCallScalarMethod(c, kRHVMethodVmProtect, in, 3, NULL, NULL);
}

static kern_return_t vm_reset_call(io_connect_t c, uint16_t cs, uint16_t ip)
{
    uint64_t in[2] = { cs, ip };
    return IOConnectCallScalarMethod(c, kRHVMethodVmReset, in, 2, NULL, NULL);
}

static kern_return_t vm_create_call(io_connect_t c, uint32_t flags, uint64_t *ramSize)
{
    uint64_t in = flags, out = 0;
    uint32_t outCnt = 1;
    kern_return_t kr = IOConnectCallScalarMethod(c, kRHVMethodVmCreate, &in, 1, &out, &outCnt);
    *ramSize = out;
    return kr;
}

/* Runs the guest until something other than a time slice happens. */
static int run_guest(io_connect_t c, int skip, RHVExit *ex)
{
    for (int i = 0; i < 100000; i++) {
        uint64_t in = skip ? 1 : 0;
        size_t sz = sizeof(*ex);
        kern_return_t kr = IOConnectCallMethod(c, kRHVMethodVmRun, &in, 1, NULL, 0, NULL, NULL, ex, &sz);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "VmRun failed: 0x%x\n", kr);
            return -1;
        }
        skip = 0;
        if (ex->reason != RHV_EXIT_TIMESLICE)
            return 0;
    }
    fprintf(stderr, "guest never left its time slice\n");
    return -1;
}

static int get_regs(io_connect_t c, RHVRegs *r)
{
    size_t sz = sizeof(*r);
    kern_return_t kr = IOConnectCallStructMethod(c, kRHVMethodVmGetRegs, NULL, 0, r, &sz);
    if (kr != KERN_SUCCESS)
        fprintf(stderr, "VmGetRegs failed: 0x%x\n", kr);
    return kr == KERN_SUCCESS ? 0 : -1;
}

static int get_stats(io_connect_t c, RHVStats *s)
{
    size_t sz = sizeof(*s);
    kern_return_t kr = IOConnectCallStructMethod(c, kRHVMethodGetStats, NULL, 0, s, &sz);
    if (kr != KERN_SUCCESS || sz != sizeof(*s)) {
        fprintf(stderr, "GetStats failed: 0x%x\n", kr);
        return -1;
    }
    return 0;
}

static void describe_exit(const RHVExit *ex)
{
    printf("  exit: %s code=0x%" PRIx64 " info1=0x%" PRIx64 " info2=0x%" PRIx64 " rip=0x%" PRIx64 "\n",
           reason_name(ex->reason), ex->exitCode, ex->info1, ex->info2, ex->rip);
}

/*
 * Guest and process look at the same physical page. If the guest accesses it with a different memory type than
 * our WB mapping, our cache can hold a stale copy. peek() reads a byte, flushes its cache line, reads again, and
 * counts how often the two differ: that is direct evidence of incoherence, and the flushed value is the truth.
 */
static int staleReads;

static void flush_buf(const void *p, size_t bytes)
{
    for (size_t o = 0; o < bytes; o += 64)
        _mm_clflush((const uint8_t *)p + o);
    _mm_mfence();
}

static uint8_t peek(const uint8_t *buf, size_t off)
{
    uint8_t cached = *(volatile const uint8_t *)(buf + off);
    _mm_clflush(buf + off);
    _mm_mfence();
    uint8_t fresh = *(volatile const uint8_t *)(buf + off);
    if (cached != fresh) {
        staleReads++;
        printf("  note: stale cache line at offset 0x%zx: cached 0x%02x, memory 0x%02x\n", off, cached, fresh);
    }
    return fresh;
}

static uint8_t *alloc_buffer(size_t bytes)
{
    uint8_t *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    memset(p, 0, bytes);   /* touch every page so it is resident */
    flush_buf(p, bytes);
    return p;
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

    RHVStats before;
    if (get_stats(conn, &before))
        return 1;

    mach_vm_address_t ram = 0;
    mach_vm_size_t ramSize = 0;
    int ramMapped = 0;
    uint64_t builtin = 0;
    uint8_t *buf = alloc_buffer(4 * PAGE);
    uint8_t *buf2 = alloc_buffer(PAGE);
    uint8_t *buf3 = alloc_buffer(PAGE);
    if (!buf || !buf2 || !buf3)
        return 1;

    kr = vm_create_call(conn, 0, &builtin);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "VmCreate failed: 0x%x (root? rhv_enable_svm=1? see `rhv-info log`)\n", kr);
        return 1;
    }
    printf("VM created, %" PRIu64 " KiB built-in RAM\n", builtin / 1024);

    kr = IOConnectMapMemory64(conn, kRHVMemGuestRam, mach_task_self(), &ram, &ramSize, kIOMapAnywhere);
    if (kr != KERN_SUCCESS || ramSize < 0x1200) {
        fprintf(stderr, "map guest RAM failed: 0x%x\n", kr);
        goto done;
    }
    ramMapped = 1;
    memcpy((void *)(uintptr_t)(ram + 0x1000), kProgA, sizeof(kProgA));
    memcpy((void *)(uintptr_t)(ram + 0x1100), kProgB, sizeof(kProgB));

    RHVExit ex;
    RHVRegs regs;

    /* ---- Stage 1: pages we map are the guest's memory ---- */
    printf("stage 1: map two pages at 0x%llx, guest writes into them\n", HMA_BASE);
    kr = vm_map_call(conn, buf, HMA_BASE, 2 * PAGE, RHV_MEM_READ | RHV_MEM_WRITE);
    EXPECT(kr == KERN_SUCCESS, "VmMap returned 0x%x", kr);
    if (kr != KERN_SUCCESS)
        goto done;
    if (vm_reset_call(conn, 0, 0x1000) || run_guest(conn, 0, &ex))
        goto done;
    describe_exit(&ex);
    EXPECT(ex.reason == RHV_EXIT_HLT, "guest ran to its first HLT");
    uint8_t b0 = peek(buf, 0x10), b1 = peek(buf, 0x1010);
    EXPECT(b0 == 0xAB, "guest byte landed in our buffer (page 0: 0x%02x)", b0);
    EXPECT(b1 == 0xCD, "guest byte landed in our buffer (page 1: 0x%02x)", b1);
    if (!get_regs(conn, &regs))
        EXPECT((regs.rax & 0xFF) == 0xAB, "guest read its own write back (al=0x%02" PRIx64 ")",
               regs.rax & 0xFF);

    /* ---- Stage 2: read-only page -> write fault -> unprotect -> retry ---- */
    printf("stage 2: read-only page faults on write\n");
    kr = vm_protect_call(conn, HMA_BASE + PAGE, PAGE, RHV_MEM_READ);
    EXPECT(kr == KERN_SUCCESS, "VmProtect read-only returned 0x%x", kr);
    if (run_guest(conn, 1, &ex))
        goto done;
    describe_exit(&ex);
    EXPECT(ex.reason == RHV_EXIT_NPF, "write to a read-only page exits with NPF");
    EXPECT(ex.info2 == HMA_BASE + PAGE + 0x10, "fault address is 0x%" PRIx64, ex.info2);
    EXPECT(rhv_npf_present(ex.info1) && rhv_npf_is_write(ex.info1) && !rhv_npf_is_fetch(ex.info1),
           "fault is a write to a present page (info1=0x%" PRIx64 ")", ex.info1);
    EXPECT(peek(buf, 0x1010) == 0xCD, "the blocked write did not change the page (0x%02x)", peek(buf, 0x1010));
    kr = vm_protect_call(conn, HMA_BASE + PAGE, PAGE, RHV_MEM_READ | RHV_MEM_WRITE);
    EXPECT(kr == KERN_SUCCESS, "VmProtect read-write returned 0x%x", kr);
    kr = vm_protect_call(conn, HMA_BASE + 8 * PAGE, PAGE, RHV_MEM_READ);
    EXPECT(kr != KERN_SUCCESS, "VmProtect on an unmapped page is refused (0x%x)", kr);

    /* ---- Stage 3: lazy mapping from the fault handler ---- */
    printf("stage 3: unmapped page faults, handler maps it, guest retries\n");
    if (run_guest(conn, 0, &ex))      /* retry the faulting instruction */
        goto done;
    describe_exit(&ex);
    EXPECT(peek(buf, 0x1010) == 0xEF, "retried write went through (0x%02x)", peek(buf, 0x1010));
    EXPECT(ex.reason == RHV_EXIT_NPF && ex.info2 == HMA_BASE + 2 * PAGE + 0x10,
           "touching the unmapped page exits with NPF at 0x%" PRIx64, ex.info2);
    EXPECT(!rhv_npf_present(ex.info1) && rhv_npf_is_write(ex.info1),
           "fault is a write to a page that is not mapped (info1=0x%" PRIx64 ")", ex.info1);
    kr = vm_map_call(conn, buf + 2 * PAGE, HMA_BASE + 2 * PAGE, PAGE, RHV_MEM_READ | RHV_MEM_WRITE);
    EXPECT(kr == KERN_SUCCESS, "VmMap from the fault handler returned 0x%x", kr);
    if (run_guest(conn, 0, &ex))
        goto done;
    describe_exit(&ex);
    EXPECT(ex.reason == RHV_EXIT_HLT, "guest reached its second HLT");
    EXPECT(peek(buf, 2 * PAGE + 0x10) == 0x77, "lazily mapped page received the write (0x%02x)",
           peek(buf, 2 * PAGE + 0x10));

    /* ---- Stage 4: unmap, fault, remap a different buffer ---- */
    printf("stage 4: unmap, then map a different buffer at the same address\n");
    kr = vm_unmap_call(conn, HMA_BASE, 3 * PAGE);
    EXPECT(kr == KERN_SUCCESS, "VmUnmap returned 0x%x", kr);
    if (vm_reset_call(conn, 0, 0x1100) || run_guest(conn, 0, &ex))
        goto done;
    describe_exit(&ex);
    EXPECT(ex.reason == RHV_EXIT_NPF && ex.info2 == HMA_BASE + 0x10 && !rhv_npf_present(ex.info1) &&
               !rhv_npf_is_write(ex.info1),
           "read after unmap exits with a not-present read fault");
    buf2[0x10] = 0x5A;
    flush_buf(buf2, PAGE);     /* push our write to memory so a guest with a different cache type sees it */
    kr = vm_map_call(conn, buf2, HMA_BASE, PAGE, RHV_MEM_READ);
    EXPECT(kr == KERN_SUCCESS, "VmMap of a different buffer returned 0x%x", kr);
    if (run_guest(conn, 0, &ex))
        goto done;
    describe_exit(&ex);
    EXPECT(ex.reason == RHV_EXIT_HLT, "guest reached HLT");
    if (!get_regs(conn, &regs))
        EXPECT((regs.rax & 0xFF) == 0x5A, "guest read the byte we wrote into the new buffer (al=0x%02" PRIx64 ")",
               regs.rax & 0xFF);

    /* ---- Stage 5: refusals ---- */
    printf("stage 5: bad requests are refused\n");
    kr = vm_map_call(conn, buf3, HMA_BASE, PAGE, RHV_MEM_READ);
    EXPECT(kr == (kern_return_t)kIOReturnBusy, "mapping over a mapped page: 0x%x", kr);
    kr = vm_map_call(conn, buf3, 0xF0000, PAGE, RHV_MEM_READ);
    EXPECT(kr == (kern_return_t)kIOReturnBusy, "mapping over the built-in RAM: 0x%x", kr);
    kr = vm_map_call(conn, buf3 + 1, 0x200000, PAGE, RHV_MEM_READ);
    EXPECT(kr != KERN_SUCCESS, "unaligned user address: 0x%x", kr);
    kr = vm_map_call(conn, buf3, 0x200001, PAGE, RHV_MEM_READ);
    EXPECT(kr != KERN_SUCCESS, "unaligned guest address: 0x%x", kr);
    kr = vm_map_call(conn, buf3, 0x200000, PAGE + 1, RHV_MEM_READ);
    EXPECT(kr != KERN_SUCCESS, "size that is not a page multiple: 0x%x", kr);
    kr = vm_map_call(conn, buf3, 0x200000, 0, RHV_MEM_READ);
    EXPECT(kr != KERN_SUCCESS, "zero size: 0x%x", kr);
    kr = vm_map_call(conn, buf3, 0x200000, PAGE, 0x80);
    EXPECT(kr != KERN_SUCCESS, "unknown flag bit: 0x%x", kr);
    kr = vm_map_call(conn, buf3, 1ull << 48, PAGE, RHV_MEM_READ);
    EXPECT(kr != KERN_SUCCESS, "guest address beyond 48 bits: 0x%x", kr);
    kr = vm_map_call(conn, (const void *)0x10, 0x200000, PAGE, RHV_MEM_READ);
    EXPECT(kr != KERN_SUCCESS, "unmapped user address: 0x%x", kr);

    /* ---- Stage 6: a VM that owns no memory at all ---- */
    printf("stage 6: VM without built-in RAM\n");
    if (ramMapped) {
        IOConnectUnmapMemory64(conn, kRHVMemGuestRam, mach_task_self(), ram);
        ramMapped = 0;
    }
    IOConnectCallScalarMethod(conn, kRHVMethodVmDestroy, NULL, 0, NULL, NULL);
    kr = vm_create_call(conn, RHV_VMCREATE_NO_RAM, &builtin);
    EXPECT(kr == KERN_SUCCESS && builtin == 0, "VmCreate(NO_RAM) returned 0x%x, RAM size %" PRIu64, kr, builtin);
    if (kr != KERN_SUCCESS)
        goto done;
    memcpy(buf3, kProgC, sizeof(kProgC));
    flush_buf(buf3, PAGE);
    kr = vm_map_call(conn, buf3, 0, PAGE, RHV_MEM_READ | RHV_MEM_EXEC);
    EXPECT(kr == KERN_SUCCESS, "VmMap of the code page at guest address 0 returned 0x%x", kr);
    if (run_guest(conn, 0, &ex))
        goto done;
    describe_exit(&ex);
    EXPECT(ex.reason == RHV_EXIT_HLT, "guest executed from our buffer and reached HLT");
    if (!get_regs(conn, &regs))
        EXPECT((regs.rax & 0xFFFF) == 0x1234, "ax=0x%04" PRIx64, regs.rax & 0xFFFF);
    kr = vm_protect_call(conn, 0, PAGE, RHV_MEM_READ);   /* now not executable */
    EXPECT(kr == KERN_SUCCESS, "VmProtect to non-executable returned 0x%x", kr);
    if (vm_reset_call(conn, 0, 0) || run_guest(conn, 0, &ex))
        goto done;
    describe_exit(&ex);
    EXPECT(ex.reason == RHV_EXIT_NPF && rhv_npf_is_fetch(ex.info1) && rhv_npf_present(ex.info1),
           "instruction fetch from a non-executable page exits with an NPF fetch fault");

done:
    /* ---- Stage 7: nothing stays pinned ---- */
    printf("stage 7: cleanup\n");
    if (ramMapped)
        IOConnectUnmapMemory64(conn, kRHVMemGuestRam, mach_task_self(), ram);
    IOConnectCallScalarMethod(conn, kRHVMethodVmDestroy, NULL, 0, NULL, NULL);
    RHVStats after;
    if (!get_stats(conn, &after)) {
        EXPECT(after.vmPagesMapped == before.vmPagesMapped,
               "mapped-page counter back to %" PRIu64 " (is %" PRIu64 ")", before.vmPagesMapped,
               after.vmPagesMapped);
        printf("  stats: vmMaps +%" PRIu64 ", vmMapFailures +%" PRIu64 ", vmUnmaps +%" PRIu64
               ", vmProtects +%" PRIu64 ", vmNptFaults +%" PRIu64 ", vmInvalidExits +%" PRIu64 "\n",
               after.vmMaps - before.vmMaps, after.vmMapFailures - before.vmMapFailures,
               after.vmUnmaps - before.vmUnmaps, after.vmProtects - before.vmProtects,
               after.vmNptFaults - before.vmNptFaults, after.vmInvalidExits - before.vmInvalidExits);
    }
    IOServiceClose(conn);

    if (staleReads)
        printf("NOTE: %d read(s) saw a stale cache line: the guest and this process disagree about the memory type "
               "of the shared pages\n", staleReads);
    if (failures) {
        printf("FAIL: %d check(s) failed, see `rhv-info log` for the kernel side\n", failures);
        return 1;
    }
    printf("PASS: all stages\n");
    return 0;
}
