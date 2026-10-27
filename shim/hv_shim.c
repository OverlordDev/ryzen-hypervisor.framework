/*
 * hv_shim.c - the Hypervisor.framework front end for AMD CPUs.
 *
 * Interposes the x86 Hypervisor.framework entry points (dyld __interpose), so an unmodified app (QEMU, ...) calls the
 * RyzenHV kext instead of Apple's VT-x implementation. The behaviour is in rhv_core.c; this file only adapts the
 * real prototypes, sets the backend up, and traces.
 *
 *   sudo env DYLD_INSERT_LIBRARIES=build/libryzenhv.dylib RYZENHV_TRACE=1 <app> ...
 *
 * (sudo, because the kext only lets root create VMs for now; `env` because sudo strips DYLD_* from the environment.)
 * Hardened-runtime apps need AMFI relaxed, which is the case on this Ryzentosh (amfi_get_out_of_my_way, amfi=0x80).
 *
 * The prototypes come from the SDK header, so the compiler tells us if a signature does not match your SDK.
 */
#include <Hypervisor/Hypervisor.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "rhv_core.h"
#include "rhv_iokit.h"

#define DYLD_INTERPOSE(_replacement, _replacee)                                   \
    __attribute__((used)) static struct {                                         \
        const void *replacement;                                                  \
        const void *replacee;                                                     \
    } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = {   \
        (const void *)(unsigned long)&_replacement,                               \
        (const void *)(unsigned long)&_replacee                                   \
    }

/* ---- tracing: RYZENHV_TRACE=1 (stderr), RYZENHV_TRACE_FILE=<path> ---- */

static int g_trace;
static FILE *g_stream;
static double g_t0;

static double seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void rhv_trace(const char *fmt, ...)
{
    if (!g_trace)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(g_stream, "[ryzenhv %8.4f pid %d] ", seconds() - g_t0, (int)getpid());
    vfprintf(g_stream, fmt, ap);
    fputc('\n', g_stream);
    fflush(g_stream);
    va_end(ap);
}

__attribute__((constructor)) static void rhv_shim_init(void)
{
    const char *v = getenv("RYZENHV_TRACE");
    g_trace = v && *v && *v != '0';
    const char *path = getenv("RYZENHV_TRACE_FILE");
    g_stream = path && *path ? fopen(path, "a") : NULL;
    if (!g_stream)
        g_stream = stderr;
    g_t0 = seconds();
    if (g_trace)
        rhv_core_trace = rhv_trace;

    RhvBackend b;
    rhv_iokit_backend(&b);
    rhv_core_init(&b);
    rhv_trace("shim loaded (ABI %d)", RHV_ABI_VERSION);
}

#define RESULT(name, expr)                                                         \
    do {                                                                           \
        hv_return_t _r = (hv_return_t)(expr);                                      \
        if (g_trace && _r != HV_SUCCESS)                                           \
            rhv_trace("%s -> error %#x", name, (unsigned)_r);                      \
        return _r;                                                                 \
    } while (0)

/* ---- the implemented API ---- */

static hv_return_t rhv_hv_vm_create(hv_vm_options_t flags)
{
    rhv_trace("hv_vm_create(%#llx)", (unsigned long long)flags);
    RESULT("hv_vm_create", rhv_vm_create(flags));
}
DYLD_INTERPOSE(rhv_hv_vm_create, hv_vm_create);

static hv_return_t rhv_hv_vm_destroy(void)
{
    rhv_trace("hv_vm_destroy()");
    RESULT("hv_vm_destroy", rhv_vm_destroy());
}
DYLD_INTERPOSE(rhv_hv_vm_destroy, hv_vm_destroy);

static hv_return_t rhv_hv_vm_map(hv_uvaddr_t uva, hv_gpaddr_t gpa, size_t size, hv_memory_flags_t flags)
{
    rhv_trace("hv_vm_map(uva=%p gpa=%#llx size=%#zx flags=%#llx)", uva, (unsigned long long)gpa, size, (unsigned long long)flags);
    RESULT("hv_vm_map", rhv_vm_map((uint64_t)(uintptr_t)uva, gpa, size, flags));
}
DYLD_INTERPOSE(rhv_hv_vm_map, hv_vm_map);

static hv_return_t rhv_hv_vm_unmap(hv_gpaddr_t gpa, size_t size)
{
    rhv_trace("hv_vm_unmap(gpa=%#llx size=%#zx)", (unsigned long long)gpa, size);
    RESULT("hv_vm_unmap", rhv_vm_unmap(gpa, size));
}
DYLD_INTERPOSE(rhv_hv_vm_unmap, hv_vm_unmap);

