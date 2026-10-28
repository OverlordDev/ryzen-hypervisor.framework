/*
 * VmcsState.hpp - the "VMCS" a Hypervisor.framework client sees, implemented on top of a VMCB.
 *
 * A client calls hv_vmx_vcpu_write_vmcs(field, value) and hv_vmx_vcpu_read_vmcs(field, &value) and
 * sets execution controls (CPU_BASED, PIN_BASED, exception bitmap, ...). On AMD there is no VMCS: the guest
 * state lives in the VMCB and "controls" are intercept bits. This class keeps a shadow value for every field,
 * forwards state fields to/from the VMCB, and turns the control fields into intercepts right before VMRUN.
 *
 * Pure C++ over a caller-owned Vmcb buffer, tested on the host (tests/vmcs_state_test.cpp).
 *
 * Known gaps (documented in docs/PHASE3.md): CR0/CR4 read shadows, NMI-window exiting, MSR/IO bitmaps given
 * by the guest (we intercept all MSRs and, when I/O exiting is on, all ports), VPID, posted interrupts.
 */
#ifndef RYZENHV_VMCSSTATE_HPP
#define RYZENHV_VMCSSTATE_HPP

#include <string.h>

#include "MsrMap.hpp"
#include "VmxMap.hpp"

namespace rhv {
namespace vmx {

/* The VMCS fields this class treats specially. */
enum : uint32_t {
    F_PIN_BASED          = 0x4000,
    F_CPU_BASED          = 0x4002,
    F_EXC_BITMAP         = 0x4004,
    F_VMEXIT_CONTROLS    = 0x400c,
    F_VMENTRY_CONTROLS   = 0x4012,
    F_VMENTRY_IRQ_INFO   = 0x4016,
    F_VMENTRY_EXC_ERROR  = 0x4018,
    F_VMENTRY_INSTR_LEN  = 0x401a,
    F_TPR_THRESHOLD      = 0x401c,
    F_CPU_BASED2         = 0x401e,
    F_CR0_MASK           = 0x6000,
    F_CR4_MASK           = 0x6002,
    F_GUEST_CR4          = 0x6804,
    F_RO_INSTR_ERROR     = 0x4400,
    F_RO_EXIT_REASON     = 0x4402,
    F_RO_IRQ_INFO        = 0x4404,
    F_RO_IRQ_ERROR       = 0x4406,
    F_RO_IDT_INFO        = 0x4408,
    F_RO_IDT_ERROR       = 0x440a,
    F_RO_INSTR_LEN       = 0x440c,
    F_RO_QUALIFICATION   = 0x6400,
    F_RO_GUEST_PHYS      = 0x2400,
    F_GUEST_INTERRUPTIBILITY = 0x4824,
    F_GUEST_ACTIVITY     = 0x4826,
    F_GUEST_EFER         = 0x2806,
    F_GUEST_RIP          = 0x681e
};

/* CPU_BASED (primary processor-based) execution controls, Intel SDM Vol. 3, table 24-6. */
enum : uint32_t {
    CPU_INTR_WINDOW   = 1u << 2,
    CPU_TSC_OFFSETING = 1u << 3,
    CPU_HLT_EXIT      = 1u << 7,
    CPU_INVLPG_EXIT   = 1u << 9,
    CPU_MWAIT_EXIT    = 1u << 10,
    CPU_RDPMC_EXIT    = 1u << 11,
    CPU_RDTSC_EXIT    = 1u << 12,
    CPU_CR3_LOAD_EXIT = 1u << 15,
    CPU_CR3_STORE_EXIT = 1u << 16,
    CPU_CR8_LOAD_EXIT = 1u << 19,
    CPU_CR8_STORE_EXIT = 1u << 20,
    CPU_TPR_SHADOW    = 1u << 21,
    CPU_MOV_DR_EXIT   = 1u << 23,
    CPU_IO_EXIT       = 1u << 24,
    CPU_IO_BITMAPS    = 1u << 25,
    CPU_MONITOR_EXIT  = 1u << 29,
    CPU_PAUSE_EXIT    = 1u << 30
};
/* CPU_BASED2 */
enum : uint32_t { CPU2_WBINVD_EXIT = 1u << 6 };
/* VM-entry controls */
enum : uint32_t { ENTRY_IA32E_GUEST = 1u << 9 };

/* VMCB intercept bits that are not named in VMCB.hpp. Vector 3 = VMCB 0x00C, vector 4 = 0x010. */
enum : uint32_t {
    V3_VINTR   = 1u << 4,
    V3_RDTSC   = 1u << 14,
    V3_RDPMC   = 1u << 15,
    V3_PAUSE   = 1u << 23,
    V3_INVLPG  = 1u << 25
};
enum : uint32_t { V4_RDTSCP_BIT = 1u << 7, V4_XSETBV_BIT = 1u << 13 };

/* CR4.VMXE: Intel clients (QEMU's HVF backend) always set it in the guest CR4 and hide it with the CR4 mask/shadow. It
 * is reserved on AMD and makes VMRUN fail with VMEXIT_INVALID, so it never reaches the VMCB; the field remembers it. */
static const uint64_t CR4_VMXE = 1ull << 13;
static const uint64_t EFER_LME = 1ull << 8;
static const uint64_t EFER_LMA = 1ull << 10;
static const uint32_t INT_CTL_V_IRQ = 1u << 8;
static const uint32_t INT_CTL_V_IGN_TPR = 1u << 20;

enum Result { R_OK = 0, R_INVALID_FIELD, R_READ_ONLY };

/*
 * VM-entry interruption information -> VMCB EVENTINJ (low 32 bits). Returns 0 when nothing is to be injected.
 * VMX types: 0 external, 2 NMI, 3 hardware exception, 4 software interrupt, 5 privileged software exception,
 * 6 software exception. SVM types: 0 interrupt, 2 NMI, 3 exception, 4 software interrupt.
 */
static inline uint32_t eventInjFromVmx(uint32_t info, bool *isSoftware)
{
    *isSoftware = false;
    if (!(info & (1u << 31)))
        return 0;
    uint32_t vector = info & 0xFF;
    uint32_t type   = (info >> 8) & 7;
    uint32_t svm;
    switch (type) {
    case 0: svm = 0; break;
    case 2: svm = 2; break;
    case 3: svm = 3; break;
    case 4: case 5: case 6: svm = 4; *isSoftware = true; break;
    default: return 0;
    }
    return (1u << 31) | ((info & (1u << 11)) ? (1u << 11) : 0) | (svm << 8) | vector;
}

class VmcsState {
public:
    VmcsState() : fVmcb(nullptr) { memset(fShadow, 0, sizeof(fShadow)); memset(fDr, 0, sizeof(fDr)); }
    explicit VmcsState(rhv::vmcb::Vmcb *vmcb) : fVmcb(vmcb) { memset(fShadow, 0, sizeof(fShadow)); memset(fDr, 0, sizeof(fDr)); }

