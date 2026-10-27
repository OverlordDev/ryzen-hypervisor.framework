/*
 * vmcs_state_test.cpp - host-side tests for kext/Source/VmcsState.hpp: VMCS reads and writes landing in the
 * VMCB, execution controls becoming intercepts, event injection, exit recording.
 */
#include <stdio.h>
#include <string.h>

#include "../kext/Source/VmcsState.hpp"
#include "../shim/rhv_fields.h"

using namespace rhv::vmx;
using namespace rhv::vmcb;
namespace msr = rhv::msr;

namespace {

int fails = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
            fails++;                                                      \
        }                                                                 \
    } while (0)

struct Fixture {
    Vmcb      vmcb;
    VmcsState st;
    Fixture() : vmcb(), st(&vmcb) { memset(&vmcb, 0, sizeof(vmcb)); }
};

uint64_t rd(Fixture &f, uint32_t enc)
{
    uint64_t v = ~0ull;
    CHECK(f.st.read(enc, &v) == R_OK);
    return v;
}

void testStateFields()
{
    Fixture f;

    CHECK(f.st.write(0x681e, 0x1000) == R_OK);                       /* RIP */
    CHECK(f.vmcb.save.rip == 0x1000 && rd(f, 0x681e) == 0x1000);
    CHECK(f.st.write(0x681c, 0xfff0) == R_OK && f.vmcb.save.rsp == 0xfff0);
    CHECK(f.st.write(0x6800, 0x80050033) == R_OK && f.vmcb.save.cr0 == 0x80050033);
    CHECK(f.st.write(0x6802, 0x1234000) == R_OK && f.vmcb.save.cr3 == 0x1234000);
    CHECK(f.st.write(0x6804, 0x2620) == R_OK && f.vmcb.save.cr4 == 0x620);   /* VMXE (bit 13) never reaches the VMCB ... */
    { uint64_t c4 = 0; CHECK(f.st.read(0x6804, &c4) == R_OK && c4 == 0x2620); }   /* ... but reads back */
    CHECK(f.st.write(0x6804, 0x620) == R_OK && f.vmcb.save.cr4 == 0x620);
    { uint64_t c4 = 0; CHECK(f.st.read(0x6804, &c4) == R_OK && c4 == 0x620); }
    CHECK(f.st.write(0x6820, 0x2) == R_OK && f.vmcb.save.rflags == 0x2);
    CHECK(f.st.write(0x2804, 0x0007040600070406ull) == R_OK && f.vmcb.save.gPat == 0x0007040600070406ull);

    /* Segments: selector, limit, base, access rights. A flat 64-bit code segment. */
    CHECK(f.st.write(0x0802, 0x08) == R_OK && f.vmcb.save.cs.selector == 0x08);
    CHECK(f.st.write(0x4802, 0xFFFFFFFF) == R_OK && f.vmcb.save.cs.limit == 0xFFFFFFFF);
    CHECK(f.st.write(0x6808, 0) == R_OK && f.vmcb.save.cs.base == 0);
    CHECK(f.st.write(0x4816, 0xA09B) == R_OK && f.vmcb.save.cs.attrib == 0xA9B);
    CHECK(rd(f, 0x4816) == 0xA09B);
    /* ...and an unusable one, as 64-bit mode leaves DS/ES. */
    CHECK(f.st.write(0x481a, 0x10000) == R_OK && f.vmcb.save.ds.attrib == 0);
    CHECK((rd(f, 0x481a) & AR_UNUSABLE) != 0);
    /* TR and the descriptor tables. */
    CHECK(f.st.write(0x6814, 0xFFFFFF8000123000ull) == R_OK && f.vmcb.save.tr.base == 0xFFFFFF8000123000ull);
    CHECK(f.st.write(0x6816, 0x1000) == R_OK && f.vmcb.save.gdtr.base == 0x1000);
    CHECK(f.st.write(0x4810, 0x7F) == R_OK && f.vmcb.save.gdtr.limit == 0x7F);
    CHECK(f.st.write(0x6818, 0x2000) == R_OK && f.vmcb.save.idtr.base == 0x2000);
    CHECK(f.st.write(0x4812, 0xFFF) == R_OK && f.vmcb.save.idtr.limit == 0xFFF);

    /* Width masking: a 16-bit selector and a 32-bit limit drop the high bits. */
    CHECK(f.st.write(0x0804, 0xFFFF0010) == R_OK && f.vmcb.save.ss.selector == 0x10);
    CHECK(f.st.write(0x4804, 0x1FFFFFFFFull) == R_OK && f.vmcb.save.ss.limit == 0xFFFFFFFF);
    /* SYSENTER_CS is 32 bits in the VMCS but a 64-bit slot in the VMCB. */
    CHECK(f.st.write(0x482a, 0xFFFFFFFF08ull) == R_OK && f.vmcb.save.sysenterCs == 0xFFFFFF08 && rd(f, 0x482a) == 0xFFFFFF08);
    CHECK(f.st.write(0x6824, 0xffffff8000001000ull) == R_OK && f.vmcb.save.sysenterEsp == 0xffffff8000001000ull);

    /* EFER: SVME stays set on the AMD side and is invisible to the guest. */
    CHECK(f.st.write(0x2806, 0xD01) == R_OK);                         /* SCE | LME | LMA | NXE */
    CHECK((f.vmcb.save.efer & EFER_SVME) && (f.vmcb.save.efer & 0xD01) == 0xD01);
    CHECK(rd(f, 0x2806) == 0xD01);
    CHECK(f.st.write(0x2806, 0) == R_OK && (f.vmcb.save.efer & EFER_SVME) && rd(f, 0x2806) == 0);

    /* Direct controls. */
    CHECK(f.st.write(0x2010, 0xdeadbeef00ull) == R_OK && f.vmcb.control.tscOffset == 0xdeadbeef00ull);
    CHECK(f.st.write(0x4004, 0x20000) == R_OK && f.vmcb.control.interceptExc == 0x20000);   /* intercept #UD? bit 17 = #AC */
}

