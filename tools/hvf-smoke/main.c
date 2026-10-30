/*
 * hvf-smoke - a small VMM written against the PUBLIC x86 Hypervisor.framework API, nothing else.
 *
 * It does what QEMU's HVF accelerator does, in the same order: create a VM, map memory, create a vCPU, read the VMX
 * capabilities and build the execution controls from them, set up a real-mode machine through VMCS fields and
 * registers, then run it and handle exits by reading the VMCS exit reason. On a real Intel Mac the same binary runs
 * against Apple's framework; on this Ryzen it runs through the RyzenHV shim:
 *
 *     sudo env DYLD_INSERT_LIBRARIES=build/libryzenhv.dylib ./build/hvf-smoke
 *     (or: scripts/run-with-shim.sh ./build/hvf-smoke)
 *
 * It checks: the VMX capability path, controls, registers, MSRs, FPU state, serial output through I/O exits, CPUID,
 * HLT, an EPT violation handled by mapping memory, hv_vcpu_interrupt from another thread, and error codes.
 */
#include <Hypervisor/Hypervisor.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096u
#define RAM_SIZE (1u << 20)

static int failures;
#define EXPECT(cond, ...)                                  \
    do {                                                   \
        if (cond) {                                        \
            printf("  ok: ");                              \
            printf(__VA_ARGS__);                           \
            printf("\n");                                  \
        } else {                                           \
            printf("  FAIL (line %d): ", __LINE__);        \
            printf(__VA_ARGS__);                           \
            printf("\n");                                  \
            failures++;                                    \
        }                                                  \
    } while (0)

/* QEMU's cap2ctrl: controls that must be 1, plus the wanted ones the CPU allows. */
static uint64_t cap2ctrl(uint64_t cap, uint64_t ctrl)
{
    return (ctrl | (cap & 0xffffffff)) & (cap >> 32);
}

static uint64_t rvmcs(hv_vcpuid_t v, uint32_t f)
{
    uint64_t x = 0;
    hv_return_t r = hv_vmx_vcpu_read_vmcs(v, f, &x);
    if (r != HV_SUCCESS)
        fprintf(stderr, "hv_vmx_vcpu_read_vmcs(%#x) = %#x\n", f, (unsigned)r);
    return x;
}

static void wvmcs(hv_vcpuid_t v, uint32_t f, uint64_t x)
{
    hv_return_t r = hv_vmx_vcpu_write_vmcs(v, f, x);
    if (r != HV_SUCCESS)
        fprintf(stderr, "hv_vmx_vcpu_write_vmcs(%#x) = %#x\n", f, (unsigned)r);
}

static uint64_t rreg(hv_vcpuid_t v, hv_x86_reg_t r)
{
    uint64_t x = 0;
    hv_return_t k = hv_vcpu_read_register(v, r, &x);
    if (k != HV_SUCCESS)
        fprintf(stderr, "hv_vcpu_read_register(%d) = %#x\n", (int)r, (unsigned)k);
    return x;
}

static void wreg(hv_vcpuid_t v, hv_x86_reg_t r, uint64_t x)
{
    hv_return_t k = hv_vcpu_write_register(v, r, x);
    if (k != HV_SUCCESS)
        fprintf(stderr, "hv_vcpu_write_register(%d) = %#x\n", (int)r, (unsigned)k);
}

/* Guest programs ------------------------------------------------------------------------------------------- */

/* 0x1000: print the string at 0x1100 through port 0x3F8, CPUID leaf 0x40000000, HLT. */
static const uint8_t kProg1[] = {
    0xBA, 0xF8, 0x03, 0xBE, 0x00, 0x11, 0xAC, 0x84, 0xC0, 0x74, 0x03, 0xEE, 0xEB, 0xF8,
    0x66, 0xB8, 0x00, 0x00, 0x00, 0x40, 0x0F, 0xA2, 0xF4, 0xEB, 0xFD,
};
static const char kMessage[] = "Hello from a guest started with the public Hypervisor API!\n";