    void attach(rhv::vmcb::Vmcb *vmcb) { fVmcb = vmcb; }
    void reset() { memset(fShadow, 0, sizeof(fShadow)); memset(fDr, 0, sizeof(fDr)); fTscAux = 0; }

    /* IA32_TSC_AUX of the guest (RDTSCP): the kext puts it in the real MSR while the guest runs. 32 bits. */
    uint64_t tscAux() const { return fTscAux; }
    void     setTscAux(uint64_t v) { fTscAux = v & 0xFFFFFFFFull; }

    /* DR0-DR3 of the guest. The kext loads them into the CPU around VMRUN, VMRUN itself does not. */
    uint64_t dr(unsigned i) const { return fDr[i & 3]; }
    void     setDr(unsigned i, uint64_t v) { fDr[i & 3] = v; }

    Result read(uint32_t enc, uint64_t *out) const
    {
        int idx = fieldIndex(enc);
        if (idx < 0 && !isExtField(enc))
            return R_INVALID_FIELD;
        StateMap m = mapField(enc);
        switch (m.kind) {
        case KIND_EXT_DR:
            *out = fDr[m.vmcbOff];
            return R_OK;
        case KIND_VMCB: {
            uint64_t v = loadBytes(m.vmcbOff, m.vmcbSize);
            if (enc == F_GUEST_CR4)
                v |= fShadow[idx] & CR4_VMXE;
            *out = v & mask(fieldBytes(enc));
            return R_OK;
        }
        case KIND_SEG_AR:
            *out = arFromAttrib((uint16_t)loadBytes(m.vmcbOff, 2));
            return R_OK;
        case KIND_EFER:
            *out = loadBytes(m.vmcbOff, 8) & ~rhv::vmcb::EFER_SVME;     /* SVME is ours, the guest never sees it */
            return R_OK;
        case KIND_READONLY:
        case KIND_SHADOW:
        default:
            if (enc == F_GUEST_INTERRUPTIBILITY) {
                /* Bit 0/1 (blocking by STI / MOV SS) is the VMCB interrupt shadow; the rest lives in the shadow. */
                uint64_t v = fShadow[idx] & ~3ull;
                if (fVmcb->control.intState & 1)
                    v |= (fShadow[idx] & 2) ? 2 : 1;
                *out = v;
                return R_OK;
            }
            *out = fShadow[idx];
            return R_OK;
        }
    }