void testShadowAndErrors()
{
    Fixture f;
    uint64_t v;

    /* Plain control fields are stored and read back, nothing else changes. */
    CHECK(f.st.write(0x4002, 0x8401E172) == R_OK && rd(f, 0x4002) == 0x8401E172);
    CHECK(f.st.write(0x401e, 0x82) == R_OK && rd(f, 0x401e) == 0x82);
    CHECK(f.st.write(0x6004, 0x80050033) == R_OK && rd(f, 0x6004) == 0x80050033);           /* CR0 shadow */
    CHECK(f.st.write(0x2000, 0xAAAA0000ull) == R_OK && rd(f, 0x2000) == 0xAAAA0000ull);   /* IO bitmap A */
    CHECK(f.st.write(0x4000, 0x1FFFFFFFFull) == R_OK && rd(f, 0x4000) == 0xFFFFFFFF);     /* 32-bit control is masked */

    /* Unknown encodings. */
    CHECK(f.st.read(0x1, &v) == R_INVALID_FIELD && f.st.write(0x7000, 1) == R_INVALID_FIELD);
    CHECK(f.st.read(0xFFFFFFFF, &v) == R_INVALID_FIELD);

    /* Exit information is read-only for the client. */
    CHECK(f.st.write(0x4402, 1) == R_READ_ONLY && f.st.write(0x6400, 1) == R_READ_ONLY);
    CHECK(f.st.read(0x4402, &v) == R_OK && v == 0);
}

void testInterruptibility()
{
    Fixture f;
    CHECK(f.st.write(0x4824, 1) == R_OK);                      /* blocking by STI */
    CHECK((f.vmcb.control.intState & 1) && rd(f, 0x4824) == 1);
    CHECK(f.st.write(0x4824, 0) == R_OK);
    CHECK(!(f.vmcb.control.intState & 1) && rd(f, 0x4824) == 0);
    CHECK(f.st.write(0x4824, 8) == R_OK);                      /* NMI blocking is kept in the shadow */
    CHECK(rd(f, 0x4824) == 8);
    f.vmcb.control.intState = 1;                               /* hardware set the shadow (after STI) */
    CHECK(rd(f, 0x4824) == 8 + 1);
}

