/*
 * RyzenHVShared.h
 *
 * ABI shared between the kext, the userland shim and the tools.
 * Plain C, no kernel/userland-only includes, so both sides can use it.
 *
 * Bump RHV_ABI_VERSION whenever a struct or selector below changes.
 */
#ifndef RYZENHV_SHARED_H
#define RYZENHV_SHARED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RHV_ABI_VERSION 6
#define RHV_SERVICE_NAME "com_overlorddev_RyzenHV"
#define RHV_MAX_CPUS 64

/*
 * IOUserClient selectors. Add new ones at the end.
 *
 *   GetInfo      out: struct RHVInfo
 *   ReadLog      in: scalar[0] = cursor            out: struct RHVLogChunk
 *   SetLogLevel  in: scalar[0] = level (root only)
 *   GetStats     out: struct RHVStats
 *   SelfTest     out: scalar[0] = cpus ok, scalar[1] = cpus checked (root only)
 *
 * Phase 1: one VM with one vCPU per user client (root only).
 *   VmCreate     in: scalar[0] = RHV_VMCREATE_* flags
 *                out: scalar[0] = built-in guest RAM size (0 with NO_RAM). Needs SVM
 *                active on all CPUs (boot-arg rhv_enable_svm=1). The built-in RAM is
 *                mappable with IOConnectMapMemory64(conn, kRHVMemGuestRam, ...).
 *   VmDestroy
 *   VmReset      in: scalar[0] = CS selector, scalar[1] = IP. Real-mode reset
 *                state, GPRs cleared, RIP = IP, CS.base = CS << 4.
 *   VmSetRegs    in: struct RHVRegs
 *   VmGetRegs    out: struct RHVRegs
 *   VmRun        in: scalar[0] = 1 to resume at the next instruction (skip the
 *                one that caused the previous exit)   out: struct RHVExit
 *
 * Phase 2: guest physical memory backed by the calling process's own pages
 * (the equivalent of hv_vm_map / hv_vm_unmap / hv_vm_protect). All addresses
 * and sizes must be multiples of 4096. The pages are pinned until unmapped or
 * the VM goes away. Mapping over an existing mapping fails (kIOReturnBusy).
 *   VmMap        in: scalar[0] = user virtual address, scalar[1] = guest physical
 *                address, scalar[2] = size, scalar[3] = RHV_MEM_* flags
 *   VmUnmap      in: scalar[0] = guest physical address, scalar[1] = size.
 *                Holes in the range are fine, any sub-range of a mapping works.
 *   VmProtect    in: scalar[0] = guest physical address, scalar[1] = size,
 *                scalar[2] = RHV_MEM_* flags. Every page must be mapped.
 *
 * Phase 3: VMCS access for VMs created with RHV_VMCREATE_VMX (hv_vmx_vcpu_read_vmcs / write_vmcs).
 * The field numbers are Intel's VMCS encodings. Execution controls written this way become VMCB intercepts
 * at every VmRun, and after VmRun the VMX exit reason, qualification, etc. are readable as VMCS_RO_* fields.
 *   VmcsRead     in: scalar[0] = field                       out: scalar[0] = value
 *                (kIOReturnBadArgument for a field the framework does not have)
 *   VmcsWrite    in: scalar[0] = field, scalar[1] = value
 *                (kIOReturnNotWritable for the read-only exit fields)
 *
 * Extended state (x87/SSE/AVX) of the guest, kept by the kext and swapped in and out around VMRUN.
 *   VmGetFpu     out: struct RHVFpuState
 *   VmSetFpu     in: struct RHVFpuState. xcr0 must be a legal XCR0 (x87 set, AVX needs SSE, ...) and a subset of the
 *                host's; area is the XSAVE standard format (the same layout hv_vcpu_read_fpstate returns), and
 *                `size` must equal what VmGetFpu reported. kIOReturnBadArgument otherwise.
 *                A client emulating the guest's XSETBV (VMX exit reason 55) sets xcr0 this way.
 */
