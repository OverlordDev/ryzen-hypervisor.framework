/*
 * VMCB.hpp - AMD SVM Virtual Machine Control Block layout.
 *
 * Source: AMD64 Architecture Programmer's Manual Vol. 2, Appendix B.
 * Pure data, only <stdint.h>/<stddef.h>, so tests/vmcb_layout.cpp can verify
 * every offset on any host. If an offset is wrong the guest silently
 * misbehaves, so those static_asserts are the first line of defence.
 */
#ifndef RYZENHV_VMCB_HPP
#define RYZENHV_VMCB_HPP

#include <stddef.h>
#include <stdint.h>

namespace rhv {
namespace vmcb {

#define RHV_PACKED __attribute__((packed))

struct RHV_PACKED Seg {
    uint16_t selector;
    uint16_t attrib;     /* VMCB format, see segAttrib() */
    uint32_t limit;
    uint64_t base;
};

/* Control area, offsets 0x000 .. 0x3FF. */
struct RHV_PACKED Control {
    uint32_t interceptCr;        /* 0x000 reads in 15:0, writes in 31:16 */
    uint32_t interceptDr;        /* 0x004 */
    uint32_t interceptExc;       /* 0x008 */
    uint32_t interceptVec3;      /* 0x00C INTR, NMI, ..., CPUID, HLT, IOIO, MSR, SHUTDOWN */
    uint32_t interceptVec4;      /* 0x010 VMRUN, VMMCALL, VMLOAD, ... */
    uint32_t interceptVec5;      /* 0x014 */
    uint8_t  rsvd1[36];          /* 0x018 */
    uint16_t pauseFilterThresh;  /* 0x03C */
    uint16_t pauseFilterCount;   /* 0x03E */
    uint64_t iopmBasePa;         /* 0x040 */
    uint64_t msrpmBasePa;        /* 0x048 */
    uint64_t tscOffset;          /* 0x050 */
    uint32_t asid;               /* 0x058 */
    uint8_t  tlbCtl;             /* 0x05C */
    uint8_t  rsvd2[3];           /* 0x05D */
    uint32_t intCtl;             /* 0x060 */
    uint32_t intVector;          /* 0x064 */
    uint32_t intState;           /* 0x068 */
    uint32_t rsvd3;              /* 0x06C */
    uint64_t exitCode;           /* 0x070 */
    uint64_t exitInfo1;          /* 0x078 */
    uint64_t exitInfo2;          /* 0x080 */
    uint32_t exitIntInfo;        /* 0x088 */
    uint32_t exitIntInfoErr;     /* 0x08C */
    uint64_t nestedCtl;          /* 0x090 bit 0 = NP_ENABLE */
    uint64_t avicVapicBar;       /* 0x098 */
    uint64_t ghcbGpa;            /* 0x0A0 */
    uint32_t eventInj;           /* 0x0A8 */
    uint32_t eventInjErr;        /* 0x0AC */
    uint64_t nestedCr3;          /* 0x0B0 */
    uint64_t virtExt;            /* 0x0B8 */
    uint32_t clean;              /* 0x0C0 */
    uint32_t rsvd5;              /* 0x0C4 */
    uint64_t nextRip;            /* 0x0C8 */
    uint8_t  insnLen;            /* 0x0D0 */
    uint8_t  insnBytes[15];      /* 0x0D1 */
    uint8_t  rsvd6[0x400 - 0x0E0];
};

/* State save area, offsets 0x400 .. (VMCB-relative). Offsets below are relative to 0x400. */
struct RHV_PACKED Save {
    Seg      es, cs, ss, ds, fs, gs, gdtr, ldtr, idtr, tr;   /* 0x000 .. 0x09F */
    uint8_t  rsvd1[0xCB - 0xA0];
    uint8_t  cpl;                /* 0x0CB */
    uint8_t  rsvd2[4];
    uint64_t efer;               /* 0x0D0 */
    uint8_t  rsvd3[0x148 - 0x0D8];
    uint64_t cr4;                /* 0x148 */
    uint64_t cr3;                /* 0x150 */
    uint64_t cr0;                /* 0x158 */
    uint64_t dr7;                /* 0x160 */
    uint64_t dr6;                /* 0x168 */
    uint64_t rflags;             /* 0x170 */
    uint64_t rip;                /* 0x178 */
    uint8_t  rsvd4[0x1D8 - 0x180];
    uint64_t rsp;                /* 0x1D8 */
    uint64_t sCet, ssp, isstAddr;/* 0x1E0 .. 0x1F0 */
    uint64_t rax;                /* 0x1F8 */
    uint64_t star;               /* 0x200 */
    uint64_t lstar;              /* 0x208 */
    uint64_t cstar;              /* 0x210 */
    uint64_t sfmask;             /* 0x218 */
    uint64_t kernelGsBase;       /* 0x220 */
    uint64_t sysenterCs;         /* 0x228 */
    uint64_t sysenterEsp;        /* 0x230 */
    uint64_t sysenterEip;        /* 0x238 */
    uint64_t cr2;                /* 0x240 */
    uint8_t  rsvd5[0x268 - 0x248];
    uint64_t gPat;               /* 0x268 */
    uint64_t dbgctl;             /* 0x270 */
    uint64_t brFrom, brTo, lastExcFrom, lastExcTo;  /* 0x278 .. 0x290 */
};

struct RHV_PACKED Vmcb {
    Control control;
    Save    save;
    uint8_t pad[4096 - sizeof(Control) - sizeof(Save)];
};

static_assert(sizeof(Seg) == 16, "Seg");
static_assert(sizeof(Control) == 0x400, "Control must be 1 KiB");
static_assert(sizeof(Vmcb) == 4096, "VMCB must be one page");

#define RHV_VMCB_CTL(f, off)  static_assert(offsetof(Control, f) == (off), "Control." #f)
RHV_VMCB_CTL(interceptCr,   0x000);
RHV_VMCB_CTL(interceptDr,   0x004);
RHV_VMCB_CTL(interceptExc,  0x008);
RHV_VMCB_CTL(interceptVec3, 0x00C);
RHV_VMCB_CTL(interceptVec4, 0x010);
RHV_VMCB_CTL(pauseFilterThresh, 0x03C);
RHV_VMCB_CTL(iopmBasePa,    0x040);
RHV_VMCB_CTL(msrpmBasePa,   0x048);
RHV_VMCB_CTL(tscOffset,     0x050);
RHV_VMCB_CTL(asid,          0x058);
RHV_VMCB_CTL(tlbCtl,        0x05C);
RHV_VMCB_CTL(intCtl,        0x060);
RHV_VMCB_CTL(intState,      0x068);
RHV_VMCB_CTL(exitCode,      0x070);
RHV_VMCB_CTL(exitInfo1,     0x078);
RHV_VMCB_CTL(exitInfo2,     0x080);
RHV_VMCB_CTL(exitIntInfo,   0x088);
RHV_VMCB_CTL(nestedCtl,     0x090);
RHV_VMCB_CTL(eventInj,      0x0A8);
RHV_VMCB_CTL(nestedCr3,     0x0B0);
RHV_VMCB_CTL(clean,         0x0C0);
RHV_VMCB_CTL(nextRip,       0x0C8);
RHV_VMCB_CTL(insnLen,       0x0D0);
#undef RHV_VMCB_CTL

#define RHV_VMCB_SAVE(f, off) static_assert(offsetof(Save, f) == (off), "Save." #f)
RHV_VMCB_SAVE(es,     0x000);
RHV_VMCB_SAVE(cs,     0x010);
RHV_VMCB_SAVE(ss,     0x020);
RHV_VMCB_SAVE(ds,     0x030);
RHV_VMCB_SAVE(fs,     0x040);
RHV_VMCB_SAVE(gs,     0x050);
RHV_VMCB_SAVE(gdtr,   0x060);
RHV_VMCB_SAVE(ldtr,   0x070);
RHV_VMCB_SAVE(idtr,   0x080);
RHV_VMCB_SAVE(tr,     0x090);
RHV_VMCB_SAVE(cpl,    0x0CB);
RHV_VMCB_SAVE(efer,   0x0D0);
RHV_VMCB_SAVE(cr4,    0x148);
RHV_VMCB_SAVE(cr3,    0x150);
RHV_VMCB_SAVE(cr0,    0x158);
RHV_VMCB_SAVE(dr7,    0x160);
RHV_VMCB_SAVE(dr6,    0x168);
RHV_VMCB_SAVE(rflags, 0x170);
RHV_VMCB_SAVE(rip,    0x178);
RHV_VMCB_SAVE(rsp,    0x1D8);
RHV_VMCB_SAVE(rax,    0x1F8);
RHV_VMCB_SAVE(star,   0x200);
RHV_VMCB_SAVE(kernelGsBase, 0x220);
RHV_VMCB_SAVE(sysenterEip,  0x238);
RHV_VMCB_SAVE(cr2,    0x240);
RHV_VMCB_SAVE(gPat,   0x268);
RHV_VMCB_SAVE(dbgctl, 0x270);
#undef RHV_VMCB_SAVE

/* The save area starts at VMCB offset 0x400. */
static_assert(offsetof(Vmcb, save) == 0x400, "Save area must start at 0x400");

/* Intercept bits. Vector 3 lives at VMCB 0x00C, vector 4 at 0x010. */
enum : uint32_t {
    V3_INTR     = 1u << 0,
    V3_NMI      = 1u << 1,
    V3_SMI      = 1u << 2,
    V3_INIT     = 1u << 3,
    V3_CPUID    = 1u << 18,
    V3_INVD     = 1u << 22,
    V3_HLT      = 1u << 24,
    V3_IOIO     = 1u << 27,
    V3_MSR      = 1u << 28,
    V3_SHUTDOWN = 1u << 31
};

enum : uint32_t {
    V4_VMRUN    = 1u << 0,   /* mandatory: VMRUN refuses a VMCB without it */
    V4_VMMCALL  = 1u << 1,
    V4_VMLOAD   = 1u << 2,
    V4_VMSAVE   = 1u << 3,
    V4_STGI     = 1u << 4,
    V4_CLGI     = 1u << 5,
    V4_SKINIT   = 1u << 6,
    V4_WBINVD   = 1u << 9,
    V4_MONITOR  = 1u << 10,
    V4_MWAIT    = 1u << 11
};

/* control.intCtl */
static const uint32_t INT_CTL_V_INTR_MASKING = 1u << 24;

/* control.nestedCtl */
static const uint64_t NESTED_CTL_NP_ENABLE = 1ull << 0;

/* control.tlbCtl */
enum : uint8_t { TLB_DO_NOTHING = 0, TLB_FLUSH_ALL = 1 };

/* SVM exit codes the kernel itself cares about (users get the raw value too). */
enum : uint64_t {
    EXIT_INTR     = 0x60,
    EXIT_NMI      = 0x61,
    EXIT_CPUID    = 0x72,
    EXIT_HLT      = 0x78,
    EXIT_IOIO     = 0x7B,
    EXIT_MSR      = 0x7C,
    EXIT_SHUTDOWN = 0x7F,
    EXIT_VMMCALL  = 0x81,
    EXIT_NPF      = 0x400,
    EXIT_INVALID  = ~0ull
};

/* Guest EFER must have SVME set, otherwise VMRUN fails its consistency checks. */
static const uint64_t EFER_SVME = 1ull << 12;

/*
 * VMCB segment attribute: a 12-bit field,
 *   0-3 type, 4 S, 5-6 DPL, 7 P, 8 AVL, 9 L, 10 D/B, 11 G.
 * Converts from the VMX/descriptor "access rights" layout (AVL/L/D/G at 12-15).
 */
static inline uint16_t segAttrib(uint32_t accessRights)
{
    return (uint16_t)((accessRights & 0xFF) | ((accessRights >> 4) & 0xF00));
}

/* Real-mode segment: present, S=1, data read/write accessed (0x93) or code exec/read accessed (0x9B). */
static const uint16_t ATTR_REAL_DATA = 0x93;
static const uint16_t ATTR_REAL_CODE = 0x9B;
static const uint16_t ATTR_LDT       = 0x82;
static const uint16_t ATTR_TSS_BUSY  = 0x8B;

} // namespace vmcb
} // namespace rhv

#endif /* RYZENHV_VMCB_HPP */