    Result write(uint32_t enc, uint64_t value)
    {
        int idx = fieldIndex(enc);
        if (idx < 0 && !isExtField(enc))
            return R_INVALID_FIELD;
        StateMap m = mapField(enc);
        value &= mask(fieldBytes(enc));
        switch (m.kind) {
        case KIND_EXT_DR:
            fDr[m.vmcbOff] = value;
            return R_OK;
        case KIND_VMCB:
            if (enc == F_GUEST_CR4) {
                fShadow[idx] = value & CR4_VMXE;
                value &= ~CR4_VMXE;
            }
            storeBytes(m.vmcbOff, m.vmcbSize, value);
            return R_OK;
        case KIND_SEG_AR:
            storeBytes(m.vmcbOff, 2, attribFromAr((uint32_t)value));
            return R_OK;
        case KIND_EFER:
            storeBytes(m.vmcbOff, 8, value | rhv::vmcb::EFER_SVME);
            return R_OK;
        case KIND_READONLY:
            return R_READ_ONLY;
        case KIND_SHADOW:
        default:
            fShadow[idx] = value;
            if (enc == F_GUEST_INTERRUPTIBILITY) {
                if (value & 3)
                    fVmcb->control.intState |= 1;
                else
                    fVmcb->control.intState &= ~1u;
            }
            return R_OK;
        }
    }

    /* The client uses the TPR shadow and has left CR8 accesses to the hardware (CR8-load exiting off). */
    bool tprShadowActive() const
    {
        const uint32_t cpu = (uint32_t)get(F_CPU_BASED);
        return (cpu & CPU_TPR_SHADOW) && !(cpu & CPU_CR8_LOAD_EXIT);
    }
    /* TPR threshold, bits 3:0 (the class of the interrupt that waits for a lower TPR); 0 means "never exit". */
    uint32_t tprThreshold() const { return (uint32_t)get(F_TPR_THRESHOLD) & 0xF; }

    /* Convenience for the kext's own use. */
    uint64_t get(uint32_t enc) const { uint64_t v = 0; read(enc, &v); return v; }