/* 0x1200: store a byte at DS:0x1020 (= 0x101010), which is not mapped yet, then HLT. */
static const uint8_t kProg2[] = { 0xB8, 0xFF, 0xFF, 0x8E, 0xD8, 0xC6, 0x06, 0x20, 0x10, 0x42, 0xF4 };

/* 0x1300: a tight loop, to be interrupted from another thread. */
static const uint8_t kProg3[] = { 0xEB, 0xFE };

/* A real-mode machine, the way QEMU's hvf_reset_vcpu describes one. */
static void setup_real_mode(hv_vcpuid_t v, uint16_t ip)
{
    static const struct { uint32_t sel, lim, ar, base; } seg[] = {
        { VMCS_GUEST_ES, VMCS_GUEST_ES_LIMIT, VMCS_GUEST_ES_AR, VMCS_GUEST_ES_BASE },
        { VMCS_GUEST_CS, VMCS_GUEST_CS_LIMIT, VMCS_GUEST_CS_AR, VMCS_GUEST_CS_BASE },
        { VMCS_GUEST_SS, VMCS_GUEST_SS_LIMIT, VMCS_GUEST_SS_AR, VMCS_GUEST_SS_BASE },
        { VMCS_GUEST_DS, VMCS_GUEST_DS_LIMIT, VMCS_GUEST_DS_AR, VMCS_GUEST_DS_BASE },
        { VMCS_GUEST_FS, VMCS_GUEST_FS_LIMIT, VMCS_GUEST_FS_AR, VMCS_GUEST_FS_BASE },
        { VMCS_GUEST_GS, VMCS_GUEST_GS_LIMIT, VMCS_GUEST_GS_AR, VMCS_GUEST_GS_BASE },
        { VMCS_GUEST_LDTR, VMCS_GUEST_LDTR_LIMIT, VMCS_GUEST_LDTR_AR, VMCS_GUEST_LDTR_BASE },
        { VMCS_GUEST_TR, VMCS_GUEST_TR_LIMIT, VMCS_GUEST_TR_AR, VMCS_GUEST_TR_BASE },
    };
    for (int i = 0; i < 8; i++) {
        wvmcs(v, seg[i].sel, 0);
        wvmcs(v, seg[i].lim, 0xFFFF);
        wvmcs(v, seg[i].base, 0);
        wvmcs(v, seg[i].ar, i == 1 ? 0x9B : i == 6 ? 0x82 : i == 7 ? 0x8B : 0x93);
    }
    wvmcs(v, VMCS_GUEST_GDTR_BASE, 0);
    wvmcs(v, VMCS_GUEST_GDTR_LIMIT, 0xFFFF);
    wvmcs(v, VMCS_GUEST_IDTR_BASE, 0);
    wvmcs(v, VMCS_GUEST_IDTR_LIMIT, 0xFFFF);
    wvmcs(v, VMCS_GUEST_CR0, 0x20 | 0x10);       /* NE | ET, protected mode off */
    wvmcs(v, VMCS_CTRL_CR0_MASK, 0);
    wvmcs(v, VMCS_CTRL_CR0_SHADOW, 0);
    wvmcs(v, VMCS_GUEST_CR3, 0);
    wvmcs(v, VMCS_GUEST_CR4, 0);
    wvmcs(v, VMCS_GUEST_IA32_EFER, 0);
    wreg(v, HV_X86_RFLAGS, 2);
    wreg(v, HV_X86_RSP, 0);
    wreg(v, HV_X86_RIP, ip);
}

static hv_vcpuid_t g_vcpu;

static void *interrupter(void *arg)
{
    (void)arg;
    usleep(30 * 1000);
    hv_vcpuid_t v = g_vcpu;
    hv_return_t r = hv_vcpu_interrupt(&v, 1);
    *(hv_return_t *)arg = r;
    return NULL;
}

