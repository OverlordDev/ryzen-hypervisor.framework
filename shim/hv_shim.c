/*
 * hv_shim.c - userland front end, skeleton.
 *
 * Interposes the x86 Hypervisor.framework entry points so that an unmodified
 * app (QEMU, ...) ends up here instead of in Apple's VMX-only implementation.
 * Today every call just traces itself and returns HV_UNSUPPORTED. The plan is
 * to forward to the kext through IOConnectCallMethod, see docs/ARCHITECTURE.md.
 *
 * Usage:
 *   RYZENHV_TRACE=1 DYLD_INSERT_LIBRARIES=build/libryzenhv.dylib <app> ...
 *
 * Needs SIP / AMFI relaxed for hardened-runtime apps, which is why the long
 * term plan is a kernel-side front end (docs/ARCHITECTURE.md, option A).
 *
 * The prototypes come from the SDK header, so the compiler tells us if one of
 * the signatures below does not match your macOS SDK.
 */
#include <Hypervisor/Hypervisor.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define DYLD_INTERPOSE(_replacement, _replacee)                                   \
    __attribute__((used)) static struct {                                         \
        const void *replacement;                                                  \
        const void *replacee;                                                     \
    } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = {   \
        (const void *)(unsigned long)&_replacement,                               \
        (const void *)(unsigned long)&_replacee                                   \
    }

static int rhv_trace_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("RYZENHV_TRACE");
        cached = (v && *v && *v != '0') ? 1 : 0;
    }
    return cached;
}

/*
 * RYZENHV_TRACE=1            trace to stderr
 * RYZENHV_TRACE_FILE=<path>  append to a file instead (handy for apps that swallow stderr)
 * Every line: seconds since the first call, pid, then the call.
 */
static FILE *rhv_trace_stream(void)
{
    static FILE *stream;
    if (!stream) {
        const char *path = getenv("RYZENHV_TRACE_FILE");
        stream = path && *path ? fopen(path, "a") : NULL;
        if (!stream)
            stream = stderr;
    }
    return stream;
}

static double rhv_trace_seconds(void)
{
    static struct timespec first;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!first.tv_sec && !first.tv_nsec)
        first = now;
    return (double)(now.tv_sec - first.tv_sec) + (double)(now.tv_nsec - first.tv_nsec) / 1e9;
}

#define TRACE(...)                                                              \
    do {                                                                        \
        if (rhv_trace_enabled()) {                                              \
            FILE *rhv_f = rhv_trace_stream();                                   \
            fprintf(rhv_f, "[ryzenhv %9.6f pid %d] ", rhv_trace_seconds(),      \
                    (int)getpid());                                             \
            fprintf(rhv_f, __VA_ARGS__);                                        \
            fputc('\n', rhv_f);                                                 \
            fflush(rhv_f);                                                      \
        }                                                                       \
    } while (0)

/* VM lifecycle and memory */

static hv_return_t rhv_hv_vm_create(hv_vm_options_t flags)
{
    TRACE("hv_vm_create(flags=0x%llx)", (unsigned long long)flags);
    return HV_UNSUPPORTED;
}
DYLD_INTERPOSE(rhv_hv_vm_create, hv_vm_create);

static hv_return_t rhv_hv_vm_destroy(void)
{
    TRACE("hv_vm_destroy()");
    return HV_UNSUPPORTED;
}
DYLD_INTERPOSE(rhv_hv_vm_destroy, hv_vm_destroy);

static hv_return_t rhv_hv_vm_map(hv_uvaddr_t uva, hv_gpaddr_t gpa, size_t size,
                                 hv_memory_flags_t flags)
{
    TRACE("hv_vm_map(uva=%p gpa=0x%llx size=0x%zx flags=0x%llx)", (void *)uva,
          (unsigned long long)gpa, size, (unsigned long long)flags);
    return HV_UNSUPPORTED;
}
DYLD_INTERPOSE(rhv_hv_vm_map, hv_vm_map);

static hv_return_t rhv_hv_vm_unmap(hv_gpaddr_t gpa, size_t size)
{
    TRACE("hv_vm_unmap(gpa=0x%llx size=0x%zx)", (unsigned long long)gpa, size);
    return HV_UNSUPPORTED;
}
DYLD_INTERPOSE(rhv_hv_vm_unmap, hv_vm_unmap);

