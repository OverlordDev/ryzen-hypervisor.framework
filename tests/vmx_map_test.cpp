/*
 * vmx_map_test.cpp - host-side tests for kext/Source/VmxMap.hpp.
 *
 * When the Hypervisor.framework SDK headers are available (any Mac with Xcode) every field in
 * VmcsFields.def is compared against Apple's own constant of the same name.
 */
#include <initializer_list>
#include <stdio.h>
#include <string.h>

#include "../kext/Source/VmxMap.hpp"

#if defined(__x86_64__) && __has_include(<Hypervisor/hv_arch_vmx.h>)
#include <Hypervisor/hv_arch_vmx.h>
#define HAVE_SDK 1
#endif

using namespace rhv::vmx;
using namespace rhv::vmcb;

namespace {

int fails = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
            fails++;                                                      \
        }                                                                 \
    } while (0)

void testFieldList()
{
    CHECK(kNumFields > 150);
    for (unsigned i = 1; i < kNumFields; i++)
        CHECK(kFields[i - 1].encoding < kFields[i].encoding);          /* sorted and unique */
    for (unsigned i = 0; i < kNumFields; i++) {
        CHECK(fieldIndex(kFields[i].encoding) == (int)i);
        CHECK(!fieldIsHighHalf(kFields[i].encoding));                  /* the framework lists full fields only */
    }
    CHECK(fieldIndex(0x1) == -1 && fieldIndex(0x681f) == -1 && fieldIndex(0xFFFFFFFF) == -1);

#ifdef HAVE_SDK
    /* Every entry must equal Apple's constant of the same name. */
    int checked = 0;
#define F(name, enc) CHECK((uint32_t)(name) == (uint32_t)(enc)); checked++;
#include "../kext/Source/VmcsFields.def"
#undef F
    CHECK(checked == (int)kNumFields);
    printf("  (checked %d fields against the SDK headers)\n", checked);
#else
    printf("  (no SDK headers: skipped the comparison with Apple's constants)\n");
#endif

    /* Width from the encoding. */
    CHECK(fieldBytes(0x0800) == 2);        /* GUEST_ES selector */
    CHECK(fieldBytes(0x4800) == 4);        /* GUEST_ES_LIMIT */
    CHECK(fieldBytes(0x2802) == 8);        /* GUEST_IA32_DEBUGCTL */
    CHECK(fieldBytes(0x681e) == 8);        /* GUEST_RIP */
}