void testControls()
{
    Fixture f;

    /* Nothing asked: only what the host needs. */
    f.st.applyControls();
    const Control &c = f.vmcb.control;
    uint32_t base3 = V3_INTR | V3_NMI | V3_SMI | V3_INIT | V3_CPUID | V3_INVD | V3_SHUTDOWN | V3_MSR | V3_IOIO;
    CHECK((c.interceptVec3 & base3) == base3);                  /* port I/O always goes to the client */
    CHECK(!(c.interceptVec3 & (V3_HLT | V3_RDTSC | V3_PAUSE | V3_INVLPG | V3_VINTR)));
    CHECK((c.interceptVec4 & (V4_VMRUN | V4_VMMCALL | V4_VMLOAD | V4_VMSAVE | V4_STGI | V4_CLGI | V4_SKINIT)) ==
          (V4_VMRUN | V4_VMMCALL | V4_VMLOAD | V4_VMSAVE | V4_STGI | V4_CLGI | V4_SKINIT));
    CHECK(c.interceptVec4 & V4_XSETBV_BIT);
    CHECK(!(c.interceptVec4 & (V4_MWAIT | V4_MONITOR | V4_WBINVD)));
    CHECK(c.interceptCr == 0 && c.interceptDr == 0);
    CHECK(c.intCtl & INT_CTL_V_INTR_MASKING);
    CHECK((f.vmcb.save.rflags & 2) != 0);

    /* The typical QEMU/HVF set: HLT exit, unconditional I/O exit, MOV DR exit, PAUSE, MWAIT, MONITOR, secondary on. */
    f.st.write(0x4002, CPU_HLT_EXIT | CPU_IO_EXIT | CPU_MOV_DR_EXIT | CPU_PAUSE_EXIT | CPU_MWAIT_EXIT | CPU_MONITOR_EXIT | CPU_INVLPG_EXIT | (1u << 31));
    f.st.write(0x401e, CPU2_WBINVD_EXIT);
    f.st.applyControls();
    CHECK((c.interceptVec3 & (V3_HLT | V3_IOIO | V3_PAUSE | V3_INVLPG)) == (V3_HLT | V3_IOIO | V3_PAUSE | V3_INVLPG));
    CHECK((c.interceptVec4 & (V4_MWAIT | V4_MONITOR | V4_WBINVD)) == (V4_MWAIT | V4_MONITOR | V4_WBINVD));
    CHECK(c.interceptDr == 0xFFFFFFFFu);
    CHECK((c.interceptVec3 & base3) == base3);                 /* host intercepts survive */

    /* Turning controls back off removes the intercepts. */
    f.st.write(0x4002, 0);
    f.st.write(0x401e, 0);
    f.st.applyControls();
    CHECK(!(c.interceptVec3 & (V3_HLT | V3_PAUSE)) && (c.interceptVec3 & V3_IOIO) && !(c.interceptVec4 & (V4_MWAIT | V4_WBINVD)) && c.interceptDr == 0);

    /* The client can never switch the host's own intercepts off. */
    f.st.write(0x4000, 0);                                      /* "no external-interrupt exiting" */
    f.st.applyControls();
    CHECK((c.interceptVec3 & (V3_INTR | V3_NMI | V3_SHUTDOWN)) == (V3_INTR | V3_NMI | V3_SHUTDOWN));

    /* RDTSC / RDPMC / CR3 / CR8 / CR0 and CR4 masks. */
    f.st.write(0x4002, CPU_RDTSC_EXIT | CPU_RDPMC_EXIT | CPU_CR3_LOAD_EXIT | CPU_CR3_STORE_EXIT | CPU_CR8_LOAD_EXIT);
    f.st.write(0x6000, 0x60000000);                             /* CR0 mask: CD, NW */
    f.st.write(0x6002, 0);
    f.st.applyControls();
    CHECK((c.interceptVec3 & (V3_RDTSC | V3_RDPMC)) == (V3_RDTSC | V3_RDPMC));
    CHECK(c.interceptCr == ((1u << 3) | (1u << 19) | (1u << 24) | (1u << 16)));   /* CR3 store, CR3 load, CR8 load, CR0 write (mask set) */
    f.st.write(0x6000, 0);
    f.st.write(0x6002, 0x20);                                   /* CR4 mask: PAE */
    f.st.applyControls();
    CHECK((c.interceptCr & (1u << 20)) && !(c.interceptCr & (1u << 16)));

    /* Interrupt window exiting asks for a virtual interrupt and the VINTR exit. */
    f.st.write(0x4002, CPU_INTR_WINDOW);
    f.st.applyControls();
    CHECK((c.interceptVec3 & V3_VINTR) && (c.intCtl & INT_CTL_V_IRQ) && (c.intCtl & INT_CTL_V_INTR_MASKING));
    f.st.write(0x4002, 0);
    f.st.applyControls();
    CHECK(!(c.interceptVec3 & V3_VINTR) && !(c.intCtl & INT_CTL_V_IRQ) && (c.intCtl & INT_CTL_V_INTR_MASKING));

    /* IA-32e mode guest entry control drives EFER.LMA, SVME stays on. */
    f.st.write(0x2806, EFER_LME | 1);
    f.st.write(0x4012, ENTRY_IA32E_GUEST);
    f.st.applyControls();
    CHECK((f.vmcb.save.efer & EFER_LMA) && (f.vmcb.save.efer & EFER_LME) && (f.vmcb.save.efer & EFER_SVME));
    f.st.write(0x4012, 0);
    f.st.applyControls();
    CHECK(!(f.vmcb.save.efer & EFER_LMA) && (f.vmcb.save.efer & EFER_SVME));
}