static hv_return_t rhv_hv_vm_protect(hv_gpaddr_t gpa, size_t size, hv_memory_flags_t flags)
{
    rhv_trace("hv_vm_protect(gpa=%#llx size=%#zx flags=%#llx)", (unsigned long long)gpa, size, (unsigned long long)flags);
    RESULT("hv_vm_protect", rhv_vm_protect(gpa, size, flags));
}
DYLD_INTERPOSE(rhv_hv_vm_protect, hv_vm_protect);

static hv_return_t rhv_hv_vm_sync_tsc(uint64_t tsc)
{
    rhv_trace("hv_vm_sync_tsc(%#llx)", (unsigned long long)tsc);
    RESULT("hv_vm_sync_tsc", rhv_vm_sync_tsc(tsc));
}
DYLD_INTERPOSE(rhv_hv_vm_sync_tsc, hv_vm_sync_tsc);

static hv_return_t rhv_hv_vcpu_create(hv_vcpuid_t *vcpu, hv_vcpu_options_t flags)
{
    uint32_t id = 0;
    hv_return_t r = (hv_return_t)rhv_vcpu_create(&id, flags);
    if (r == HV_SUCCESS && vcpu)
        *vcpu = id;
    rhv_trace("hv_vcpu_create(flags=%#llx) -> %#x vcpu %u", (unsigned long long)flags, (unsigned)r, id);
    return r;
}
DYLD_INTERPOSE(rhv_hv_vcpu_create, hv_vcpu_create);

static hv_return_t rhv_hv_vcpu_destroy(hv_vcpuid_t vcpu)
{
    rhv_trace("hv_vcpu_destroy(%u)", vcpu);
    RESULT("hv_vcpu_destroy", rhv_vcpu_destroy(vcpu));
}
DYLD_INTERPOSE(rhv_hv_vcpu_destroy, hv_vcpu_destroy);

static hv_return_t rhv_hv_vcpu_run(hv_vcpuid_t vcpu)
{
    hv_return_t r = (hv_return_t)rhv_vcpu_run(vcpu);
    if (g_trace) {
        uint64_t reason = 0, rip = 0;
        rhv_vmx_vcpu_read_vmcs(vcpu, 0x4402, &reason);
        rhv_vmx_vcpu_read_vmcs(vcpu, 0x681e, &rip);
        rhv_trace("hv_vcpu_run(%u) -> %#x exit reason %llu rip %#llx", vcpu, (unsigned)r, (unsigned long long)(reason & 0xFFFF),
                  (unsigned long long)rip);
    }
    return r;
}
DYLD_INTERPOSE(rhv_hv_vcpu_run, hv_vcpu_run);

static hv_return_t rhv_hv_vcpu_run_until(hv_vcpuid_t vcpu, uint64_t deadline)
{
    (void)deadline;                       /* the kernel's own time slice bounds every run */
    return rhv_hv_vcpu_run(vcpu);
}
DYLD_INTERPOSE(rhv_hv_vcpu_run_until, hv_vcpu_run_until);

static hv_return_t rhv_hv_vcpu_interrupt(hv_vcpuid_t *vcpus, unsigned int count)
{
    RESULT("hv_vcpu_interrupt", rhv_vcpu_interrupt(vcpus, count));
}
DYLD_INTERPOSE(rhv_hv_vcpu_interrupt, hv_vcpu_interrupt);

static hv_return_t rhv_hv_vcpu_read_register(hv_vcpuid_t vcpu, hv_x86_reg_t reg, uint64_t *value)
{
    RESULT("hv_vcpu_read_register", rhv_vcpu_read_register(vcpu, (uint32_t)reg, value));
}
DYLD_INTERPOSE(rhv_hv_vcpu_read_register, hv_vcpu_read_register);

static hv_return_t rhv_hv_vcpu_write_register(hv_vcpuid_t vcpu, hv_x86_reg_t reg, uint64_t value)
{
    RESULT("hv_vcpu_write_register", rhv_vcpu_write_register(vcpu, (uint32_t)reg, value));
}
DYLD_INTERPOSE(rhv_hv_vcpu_write_register, hv_vcpu_write_register);

static hv_return_t rhv_hv_vcpu_read_fpstate(hv_vcpuid_t vcpu, void *buffer, size_t size)
{
    RESULT("hv_vcpu_read_fpstate", rhv_vcpu_read_fpstate(vcpu, buffer, size));
}
DYLD_INTERPOSE(rhv_hv_vcpu_read_fpstate, hv_vcpu_read_fpstate);

static hv_return_t rhv_hv_vcpu_write_fpstate(hv_vcpuid_t vcpu, void *buffer, size_t size)
{
    RESULT("hv_vcpu_write_fpstate", rhv_vcpu_write_fpstate(vcpu, buffer, size));
}
DYLD_INTERPOSE(rhv_hv_vcpu_write_fpstate, hv_vcpu_write_fpstate);