void testMapField()
{
    /* Spot checks against offsets we know from the AMD manual. */
    StateMap m = mapField(0x681e);                                     /* GUEST_RIP */
    CHECK(m.kind == KIND_VMCB && m.vmcbOff == 0x400 + 0x178 && m.vmcbSize == 8);
    m = mapField(0x681c);  CHECK(m.kind == KIND_VMCB && m.vmcbOff == 0x400 + 0x1D8);   /* RSP */
    m = mapField(0x6820);  CHECK(m.kind == KIND_VMCB && m.vmcbOff == 0x400 + 0x170);   /* RFLAGS */
    m = mapField(0x6800);  CHECK(m.kind == KIND_VMCB && m.vmcbOff == 0x400 + 0x158);   /* CR0 */
    m = mapField(0x6802);  CHECK(m.kind == KIND_VMCB && m.vmcbOff == 0x400 + 0x150);   /* CR3 */
    m = mapField(0x6804);  CHECK(m.kind == KIND_VMCB && m.vmcbOff == 0x400 + 0x148);   /* CR4 */
    m = mapField(0x2806);  CHECK(m.kind == KIND_EFER && m.vmcbOff == 0x400 + 0x0D0);
    m = mapField(0x2804);  CHECK(m.kind == KIND_VMCB && m.vmcbOff == 0x400 + 0x268);   /* PAT */
    m = mapField(0x0802);  CHECK(m.kind == KIND_VMCB && m.vmcbOff == 0x400 + 0x010 && m.vmcbSize == 2);   /* CS selector */
    m = mapField(0x4802);  CHECK(m.kind == KIND_VMCB && m.vmcbOff == 0x400 + 0x010 + 4 && m.vmcbSize == 4); /* CS limit */
    m = mapField(0x6808);  CHECK(m.kind == KIND_VMCB && m.vmcbOff == 0x400 + 0x010 + 8 && m.vmcbSize == 8); /* CS base */
    m = mapField(0x4816);  CHECK(m.kind == KIND_SEG_AR && m.vmcbOff == 0x400 + 0x010 + 2);                   /* CS AR -> attrib */
    m = mapField(0x4822);  CHECK(m.kind == KIND_SEG_AR && m.vmcbOff == 0x400 + 0x090 + 2);                   /* TR AR */
    m = mapField(0x6816);  CHECK(m.kind == KIND_VMCB && m.vmcbOff == 0x400 + 0x060 + 8);                     /* GDTR base */
    m = mapField(0x4812);  CHECK(m.kind == KIND_VMCB && m.vmcbOff == 0x400 + 0x080 + 4);                     /* IDTR limit */
    m = mapField(0x2010);  CHECK(m.kind == KIND_VMCB && m.vmcbOff == 0x050 && m.vmcbSize == 8);              /* TSC offset */
    m = mapField(0x4004);  CHECK(m.kind == KIND_VMCB && m.vmcbOff == 0x008 && m.vmcbSize == 4);              /* exception bitmap */
    m = mapField(0x4402);  CHECK(m.kind == KIND_READONLY);             /* exit reason */
    m = mapField(0x4002);  CHECK(m.kind == KIND_SHADOW);               /* CPU_BASED controls */

    /* Every mapped field stays inside the VMCB, naturally aligned for its size, and none overlaps another. */
    static uint8_t used[4096];
    for (unsigned i = 0; i < kNumFields; i++) {
        StateMap s = mapField(kFields[i].encoding);
        if (s.kind != KIND_VMCB && s.kind != KIND_SEG_AR && s.kind != KIND_EFER)
            continue;
        CHECK(s.vmcbSize == 2 || s.vmcbSize == 4 || s.vmcbSize == 8);
        CHECK(s.vmcbOff + s.vmcbSize <= 4096);
        CHECK(s.vmcbOff % s.vmcbSize == 0);
        CHECK(s.vmcbSize <= 8);
        for (unsigned b = 0; b < s.vmcbSize; b++) {
            if (used[s.vmcbOff + b]) {
                printf("  overlap at 0x%x (field %s)\n", s.vmcbOff + b, kFields[i].name);
                CHECK(false);
            }
            used[s.vmcbOff + b] = 1;
        }
        /* The VMCS field width is never narrower than what we store. */
        CHECK(fieldBytes(kFields[i].encoding) >= (s.kind == KIND_SEG_AR ? 2u : (unsigned)s.vmcbSize) ||
              s.vmcbSize == 8);
    }
    /* Read-only exit fields are all real fields. */
    CHECK(fieldIndex(0x4402) >= 0 && fieldIndex(0x6400) >= 0 && fieldIndex(0x2400) >= 0);
}

void testExtensionFields()
{
    CHECK(isExtField(X_CR2) && isExtField(X_DR6) && isExtField(X_TPR));
    CHECK(isExtField(X_DR0) && isExtField(X_DR3));
    CHECK(!isExtField(0xF001) && !isExtField(0xF006) && !isExtField(0xF018) && !isExtField(0x681e));
    for (unsigned e : { (unsigned)X_CR2, (unsigned)X_DR6, (unsigned)X_TPR, (unsigned)X_DR0, (unsigned)X_DR3 }) {
        CHECK(fieldIndex(e) == -1);                     /* never confused with an Intel field */
        CHECK(fieldBytes(e) == 8);
    }
    StateMap m = mapField(X_CR2);   CHECK(m.kind == KIND_VMCB && m.vmcbOff == 0x400 + 0x240 && m.vmcbSize == 8);
    m = mapField(X_DR6);            CHECK(m.kind == KIND_VMCB && m.vmcbOff == 0x400 + 0x168);
    m = mapField(X_TPR);            CHECK(m.kind == KIND_VMCB && m.vmcbOff == 0x060 && m.vmcbSize == 1);
    for (unsigned i = 0; i < 4; i++) {
        m = mapField(X_DR0 + 2 * i);
        CHECK(m.kind == KIND_EXT_DR && m.vmcbOff == i);
    }
}