void testEventInjection()
{
    bool soft;
    CHECK(eventInjFromVmx(0, &soft) == 0 && !soft);                                       /* nothing valid */
    CHECK(eventInjFromVmx((1u << 31) | 0x20, &soft) == ((1u << 31) | 0x20) && !soft);      /* external interrupt, vector 0x20 */
    CHECK(eventInjFromVmx((1u << 31) | (2u << 8) | 2, &soft) == ((1u << 31) | (2u << 8) | 2));   /* NMI */
    uint32_t gp = (1u << 31) | (1u << 11) | (3u << 8) | 13;                                /* #GP with error code */
    CHECK(eventInjFromVmx(gp, &soft) == gp && !soft);
    CHECK(eventInjFromVmx((1u << 31) | (4u << 8) | 0x80, &soft) == ((1u << 31) | (4u << 8) | 0x80) && soft);   /* INT 0x80 */
    CHECK(eventInjFromVmx((1u << 31) | (6u << 8) | 3, &soft) == ((1u << 31) | (4u << 8) | 3) && soft);         /* INT3 */
    CHECK(eventInjFromVmx((1u << 31) | (7u << 8) | 1, &soft) == 0);                        /* "other" type: not injectable */

    Fixture f;
    f.vmcb.save.rip = 0x1000;
    f.st.write(0x4016, (1u << 31) | (1u << 11) | (3u << 8) | 14);                          /* #PF */
    f.st.write(0x4018, 0x6);
    f.st.applyControls();
    CHECK(f.vmcb.control.eventInj == ((1u << 31) | (1u << 11) | (3u << 8) | 14) && f.vmcb.control.eventInjErr == 6);
    CHECK((rd(f, 0x4016) & (1u << 31)) == 0);                  /* consumed: the second run injects nothing */
    f.st.applyControls();
    CHECK(f.vmcb.control.eventInj == 0);
    /* Software interrupts need NRIPS: next_rip = rip + instruction length. */
    f.st.write(0x4016, (1u << 31) | (4u << 8) | 0x21);
    f.st.write(0x401a, 2);
    f.st.applyControls();
    CHECK(f.vmcb.control.nextRip == 0x1002);
    /* Clearing the valid bit stops injecting. */
    f.st.write(0x4016, 0);
    f.st.applyControls();
    CHECK(f.vmcb.control.eventInj == 0);
}