static hv_return_t rhv_hv_vcpu_read_msr(hv_vcpuid_t vcpu, uint32_t msr, uint64_t *value)
{
    RESULT("hv_vcpu_read_msr", rhv_vcpu_read_msr(vcpu, msr, value));
}
DYLD_INTERPOSE(rhv_hv_vcpu_read_msr, hv_vcpu_read_msr);

static hv_return_t rhv_hv_vcpu_write_msr(hv_vcpuid_t vcpu, uint32_t msr, uint64_t value)
{
    RESULT("hv_vcpu_write_msr", rhv_vcpu_write_msr(vcpu, msr, value));
}
DYLD_INTERPOSE(rhv_hv_vcpu_write_msr, hv_vcpu_write_msr);

static hv_return_t rhv_hv_vcpu_enable_native_msr(hv_vcpuid_t vcpu, uint32_t msr, _Bool enable)
{
    rhv_trace("hv_vcpu_enable_native_msr(%u, %#x, %d)", vcpu, msr, (int)enable);
    RESULT("hv_vcpu_enable_native_msr", rhv_vcpu_enable_native_msr(vcpu, msr, enable));
}
DYLD_INTERPOSE(rhv_hv_vcpu_enable_native_msr, hv_vcpu_enable_native_msr);

/* "Managed" MSRs are native ones the framework also saves and restores across its own context switches; the kext keeps
 * guest MSR state in the VMCB, which VMSAVE/VMLOAD already switch, so there is no difference for us. */
static hv_return_t rhv_hv_vcpu_enable_managed_msr(hv_vcpuid_t vcpu, uint32_t msr, _Bool enable)
{
    return rhv_hv_vcpu_enable_native_msr(vcpu, msr, enable);
}
DYLD_INTERPOSE(rhv_hv_vcpu_enable_managed_msr, hv_vcpu_enable_managed_msr);

static hv_return_t rhv_hv_vcpu_flush(hv_vcpuid_t vcpu)
{
    RESULT("hv_vcpu_flush", rhv_vcpu_flush(vcpu));
}
DYLD_INTERPOSE(rhv_hv_vcpu_flush, hv_vcpu_flush);

static hv_return_t rhv_hv_vcpu_invalidate_tlb(hv_vcpuid_t vcpu)
{
    RESULT("hv_vcpu_invalidate_tlb", rhv_vcpu_invalidate_tlb(vcpu));
}
DYLD_INTERPOSE(rhv_hv_vcpu_invalidate_tlb, hv_vcpu_invalidate_tlb);

static hv_return_t rhv_hv_vcpu_get_exec_time(hv_vcpuid_t vcpu, uint64_t *time)
{
    RESULT("hv_vcpu_get_exec_time", rhv_vcpu_get_exec_time(vcpu, time));
}
DYLD_INTERPOSE(rhv_hv_vcpu_get_exec_time, hv_vcpu_get_exec_time);

static hv_return_t rhv_hv_vmx_vcpu_read_vmcs(hv_vcpuid_t vcpu, uint32_t field, uint64_t *value)
{
    RESULT("hv_vmx_vcpu_read_vmcs", rhv_vmx_vcpu_read_vmcs(vcpu, field, value));
}
DYLD_INTERPOSE(rhv_hv_vmx_vcpu_read_vmcs, hv_vmx_vcpu_read_vmcs);

static hv_return_t rhv_hv_vmx_vcpu_write_vmcs(hv_vcpuid_t vcpu, uint32_t field, uint64_t value)
{
    RESULT("hv_vmx_vcpu_write_vmcs", rhv_vmx_vcpu_write_vmcs(vcpu, field, value));
}
DYLD_INTERPOSE(rhv_hv_vmx_vcpu_write_vmcs, hv_vmx_vcpu_write_vmcs);

static hv_return_t rhv_hv_vmx_read_capability(hv_vmx_capability_t field, uint64_t *value)
{
    RESULT("hv_vmx_read_capability", rhv_vmx_read_capability((uint32_t)field, value));
}
DYLD_INTERPOSE(rhv_hv_vmx_read_capability, hv_vmx_read_capability);

/* ---- everything else: HV_UNSUPPORTED, never Apple's implementation ---- */

#define RHV_STUB(name, args)                                                        \
    static hv_return_t rhv_stub_##name args                                         \
    {                                                                               \
        rhv_trace("%s: not supported by RyzenHV", #name);                           \
        return (hv_return_t)RHV_HV_UNSUPPORTED;                                     \
    }                                                                               \
    DYLD_INTERPOSE(rhv_stub_##name, name);
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "hv_stubs.inc"