void testAccessRights()
{
    /* Flat 64-bit code segment: type 0xB, S=1, DPL=0, P=1, L=1, G=1 -> AR 0xA09B. */
    CHECK(attribFromAr(0xA09B) == 0xA9B);
    CHECK(arFromAttrib(0xA9B) == 0xA09B);
    /* Flat 32-bit data: type 3, S, P, D/B, G -> 0xC093. */
    CHECK(attribFromAr(0xC093) == 0xC93 && arFromAttrib(0xC93) == 0xC093);
    /* Real-mode data (our VMCB uses 0x93). */
    CHECK(arFromAttrib(0x93) == 0x93 && attribFromAr(0x93) == 0x93);
    /* TSS busy 64-bit: type 0xB, S=0, P=1 -> 0x8B. LDT: 0x82. */
    CHECK(attribFromAr(0x8B) == 0x8B && arFromAttrib(0x8B) == 0x8B);
    /* Unusable segment becomes not-present and back. */
    CHECK(attribFromAr(0x10000) == 0 && attribFromAr(0x1C093) == 0);
    CHECK((arFromAttrib(0) & AR_UNUSABLE) != 0);
    CHECK((arFromAttrib(0x93) & AR_UNUSABLE) == 0);
    /* Round trip for every usable combination of the bits that exist on both sides. */
    for (uint32_t a = 0; a < 0x1000; a++) {
        if (!(a & 0x80))
            continue;
        CHECK(attribFromAr(arFromAttrib((uint16_t)a)) == a);
    }
    /* Agrees with the existing helper used for the reset state. */
    CHECK(rhv::vmcb::segAttrib(0xA09B) == attribFromAr(0xA09B));
}

SvmExit ex(uint64_t code, uint64_t i1 = 0, uint64_t i2 = 0, uint64_t rip = 0x1000, uint64_t next = 0, uint32_t perm = 0)
{
    SvmExit e = {};
    e.code = code; e.info1 = i1; e.info2 = i2; e.rip = rip; e.nextRip = next; e.nptPerm = perm;
    return e;
}