void testRecordExit()
{
    Fixture f;
    f.vmcb.save.rip = 0x1014;
    f.vmcb.control.nextRip = 0x1016;
    f.vmcb.control.exitCode = EXIT_CPUID;
    CHECK(f.st.recordExit(0));
    CHECK(rd(f, 0x4402) == VMX_CPUID && rd(f, 0x440c) == 2);

    /* I/O out to 0x3F8. */
    f.vmcb.control.exitCode = EXIT_IOIO;
    f.vmcb.control.exitInfo1 = ((uint64_t)0x3F8 << 16) | (1u << 4);
    f.vmcb.save.rip = 0x100b; f.vmcb.control.nextRip = 0x100c;
    CHECK(f.st.recordExit(0) && rd(f, 0x4402) == VMX_IO && (rd(f, 0x6400) >> 16) == 0x3F8 && rd(f, 0x440c) == 1);

    /* EPT violation: write to a page that is read/execute only. */
    f.vmcb.control.exitCode = EXIT_NPF;
    f.vmcb.control.exitInfo1 = 0x7;
    f.vmcb.control.exitInfo2 = 0x101010;
    f.vmcb.control.nextRip = 0;
    CHECK(f.st.recordExit(1 | 4) && rd(f, 0x4402) == VMX_EPT_VIOLATION && rd(f, 0x2400) == 0x101010);
    CHECK((rd(f, 0x6400) & 7) == 2 && (rd(f, 0x6400) & (1 << 3)) && !(rd(f, 0x6400) & (1 << 4)));

    /* An interrupted event delivery is reported as IDT-vectoring information. */
    f.vmcb.control.exitCode = EXIT_HLT;
    f.vmcb.control.exitIntInfo = (1u << 31) | (3u << 8) | 14;
    f.vmcb.control.exitIntInfoErr = 4;
    CHECK(f.st.recordExit(0) && rd(f, 0x4408) == ((1u << 31) | (3u << 8) | 14) && rd(f, 0x440a) == 4);

    /* Exits with no VMX meaning leave the fields alone and say so. */
    f.vmcb.control.exitCode = 0x80;                             /* VMRUN executed by the guest */
    CHECK(!f.st.recordExit(0));
    CHECK(rd(f, 0x4402) == VMX_HLT);
}

void testExtensionsAndMsrs()
{
    Fixture f;
    uint64_t v;

    /* CR2, DR6, TPR live in the VMCB; DR0-DR3 are the kext's. */
    CHECK(f.st.write(X_CR2, 0xdeadbeef000ull) == R_OK && f.vmcb.save.cr2 == 0xdeadbeef000ull && rd(f, X_CR2) == 0xdeadbeef000ull);
    CHECK(f.st.write(X_DR6, 0xFFFF0FF1ull) == R_OK && f.vmcb.save.dr6 == 0xFFFF0FF1ull);
    f.vmcb.control.intCtl = INT_CTL_V_INTR_MASKING | 0x77;
    CHECK(f.st.write(X_TPR, 0xB) == R_OK && (f.vmcb.control.intCtl & 0xFF) == 0xB && (f.vmcb.control.intCtl & INT_CTL_V_INTR_MASKING));
    CHECK(rd(f, X_TPR) == 0xB);
    for (unsigned i = 0; i < 4; i++) {
        CHECK(f.st.write(X_DR0 + 2 * i, 0x1000 * (i + 1)) == R_OK);
        CHECK(f.st.dr(i) == 0x1000 * (i + 1) && rd(f, X_DR0 + 2 * i) == 0x1000 * (i + 1));
    }
    CHECK(f.st.read(0xF006, &v) == R_INVALID_FIELD && f.st.read(0xF011, &v) == R_INVALID_FIELD && f.st.read(0xF018, &v) == R_INVALID_FIELD);

    /* MSRs: native ones are VMCB slots. */
    CHECK(f.st.writeMsr(msr::MSR_LSTAR, 0, 0xffffff8000001234ull) == R_OK && f.vmcb.save.lstar == 0xffffff8000001234ull);
    uint64_t out = 0;
    CHECK(f.st.readMsr(msr::MSR_LSTAR, 0, &out) == R_OK && out == 0xffffff8000001234ull);
    CHECK(f.st.writeMsr(msr::MSR_KERNEL_GS_BASE, 0, 0x55) == R_OK && f.vmcb.save.kernelGsBase == 0x55);
    CHECK(f.st.writeMsr(msr::MSR_FS_BASE, 0, 0x7000) == R_OK && f.vmcb.save.fs.base == 0x7000);
    CHECK(f.st.writeMsr(msr::MSR_SYSENTER_CS, 0, 0x1FFFFFF08ull) == R_OK && f.vmcb.save.sysenterCs == 0xFFFFFF08);   /* 32-bit MSR */
    CHECK(f.st.writeMsr(msr::MSR_EFER, 0, 0xD01) == R_OK && (f.vmcb.save.efer & EFER_SVME));
    CHECK(f.st.readMsr(msr::MSR_EFER, 0, &out) == R_OK && out == 0xD01);
    CHECK(f.st.writeMsr(0x1B, 0, 1) == R_INVALID_FIELD && f.st.readMsr(0xC0000103, 0, &out) == R_INVALID_FIELD);
    /* The guest TSC is the host TSC plus the offset; writing it sets the offset. */
    CHECK(f.st.writeMsr(msr::MSR_TSC, 1000, 5000) == R_OK && f.vmcb.control.tscOffset == 4000);
    CHECK(f.st.readMsr(msr::MSR_TSC, 1500, &out) == R_OK && out == 5500);
    CHECK(f.st.writeMsr(msr::MSR_TSC, 9000, 100) == R_OK && f.vmcb.control.tscOffset == (uint64_t)(100 - 9000));   /* wraps like hardware */

    /* A synthetic exit looks like an external interrupt and clears the rest. */
    f.vmcb.control.exitCode = EXIT_IOIO;
    f.vmcb.control.exitInfo1 = ((uint64_t)0x3F8 << 16) | (1u << 4);
    CHECK(f.st.recordExit(0) && rd(f, 0x4402) == VMX_IO);
    f.st.recordSynthetic(VMX_EXT_INTR);
    CHECK(rd(f, 0x4402) == VMX_EXT_INTR && rd(f, 0x6400) == 0 && rd(f, 0x440c) == 0 && rd(f, 0x4404) == 0);

    /* An exit with an interrupted delivery, then a synthetic one: the old vectoring info must not survive. */
    f.vmcb.control.exitCode = EXIT_NPF;
    f.vmcb.control.exitIntInfo = 0x80000030;
    CHECK(f.st.recordExit(0) && rd(f, 0x4408) == 0x80000030);
    f.st.recordSynthetic(VMX_EXT_INTR);
    CHECK(rd(f, 0x4408) == 0 && rd(f, 0x440a) == 0);
    /* An injection that never got in is handed back for the client to repeat. */
    f.st.recordSynthetic(VMX_EXT_INTR, 0x800000ec, 0);
    CHECK(rd(f, 0x4408) == 0x800000ec && rd(f, 0x4402) == VMX_EXT_INTR);
    f.st.recordSynthetic(VMX_EXT_INTR, 0x80000b0e, 0x14);                 /* exception with an error code */
    CHECK(rd(f, 0x4408) == 0x80000b0e && rd(f, 0x440a) == 0x14);
}