    /*
     * Turns the control fields into VMCB intercepts and event injection. Call right before every VMRUN.
     * Intercepts the host needs to stay in control are always on, whatever the client asked for.
     */
    void applyControls()
    {
        using namespace rhv::vmcb;
        Control &c = fVmcb->control;
        const uint32_t cpu  = (uint32_t)get(F_CPU_BASED);
        const uint32_t cpu2 = (uint32_t)get(F_CPU_BASED2);

        /* Always: host interrupts, NMI/SMI/INIT, triple fault, CPUID and INVD (VMX always exits on both). */
        uint32_t v3 = V3_INTR | V3_NMI | V3_SMI | V3_INIT | V3_CPUID | V3_INVD | V3_SHUTDOWN;
        uint32_t v4 = V4_VMRUN | V4_VMMCALL | V4_VMLOAD | V4_VMSAVE | V4_STGI | V4_CLGI | V4_SKINIT | V4_XSETBV_BIT;

        if (cpu & CPU_HLT_EXIT)     v3 |= V3_HLT;
        if (cpu & CPU_INVLPG_EXIT)  v3 |= V3_INVLPG;
        if (cpu & CPU_RDPMC_EXIT)   v3 |= V3_RDPMC;
        if (cpu & CPU_RDTSC_EXIT) {
            /* Intel: with RDTSC exiting, RDTSCP exits too. Leaving it out lets RDTSCP read the raw host TSC while the
             * client hands out its own virtual TSC for RDTSC: two clocks hours apart, and Windows' QPC goes negative. */
            v3 |= V3_RDTSC;
            v4 |= V4_RDTSCP_BIT;
        }
        if (cpu & CPU_PAUSE_EXIT)   v3 |= V3_PAUSE;
        v3 |= V3_IOIO;      /* Hypervisor.framework always hands port I/O to the client, whatever CPU_BASED says */
        v3 |= V3_MSR;                                              /* all MSRs exit; see Vm::fMsrpm for native ones */
        if (cpu & CPU_MWAIT_EXIT)   v4 |= V4_MWAIT;
        if (cpu & CPU_MONITOR_EXIT) v4 |= V4_MONITOR;
        if (cpu2 & CPU2_WBINVD_EXIT) v4 |= V4_WBINVD;

        /* Control registers: CR3/CR8 per control, CR0/CR4 writes when the client masks any bit of them. */
        uint32_t cr = 0;
        if (cpu & CPU_CR3_STORE_EXIT) cr |= 1u << 3;
        if (cpu & CPU_CR3_LOAD_EXIT)  cr |= 1u << (16 + 3);
        if (cpu & CPU_CR8_STORE_EXIT) cr |= 1u << 8;
        if (cpu & CPU_CR8_LOAD_EXIT)  cr |= 1u << (16 + 8);
        /*
         * TPR shadow: the guest's CR8 is V_TPR (VMRUN virtualizes it when V_INTR_MASKING is on) and a MOV to CR8 does not
         * exit, except while the client has a TPR threshold set: then the write is intercepted so Vm::run can compare
         * the new TPR with the threshold (Intel exits with "TPR below threshold" at that point).
         */
        if (tprShadowActive() && tprThreshold())
            cr |= 1u << (16 + 8);
        if (get(F_CR0_MASK)) cr |= 1u << (16 + 0);
        if (get(F_CR4_MASK)) cr |= 1u << (16 + 4);

        /* Interrupt window: ask the CPU for a virtual interrupt and exit when the guest can take it. */
        if (cpu & CPU_INTR_WINDOW) {
            v3 |= V3_VINTR;
            c.intCtl |= INT_CTL_V_IRQ | INT_CTL_V_IGN_TPR;
        } else {
            c.intCtl &= ~(INT_CTL_V_IRQ | INT_CTL_V_IGN_TPR);
        }
        c.intCtl |= INT_CTL_V_INTR_MASKING;

        c.interceptVec3 = v3;
        c.interceptVec4 = v4;
        c.interceptCr   = cr;
        c.interceptDr   = (cpu & CPU_MOV_DR_EXIT) ? 0xFFFFFFFFu : 0;
        /* interceptExc is the exception bitmap itself (F_EXC_BITMAP maps straight onto it). */

        /* The EFER.LMA bit follows the "IA-32e mode guest" entry control, like VM entry does. */
        uint64_t efer = fVmcb->save.efer | EFER_SVME;
        if (get(F_VMENTRY_CONTROLS) & ENTRY_IA32E_GUEST) efer |= EFER_LMA; else efer &= ~EFER_LMA;
        fVmcb->save.efer = efer;

        fVmcb->save.rflags |= 2;                        /* bit 1 is always set */

        /* Event injection. */
        bool soft = false;
        uint32_t ev = eventInjFromVmx((uint32_t)get(F_VMENTRY_IRQ_INFO), &soft);
        c.eventInj    = ev;
        c.eventInjErr = (uint32_t)get(F_VMENTRY_EXC_ERROR);
        if (ev && soft)
            c.nextRip = fVmcb->save.rip + get(F_VMENTRY_INSTR_LEN);
        /* Like VM entry: the valid bit is consumed, the event is injected once. */
        set(F_VMENTRY_IRQ_INFO, get(F_VMENTRY_IRQ_INFO) & ~(1ull << 31));
    }

    /*
     * Fills the read-only exit fields after a #VMEXIT. `perm` is the NPT rights of the faulting page for NPF.
     * Returns false when the exit has no VMX equivalent (the caller injects #UD for SVM instructions).
     */
    bool recordExit(uint32_t nptPerm)
    {
        SvmExit in;
        in.code = fVmcb->control.exitCode;
        in.info1 = fVmcb->control.exitInfo1;
        in.info2 = fVmcb->control.exitInfo2;
        in.rip = fVmcb->save.rip;
        in.nextRip = fVmcb->control.nextRip;
        in.nptPerm = nptPerm;

        VmxExit out;
        bool ok = translateExit(in, &out);
        if (!ok)
            return false;

        set(F_RO_EXIT_REASON, out.reason);
        set(F_RO_QUALIFICATION, out.qual);
        set(F_RO_INSTR_LEN, out.instrLen);
        set(F_RO_IRQ_INFO, out.intrInfo);
        set(F_RO_IRQ_ERROR, out.intrError);
        set(F_RO_GUEST_PHYS, out.gpa);
        /* Interrupted event delivery: SVM's EXITINTINFO has the same layout as VMX's IDT-vectoring information. */
        set(F_RO_IDT_INFO, fVmcb->control.exitIntInfo);
        set(F_RO_IDT_ERROR, fVmcb->control.exitIntInfoErr);
        return true;
    }