static hv_return_t rhv_hv_vm_protect(hv_gpaddr_t gpa, size_t size, hv_memory_flags_t flags)
{
    TRACE("hv_vm_protect(gpa=0x%llx size=0x%zx flags=0x%llx)", (unsigned long long)gpa, size,
          (unsigned long long)flags);
    return HV_UNSUPPORTED;
}
DYLD_INTERPOSE(rhv_hv_vm_protect, hv_vm_protect);

/* vCPU */

static hv_return_t rhv_hv_vcpu_create(hv_vcpuid_t *vcpu, hv_vcpu_options_t flags)
{
    (void)vcpu;
    TRACE("hv_vcpu_create(flags=0x%llx)", (unsigned long long)flags);
    return HV_UNSUPPORTED;
}
DYLD_INTERPOSE(rhv_hv_vcpu_create, hv_vcpu_create);

static hv_return_t rhv_hv_vcpu_destroy(hv_vcpuid_t vcpu)
{
    TRACE("hv_vcpu_destroy(vcpu=%u)", (unsigned)vcpu);
    return HV_UNSUPPORTED;
}
DYLD_INTERPOSE(rhv_hv_vcpu_destroy, hv_vcpu_destroy);

static hv_return_t rhv_hv_vcpu_run(hv_vcpuid_t vcpu)
{
    TRACE("hv_vcpu_run(vcpu=%u)", (unsigned)vcpu);
    return HV_UNSUPPORTED;
}
DYLD_INTERPOSE(rhv_hv_vcpu_run, hv_vcpu_run);

static hv_return_t rhv_hv_vcpu_read_register(hv_vcpuid_t vcpu, hv_x86_reg_t reg, uint64_t *value)
{
    (void)value;
    TRACE("hv_vcpu_read_register(vcpu=%u reg=%d)", (unsigned)vcpu, (int)reg);
    return HV_UNSUPPORTED;
}
DYLD_INTERPOSE(rhv_hv_vcpu_read_register, hv_vcpu_read_register);

static hv_return_t rhv_hv_vcpu_write_register(hv_vcpuid_t vcpu, hv_x86_reg_t reg, uint64_t value)
{
    TRACE("hv_vcpu_write_register(vcpu=%u reg=%d value=0x%llx)", (unsigned)vcpu, (int)reg,
          (unsigned long long)value);
    return HV_UNSUPPORTED;
}
DYLD_INTERPOSE(rhv_hv_vcpu_write_register, hv_vcpu_write_register);

/* VMX-shaped parts: these are what the VMCS -> VMCB translation layer will back. */

static hv_return_t rhv_hv_vmx_vcpu_read_vmcs(hv_vcpuid_t vcpu, uint32_t field, uint64_t *value)
{
    (void)value;
    TRACE("hv_vmx_vcpu_read_vmcs(vcpu=%u field=0x%x)", (unsigned)vcpu, field);
    return HV_UNSUPPORTED;
}
DYLD_INTERPOSE(rhv_hv_vmx_vcpu_read_vmcs, hv_vmx_vcpu_read_vmcs);

static hv_return_t rhv_hv_vmx_vcpu_write_vmcs(hv_vcpuid_t vcpu, uint32_t field, uint64_t value)
{
    TRACE("hv_vmx_vcpu_write_vmcs(vcpu=%u field=0x%x value=0x%llx)", (unsigned)vcpu, field,
          (unsigned long long)value);
    return HV_UNSUPPORTED;
}
DYLD_INTERPOSE(rhv_hv_vmx_vcpu_write_vmcs, hv_vmx_vcpu_write_vmcs);

static hv_return_t rhv_hv_vmx_read_capability(hv_vmx_capability_t field, uint64_t *value)
{
    (void)value;
    TRACE("hv_vmx_read_capability(field=%d)", (int)field);
    return HV_UNSUPPORTED;
}
DYLD_INTERPOSE(rhv_hv_vmx_read_capability, hv_vmx_read_capability);