/* The shim decides up front which VMCS writes the kernel will refuse (it buffers them); it must agree with VmcsState
 * on every possible encoding. */
void testShimFieldTable()
{
    Fixture f;
    for (uint32_t enc = 0; enc < 0x10000; enc++) {
        uint64_t v = 0;
        Result rr = f.st.read(enc, &v);
        Result wr = f.st.write(enc, 0);
        int shimRead  = rhv_vmcs_read_status(enc);
        int shimWrite = rhv_vmcs_write_status(enc, 0);
        CHECK((rr == R_OK) == (shimRead == RHV_FIELD_OK));
        int want = wr == R_OK ? RHV_FIELD_OK : wr == R_READ_ONLY ? RHV_FIELD_READ_ONLY : RHV_FIELD_INVALID;
        if (want != shimWrite) {
            printf("field %#x: kernel %d, shim %d\n", enc, want, shimWrite);
            fails++;
        }
    }
    CHECK(rhv_vmcs_write_status(0xF010, 0x0000800000000000ull) == RHV_FIELD_BAD_VALUE);     /* non-canonical DR0 */
    CHECK(rhv_vmcs_write_status(0xF010, 0xFFFF800000000000ull) == RHV_FIELD_OK);
    CHECK(rhv_vmcs_write_status(0xF002, 0x0000800000000000ull) == RHV_FIELD_OK);            /* DR6 is not loaded */
}

} // namespace

int main()
{
    testShimFieldTable();
    testStateFields();
    testShadowAndErrors();
    testInterruptibility();
    testControls();
    testEventInjection();
    testRecordExit();
    testExtensionsAndMsrs();
    if (fails) {
        printf("vmcs_state_test: %d FAILED\n", fails);
        return 1;
    }
    printf("vmcs_state_test: OK\n");
    return 0;
}
