/*
 * VmxMap.hpp - translating between what Intel's Hypervisor.framework API exposes (VMCS fields, VMX exit
 * reasons) and what an AMD CPU has (VMCB, SVM exit codes).
 *
 * Pure C++ (stdint/stddef only), so tests/vmx_map_test.cpp checks it on any host, including against the
 * constants in the real SDK headers.
 *
 * What is here:
 *   - the list of VMCS fields (VmcsFields.def, generated) and where each one lives on our side
 *   - segment access-rights conversion (VMX "AR" <-> VMCB "attrib")
 *   - exit translation: SVM exit code + EXITINFO -> VMX exit reason + qualification
 *
 * What is not: applying VMX execution controls to VMCB intercepts, and interrupt injection. Those need
 * the vCPU state and come next (docs/PHASE3.md).
 */
#ifndef RYZENHV_VMXMAP_HPP
#define RYZENHV_VMXMAP_HPP

#include <stddef.h>
#include <stdint.h>

#include "VMCB.hpp"

namespace rhv {
namespace vmx {

/* ------------------------------------------------------------------------------------------------------ */
/* VMCS field list                                                                                         */
/* ------------------------------------------------------------------------------------------------------ */

struct FieldDesc {
    const char *name;
    uint32_t    encoding;
};

#define F(name, enc) { #name, enc },
static const FieldDesc kFields[] = {
#include "VmcsFields.def"
};
#undef F

static const unsigned kNumFields = sizeof(kFields) / sizeof(kFields[0]);

/* Index of `encoding` in kFields (sorted by encoding), or -1 when it is not a field the framework exposes. */
static inline int fieldIndex(uint32_t encoding)
{
    unsigned lo = 0, hi = kNumFields;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (kFields[mid].encoding == encoding)
            return (int)mid;
        if (kFields[mid].encoding < encoding)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

/* Bytes a field holds, from bits 14:13 of its encoding (Intel SDM Vol. 3, Appendix B). */
static inline unsigned fieldBytes(uint32_t encoding)
{
    switch ((encoding >> 13) & 3) {
    case 0:  return 2;    /* 16-bit */
    case 2:  return 4;    /* 32-bit */
    default: return 8;    /* 64-bit and natural width (64 on x86-64) */
    }
}

/* Access type, bit 0: for 64-bit fields, "high" half. The framework only uses full accesses. */
static inline bool fieldIsHighHalf(uint32_t encoding) { return (encoding & 1) != 0; }

/* How the kext keeps a field. */
enum Kind : uint8_t {
    KIND_SHADOW = 0,   /* stored, read back, no effect on the VMCB (most control fields) */
    KIND_VMCB,         /* lives in a VMCB field: `vmcbOff`, `vmcbSize` */
    KIND_SEG_AR,       /* segment access rights, needs conversion */
    KIND_EFER,         /* guest EFER, SVME is kept set on the AMD side */
    KIND_READONLY      /* exit information, written by the kext on each exit */
};

struct StateMap {
    Kind     kind;
    uint16_t vmcbOff;    /* byte offset in the 4 KiB VMCB (control area 0x000.., save area 0x400..) */
    uint8_t  vmcbSize;   /* bytes in the VMCB */
};

#define RHV_SAVE_OFF(member) ((uint16_t)(0x400 + offsetof(rhv::vmcb::Save, member)))
#define RHV_CTL_OFF(member)  ((uint16_t)offsetof(rhv::vmcb::Control, member))

/*
 * Where does `encoding` live? Fields not listed here are KIND_SHADOW if they are valid VMCS fields.
 * Segment fields: selector 16-bit, limit 32-bit, base natural, AR 32-bit, all map to the same VMCB Seg.
 */
static inline StateMap mapField(uint32_t enc)
{
    using namespace rhv::vmcb;
    struct Seg { uint32_t sel, limit, ar, base; uint16_t off; };
    static const Seg kSegs[] = {
        { 0x0800, 0x4800, 0x4814, 0x6806, RHV_SAVE_OFF(es) },
        { 0x0802, 0x4802, 0x4816, 0x6808, RHV_SAVE_OFF(cs) },
        { 0x0804, 0x4804, 0x4818, 0x680a, RHV_SAVE_OFF(ss) },
        { 0x0806, 0x4806, 0x481a, 0x680c, RHV_SAVE_OFF(ds) },
        { 0x0808, 0x4808, 0x481c, 0x680e, RHV_SAVE_OFF(fs) },
        { 0x080a, 0x480a, 0x481e, 0x6810, RHV_SAVE_OFF(gs) },
        { 0x080c, 0x480c, 0x4820, 0x6812, RHV_SAVE_OFF(ldtr) },
        { 0x080e, 0x480e, 0x4822, 0x6814, RHV_SAVE_OFF(tr) },
    };
    for (const Seg &s : kSegs) {
        if (enc == s.sel)   return { KIND_VMCB, (uint16_t)(s.off + offsetof(rhv::vmcb::Seg, selector)), 2 };
        if (enc == s.limit) return { KIND_VMCB, (uint16_t)(s.off + offsetof(rhv::vmcb::Seg, limit)), 4 };
        if (enc == s.ar)    return { KIND_SEG_AR, (uint16_t)(s.off + offsetof(rhv::vmcb::Seg, attrib)), 2 };
        if (enc == s.base)  return { KIND_VMCB, (uint16_t)(s.off + offsetof(rhv::vmcb::Seg, base)), 8 };
    }
    switch (enc) {
    /* GDTR/IDTR have only a base and a limit. */
    case 0x4810: return { KIND_VMCB, (uint16_t)(RHV_SAVE_OFF(gdtr) + offsetof(rhv::vmcb::Seg, limit)), 4 };
    case 0x4812: return { KIND_VMCB, (uint16_t)(RHV_SAVE_OFF(idtr) + offsetof(rhv::vmcb::Seg, limit)), 4 };
    case 0x6816: return { KIND_VMCB, (uint16_t)(RHV_SAVE_OFF(gdtr) + offsetof(rhv::vmcb::Seg, base)), 8 };
    case 0x6818: return { KIND_VMCB, (uint16_t)(RHV_SAVE_OFF(idtr) + offsetof(rhv::vmcb::Seg, base)), 8 };

    case 0x6800: return { KIND_VMCB, RHV_SAVE_OFF(cr0), 8 };       /* GUEST_CR0 */
    case 0x6802: return { KIND_VMCB, RHV_SAVE_OFF(cr3), 8 };       /* GUEST_CR3 */
    case 0x6804: return { KIND_VMCB, RHV_SAVE_OFF(cr4), 8 };       /* GUEST_CR4 */
    case 0x681a: return { KIND_VMCB, RHV_SAVE_OFF(dr7), 8 };       /* GUEST_DR7 */
    case 0x681c: return { KIND_VMCB, RHV_SAVE_OFF(rsp), 8 };       /* GUEST_RSP */
    case 0x681e: return { KIND_VMCB, RHV_SAVE_OFF(rip), 8 };       /* GUEST_RIP */
    case 0x6820: return { KIND_VMCB, RHV_SAVE_OFF(rflags), 8 };    /* GUEST_RFLAGS */

    case 0x482a: return { KIND_VMCB, RHV_SAVE_OFF(sysenterCs), 8 };   /* GUEST_IA32_SYSENTER_CS (low 32 bits used) */
    case 0x6824: return { KIND_VMCB, RHV_SAVE_OFF(sysenterEsp), 8 };
    case 0x6826: return { KIND_VMCB, RHV_SAVE_OFF(sysenterEip), 8 };
    case 0x2802: return { KIND_VMCB, RHV_SAVE_OFF(dbgctl), 8 };        /* GUEST_IA32_DEBUGCTL */
    case 0x2804: return { KIND_VMCB, RHV_SAVE_OFF(gPat), 8 };          /* GUEST_IA32_PAT */
    case 0x2806: return { KIND_EFER, RHV_SAVE_OFF(efer), 8 };          /* GUEST_IA32_EFER */

    /* Controls that are a direct VMCB field. */
    case 0x2010: return { KIND_VMCB, RHV_CTL_OFF(tscOffset), 8 };      /* CTRL_TSC_OFFSET */
    case 0x4004: return { KIND_VMCB, RHV_CTL_OFF(interceptExc), 4 };   /* CTRL_EXC_BITMAP: bit n = vector n on both */

    /* Exit information, set by the kext. */
    case 0x4402: case 0x4404: case 0x4406: case 0x4408: case 0x440a: case 0x440c: case 0x4400:
    case 0x6400: case 0x2400: case 0x640a:
        return { KIND_READONLY, 0, 0 };
    default:
        break;
    }
    return { KIND_SHADOW, 0, 0 };
}

/* ------------------------------------------------------------------------------------------------------ */
/* Segment access rights                                                                                   */
/* ------------------------------------------------------------------------------------------------------ */

/*
 * VMX access rights: 3:0 type, 4 S, 6:5 DPL, 7 P, 12 AVL, 13 L, 14 D/B, 15 G, 16 "unusable".
 * VMCB attrib (12 bits): 3:0 type, 4 S, 6:5 DPL, 7 P, 8 AVL, 9 L, 10 D/B, 11 G.
 */
static const uint32_t AR_UNUSABLE = 1u << 16;

static inline uint16_t attribFromAr(uint32_t ar)
{
    if (ar & AR_UNUSABLE)
        return 0;                                     /* P=0: unusable on both sides */
    return (uint16_t)((ar & 0xFF) | ((ar >> 4) & 0xF00));
}

static inline uint32_t arFromAttrib(uint16_t attrib)
{
    uint32_t ar = (uint32_t)(attrib & 0xFF) | ((uint32_t)(attrib & 0xF00) << 4);
    if (!(attrib & 0x80))
        ar |= AR_UNUSABLE;                            /* not present -> unusable */
    return ar;
}

/* ------------------------------------------------------------------------------------------------------ */
/* Exits                                                                                                   */
/* ------------------------------------------------------------------------------------------------------ */

/* VMX basic exit reasons (Intel SDM Vol. 3, Appendix C). */
enum ExitReason : uint32_t {
    VMX_EXC_NMI = 0, VMX_EXT_INTR = 1, VMX_TRIPLE_FAULT = 2, VMX_INIT = 3, VMX_IRQ_WINDOW = 7,
    VMX_NMI_WINDOW = 8, VMX_TASK_SWITCH = 9, VMX_CPUID = 10, VMX_HLT = 12, VMX_INVD = 13, VMX_INVLPG = 14,
    VMX_RDPMC = 15, VMX_RDTSC = 16, VMX_VMCALL = 18, VMX_CR_ACCESS = 28, VMX_DR_ACCESS = 29, VMX_IO = 30,
    VMX_RDMSR = 31, VMX_WRMSR = 32, VMX_INVALID_GUEST_STATE = 33, VMX_MWAIT = 36, VMX_MONITOR = 39,
    VMX_PAUSE = 40, VMX_EPT_VIOLATION = 48, VMX_RDTSCP = 51, VMX_WBINVD = 54, VMX_XSETBV = 55
};

/* Bit 31 of the exit reason: the VM entry itself failed. */
static const uint32_t VMX_ENTRY_FAILURE = 1u << 31;

/* VM-exit interruption information (Intel SDM Vol. 3, 24.9.2). */
static const uint32_t INTR_VALID      = 1u << 31;
static const uint32_t INTR_ERRCODE    = 1u << 11;
enum IntrType : uint32_t { INTR_EXT = 0, INTR_NMI = 2, INTR_HW_EXC = 3, INTR_SW_INT = 4, INTR_SW_EXC = 6 };

struct SvmExit {
    uint64_t code;       /* control.exitCode */
    uint64_t info1;      /* control.exitInfo1 */
    uint64_t info2;      /* control.exitInfo2 */
    uint64_t rip;        /* save.rip */
    uint64_t nextRip;    /* control.nextRip, 0 when not saved */
    uint32_t nptPerm;    /* for NPF: rights the faulting page has (npt::PERM_*), 0 when unmapped */
};

struct VmxExit {
    uint32_t reason;     /* VMCS_RO_EXIT_REASON */
    uint64_t qual;       /* VMCS_RO_EXIT_QUALIFIC */
    uint32_t instrLen;   /* VMCS_RO_VMEXIT_INSTR_LEN */
    uint32_t intrInfo;   /* VMCS_RO_VMEXIT_IRQ_INFO */
    uint32_t intrError;  /* VMCS_RO_VMEXIT_IRQ_ERROR */
    uint64_t gpa;        /* VMCS_GUEST_PHYSICAL_ADDRESS, EPT violations */
};

/* Exceptions that push an error code: #DF, #TS, #NP, #SS, #GP, #PF, #AC, #CP. */
static inline bool vectorHasErrorCode(unsigned v)
{
    return v == 8 || (v >= 10 && v <= 14) || v == 17 || v == 21;
}

/*
 * Fills `out` for an SVM exit. Returns false when there is no VMX equivalent (the caller then injects #UD
 * for the SVM instructions, or treats it as an internal error).
 */
static inline bool translateExit(const SvmExit &in, VmxExit *out)
{
    using namespace rhv::vmcb;
    *out = VmxExit();
    const uint64_t code = in.code;

    out->instrLen = in.nextRip > in.rip ? (uint32_t)(in.nextRip - in.rip) : 0;

    /* Control register reads (0x00-0x0F) and writes (0x10-0x1F), debug registers (0x20-0x3F). */
    if (code <= 0x1F || code == 0x65) {
        unsigned cr    = code == 0x65 ? 0 : (unsigned)(code & 0xF);
        bool     write = code == 0x65 || code >= 0x10;
        uint64_t gpr   = (in.info1 >> 63) ? (in.info1 & 0xF) : 0;   /* decode assists: bit 63 = valid */
        out->reason = VMX_CR_ACCESS;
        out->qual   = (uint64_t)cr | ((write ? 0u : 1u) << 4) | (gpr << 8);
        return true;
    }
    if (code >= 0x20 && code <= 0x3F) {
        unsigned dr    = (unsigned)(code & 0xF);
        bool     write = code >= 0x30;
        uint64_t gpr   = in.info1 & 0xF;
        out->reason = VMX_DR_ACCESS;
        out->qual   = (uint64_t)dr | ((write ? 0u : 1u) << 4) | (gpr << 8);
        return true;
    }
    /* Exceptions 0-31 (#DB, #BP, #PF, ...). EXITINFO1 carries the error code when there is one. */
    if (code >= 0x40 && code <= 0x5F) {
        unsigned vec = (unsigned)(code - 0x40);
        out->reason   = VMX_EXC_NMI;
        out->intrInfo = INTR_VALID | ((uint32_t)(vec == 3 ? INTR_SW_EXC : INTR_HW_EXC) << 8) | vec;
        if (vectorHasErrorCode(vec)) {
            out->intrInfo |= INTR_ERRCODE;
            out->intrError = (uint32_t)in.info1;
        }
        if (vec == 14)
            out->qual = in.info2;               /* #PF: faulting linear address, like CR2 */
        return true;
    }

    switch (code) {
    case EXIT_INTR:  out->reason = VMX_EXT_INTR; return true;
    case EXIT_NMI:
        out->reason   = VMX_EXC_NMI;
        out->intrInfo = INTR_VALID | ((uint32_t)INTR_NMI << 8) | 2;
        return true;
    case 0x63:       out->reason = VMX_INIT; return true;
    case 0x64:       out->reason = VMX_IRQ_WINDOW; return true;          /* VINTR */
    case 0x6E:       out->reason = VMX_RDTSC; return true;
    case 0x6F:       out->reason = VMX_RDPMC; return true;
    case EXIT_CPUID: out->reason = VMX_CPUID; return true;
    case 0x76:       out->reason = VMX_INVD; return true;
    case 0x77:       out->reason = VMX_PAUSE; return true;
    case EXIT_HLT:   out->reason = VMX_HLT; return true;
    case 0x79:       out->reason = VMX_INVLPG; return true;
    case 0x7D:       out->reason = VMX_TASK_SWITCH; return true;
    case EXIT_SHUTDOWN: out->reason = VMX_TRIPLE_FAULT; return true;
    case EXIT_VMMCALL:  out->reason = VMX_VMCALL; return true;
    case 0x87:       out->reason = VMX_RDTSCP; return true;
    case 0x89:       out->reason = VMX_WBINVD; return true;
    case 0x8A:       out->reason = VMX_MONITOR; return true;
    case 0x8B:       out->reason = VMX_MWAIT; return true;
    case 0x8D:       out->reason = VMX_XSETBV; return true;

    case EXIT_MSR:
        out->reason = (in.info1 & 1) ? VMX_WRMSR : VMX_RDMSR;            /* EXITINFO1: 0 = RDMSR, 1 = WRMSR */
        return true;

    case EXIT_IOIO: {
        /*
         * SVM EXITINFO1: 0 TYPE (1 = IN), 2 STR, 3 REP, 4/5/6 SZ8/SZ16/SZ32, 31:16 port.
         * VMX qualification: 2:0 size-1, 3 direction (1 = IN), 4 string, 5 REP, 6 immediate operand, 31:16 port.
         */
        unsigned size = (in.info1 & (1u << 4)) ? 1 : (in.info1 & (1u << 5)) ? 2 : (in.info1 & (1u << 6)) ? 4 : 0;
        if (size == 0)
            return false;
        out->reason = VMX_IO;
        out->qual   = (uint64_t)(size - 1) | ((in.info1 & 1) ? (1u << 3) : 0) | ((in.info1 & (1u << 2)) ? (1u << 4) : 0) |
                      ((in.info1 & (1u << 3)) ? (1u << 5) : 0) | (((in.info1 >> 16) & 0xFFFF) << 16);
        return true;
    }

    case EXIT_NPF: {
        /*
         * SVM EXITINFO1 is a page-fault style error code (bit 0 P, 1 W, 4 ID); EXITINFO2 the guest physical address.
         * VMX EPT-violation qualification: 0 data read, 1 data write, 2 instruction fetch, 3/4/5 the page was
         * readable/writable/executable. The page rights come from our NPT, which the caller looks up.
         */
        bool write = (in.info1 & (1u << 1)) != 0;
        bool fetch = (in.info1 & (1u << 4)) != 0;
        out->reason = VMX_EPT_VIOLATION;
        out->qual   = (fetch ? (1u << 2) : write ? (1u << 1) : (1u << 0)) |
                      ((in.nptPerm & 1u) ? (1u << 3) : 0) | ((in.nptPerm & 2u) ? (1u << 4) : 0) |
                      ((in.nptPerm & 4u) ? (1u << 5) : 0);
        out->gpa = in.info2;
        out->instrLen = 0;
        return true;
    }

    case EXIT_INVALID:
        out->reason = VMX_ENTRY_FAILURE | VMX_INVALID_GUEST_STATE;
        return true;
    default:
        return false;
    }
}

} // namespace vmx
} // namespace rhv

#endif /* RYZENHV_VMXMAP_HPP */