    /*
     * Guest MSR access for hv_vcpu_read_msr / hv_vcpu_write_msr: only MSRs the VMCB holds (MsrMap.hpp). `hostTsc` is
     * the host's current TSC, needed for the TSC MSR. R_INVALID_FIELD for MSRs we do not keep.
     */
    Result readMsr(uint32_t msr, uint64_t hostTsc, uint64_t *out) const
    {
        msr::Info mi = msr::info(msr);
        switch (mi.kind) {
        case msr::K_VMCB:
            *out = loadBytes(mi.vmcbOff, 8) & (mi.valueBits >= 64 ? ~0ull : ((1ull << mi.valueBits) - 1));
            return R_OK;
        case msr::K_EFER:
            return read(F_GUEST_EFER, out);
        case msr::K_TSC:
            *out = hostTsc + fVmcb->control.tscOffset;
            return R_OK;
        case msr::K_TSC_AUX:
            *out = fTscAux;
            return R_OK;
        default:
            return R_INVALID_FIELD;
        }
    }

    Result writeMsr(uint32_t msr, uint64_t hostTsc, uint64_t value)
    {
        msr::Info mi = msr::info(msr);
        switch (mi.kind) {
        case msr::K_VMCB:
            storeBytes(mi.vmcbOff, 8, value & (mi.valueBits >= 64 ? ~0ull : ((1ull << mi.valueBits) - 1)));
            return R_OK;
        case msr::K_EFER:
            return write(F_GUEST_EFER, value);
        case msr::K_TSC:
            fVmcb->control.tscOffset = value - hostTsc;
            return R_OK;
        case msr::K_TSC_AUX:
            setTscAux(value);
            return R_OK;
        default:
            return R_INVALID_FIELD;
        }
    }

    /*
     * VmRun returned without a guest-caused exit (time slice over, or hv_vcpu_interrupt): to a Hypervisor.framework
     * client that looks like an external-interrupt exit, which is what the real framework reports.
     */
    /*
     * `idtInfo`/`idtError`: an event the client asked to inject (or one whose delivery was interrupted) that was not
     * delivered before this exit. It is handed back in the IDT-vectoring information so the client injects it again;
     * without it the interrupt is lost (QEMU's APIC then keeps it "in service" and blocks every lower vector). 0 when
     * there is none: the field must not keep the previous exit's value either.
     */
    void recordSynthetic(uint32_t reason, uint32_t idtInfo = 0, uint32_t idtError = 0)
    {
        set(F_RO_EXIT_REASON, reason);
        set(F_RO_QUALIFICATION, 0);
        set(F_RO_INSTR_LEN, 0);
        set(F_RO_IRQ_INFO, 0);
        set(F_RO_IRQ_ERROR, 0);
        set(F_RO_IDT_INFO, idtInfo);
        set(F_RO_IDT_ERROR, idtError);
    }

private:
    static uint64_t mask(unsigned bytes) { return bytes >= 8 ? ~0ull : ((1ull << (bytes * 8)) - 1); }

    uint64_t loadBytes(unsigned off, unsigned size) const
    {
        uint64_t v = 0;
        memcpy(&v, reinterpret_cast<const uint8_t *>(fVmcb) + off, size);
        return v;
    }

    void storeBytes(unsigned off, unsigned size, uint64_t v)
    {
        /* A 32-bit VMCS field into a 64-bit VMCB slot (SYSENTER_CS) is stored zero-extended. */
        memcpy(reinterpret_cast<uint8_t *>(fVmcb) + off, &v, size);
    }

    void set(uint32_t enc, uint64_t v)
    {
        int idx = fieldIndex(enc);
        if (idx >= 0)
            fShadow[idx] = v & mask(fieldBytes(enc));
    }

    rhv::vmcb::Vmcb *fVmcb;
    uint64_t         fShadow[kNumFields];
    uint64_t         fDr[4];
    uint64_t         fTscAux = 0;
};

} // namespace vmx
} // namespace rhv

#endif /* RYZENHV_VMCSSTATE_HPP */