enum {
    kRHVMethodGetInfo = 0,
    kRHVMethodReadLog,
    kRHVMethodSetLogLevel,
    kRHVMethodGetStats,
    kRHVMethodSelfTest,
    kRHVMethodVmCreate,
    kRHVMethodVmDestroy,
    kRHVMethodVmReset,
    kRHVMethodVmSetRegs,
    kRHVMethodVmGetRegs,
    kRHVMethodVmRun,
    kRHVMethodVmMap,
    kRHVMethodVmUnmap,
    kRHVMethodVmProtect,
    kRHVMethodVmcsRead,
    kRHVMethodVmcsWrite,
    kRHVMethodVmGetFpu,
    kRHVMethodVmSetFpu,
    kRHVMethodCount
};

/* clientMemoryForType() types. */
enum { kRHVMemGuestRam = 0 };

#define RHV_GUEST_RAM_SIZE (1u << 20)   /* built-in RAM: 1 MiB at guest physical 0 */

/* Guest extended state. The area is the XSAVE standard format; `size` is the host's XSAVE size for its XCR0. */
#define RHV_FPU_AREA_MAX 3072
typedef struct RHVFpuState {
    uint64_t xcr0;
    uint32_t size;
    uint32_t reserved;
    uint8_t  area[RHV_FPU_AREA_MAX];
} RHVFpuState;

/* VmCreate flags. */
enum {
    RHV_VMCREATE_NO_RAM = 1u << 0,   /* do not map the built-in 1 MiB, the guest starts empty */
    RHV_VMCREATE_VMX    = 1u << 1    /* the client drives the guest through VMCS fields (Hypervisor.framework style) */
};

/* VmMap / VmProtect flags, same values as hv_memory_flags_t. Write or execute imply read. */
enum { RHV_MEM_READ = 1u << 0, RHV_MEM_WRITE = 1u << 1, RHV_MEM_EXEC = 1u << 2 };

/* Largest single VmMap, in bytes. */
#define RHV_MAP_MAX_SIZE (16ull << 30)

/* Kernel log levels. A message is kept when its level <= the current level. */
enum {
    RHV_LOG_NONE  = 0,
    RHV_LOG_ERROR = 1,
    RHV_LOG_WARN  = 2,
    RHV_LOG_INFO  = 3,
    RHV_LOG_DEBUG = 4,
    RHV_LOG_TRACE = 5
};

/* CPUID Fn8000_000A EDX: SVM feature bits (AMD APM Vol.2, 15.34). */
enum {
    RHV_SVM_FEAT_NP             = 1u << 0,   /* nested paging (NPT)            */
    RHV_SVM_FEAT_LBRV           = 1u << 1,   /* LBR virtualization             */
    RHV_SVM_FEAT_SVML           = 1u << 2,   /* SVM lock                       */
    RHV_SVM_FEAT_NRIPS          = 1u << 3,   /* next-RIP saved on #VMEXIT      */
    RHV_SVM_FEAT_TSCRATE        = 1u << 4,   /* TSC ratio MSR                  */
    RHV_SVM_FEAT_VMCB_CLEAN     = 1u << 5,   /* VMCB clean bits                */
    RHV_SVM_FEAT_FLUSH_BY_ASID  = 1u << 6,
    RHV_SVM_FEAT_DECODE_ASSISTS = 1u << 7,
    RHV_SVM_FEAT_PAUSE_FILTER   = 1u << 10,
    RHV_SVM_FEAT_PAUSE_THRESH   = 1u << 12,
    RHV_SVM_FEAT_AVIC           = 1u << 13,
    RHV_SVM_FEAT_VVMSAVE        = 1u << 15,  /* virtual VMSAVE/VMLOAD          */
    RHV_SVM_FEAT_VGIF           = 1u << 16   /* virtual GIF                    */
};

/* Result of kRHVMethodGetInfo. All fields are uint32_t on purpose. */
typedef struct RHVInfo {
    uint32_t abiVersion;
    uint32_t vendorIsAmd;
    uint32_t cpuFamily;          /* display family, 0x19 on Zen 3          */
    uint32_t cpuModel;           /* display model,  0x50 on Cezanne        */
    uint32_t cpuStepping;
    uint32_t svmSupported;       /* CPUID Fn8000_0001 ECX[2]               */
    uint32_t svmDisabledByBios;  /* VM_CR.SVMDIS                           */
    uint32_t svmLocked;          /* VM_CR.LOCK                             */
    uint32_t svmRevision;        /* CPUID Fn8000_000A EAX[7:0]             */
    uint32_t numAsids;           /* CPUID Fn8000_000A EBX                  */
    uint32_t svmFeatures;        /* CPUID Fn8000_000A EDX, RHV_SVM_FEAT_*  */
    uint32_t nCpus;              /* logical CPUs probed                    */
    uint32_t nCpusSvmUsable;     /* supported && !SVMDIS                   */
    uint32_t nCpusSvmActive;     /* EFER.SVME set by us                    */
} RHVInfo;