int main(void)
{
    hv_return_t r = hv_vm_create(HV_VM_DEFAULT);
    if (r != HV_SUCCESS) {
        fprintf(stderr, "hv_vm_create failed: %#x. On a Ryzen run this through the shim (scripts/run-with-shim.sh).\n", (unsigned)r);
        return 1;
    }
    printf("hv_vm_create ok\n");

    uint8_t *ram = mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    memset(ram, 0, RAM_SIZE);
    memcpy(ram + 0x1000, kProg1, sizeof(kProg1));
    memcpy(ram + 0x1100, kMessage, sizeof(kMessage));
    memcpy(ram + 0x1200, kProg2, sizeof(kProg2));
    memcpy(ram + 0x1300, kProg3, sizeof(kProg3));

    printf("stage 1: setup\n");
    EXPECT(hv_vm_map(ram, 0, RAM_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC) == HV_SUCCESS, "hv_vm_map of 1 MiB of RAM");
    EXPECT(hv_vm_map(ram, 0, RAM_SIZE, HV_MEMORY_READ) != HV_SUCCESS, "mapping over an existing mapping is refused");
    hv_vcpuid_t v = 0;
    r = hv_vcpu_create(&v, HV_VCPU_DEFAULT);
    EXPECT(r == HV_SUCCESS, "hv_vcpu_create (%#x)", (unsigned)r);
    if (r != HV_SUCCESS)
        return 1;
    g_vcpu = v;

    uint64_t cap_pin = 0, cap_proc = 0, cap_proc2 = 0, cap_entry = 0, cap_exit = 0;
    EXPECT(hv_vmx_read_capability(HV_VMX_CAP_PINBASED, &cap_pin) == HV_SUCCESS &&
           hv_vmx_read_capability(HV_VMX_CAP_PROCBASED, &cap_proc) == HV_SUCCESS &&
           hv_vmx_read_capability(HV_VMX_CAP_PROCBASED2, &cap_proc2) == HV_SUCCESS &&
           hv_vmx_read_capability(HV_VMX_CAP_ENTRY, &cap_entry) == HV_SUCCESS &&
           hv_vmx_read_capability(HV_VMX_CAP_EXIT, &cap_exit) == HV_SUCCESS, "VMX capabilities can be read");

    /* What QEMU asks for: HLT/MWAIT exiting, TSC offsetting, TPR shadow, secondary controls. */
    wvmcs(v, VMCS_CTRL_PIN_BASED, cap2ctrl(cap_pin, 0));
    wvmcs(v, VMCS_CTRL_CPU_BASED, cap2ctrl(cap_proc, CPU_BASED_HLT | CPU_BASED_MWAIT | CPU_BASED_TSC_OFFSET | CPU_BASED_TPR_SHADOW) | CPU_BASED_SECONDARY_CTLS);
    wvmcs(v, VMCS_CTRL_CPU_BASED2, cap2ctrl(cap_proc2, 0));
    wvmcs(v, VMCS_CTRL_VMENTRY_CONTROLS, cap2ctrl(cap_entry, 0));
    wvmcs(v, VMCS_CTRL_EXC_BITMAP, 0);
    EXPECT((rvmcs(v, VMCS_CTRL_CPU_BASED) & CPU_BASED_HLT) != 0, "HLT exiting is on in the controls the capabilities allowed");
    EXPECT((rvmcs(v, VMCS_CTRL_CPU_BASED) & CPU_BASED_TPR_SHADOW) == 0, "TPR shadow was not offered, so it is not set");

    /* MSRs the guest kernel uses a lot, accessed natively. */
    static const uint32_t native[] = { 0xC0000081, 0xC0000082, 0xC0000083, 0xC0000084, 0xC0000100, 0xC0000101, 0xC0000102, 0x174, 0x175, 0x176 };
    int nativeOk = 0;
    for (unsigned i = 0; i < sizeof(native) / sizeof(native[0]); i++)
        nativeOk += hv_vcpu_enable_native_msr(v, native[i], true) == HV_SUCCESS;
    EXPECT(nativeOk == (int)(sizeof(native) / sizeof(native[0])), "hv_vcpu_enable_native_msr for %d MSRs", nativeOk);
    EXPECT(hv_vcpu_write_msr(v, 0xC0000082, 0xffffff8000100000ull) == HV_SUCCESS, "hv_vcpu_write_msr(LSTAR)");
    uint64_t msr = 0;
    EXPECT(hv_vcpu_read_msr(v, 0xC0000082, &msr) == HV_SUCCESS && msr == 0xffffff8000100000ull, "LSTAR reads back");

    setup_real_mode(v, 0x1000);
    EXPECT(rreg(v, HV_X86_RIP) == 0x1000, "RIP is 0x1000");
    wreg(v, HV_X86_RAX, 0x1122334455667788ull);
    wreg(v, HV_X86_R15, 0xF15);
    EXPECT(rreg(v, HV_X86_RAX) == 0x1122334455667788ull && rreg(v, HV_X86_R15) == 0xF15, "general registers round trip");
    wreg(v, HV_X86_CR2, 0xc2c2000);
    wreg(v, HV_X86_DR6, 0xFFFF0FF0);
    EXPECT(rreg(v, HV_X86_CR2) == 0xc2c2000 && rreg(v, HV_X86_DR6) == 0xFFFF0FF0, "CR2 and DR6 round trip");
    uint64_t bad;
    /* the shim refuses CR1; Apple's own framework reads it straight from its shared page and answers 0 */
    bad = 0xdead;
    hv_return_t cr1 = hv_vcpu_read_register(v, HV_X86_CR1, &bad);
    EXPECT(cr1 != HV_SUCCESS || bad == 0, "CR1 does not exist (an error, or 0 as Apple's framework answers)");
    EXPECT(rreg(v, HV_X86_XCR0) == 1, "XCR0 starts as 1");

    uint8_t fp[4096];
    memset(fp, 0xEE, sizeof(fp));
    EXPECT(hv_vcpu_read_fpstate(v, fp, sizeof(fp)) == HV_SUCCESS, "hv_vcpu_read_fpstate");
    uint32_t mxcsr;
    memcpy(&mxcsr, fp + 24, 4);
    EXPECT(mxcsr == 0x1F80, "MXCSR in the saved state is the power-on value");

    printf("stage 2: serial output, CPUID, HLT\n--- guest output ---\n");
    wreg(v, HV_X86_RAX, 0);
    int halted = 0, ios = 0, cpuid = 0, other = 0;
    for (int n = 0; n < 5000 && !halted; n++) {
        r = hv_vcpu_run(v);
        if (r != HV_SUCCESS) {
            fprintf(stderr, "hv_vcpu_run failed: %#x\n", (unsigned)r);
            break;
        }
        uint64_t reason = rvmcs(v, VMCS_RO_EXIT_REASON) & 0xFFFF;
        uint64_t len = rvmcs(v, VMCS_RO_VMEXIT_INSTR_LEN);
        switch (reason) {
        case VMX_REASON_IRQ:
            break;                                                    /* a host interrupt: just run again */
        case VMX_REASON_IO: {
            uint64_t q = rvmcs(v, VMCS_RO_EXIT_QUALIFIC);
            if ((q >> 16) == 0x3F8 && !(q & 8)) {
                putchar((int)(rreg(v, HV_X86_RAX) & 0xFF));
                ios++;
            } else
                other++;
            wreg(v, HV_X86_RIP, rreg(v, HV_X86_RIP) + len);
            break;
        }
        case VMX_REASON_CPUID:
            wreg(v, HV_X86_RAX, 0x52485631);
            wreg(v, HV_X86_RBX, 0);
            wreg(v, HV_X86_RCX, 0);
            wreg(v, HV_X86_RDX, 0);
            wreg(v, HV_X86_RIP, rreg(v, HV_X86_RIP) + len);
            cpuid++;
            break;
        case VMX_REASON_HLT:
            halted = 1;
            break;
        default:
            printf("unexpected exit reason %" PRIu64 "\n", reason);
            other++;
            halted = 1;
        }
    }
    EXPECT(halted && other == 0, "the guest reached HLT with no unexpected exits");
    EXPECT(ios == (int)strlen(kMessage), "%d characters arrived as I/O exits", ios);
    EXPECT(cpuid == 1 && (rreg(v, HV_X86_RAX) & 0xFFFFFFFF) == 0x52485631, "CPUID was answered and the guest kept the result");

    printf("stage 3: EPT violation, hv_vm_map from the handler\n");
    setup_real_mode(v, 0x1200);
    r = hv_vcpu_run(v);
    EXPECT(r == HV_SUCCESS && (rvmcs(v, VMCS_RO_EXIT_REASON) & 0xFFFF) == VMX_REASON_EPT_VIOLATION, "the store to an unmapped page is an EPT violation");
    EXPECT(rvmcs(v, VMCS_GUEST_PHYSICAL_ADDRESS) == 0x101010, "guest-physical address 0x%" PRIx64, rvmcs(v, VMCS_GUEST_PHYSICAL_ADDRESS));
    uint8_t *extra = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    memset(extra, 0, PAGE);
    EXPECT(hv_vm_map(extra, 0x101000, PAGE, HV_MEMORY_READ | HV_MEMORY_WRITE) == HV_SUCCESS, "page mapped by the handler");
    r = hv_vcpu_run(v);
    EXPECT(r == HV_SUCCESS && (rvmcs(v, VMCS_RO_EXIT_REASON) & 0xFFFF) == VMX_REASON_HLT, "the guest continued to HLT");
    EXPECT(extra[0x10] == 0x42, "the store landed in our page (0x%02x)", extra[0x10]);
    EXPECT(hv_vm_protect(0x101000, PAGE, HV_MEMORY_READ) == HV_SUCCESS, "hv_vm_protect");
    EXPECT(hv_vm_unmap(0x101000, PAGE) == HV_SUCCESS, "hv_vm_unmap");

    printf("stage 4: hv_vcpu_interrupt from another thread\n");
    setup_real_mode(v, 0x1300);
    wvmcs(v, VMCS_CTRL_CPU_BASED, rvmcs(v, VMCS_CTRL_CPU_BASED));
    hv_return_t kickResult = HV_ERROR;
    pthread_t th;
    pthread_create(&th, NULL, interrupter, &kickResult);
    int returns = 0, ext = 0;
    for (int i = 0; i < 100 && returns < 3; i++) {                /* the loop never ends on its own: only exits get us out */
        if (hv_vcpu_run(v) != HV_SUCCESS)
            break;
        returns++;
        if ((rvmcs(v, VMCS_RO_EXIT_REASON) & 0xFFFF) == VMX_REASON_IRQ)
            ext++;
    }
    pthread_join(th, NULL);
    EXPECT(kickResult == HV_SUCCESS, "hv_vcpu_interrupt from another thread succeeded");
    EXPECT(returns >= 3 && ext == returns, "hv_vcpu_run kept coming back with an external-interrupt exit (%d times)", returns);

    printf("stage 5: error codes\n");
    EXPECT(hv_vm_map(ram + 1, 0x200000, PAGE, HV_MEMORY_READ) != HV_SUCCESS, "an unaligned address is refused");
    EXPECT(hv_vm_map(ram, 0x200000, PAGE, 0x100) != HV_SUCCESS, "unknown memory flags are refused");
    uint64_t junk;
    EXPECT(hv_vmx_vcpu_read_vmcs(v, 0x12345, &junk) != HV_SUCCESS, "an unknown VMCS field is refused");
    {
        hv_return_t dr = hv_vm_destroy();      /* HV_BUSY from the shim, HV_ERROR from Apple's own framework */
        EXPECT(dr == HV_BUSY || dr == HV_ERROR, "hv_vm_destroy with a live vCPU is refused (0x%x)", (unsigned)dr);
    }

    EXPECT(hv_vcpu_destroy(v) == HV_SUCCESS, "hv_vcpu_destroy");
    EXPECT(hv_vm_destroy() == HV_SUCCESS, "hv_vm_destroy");
    munmap(ram, RAM_SIZE);

    if (failures) {
        printf("FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: a VMM written against the public Hypervisor.framework API works\n");
    return 0;
}