void testExits()
{
    VmxExit v;

    CHECK(translateExit(ex(EXIT_CPUID, 0, 0, 0x1014, 0x1016), &v) && v.reason == VMX_CPUID && v.instrLen == 2);
    CHECK(translateExit(ex(EXIT_HLT, 0, 0, 0x1016, 0x1017), &v) && v.reason == VMX_HLT && v.instrLen == 1);
    CHECK(translateExit(ex(EXIT_SHUTDOWN), &v) && v.reason == VMX_TRIPLE_FAULT);
    CHECK(translateExit(ex(EXIT_VMMCALL), &v) && v.reason == VMX_VMCALL);
    CHECK(translateExit(ex(EXIT_INTR), &v) && v.reason == VMX_EXT_INTR && !(v.intrInfo & INTR_VALID));
    CHECK(translateExit(ex(0x64), &v) && v.reason == VMX_IRQ_WINDOW);
    CHECK(translateExit(ex(0x89), &v) && v.reason == VMX_WBINVD);
    CHECK(translateExit(ex(0x8D), &v) && v.reason == VMX_XSETBV);
    CHECK(translateExit(ex(0x6E), &v) && v.reason == VMX_RDTSC);
    CHECK(translateExit(ex(0x87), &v) && v.reason == VMX_RDTSCP);

    /* MSR: EXITINFO1 0 = RDMSR, 1 = WRMSR. */
    CHECK(translateExit(ex(EXIT_MSR, 0), &v) && v.reason == VMX_RDMSR);
    CHECK(translateExit(ex(EXIT_MSR, 1), &v) && v.reason == VMX_WRMSR);

    /* I/O: OUT DX,AL to 0x3F8 (what rhv-guest-test does): size 1, out. */
    uint64_t outb = ((uint64_t)0x3F8 << 16) | (1u << 4);
    CHECK(translateExit(ex(EXIT_IOIO, outb), &v) && v.reason == VMX_IO);
    CHECK((v.qual & 7) == 0 && !(v.qual & 8) && ((v.qual >> 16) == 0x3F8));
    /* IN AX,DX from 0x60: size 2, in. */
    uint64_t inw = ((uint64_t)0x60 << 16) | (1u << 5) | 1u;
    CHECK(translateExit(ex(EXIT_IOIO, inw), &v) && (v.qual & 7) == 1 && (v.qual & 8) && (v.qual >> 16) == 0x60);
    /* REP INSD: size 4, string, rep, in. */
    uint64_t rep = ((uint64_t)0xCF8 << 16) | (1u << 6) | (1u << 3) | (1u << 2) | 1u;
    CHECK(translateExit(ex(EXIT_IOIO, rep), &v) && (v.qual & 7) == 3 && (v.qual & 8) && (v.qual & 16) && (v.qual & 32));
    CHECK(!translateExit(ex(EXIT_IOIO, 0), &v));          /* no size bit: malformed */

    /* NPF -> EPT violation. info1 bit1 = write, bit4 = fetch. Rights come from the NPT lookup. */
    CHECK(translateExit(ex(EXIT_NPF, 0x6, 0x102010, 0x1018, 0, 0), &v) && v.reason == VMX_EPT_VIOLATION);
    CHECK((v.qual & 7) == 2 && (v.qual & 0x38) == 0 && v.gpa == 0x102010);      /* write to an unmapped page */
    CHECK(translateExit(ex(EXIT_NPF, 0x7, 0x101010, 0x1013, 0, 1 | 4 /*R,X*/), &v));
    CHECK((v.qual & 7) == 2 && (v.qual & (1 << 3)) && !(v.qual & (1 << 4)) && (v.qual & (1 << 5)));   /* write to a R-X page */
    CHECK(translateExit(ex(EXIT_NPF, 0x15, 0), &v) && (v.qual & 7) == 4);       /* instruction fetch */
    CHECK(translateExit(ex(EXIT_NPF, 0x4, 0x100010), &v) && (v.qual & 7) == 1); /* data read */
    CHECK(v.instrLen == 0);

    /* Exceptions: #PF has an error code and the faulting address; #BP is a software exception; #UD has no code. */
    CHECK(translateExit(ex(0x40 + 14, 0x2, 0xdead000), &v) && v.reason == VMX_EXC_NMI);
    CHECK((v.intrInfo & 0xFF) == 14 && (v.intrInfo & INTR_VALID) && (v.intrInfo & INTR_ERRCODE) && v.intrError == 2 && v.qual == 0xdead000);
    CHECK(((v.intrInfo >> 8) & 7) == INTR_HW_EXC);
    CHECK(translateExit(ex(0x40 + 3), &v) && ((v.intrInfo >> 8) & 7) == INTR_SW_EXC);
    CHECK(translateExit(ex(0x40 + 6), &v) && !(v.intrInfo & INTR_ERRCODE));
    CHECK(translateExit(ex(0x40 + 13, 0x10), &v) && (v.intrInfo & INTR_ERRCODE) && v.intrError == 0x10);
    CHECK(translateExit(ex(EXIT_NMI), &v) && v.reason == VMX_EXC_NMI && ((v.intrInfo >> 8) & 7) == INTR_NMI && (v.intrInfo & 0xFF) == 2);

    /* Control registers: mov to CR3 from RBX (gpr 3), mov from CR4 into R9. SVM sets bit 63 of EXITINFO1 when it decoded the GPR. */
    CHECK(translateExit(ex(0x10 + 3, (1ull << 63) | 3), &v) && v.reason == VMX_CR_ACCESS);
    CHECK((v.qual & 0xF) == 3 && ((v.qual >> 4) & 3) == 0 && ((v.qual >> 8) & 0xF) == 3);
    CHECK(translateExit(ex(4, (1ull << 63) | 9), &v) && (v.qual & 0xF) == 4 && ((v.qual >> 4) & 3) == 1 && ((v.qual >> 8) & 0xF) == 9);
    CHECK(translateExit(ex(0x65, (1ull << 63) | 1), &v) && (v.qual & 0xF) == 0 && ((v.qual >> 4) & 3) == 0);   /* selective CR0 write */
    /* Debug registers. */
    CHECK(translateExit(ex(0x30 + 7, 2), &v) && v.reason == VMX_DR_ACCESS && (v.qual & 7) == 7 && !(v.qual & 16) && ((v.qual >> 8) & 0xF) == 2);
    CHECK(translateExit(ex(0x20 + 6, 0), &v) && (v.qual & 16));

    /* VMRUN failed. */
    CHECK(translateExit(ex(EXIT_INVALID), &v) && (v.reason & VMX_ENTRY_FAILURE) && (v.reason & 0xFFFF) == VMX_INVALID_GUEST_STATE);

    /* SVM-only exits have no VMX meaning. */
    CHECK(!translateExit(ex(0x80), &v));      /* VMRUN */
    CHECK(!translateExit(ex(0x82), &v));      /* VMLOAD */
    CHECK(!translateExit(ex(0x400 + 5), &v));
}

} // namespace

int main()
{
    testFieldList();
    testMapField();
    testExtensionFields();
    testAccessRights();
    testExits();
    if (fails) {
        printf("vmx_map_test: %d FAILED\n", fails);
        return 1;
    }
    printf("vmx_map_test: OK\n");
    return 0;
}