/* Event counters kept by the kext, for "did that code path run at all?" questions. */
typedef struct RHVStats {
    uint64_t probeRuns;
    uint64_t enableCalls;
    uint64_t enableCpusOk;       /* CPUs that took the SVM enable path */
    uint64_t enableFailures;
    uint64_t disableCalls;
    uint64_t sleepEvents;
    uint64_t wakeEvents;
    uint64_t clientOpens;
    uint64_t clientCalls;
    uint64_t assertFailures;
    uint64_t selftestRuns;
    uint64_t selftestCpusOk;
    uint64_t logBytesTotal;      /* bytes ever written to the kernel log ring */
    uint64_t logLevel;           /* current RHV_LOG_* level */
    uint64_t vmCreates;
    uint64_t vmRuns;             /* VmRun calls */
    uint64_t vmExits;            /* #VMEXITs of every kind, incl. host interrupts */
    uint64_t vmInvalidExits;     /* VMEXIT_INVALID: VMRUN rejected the VMCB */
    uint64_t vmMaps;             /* successful VmMap calls */
    uint64_t vmMapFailures;
    uint64_t vmUnmaps;
    uint64_t vmProtects;
    uint64_t vmNptFaults;        /* RHV_EXIT_NPF handed to userland */
    uint64_t vmPagesMapped;      /* guest pages currently mapped, summed over VMs */
    uint64_t vmFpuMismatches;    /* host x87/SSE state differed after a VMRUN round trip: must stay 0 */
} RHVStats;

/* Guest registers as seen by userland (VmGetRegs / VmSetRegs). */
typedef struct RHVRegs {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags;
} RHVRegs;

/* Why VmRun returned, in our own words. The raw SVM data is in RHVExit too. */
enum {
    RHV_EXIT_NONE = 0,
    RHV_EXIT_TIMESLICE,    /* nothing to handle, call VmRun again */
    RHV_EXIT_HLT,
    RHV_EXIT_IO,           /* IN/OUT, decode with rhv_ioio_*() */
    RHV_EXIT_CPUID,
    RHV_EXIT_MSR,          /* info1: 0 = RDMSR, 1 = WRMSR; ECX = MSR */
    RHV_EXIT_NPF,          /* nested page fault, info2 = guest physical address */
    RHV_EXIT_SHUTDOWN,     /* triple fault */
    RHV_EXIT_INVALID,      /* VMRUN rejected the VMCB, see the kernel log */
    RHV_EXIT_VMMCALL,
    RHV_EXIT_UNHANDLED     /* anything else, look at exitCode */
};

/* Raw SVM exit codes users may want to compare against (AMD APM Vol.2, App. C). */
#define RHV_SVM_EXIT_INTR     0x60ull
#define RHV_SVM_EXIT_NMI      0x61ull
#define RHV_SVM_EXIT_CPUID    0x72ull
#define RHV_SVM_EXIT_HLT      0x78ull
#define RHV_SVM_EXIT_IOIO     0x7Bull
#define RHV_SVM_EXIT_MSR      0x7Cull
#define RHV_SVM_EXIT_SHUTDOWN 0x7Full
#define RHV_SVM_EXIT_VMMCALL  0x81ull
#define RHV_SVM_EXIT_NPF      0x400ull
#define RHV_SVM_EXIT_INVALID  (~0ull)

typedef struct RHVExit {
    uint32_t reason;             /* RHV_EXIT_* */
    uint32_t reserved;
    uint64_t exitCode;           /* raw SVM exit code */
    uint64_t info1;              /* raw EXITINFO1 */
    uint64_t info2;              /* raw EXITINFO2 */
    uint64_t nextRip;            /* VMCB next_rip, valid for HLT/CPUID/IO/MSR */
    uint64_t rip;                /* guest RIP at the exit */
} RHVExit;

/*
 * One read of the kernel log ring. Fixed size (4096 bytes) so the IOKit
 * dispatch table can check it. `cursor` is an absolute byte offset into the
 * stream of all log bytes ever written; pass `nextCursor` back to continue.
 * `dropped` counts bytes lost because the ring wrapped before you read them.
 */
#define RHV_LOG_CHUNK_SIZE 4096
#define RHV_LOG_CHUNK_DATA (RHV_LOG_CHUNK_SIZE - 24)

typedef struct RHVLogChunk {
    uint64_t nextCursor;
    uint64_t dropped;
    uint32_t length;             /* valid bytes in data[] */
    uint32_t reserved;
    char     data[RHV_LOG_CHUNK_DATA];
} RHVLogChunk;

/* Compile-time size checks that work in both C and C++. */
typedef char rhv_assert_log_chunk_size[(sizeof(RHVLogChunk) == RHV_LOG_CHUNK_SIZE) ? 1 : -1];
typedef char rhv_assert_stats_size[(sizeof(RHVStats) == 25 * sizeof(uint64_t)) ? 1 : -1];
typedef char rhv_assert_regs_size[(sizeof(RHVRegs) == 18 * sizeof(uint64_t)) ? 1 : -1];
typedef char rhv_assert_fpu_size[(sizeof(RHVFpuState) == 16 + RHV_FPU_AREA_MAX) ? 1 : -1];
typedef char rhv_assert_exit_size[(sizeof(RHVExit) == 48) ? 1 : -1];

/* IOIO exit decoding (EXITINFO1 of SVM exit 0x7B, AMD APM Vol.2, 15.10.2). */
static inline uint32_t rhv_ioio_port(uint64_t info1)    { return (uint32_t)(info1 >> 16) & 0xFFFF; }
static inline int      rhv_ioio_is_in(uint64_t info1)   { return (int)(info1 & 1); }
static inline int      rhv_ioio_is_string(uint64_t info1) { return (int)((info1 >> 2) & 1); }
static inline int      rhv_ioio_has_rep(uint64_t info1) { return (int)((info1 >> 3) & 1); }
static inline uint32_t rhv_ioio_size(uint64_t info1)
{
    if (info1 & (1u << 4)) return 1;
    if (info1 & (1u << 5)) return 2;
    if (info1 & (1u << 6)) return 4;
    return 0;
}

/*
 * NPF exit decoding (EXITINFO1 of SVM exit 0x400, AMD APM Vol.2, 15.25.6).
 * Bits 31:0 are a #PF-style error code, info2 is the faulting guest physical address.
 */
static inline int rhv_npf_present(uint64_t info1)  { return (int)(info1 & 1); }          /* page was mapped but rights were insufficient */
static inline int rhv_npf_is_write(uint64_t info1) { return (int)((info1 >> 1) & 1); }
static inline int rhv_npf_is_fetch(uint64_t info1) { return (int)((info1 >> 4) & 1); }
static inline int rhv_npf_in_walk(uint64_t info1)  { return (int)((info1 >> 33) & 1); }  /* fault while the CPU read the guest's own page tables */

/* CPUID helpers, pure functions so they can be unit-tested on any host. */

static inline uint32_t rhv_amd_family(uint32_t cpuid1Eax)
{
    uint32_t base = (cpuid1Eax >> 8) & 0xF;
    uint32_t ext  = (cpuid1Eax >> 20) & 0xFF;
    return base == 0xF ? base + ext : base;
}

static inline uint32_t rhv_amd_model(uint32_t cpuid1Eax)
{
    uint32_t base  = (cpuid1Eax >> 8) & 0xF;
    uint32_t model = (cpuid1Eax >> 4) & 0xF;
    uint32_t ext   = (cpuid1Eax >> 16) & 0xF;
    return base == 0xF ? (ext << 4) | model : model;
}

static inline uint32_t rhv_amd_stepping(uint32_t cpuid1Eax)
{
    return cpuid1Eax & 0xF;
}

/* "AuthenticAMD" as returned in EBX, EDX, ECX by CPUID leaf 0. */
static inline int rhv_is_amd_vendor(uint32_t ebx, uint32_t ecx, uint32_t edx)
{
    return ebx == 0x68747541u && edx == 0x69746E65u && ecx == 0x444D4163u;
}

#ifdef __cplusplus
}
#endif

#endif /* RYZENHV_SHARED_H */
