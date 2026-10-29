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
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <execinfo.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <mach/mach.h>
#include <stdlib.h>
#include <string.h>

static int g_hyperkit;          /* the client is HyperKit: its CPUID answers are cleaned of AMD-only features */

/* HyperKit names every vCPU thread "vcpu:<n>" before it calls hv_vcpu_create, and asserts that it gets id <n> back. */
static int hyperkit_vcpu_hint(void)
{
    char name[64];
    int n;
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) == 0 && sscanf(name, "vcpu:%d", &n) == 1)
        return n;
    return -1;
}
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include "rhv_core.h"
#include "rhv_iokit.h"

/*
 * Two ways in. Loaded with DYLD_INSERT_LIBRARIES the tuples live in __interpose and dyld applies them. For a hardened
 * process that ignores that variable (Parallels' prl_vm_app), the shim is built as a stand-in for one of the process's
 * own libraries (RHV_HIJACK, shim/hijack.c): the table then holds the *names* of the replaced functions in a section of
 * our own, and hijack.c rewrites the import pointers of every image by name (dyld_dynamic_interpose does not reach
 * the bindings of already loaded images).
 */
#ifdef RHV_HIJACK
#define DYLD_INTERPOSE(_replacement, _replacee)                                   \
    __attribute__((used)) static struct {                                         \
        const char *name;                                                         \
        const void *replacement;                                                  \
    } _interpose_##_replacee __attribute__((section("__DATA,__rhvtuples"))) = {   \
        #_replacee,                                                               \
        (const void *)(unsigned long)&_replacement                                \
    }
#else
#define DYLD_INTERPOSE(_replacement, _replacee)                                   \
    __attribute__((used)) static struct {                                         \
        const void *replacement;                                                  \
        const void *replacee;                                                     \
    } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = {   \
        (const void *)(unsigned long)&_replacement,                               \
        (const void *)(unsigned long)&_replacee                                   \
    }
#endif

/* ---- tracing: RYZENHV_TRACE=1 (stderr), RYZENHV_TRACE_FILE=<path> ---- */

static int g_trace;
static int g_quiet;
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

#ifdef RHV_HIJACK
void rhv_shim_init(void)                 /* called by hijack.c, only in the process it is meant for */
#else
__attribute__((constructor)) static void rhv_shim_init(void)
#endif
{
    const char *v = getenv("RYZENHV_TRACE");
    g_trace = v && *v && *v != '0' ? atoi(v) : 0;      /* 1: every call, 2: only what is not routine I/O, CPUID, MMIO */
    /*
     * Trace and statistics cost a lot of host CPU (a line and a flush per exit, tens of thousands of exits a second), so
     * they are off unless asked for: the file /tmp/rhv-debug or ~/.ryzenhv/debug turns them on (the wrapper script sets
     * RYZENHV_TRACE and RYZENHV_STATS for every run, so the variables alone are not a request).
     */
    {
        char home[1024];
        const char *h = getenv("HOME");
        snprintf(home, sizeof(home), "%s/.ryzenhv/debug", h ? h : "/var/empty");
        g_quiet = access("/tmp/rhv-debug", F_OK) != 0 && access(home, F_OK) != 0;
    }
    if (g_quiet)
        g_trace = 0;
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
#ifdef RHV_HIJACK
    rhv_core_offer_tpr_shadow = access("/tmp/rhv-no-vtpr", F_OK) != 0;     /* Parallels: its monitor uses the TPR shadow */
#endif
    if (strstr(getprogname(), "hyperkit")) {                                 /* Docker Desktop's VMM */
        rhv_core_offer_nmi_window = 1;
        rhv_core_vcpu_hint = hyperkit_vcpu_hint;
        g_hyperkit = 1;
    }
    rhv_trace("shim loaded (ABI %d)", RHV_ABI_VERSION);
}

#ifdef RHV_HIJACK
void rhv_hijack_log(const char *msg)       /* for hijack.c, which does not see rhv_trace */
{
    rhv_trace("%s", msg);
}
#endif

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

/* The guest-memory mappings the client made, so a dump can read guest memory (triple fault diagnostics). */
static struct { uint64_t uva, gpa, size; } g_maps[128];
static int g_nmaps;

static const uint8_t *guest_ptr(uint64_t gpa, size_t len)
{
    for (int i = g_nmaps - 1; i >= 0; i--)
        if (gpa >= g_maps[i].gpa && gpa + len <= g_maps[i].gpa + g_maps[i].size)
            return (const uint8_t *)(uintptr_t)(g_maps[i].uva + (gpa - g_maps[i].gpa));
    return NULL;
}

/* 4-level page walk of CR3; returns the guest-physical address or ~0. */
static uint64_t guest_walk(uint64_t cr3, uint64_t va)
{
    uint64_t table = cr3 & 0x000FFFFFFFFFF000ull;
    for (int level = 3; level >= 0; level--) {
        const uint64_t *t = (const uint64_t *)guest_ptr(table, 4096);
        if (!t)
            return ~0ull;
        uint64_t e = t[(va >> (12 + 9 * level)) & 511];
        if (!(e & 1))
            return ~0ull;
        if (level > 0 && (e & 0x80)) {                      /* 1 GiB or 2 MiB page */
            uint64_t mask = (1ull << (12 + 9 * level)) - 1;
            return (e & 0x000FFFFFFFFFF000ull & ~mask) | (va & mask);
        }
        table = e & 0x000FFFFFFFFFF000ull;
        if (level == 0)
            return table | (va & 0xFFF);
    }
    return ~0ull;
}

static int guest_read(uint64_t cr3, uint64_t va, void *out, size_t len)
{
    uint8_t *o = out;
    for (size_t i = 0; i < len;) {
        uint64_t gpa = guest_walk(cr3, va + i);
        size_t chunk = 4096 - ((va + i) & 0xFFF);
        if (chunk > len - i)
            chunk = len - i;
        const uint8_t *p = gpa == ~0ull ? NULL : guest_ptr(gpa, chunk);
        if (!p)
            return -1;
        memcpy(o + i, p, chunk);
        i += chunk;
    }
    return 0;
}

/*
 * A Windows guest that reboots by itself has usually bugchecked: Windows leaves a crash dump header ("PAGEDU64", bugcheck
 * code at +0x38, its four parameters at +0x40) in memory until the machine resets. When the guest asks for a reset
 * (port 0xCF9, or 0xFE to the keyboard controller) every resident page of the guest's RAM is searched for it.
 */
#include <sys/mman.h>
static void bugcheck_scan(const char *why)
{
    unsigned found = 0;
    for (int i = 0; i < g_nmaps && found < 4; i++) {
        if (g_maps[i].size < (1u << 20))
            continue;
        for (uint64_t off = 0; off < g_maps[i].size && found < 4; off += 1u << 20) {
            char vec[256];                                  /* 1 MiB = 256 pages */
            if (mincore((void *)(uintptr_t)(g_maps[i].uva + off), 1u << 20, vec) != 0)
                continue;
            for (int pg = 0; pg < 256; pg++) {
                if (!(vec[pg] & 1))
                    continue;
                const uint8_t *p = (const uint8_t *)(uintptr_t)(g_maps[i].uva + off + (uint64_t)pg * 4096);
                if (memcmp(p, "PAGEDU64", 8) == 0) {
                    uint32_t code;
                    uint64_t par[4];
                    memcpy(&code, p + 0x38, 4);
                    memcpy(par, p + 0x40, sizeof(par));
                    rhv_trace("BUGCHECK (%s): code %#x params %#llx %#llx %#llx %#llx (dump header at gpa %#llx)", why, code,
                              (unsigned long long)par[0], (unsigned long long)par[1], (unsigned long long)par[2],
                              (unsigned long long)par[3],
                              (unsigned long long)(g_maps[i].gpa + off + (uint64_t)pg * 4096));
                    found++;
                }
            }
        }
    }
    if (!found)
        rhv_trace("reset requested (%s): no Windows crash dump header in memory", why);
}

/*
 * What the guest stores to a local APIC register (the ICR, to see INIT and startup IPIs): decodes the common store
 * instructions at RIP (89 /r, C7 /0, optional prefixes and REX) and returns the value, or -1.
 */
static int64_t mmio_store_value(hv_vcpuid_t vcpu)
{
    uint64_t rip = 0, cr3 = 0;
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x681e, &rip);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x6802, &cr3);
    uint8_t b[16];
    if (guest_read(cr3, rip, b, sizeof(b)) != 0)
        return -1;
    unsigned i = 0;
    uint8_t rex = 0;
    while (i < 4 && (b[i] == 0x66 || b[i] == 0xF2 || b[i] == 0xF3 || b[i] == 0x2E || b[i] == 0x3E))
        i++;
    if ((b[i] & 0xF0) == 0x40)
        rex = b[i++];
    uint8_t op = b[i++];
    if (op != 0x89 && op != 0xC7)
        return -1;
    uint8_t modrm = b[i++];
    unsigned mod = modrm >> 6, reg = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0), rm = modrm & 7;
    if (mod == 3)
        return -1;
    if (rm == 4) {                                       /* SIB */
        uint8_t sib = b[i++];
        if (mod == 0 && (sib & 7) == 5)
            i += 4;
    } else if (mod == 0 && rm == 5) {
        i += 4;
    }
    if (mod == 1)
        i += 1;
    else if (mod == 2)
        i += 4;
    if (op == 0xC7) {
        uint32_t imm;
        memcpy(&imm, b + i, 4);
        return (int64_t)imm;
    }
    static const unsigned hvreg[16] = { 2, 3, 4, 5, 8, 9, 6, 7, 10, 11, 12, 13, 14, 15, 16, 17 };
    uint64_t v = 0;
    rhv_vcpu_read_register(vcpu, hvreg[reg], &v);
    return (int64_t)(v & 0xFFFFFFFFull);
}

static void triple_fault_dump(hv_vcpuid_t vcpu)
{
    uint64_t cr0 = 0, cr3 = 0, cr4 = 0, efer = 0, rip = 0, rsp = 0, idtb = 0, idtl = 0, gdtb = 0, gdtl = 0, csb = 0, csar = 0;
    uint64_t info = 0, info2 = 0;
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x6800, &cr0);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x6802, &cr3);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x6804, &cr4);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x2806, &efer);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x681e, &rip);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x681c, &rsp);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x6818, &idtb);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x4812, &idtl);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x6816, &gdtb);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x4810, &gdtl);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x6808, &csb);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x4816, &csar);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x4408, &info);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x440a, &info2);
    rhv_trace("TRIPLE FAULT: cr0 %#llx cr3 %#llx cr4 %#llx efer %#llx rip %#llx rsp %#llx", (unsigned long long)cr0,
              (unsigned long long)cr3, (unsigned long long)cr4, (unsigned long long)efer, (unsigned long long)rip,
              (unsigned long long)rsp);
    rhv_trace("  idt %#llx/%#llx gdt %#llx/%#llx cs base %#llx ar %#llx idt-vectoring %#llx err %#llx", (unsigned long long)idtb,
              (unsigned long long)idtl, (unsigned long long)gdtb, (unsigned long long)gdtl, (unsigned long long)csb,
              (unsigned long long)csar, (unsigned long long)info, (unsigned long long)info2);
    uint8_t code[48];
    if (guest_read(cr3, rip, code, sizeof(code)) == 0) {
        char hex[200];
        for (int i = 0; i < 48; i++)
            snprintf(hex + i * 3, 4, "%02x ", code[i]);
        rhv_trace("  code at rip: %s", hex);
    } else {
        rhv_trace("  code at rip: not readable (rip not mapped by cr3, or outside the client's mappings)");
    }
    static const struct { int vec; const char *name; } vecs[] = { { 6, "#UD" }, { 8, "#DF" }, { 13, "#GP" }, { 14, "#PF" } };
    for (size_t i = 0; i < sizeof(vecs) / sizeof(vecs[0]); i++) {
        uint8_t gate[16];
        if (guest_read(cr3, idtb + 16u * vecs[i].vec, gate, 16) == 0) {
            uint64_t off = gate[0] | gate[1] << 8 | (uint64_t)gate[6] << 16 | (uint64_t)gate[7] << 24 |
                           ((uint64_t)gate[8] | gate[9] << 8 | gate[10] << 16 | (uint64_t)gate[11] << 24) << 32;
            rhv_trace("  idt[%s]: handler %#llx selector %#x type/attr %#x ist %u", vecs[i].name, (unsigned long long)off,
                      gate[2] | gate[3] << 8, gate[5], gate[4] & 7);
        } else {
            rhv_trace("  idt[%s]: not readable", vecs[i].name);
        }
    }
    uint64_t stack[8];
    if (guest_read(cr3, rsp, stack, sizeof(stack)) == 0)
        rhv_trace("  stack: %#llx %#llx %#llx %#llx %#llx %#llx", (unsigned long long)stack[0], (unsigned long long)stack[1],
                  (unsigned long long)stack[2], (unsigned long long)stack[3], (unsigned long long)stack[4],
                  (unsigned long long)stack[5]);
}

static hv_return_t rhv_hv_vm_map(hv_uvaddr_t uva, hv_gpaddr_t gpa, size_t size, hv_memory_flags_t flags)
{
    if (g_nmaps < 128) {
        g_maps[g_nmaps].uva = (uint64_t)(uintptr_t)uva;
        g_maps[g_nmaps].gpa = gpa;
        g_maps[g_nmaps].size = size;
        g_nmaps++;
    }
    rhv_trace("hv_vm_map(uva=%p gpa=%#llx size=%#zx flags=%#llx)", uva, (unsigned long long)gpa, size, (unsigned long long)flags);
#ifdef RHV_HIJACK
    /*
     * Parallels maps one page at the local APIC's address (0xFEE00000) as the "APIC page": with Intel's hardware APIC
     * virtualization a guest access to it is intercepted, never really performed. Here it would be ordinary guest memory
     * the monitor never looks at (no LAPIC timer, no EOI, ...), so it is left unmapped and the access faults to the client.
     */
    if (gpa == 0xFEE00000ull && size == 0x1000 && access("/tmp/rhv-map-apic", F_OK) != 0) {
        return HV_SUCCESS;
    }
#endif
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

/*
 * RYZENHV_STATS=1: once a second, one line in /tmp/rhv-stats.txt (RYZENHV_STATS_FILE): VM exits per second, the share of
 * time spent in the guest, kernel round trips per exit and microseconds per exit. Cheap enough to leave on while timing.
 */
extern unsigned long long rhv_iokit_calls;
extern unsigned long long rhv_iokit_by_selector[32];
extern struct { uint32_t field; unsigned long long n; } rhv_iokit_reads[24];
extern unsigned long long rhv_core_injections;
static unsigned long long g_kicks;          /* hv_vcpu_interrupt calls */
static int g_stats = -1;
static FILE *g_stats_stream;
static struct {
    double last, inKernel, tRunStart;
    unsigned long long runs, calls0, inj0, kicks0, byReason[64];
    /* What the guest touches: I/O ports (exit 30) and 4 KiB pages of MMIO (exit 48), counted per second. */
    struct { uint64_t key; unsigned count; } io[24], mmio[24], msr[24];
} g_st;

static void stats_count(typeof(g_st.io) tab, uint64_t key)
{
    for (int i = 0; i < 24; i++) {
        if (tab[i].count && tab[i].key == key) { tab[i].count++; return; }
        if (!tab[i].count) { tab[i].key = key; tab[i].count = 1; return; }
    }
}

static void stats_print_top(const char *label, typeof(g_st.io) tab)
{
    fprintf(g_stats_stream, "           %s:", label);
    for (int shown = 0; shown < 6; shown++) {          /* the six busiest, largest first */
        int best = -1;
        for (int i = 0; i < 24; i++)
            if (tab[i].count && (best < 0 || tab[i].count > tab[best].count))
                best = i;
        if (best < 0)
            break;
        fprintf(g_stats_stream, " %#llx x%u", (unsigned long long)tab[best].key, tab[best].count);
        tab[best].count = 0;
    }
    fputc('\n', g_stats_stream);
    memset(tab, 0, sizeof(g_st.io));
}

static void stats_after_run(uint64_t reasonBasic)
{
    g_st.runs++;
    g_st.byReason[reasonBasic & 63]++;
    double now = seconds();
    if (now - g_st.last < 1.0)
        return;
    double dt = now - g_st.last;
    unsigned long long calls = rhv_iokit_calls - g_st.calls0;
    fprintf(g_stats_stream, "[%8.3f] %6.0f exits/s  guest %4.1f%%  %5.1f kernel calls/exit  %6.1f us/exit  inj %llu  kicks %llu  reasons:",
            now - g_t0, g_st.runs / dt, 100.0 * g_st.inKernel / dt, g_st.runs ? (double)calls / g_st.runs : 0.0,
            g_st.runs ? 1e6 * dt / g_st.runs : 0.0, rhv_core_injections - g_st.inj0, g_kicks - g_st.kicks0);
    for (int i = 0; i < 64; i++)
        if (g_st.byReason[i])
            fprintf(g_stats_stream, " %d:%llu", i, g_st.byReason[i]);
    fputc('\n', g_stats_stream);
    {
        static unsigned long long prev[32];
        fprintf(g_stats_stream, "           kernel calls by selector:");
        for (int i = 0; i < 32; i++) {
            if (rhv_iokit_by_selector[i] != prev[i])
                fprintf(g_stats_stream, " %d:%llu", i, rhv_iokit_by_selector[i] - prev[i]);
            prev[i] = rhv_iokit_by_selector[i];
        }
        fputc('\n', g_stats_stream);
    }
    {
        fprintf(g_stats_stream, "           single VMCS reads (cache misses):");
        for (int i = 0; i < 24; i++)
            if (rhv_iokit_reads[i].n > 200)
                fprintf(g_stats_stream, " %#x:%llu", rhv_iokit_reads[i].field, rhv_iokit_reads[i].n);
        fputc('\n', g_stats_stream);
        memset(rhv_iokit_reads, 0, sizeof(rhv_iokit_reads));
    }
    stats_print_top("io ports", g_st.io);
    stats_print_top("mmio pages", g_st.mmio);
    stats_print_top("msrs (bit 32: write)", g_st.msr);
    fflush(g_stats_stream);
    memset(g_st.byReason, 0, sizeof(g_st.byReason));
    g_st.runs = 0;
    g_st.inKernel = 0;
    g_st.calls0 = rhv_iokit_calls;
    g_st.inj0 = rhv_core_injections;
    g_st.kicks0 = g_kicks;
    g_st.last = now;
}

static int g_kbd_watch;
static void kbd_state_dump(const char *label);

static uint64_t g_hv_tscpage, g_hv_simp, g_hv_siefp;

static void hv_pages_dump(void)
{
    if (g_hv_tscpage) {
        const uint64_t *t = (const uint64_t *)guest_ptr(g_hv_tscpage, 4096);
        uint32_t tscLo, tscHi;
        __asm__ volatile("rdtsc" : "=a"(tscLo), "=d"(tscHi));
        uint64_t tsc = ((uint64_t)tscHi << 32) | tscLo;
        if (t) {
            uint64_t scale = t[1];
            int64_t offset = (int64_t)t[2];
            uint64_t ref = (uint64_t)(((unsigned __int128)tsc * scale) >> 64) + (uint64_t)offset;
            rhv_trace("HV tsc page @%#llx: seq %u scale %#llx offset %lld  -> ref time %llu (100ns) now, host tsc %llu",
                      (unsigned long long)g_hv_tscpage, (unsigned)(t[0] & 0xFFFFFFFF), (unsigned long long)scale, (long long)offset,
                      (unsigned long long)ref, (unsigned long long)tsc);
        }
    }
    for (int k = 0; k < 2; k++) {
        uint64_t gpa = k ? g_hv_siefp : g_hv_simp;
        const uint32_t *m = gpa ? (const uint32_t *)guest_ptr(gpa, 4096) : NULL;
        if (m)
            rhv_trace("HV %s @%#llx: sint0..3 slots: %u %u %u %u (message types)", k ? "SIEFP" : "SIMP", (unsigned long long)gpa,
                      m[0], m[64], m[128], m[192]);
    }
}

/* touch /tmp/rhv-madt: once, find the ACPI MADT in the guest's RAM and list the processors it declares. */
static void madt_dump(void)
{
    static int done;
    static unsigned calls;
    if (done || (++calls & 0x3FFF) != 0 || access("/tmp/rhv-madt", F_OK) != 0)
        return;
    done = 1;
    for (int i = 0; i < g_nmaps; i++) {
        if (g_maps[i].size < (1u << 20) || g_maps[i].gpa > 0x100000000ull)
            continue;
        uint64_t lim = g_maps[i].size < (0x100000000ull - g_maps[i].gpa) ? g_maps[i].size : (0x100000000ull - g_maps[i].gpa);
        for (uint64_t off = 0; off + 64 < lim; off += 16) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)(g_maps[i].uva + off);
            if (memcmp(p, "APIC", 4) != 0)
                continue;
            uint32_t len;
            memcpy(&len, p + 4, 4);
            if (len < 44 || len > 4096 || off + len > lim || p[8] < 1 || p[8] > 6)
                continue;
            uint8_t sum = 0;
            for (uint32_t k = 0; k < len; k++)
                sum += p[k];
            if (sum != 0)
                continue;
            rhv_trace("MADT at gpa %#llx, length %u, local APIC %#x", (unsigned long long)(g_maps[i].gpa + off), len, *(const uint32_t *)(p + 36));
            for (uint32_t k = 44; k + 2 <= len;) {
                uint8_t type = p[k], l = p[k + 1];
                if (l < 2)
                    break;
                if (type == 0 && l >= 8)
                    rhv_trace("  processor: acpi id %u apic id %u flags %#x", p[k + 2], p[k + 3], *(const uint32_t *)(p + k + 4));
                else if (type == 9 && l >= 16)
                    rhv_trace("  x2apic processor: x2apic id %u flags %#x uid %u", *(const uint32_t *)(p + k + 4),
                              *(const uint32_t *)(p + k + 8), *(const uint32_t *)(p + k + 12));
                else
                    rhv_trace("  entry type %u length %u", type, l);
                k += l;
            }
        }
    }
}

/* touch /tmp/rhv-dump: every second, on a timeslice exit, print the guest's registers and the code around RIP. */
static void spin_dump(hv_vcpuid_t vcpu)
{
    static double last;
    double now = seconds();
    if (now - last < 1.0 || access("/tmp/rhv-dump", F_OK) != 0)
        return;
    last = now;
    uint64_t rip = 0, cr3 = 0, rsp = 0, rfl = 0, cr0 = 0, cr4 = 0, tpr = 0;
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x681e, &rip);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x6802, &cr3);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x681c, &rsp);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x6820, &rfl);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x6800, &cr0);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x6804, &cr4);
    rhv_vcpu_read_register(vcpu, 49 /* HV_X86_TPR */, &tpr);
    uint64_t r[16];
    static const unsigned order[16] = { 2, 5, 3, 4, 6, 7, 9, 8, 10, 11, 12, 13, 14, 15, 16, 17 };
    for (int i = 0; i < 16; i++)
        rhv_vcpu_read_register(vcpu, order[i], &r[i]);
    rhv_trace("SPIN rip %#llx rfl %#llx rsp %#llx cr0 %#llx cr3 %#llx cr4 %#llx tpr %#llx", (unsigned long long)rip,
              (unsigned long long)rfl, (unsigned long long)rsp, (unsigned long long)cr0, (unsigned long long)cr3,
              (unsigned long long)cr4, (unsigned long long)tpr);
    rhv_trace("SPIN rax %#llx rbx %#llx rcx %#llx rdx %#llx rsi %#llx rdi %#llx rbp %#llx", (unsigned long long)r[0],
              (unsigned long long)r[1], (unsigned long long)r[2], (unsigned long long)r[3], (unsigned long long)r[4],
              (unsigned long long)r[5], (unsigned long long)r[6]);
    uint8_t code[128];
    if (guest_read(cr3, rip - 64, code, sizeof(code)) == 0) {
        char hex[128 * 3 + 2];
        for (int i = 0; i < 128; i++)
            snprintf(hex + i * 3, 4, "%02x ", code[i]);
        rhv_trace("SPIN code (rip-64..rip+64): %s", hex);
    }
    hv_pages_dump();
    {
        uint64_t thr = 0, cpu = 0, intr = 0, bitmap = 0;
        rhv_vmx_vcpu_read_vmcs(vcpu, 0x401c, &thr);
        rhv_vmx_vcpu_read_vmcs(vcpu, 0x4002, &cpu);
        rhv_vmx_vcpu_read_vmcs(vcpu, 0x4824, &intr);
        rhv_vmx_vcpu_read_vmcs(vcpu, 0x4004, &bitmap);
        rhv_trace("SPIN tpr-threshold %#llx cpu-based %#llx interruptibility %#llx exc-bitmap %#llx",
                  (unsigned long long)thr, (unsigned long long)cpu, (unsigned long long)intr, (unsigned long long)bitmap);
    }
    uint64_t stack[6];
    if (guest_read(cr3, rsp, stack, sizeof(stack)) == 0)
        rhv_trace("SPIN stack %#llx %#llx %#llx %#llx %#llx %#llx", (unsigned long long)stack[0], (unsigned long long)stack[1],
                  (unsigned long long)stack[2], (unsigned long long)stack[3], (unsigned long long)stack[4],
                  (unsigned long long)stack[5]);
}


/*
 * Windows guests under Parallels run on Hyper-V enlightenments the monitor emulates: synthetic timers, the "guest idle"
 * MSR, the hypercall page. Under this hypervisor the monitor never delivers the synthetic timer interrupts (no
 * interrupt is injected after the kernel starts, and the guest sits in its idle loop reading MSR 0x400000F0), so the
 * guest stalls at the boot logo. The guest only uses them if CPUID says there is a hypervisor, so the shim hides it:
 * CPUID.1:ECX[31] is cleared and the hypervisor leaves 0x40000000.. read as zeros, and Windows falls back to its
 * LAPIC timer and HLT, which the monitor handles. The monitor writes the CPUID result into the registers after the
 * exit, so the change is made on the way back in, to the cached registers, before the next run.
 * RYZENHV_SHOW_HYPERVISOR=1 leaves the CPUID leaves alone.
 */
static struct { uint32_t leaf, subleaf; int pending; } g_cpuid_all[RHV_MAX_VCPUS_PER_VM];

__attribute__((unused)) static void cpuid_note(hv_vcpuid_t vcpu)
{
    uint64_t reason = 0;
    if (vcpu >= RHV_MAX_VCPUS_PER_VM)
        return;
    __typeof__(g_cpuid_all[0]) *g_cpuid_p = &g_cpuid_all[vcpu];
#define g_cpuid (*g_cpuid_p)
    g_cpuid.pending = 0;
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x4402, &reason);
    if ((reason & 0xFFFF) != 10)
        return;
    uint64_t rax = 0, rcx = 0;
    rhv_vcpu_read_register(vcpu, 2, &rax);
    rhv_vcpu_read_register(vcpu, 3, &rcx);
    g_cpuid.leaf = (uint32_t)rax;
    g_cpuid.subleaf = (uint32_t)rcx;
    g_cpuid.pending = 1;
#undef g_cpuid
}

/*
 * HyperKit answers CPUID from the host's values, which on a Ryzen are AMD's. A Linux guest then trusts features HyperKit
 * does not emulate: MWAITX (the guest's delay loop executes it and takes a #UD), SVM, the extended topology leaf and the
 * AMD performance counters. Like the Windows fix-ups above, the clean-up is applied to the registers HyperKit wrote, on the
 * way back into the guest.
 */
__attribute__((unused)) static void cpuid_hyperkit_fixup(hv_vcpuid_t vcpu)
{
    if (vcpu >= RHV_MAX_VCPUS_PER_VM || !g_cpuid_all[vcpu].pending)
        return;
    g_cpuid_all[vcpu].pending = 0;
    uint32_t leaf = g_cpuid_all[vcpu].leaf;
    if (leaf == 0x80000001u) {
        uint64_t rcx = 0;
        rhv_vcpu_read_register(vcpu, 3, &rcx);
        const uint64_t amdOnly = (1u << 2) /* SVM */ | (1u << 22) /* TopologyExtensions */ | (1u << 23) | (1u << 24) /* PerfCtr */ |
                                 (1u << 26) /* DataBreakpointExtension */ | (1u << 27) /* PerfTsc */ | (1u << 29) /* MWAITX */;
        rhv_vcpu_write_register(vcpu, 3, rcx & ~amdOnly);
    } else if (leaf == 0x80000008u) {
        /* EBX: IBPB, IBRS, STIBP, SSBD and friends. The guest would write MSR 0x49 (PRED_CMD) and the like, which HyperKit
         * does not emulate (#GP). Report none of them. */
        rhv_vcpu_write_register(vcpu, 5, 0);
    }
}

__attribute__((unused)) static void cpuid_hide_hypervisor(hv_vcpuid_t vcpu)
{
    static int disabled = -1;
    if (disabled < 0) {
        const char *e = getenv("RYZENHV_SHOW_HYPERVISOR");
        disabled = (e && *e && *e != '0') || access("/tmp/rhv-show-hv", F_OK) == 0;
    }
    if (vcpu >= RHV_MAX_VCPUS_PER_VM)
        return;
    __typeof__(g_cpuid_all[0]) *g_cpuid_p = &g_cpuid_all[vcpu];
#define g_cpuid (*g_cpuid_p)
    if (!g_cpuid.pending || disabled)
        return;
    g_cpuid.pending = 0;
    if (g_trace) {                                         /* what the monitor answered, once per leaf/subleaf */
        static struct { uint32_t leaf, sub; } seen[160];
        static int nseen;
        int known = 0;
        for (int i = 0; i < nseen; i++)
            if (seen[i].leaf == g_cpuid.leaf && seen[i].sub == g_cpuid.subleaf)
                known = 1;
        if (!known && nseen < 160) {
            seen[nseen].leaf = g_cpuid.leaf;
            seen[nseen++].sub = g_cpuid.subleaf;
            uint64_t a = 0, b = 0, c = 0, d = 0;
            rhv_vcpu_read_register(vcpu, 2, &a);
            rhv_vcpu_read_register(vcpu, 5, &b);
            rhv_vcpu_read_register(vcpu, 3, &c);
            rhv_vcpu_read_register(vcpu, 4, &d);
            rhv_trace("cpuid %#x.%u -> eax %#x ebx %#x ecx %#x edx %#x (vcpu %u)", g_cpuid.leaf, g_cpuid.subleaf,
                      (unsigned)a, (unsigned)b, (unsigned)c, (unsigned)d, vcpu);
        }
    }
    if (g_cpuid.leaf == 4) {
        /*
         * Intel's cache and core topology leaf. The monitor passes the AMD host's leaf 4, which is empty: Windows then sees
         * every vCPU as a package of its own (4 sockets, which Windows Home does not run: it starts one CPU) and no L1
         * cache. Report what a Zen 3 core has: 32 KiB L1D and L1I (8-way), 512 KiB L2 (8-way), 16 MiB L3 (16-way, shared
         * by all), and all vCPUs in one package.
         */
        unsigned n = rhv_core_vcpu_count();
        if (n < 1)
            n = 1;
        uint32_t cores = (n - 1) << 26;
        uint32_t a = 0, b = 0, c = 0;
        switch (g_cpuid.subleaf) {
        case 0: a = cores | (1u << 8) | (1u << 5) | 1; b = 63 | (7u << 22); c = 63; break;           /* L1 data    */
        case 1: a = cores | (1u << 8) | (1u << 5) | 2; b = 63 | (7u << 22); c = 63; break;           /* L1 inst    */
        case 2: a = cores | (1u << 8) | (2u << 5) | 3; b = 63 | (7u << 22); c = 1023; break;         /* L2 unified */
        case 3: a = cores | ((n - 1) << 14) | (1u << 8) | (3u << 5) | 3; b = 63 | (15u << 22); c = 16383; break;   /* L3 */
        default: break;                                                                                /* none       */
        }
        rhv_vcpu_write_register(vcpu, 2, a);
        rhv_vcpu_write_register(vcpu, 5, b);
        rhv_vcpu_write_register(vcpu, 3, c);
        rhv_vcpu_write_register(vcpu, 4, 0);
        return;
    }
    if (g_cpuid.leaf == 0x80000008u) {
        /* AMD: ECX[7:0] = cores per package - 1, ECX[15:12] = bits of the APIC id that name the core. */
        unsigned n = rhv_core_vcpu_count();
        if (n > 1) {
            uint64_t rcx = 0;
            rhv_vcpu_read_register(vcpu, 3, &rcx);
            unsigned bits = 0;
            while ((1u << bits) < n)
                bits++;
            rcx = (rcx & ~0xF0FFull) | ((uint64_t)bits << 12) | (n - 1);
            rhv_vcpu_write_register(vcpu, 3, rcx);
        }
        return;
    }
    if (g_cpuid.leaf == 0x80000005u) {                      /* AMD L1: 32 KiB data and instruction, 8-way, 64-byte lines */
        rhv_vcpu_write_register(vcpu, 2, 0xFF40FF40u);
        rhv_vcpu_write_register(vcpu, 5, 0xFF40FF40u);
        rhv_vcpu_write_register(vcpu, 3, (32u << 24) | (8u << 16) | (1u << 8) | 64);
        rhv_vcpu_write_register(vcpu, 4, (32u << 24) | (8u << 16) | (1u << 8) | 64);
        return;
    }
    if (g_cpuid.leaf == 0x80000006u) {                      /* AMD L3: 4 MiB per core (Windows adds the cores up: 16 MiB in all), 16-way, units of 512 KiB */
        rhv_vcpu_write_register(vcpu, 4, (8u << 18) | (8u << 12) | (1u << 8) | 64);
        return;
    }
    if (g_cpuid.leaf == 0x80000001u) {
        unsigned n = rhv_core_vcpu_count();
        if (n > 1) {                                       /* CmpLegacy: the logical-processor count in leaf 1 is valid */
            uint64_t rcx = 0;
            rhv_vcpu_read_register(vcpu, 3, &rcx);
            rhv_vcpu_write_register(vcpu, 3, rcx | 2);
        }
        return;
    }
    if (g_cpuid.leaf == 1) {
        uint64_t rcx = 0;
        rhv_vcpu_read_register(vcpu, 3, &rcx);
        if (rcx & (1u << 31))
            rhv_vcpu_write_register(vcpu, 3, rcx & ~(uint64_t)(1u << 31));
    } else if (g_cpuid.leaf >= 0x40000000u && g_cpuid.leaf <= 0x400000FFu) {
        for (unsigned reg = 2; reg <= 5; reg++)           /* RAX, RCX, RDX, RBX */
            rhv_vcpu_write_register(vcpu, reg, 0);
    }
#undef g_cpuid
}


/*
 * Windows guests under Parallels talk to the hypervisor with Hyper-V hypercalls: Parallels' monitor fills the hypercall
 * page with VMCALL (0F 01 C1), which is the Intel instruction. On an AMD CPU that is #UD, and when it runs at the very
 * start of the Windows kernel (empty IDT) the guest triple-faults and Parallels resets the VM. So #UD is intercepted
 * here (the exception bitmap is the VMCB's exception intercept, vector 6 = bit 6); if the instruction at RIP is VMCALL
 * the exit is reported to the client as VMX_REASON_VMCALL with an instruction length of 3, exactly what Intel's
 * Hypervisor.framework would have done, and any other #UD is handed back to the guest.
 */
__attribute__((unused)) static int ud_fixup(hv_vcpuid_t vcpu)
{
    uint64_t reason = 0, info = 0;
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x4402, &reason);
    if ((reason & 0xFFFF) != 0)
        return 0;
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x4404, &info);
    if (!(info & 0x80000000u) || (info & 0xFF) != 6 || ((info >> 8) & 7) != 3)
        return 0;                                          /* not a hardware #UD */
    uint64_t rip = 0, cr3 = 0;
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x681e, &rip);
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x6802, &cr3);
    uint8_t code[3];
    if (guest_read(cr3, rip, code, sizeof(code)) == 0 && code[0] == 0x0F && code[1] == 0x01 && code[2] == 0xC1) {
        rhv_vcpu_override_exit(vcpu, 18, 3);
        return 0;                                          /* the client sees a VMCALL exit */
    }
    rhv_vmx_vcpu_write_vmcs(vcpu, 0x4016, 0x80000306u);    /* give the #UD back to the guest and run on */
    return 1;
}

__attribute__((unused)) static void ud_intercept(hv_vcpuid_t vcpu)
{
    uint64_t bitmap = 0;
    if (rhv_vmx_vcpu_read_vmcs(vcpu, 0x4004, &bitmap) == 0 && !(bitmap & (1u << 6)))
        rhv_vmx_vcpu_write_vmcs(vcpu, 0x4004, bitmap | (1u << 6));
}


/*
 * Parallels' monitor (libMonitor) drops every key press until the guest has written 0 to the POST code port 0x80: that
 * write arms its PS/2 keyboard queue (the flag at image offset 0x16b8c0 + 0x4a550, set by the port handler at 0x47b30,
 * which also tells the app that the guest is up). Parallels' own firmware does that under Parallels' own hypervisor;
 * under this one it never does (it only writes a stream of values to port 0xE4), so the keyboard stays dead in the
 * guest, from the firmware shell on. We run that handler ourselves, with the value 0, the first time the guest touches
 * the keyboard controller (and again after a VM reset, which clears the flag). Only for the build whose layout this is.
 * RYZENHV_NO_KBD_ARM=1 turns it off.
 */
static void parallels_arm_keyboard(hv_vcpuid_t vcpu)
{
    static int disabled = -1;
    if (disabled < 0) {
        const char *e = getenv("RYZENHV_NO_KBD_ARM");
        disabled = e && *e && *e != '0';
    }
    if (disabled)
        return;
    uint64_t reason = 0, qual = 0;
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x4402, &reason);
    if ((reason & 0xFFFF) != 30)
        return;
    rhv_vmx_vcpu_read_vmcs(vcpu, 0x6400, &qual);
    unsigned port = (unsigned)((qual >> 16) & 0xFFFF);
    if (port != 0x60 && port != 0x64)
        return;
    /*
     * The monitor is looked up on every use, not remembered: a guest reset makes Parallels unload libMonitor and load it
     * again at another address, and calling into the old one crashed the VM process.
     */
    const uint8_t *base = NULL;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *n = _dyld_get_image_name(i);
        if (n && strstr(n, "libMonitor.dylib"))
            base = (const uint8_t *)_dyld_get_image_header(i);
    }
    static const uint8_t prologue[] = { 0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0xBB, 0x02, 0x00, 0x00, 0x00 };
    if (!base || memcmp(base + 0x47b30, prologue, sizeof(prologue)) != 0)
        return;                                        /* not the Parallels 26.5.0 libMonitor: leave it alone */
    volatile uint8_t *armed = (volatile uint8_t *)base + 0x16b8c0 + 0x4a550;
    if (*armed || base[0x8686a0])
        return;
    uint8_t zero = 0;
    int (*post_code)(uint32_t port, uint32_t size, uint64_t count, void *data) =
        (int (*)(uint32_t, uint32_t, uint64_t, void *))(base + 0x47b30);
    int rc = post_code(0x80, 1, 1, &zero);
    rhv_trace("keyboard arming: POST code 0 handed to the monitor at %p (rc %d, armed %d)", (const void *)base, rc, *armed);
}


static hv_return_t rhv_hv_vcpu_run(hv_vcpuid_t vcpu)
{
    if (g_stats < 0) {
        const char *e = getenv("RYZENHV_STATS");
        g_stats = e && *e && *e != '0' && !g_quiet;
        if (g_stats) {
            const char *path = getenv("RYZENHV_STATS_FILE");
            g_stats_stream = fopen(path && *path ? path : "/tmp/rhv-stats.txt", "a");
            if (!g_stats_stream)
                g_stats = 0;
            g_st.last = seconds();
        }
    }
    static unsigned kbdPendingPort;                      /* keyboard-controller read whose result the monitor just supplied */
    static uint64_t kbdLastStatus = ~0ull;
    if (g_trace && kbdPendingPort) {
        uint64_t rax = 0;
        rhv_vcpu_read_register(vcpu, 2 /* HV_X86_RAX */, &rax);
        if (kbdPendingPort == 0x60 || (rax & 0xFF) != kbdLastStatus)
            rhv_trace("kbd port %#x read -> %#llx", kbdPendingPort, (unsigned long long)(rax & 0xFF));
        if (kbdPendingPort == 0x64)
            kbdLastStatus = rax & 0xFF;
        kbdPendingPort = 0;
    }
    double t0 = g_stats > 0 ? seconds() : 0;
    hv_return_t r;
#ifdef RHV_HIJACK
    cpuid_hide_hypervisor(vcpu);
    for (;;) {
        r = (hv_return_t)rhv_vcpu_run(vcpu);
        if (r != HV_SUCCESS || !ud_fixup(vcpu))
            break;
    }
    cpuid_note(vcpu);
    if (g_trace) {
        spin_dump(vcpu);
        madt_dump();
    }
#else
    if (g_hyperkit)
        cpuid_hyperkit_fixup(vcpu);
    r = (hv_return_t)rhv_vcpu_run(vcpu);
    if (g_hyperkit)
        cpuid_note(vcpu);
#endif
    if (g_stats > 0) {
        g_st.inKernel += seconds() - t0;
        uint64_t reason = 0;
        rhv_vmx_vcpu_read_vmcs(vcpu, 0x4402, &reason);       /* cached: QEMU reads it right after anyway */
        uint64_t detail = 0;
        if ((reason & 0xFFFF) == 30) {                        /* port, direction */
            rhv_vmx_vcpu_read_vmcs(vcpu, 0x6400, &detail);
            stats_count(g_st.io, ((detail >> 16) & 0xFFFF) | ((detail & 8) ? 0x10000u : 0));   /* bit 16: IN */
        } else if ((reason & 0xFFFF) == 31 || (reason & 0xFFFF) == 32) {   /* MSR, bit 32: write */
            uint64_t rcx = 0;
            rhv_vcpu_read_register(vcpu, 3, &rcx);
            stats_count(g_st.msr, (rcx & 0xFFFFFFFF) | ((reason & 0xFFFF) == 32 ? 1ull << 32 : 0));
        } else if ((reason & 0xFFFF) == 48) {
            rhv_vmx_vcpu_read_vmcs(vcpu, 0x2400, &detail);
            stats_count(g_st.mmio, detail & ~0xFFFull);
        }
        stats_after_run(reason & 0xFFFF);
    }
    parallels_arm_keyboard(vcpu);
    if (g_kbd_watch > 0 && g_kbd_watch-- > 0)
        kbd_state_dump("after run");
    if (g_trace) {
        uint64_t reason = 0, rip = 0;
        if (g_trace >= 2) {                                   /* only the unusual exits */
            rhv_vmx_vcpu_read_vmcs(vcpu, 0x4402, &reason);
            unsigned basic = (unsigned)(reason & 0xFFFF);
            if (basic == 30) {                                /* keyboard controller traffic is worth a line */
                uint64_t io = 0;
                rhv_vmx_vcpu_read_vmcs(vcpu, 0x6400, &io);
                unsigned port = (unsigned)((io >> 16) & 0xFFFF);
                if (!(io & 8) && port < 0x100 && port != 0x20 && port != 0x70 && port != 0x60 && port != 0x64 && port != 0x80) {
                    uint64_t rax = 0;
                    rhv_vcpu_read_register(vcpu, 2, &rax);
                    rhv_trace("out low port %#x <- %#llx", port, (unsigned long long)(rax & 0xFFFFFFFF));
                }
                if (!(io & 8) && (port == 0xCF9 || port == 0x64)) {          /* reset requests */
                    uint64_t rax = 0;
                    rhv_vcpu_read_register(vcpu, 2, &rax);
                    if ((port == 0xCF9 && (rax & 4)) || (port == 0x64 && (rax & 0xFF) == 0xFE)) {
                        uint64_t rip = 0;
                        rhv_vmx_vcpu_read_vmcs(vcpu, 0x681e, &rip);
                        rhv_trace("guest reset request: port %#x value %#llx at rip %#llx", port, (unsigned long long)(rax & 0xFF),
                                  (unsigned long long)rip);
                        bugcheck_scan(port == 0xCF9 ? "port 0xCF9" : "keyboard controller");
                    }
                }
                if (port == 0x80 && !(io & 8)) {                  /* POST code: the monitor arms the keyboard on a 0 */
                    uint64_t rax = 0;
                    rhv_vcpu_read_register(vcpu, 2, &rax);
                    rhv_trace("post code %#llx", (unsigned long long)(rax & 0xFF));
                }
                if (port == 0x60 || port == 0x64) {
                    if (io & 8)
                        kbdPendingPort = port;
                    else {
                        uint64_t rax = 0;
                        rhv_vcpu_read_register(vcpu, 2, &rax);
                        rhv_trace("kbd port %#x out %#llx", port, (unsigned long long)(rax & 0xFF));
                    }
                }
                return r;
            }
            if (basic == 31 || basic == 32) {                 /* MSR traffic: each MSR's first few accesses, with the value on a write */
                static struct { uint32_t msr; uint8_t dir; uint8_t n; } seen[256];
                static int nseen;
                uint64_t rcx = 0, rax = 0, rdx = 0;
                rhv_vcpu_read_register(vcpu, 3, &rcx);
                int slot = -1;
                for (int i = 0; i < nseen; i++)
                    if (seen[i].msr == (uint32_t)rcx && seen[i].dir == (basic == 32))
                        slot = i;
                if (slot < 0 && nseen < 256) {
                    slot = nseen++;
                    seen[slot].msr = (uint32_t)rcx;
                    seen[slot].dir = basic == 32;
                }
                if (basic == 32) {                           /* remember where the guest put its Hyper-V pages */
                    uint64_t vlo = 0, vhi = 0;
                    rhv_vcpu_read_register(vcpu, 2, &vlo);
                    rhv_vcpu_read_register(vcpu, 4, &vhi);
                    uint64_t v = (vhi << 32) | (vlo & 0xFFFFFFFF);
                    if ((uint32_t)rcx == 0x40000021 && (v & 1)) g_hv_tscpage = v & ~0xFFFull;
                    if ((uint32_t)rcx == 0x40000083 && (v & 1)) g_hv_simp = v & ~0xFFFull;
                    if ((uint32_t)rcx == 0x40000082 && (v & 1)) g_hv_siefp = v & ~0xFFFull;
                }
                if (slot >= 0 && seen[slot].n < 3) {
                    seen[slot].n++;
                    rhv_vcpu_read_register(vcpu, 2, &rax);
                    rhv_vcpu_read_register(vcpu, 4, &rdx);
                    if (basic == 32)
                        rhv_trace("wrmsr %#x <- %#llx", (unsigned)rcx, (unsigned long long)((rdx << 32) | (rax & 0xFFFFFFFF)));
                    else
                        rhv_trace("rdmsr %#x", (unsigned)rcx);
                }
                return r;
            }
            if (basic == 48) {                               /* the interrupt controllers' registers */
                uint64_t gpa = 0, q = 0;
                rhv_vmx_vcpu_read_vmcs(vcpu, 0x2400, &gpa);
                rhv_vmx_vcpu_read_vmcs(vcpu, 0x6400, &q);
                static unsigned logged;
                if (((gpa >= 0xfee00000ull && gpa < 0xfef00000ull) || (gpa >= 0xfec00000ull && gpa < 0xfed00000ull) ||
                     (gpa >= 0xfed00000ull && gpa < 0xfed01000ull)) && logged < 300) {
                    logged++;
                    rhv_trace("mmio %#llx qual %#llx", (unsigned long long)gpa, (unsigned long long)q);
                }
                if ((gpa == 0xfee00300ull || gpa == 0xfee00310ull) && (q & 2)) {            /* ICR writes: IPIs, INIT, SIPI */
                    static struct { uint64_t reg, val; unsigned n; } icr_seen[64];
                    static int icr_n;
                    int64_t v = mmio_store_value(vcpu);
                    int slot = -1;
                    for (int i = 0; i < icr_n; i++)
                        if (icr_seen[i].reg == gpa && icr_seen[i].val == (uint64_t)v)
                            slot = i;
                    if (slot < 0 && icr_n < 64) {
                        slot = icr_n++;
                        icr_seen[slot].reg = gpa;
                        icr_seen[slot].val = (uint64_t)v;
                    }
                    if (slot >= 0 && (icr_seen[slot].n++ < 3 || (icr_seen[slot].n % 2000) == 0))
                        rhv_trace("ICR %s <- %#llx (vcpu %u, %u so far)", gpa == 0xfee00300ull ? "low" : "high",
                                  (unsigned long long)v, vcpu, icr_seen[slot].n);
                }
                if (0) {
                }
                return r;
            }
            if (basic == 10 || basic == 28)
                return r;
        }
        rhv_vmx_vcpu_read_vmcs(vcpu, 0x4402, &reason);
        if ((reason & 0xFFFF) == 2)
            triple_fault_dump(vcpu);
        rhv_vmx_vcpu_read_vmcs(vcpu, 0x681e, &rip);
        uint64_t rfl = 0, intr = 0, idt = 0, ctl = 0, qual = 0;
        rhv_vmx_vcpu_read_vmcs(vcpu, 0x6820, &rfl);
        rhv_vmx_vcpu_read_vmcs(vcpu, 0x4824, &intr);     /* guest interruptibility */
        rhv_vmx_vcpu_read_vmcs(vcpu, 0x4408, &idt);      /* IDT-vectoring info */
        rhv_vmx_vcpu_read_vmcs(vcpu, 0x4002, &ctl);      /* primary processor-based controls */
        rhv_vmx_vcpu_read_vmcs(vcpu, 0x6400, &qual);
        rhv_trace("hv_vcpu_run(%u) -> %#x exit reason %llu rip %#llx rfl %#llx intr %#llx idt %#llx ctl %#llx qual %#llx", vcpu,
                  (unsigned)r, (unsigned long long)(reason & 0xFFFF), (unsigned long long)rip, (unsigned long long)rfl,
                  (unsigned long long)intr, (unsigned long long)idt, (unsigned long long)ctl, (unsigned long long)qual);
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
    g_kicks++;
    if (g_trace)
        rhv_trace("hv_vcpu_interrupt(%u)", count);
    RESULT("hv_vcpu_interrupt", rhv_vcpu_interrupt(vcpus, count));
}
DYLD_INTERPOSE(rhv_hv_vcpu_interrupt, hv_vcpu_interrupt);

/* Anonymous memory that is fine to map as guest memory (hv_vm_map pins the pages of the process). */
static hv_return_t rhv_hv_vm_allocate(void **uvap, size_t size, hv_allocate_flags_t flags)
{
    if (!uvap || size == 0 || (size & (vm_page_size - 1)) || flags != HV_ALLOCATE_DEFAULT)
        return HV_BAD_ARGUMENT;
    vm_address_t addr = 0;
    if (vm_allocate(mach_task_self(), &addr, size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
        return HV_NO_RESOURCES;
    *uvap = (void *)addr;
    if (g_trace)
        rhv_trace("hv_vm_allocate(%#zx) -> %p", size, *uvap);
    return HV_SUCCESS;
}
DYLD_INTERPOSE(rhv_hv_vm_allocate, hv_vm_allocate);

static hv_return_t rhv_hv_vm_deallocate(void *uva, size_t size)
{
    if (!uva || size == 0 || (size & (vm_page_size - 1)))
        return HV_BAD_ARGUMENT;
    return vm_deallocate(mach_task_self(), (vm_address_t)uva, size) == KERN_SUCCESS ? HV_SUCCESS : HV_BAD_ARGUMENT;
}
DYLD_INTERPOSE(rhv_hv_vm_deallocate, hv_vm_deallocate);

static uint64_t rhv_hv_tsc_clock(void)
{
    return rhv_tsc_clock();
}
DYLD_INTERPOSE(rhv_hv_tsc_clock, hv_tsc_clock);

static hv_return_t rhv_hv_vcpu_set_tsc_relative(hv_vcpuid_t vcpu, int64_t offset)
{
    rhv_trace("hv_vcpu_set_tsc_relative(%u, %lld)", vcpu, (long long)offset);
    RESULT("hv_vcpu_set_tsc_relative", rhv_vcpu_set_tsc_relative(vcpu, offset));
}
DYLD_INTERPOSE(rhv_hv_vcpu_set_tsc_relative, hv_vcpu_set_tsc_relative);

static hv_return_t rhv_hv_vcpu_set_msr_access(hv_vcpuid_t vcpu, uint32_t msr, hv_msr_flags_t flags)
{
    RESULT("hv_vcpu_set_msr_access", rhv_vcpu_set_msr_access(vcpu, msr, (uint32_t)flags));
}
DYLD_INTERPOSE(rhv_hv_vcpu_set_msr_access, hv_vcpu_set_msr_access);

static hv_return_t rhv_hv_vmx_get_msr_info(hv_vmx_msr_info_t field, uint64_t *value)
{
    RESULT("hv_vmx_get_msr_info", rhv_vmx_get_msr_info((uint32_t)field, value));
}
DYLD_INTERPOSE(rhv_hv_vmx_get_msr_info, hv_vmx_get_msr_info);

static hv_return_t rhv_hv_vmx_vcpu_set_apic_address(hv_vcpuid_t vcpu, hv_gpaddr_t gpa)
{
    RESULT("hv_vmx_vcpu_set_apic_address", rhv_vmx_vcpu_set_apic_address(vcpu, gpa));
}
DYLD_INTERPOSE(rhv_hv_vmx_vcpu_set_apic_address, hv_vmx_vcpu_set_apic_address);

static hv_return_t rhv_hv_vmx_vcpu_set_apic_address_space(hv_vcpuid_t vcpu, hv_vm_space_t asid, hv_gpaddr_t gpa)
{
    RESULT("hv_vmx_vcpu_set_apic_address_space", rhv_vmx_vcpu_set_apic_address_space(vcpu, asid, gpa));
}
DYLD_INTERPOSE(rhv_hv_vmx_vcpu_set_apic_address_space, hv_vmx_vcpu_set_apic_address_space);

/*
 * Apps decide whether Apple's hypervisor can be used by asking the kernel (kern.hv_support), which is 0 on an AMD CPU.
 * With the shim loaded the answer is yes: the shim is the hypervisor.
 */
static int rhv_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen)
{
    if (name && strcmp(name, "kern.hv_support") == 0 && oldp && oldlenp && *oldlenp >= sizeof(int) && !newp) {
        *(int *)oldp = 1;
        *oldlenp = sizeof(int);
        if (g_trace)
            rhv_trace("sysctlbyname(kern.hv_support) -> 1");
        return 0;
    }
    int r = sysctlbyname(name, oldp, oldlenp, newp, newlen);
    if (g_trace)
        rhv_trace("sysctlbyname(%s) -> %d", name ? name : "(null)", r);
    return r;
}
DYLD_INTERPOSE(rhv_sysctlbyname, sysctlbyname);

/* The numeric form: report only (kern.hv_support is looked up by name, then by MIB, by some programs). */
static int rhv_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen)
{
    int r = sysctl(name, namelen, oldp, oldlenp, newp, newlen);
    if (g_trace && namelen >= 2)
        rhv_trace("sysctl(%d.%d%s) -> %d", name[0], name[1], namelen > 2 ? ".." : "", r);
    return r;
}
DYLD_INTERPOSE(rhv_sysctl, sysctl);

static hv_return_t rhv_hv_vcpu_read_register(hv_vcpuid_t vcpu, hv_x86_reg_t reg, uint64_t *value)
{
    if (g_trace && reg == HV_X86_TPR) {
        static unsigned n;
        int32_t k = rhv_vcpu_read_register(vcpu, (uint32_t)reg, value);
        if (n++ < 60 || (n % 500) == 0)
            rhv_trace("monitor reads TPR -> %#llx (call %u)", k == 0 && value ? (unsigned long long)*value : 0ull, n);
        return (hv_return_t)k;
    }
    RESULT("hv_vcpu_read_register", rhv_vcpu_read_register(vcpu, (uint32_t)reg, value));
}
DYLD_INTERPOSE(rhv_hv_vcpu_read_register, hv_vcpu_read_register);

static hv_return_t rhv_hv_vcpu_write_register(hv_vcpuid_t vcpu, hv_x86_reg_t reg, uint64_t value)
{
    if (g_trace && reg == HV_X86_TPR) {
        static unsigned n;
        if (n++ < 60 || (n % 500) == 0)
            rhv_trace("monitor writes TPR <- %#llx (call %u)", (unsigned long long)value, n);
    }
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
    if (g_trace && field == 0x4016 && (value & (1u << 31))) {                   /* what is injected, and at which TPR */
        static unsigned n;
        uint64_t tpr = 0, rfl = 0, intr = 0;
        rhv_vmx_vcpu_read_vmcs(vcpu, 0xF004, &tpr);
        rhv_vmx_vcpu_read_vmcs(vcpu, 0x6820, &rfl);
        rhv_vmx_vcpu_read_vmcs(vcpu, 0x4824, &intr);
        if (n++ < 400 || (n % 200) == 0)
            rhv_trace("inject %#llx at tpr %#llx rflags %#llx interruptibility %#llx", (unsigned long long)value,
                      (unsigned long long)tpr, (unsigned long long)rfl, (unsigned long long)intr);
    }
#ifdef RHV_HIJACK
    if (field == 0x4004)                 /* exception bitmap: #UD always intercepted, see ud_fixup */
        value |= 1u << 6;
#endif
    if (g_trace) {                                          /* every field the client writes, the first time and when it changes */
        static struct { uint32_t field; uint64_t value; } seen[256];
        static int nseen;
        int slot = -1;
        for (int i = 0; i < nseen; i++)
            if (seen[i].field == field)
                slot = i;
        if (slot < 0 && nseen < 256) {
            slot = nseen++;
            seen[slot].field = field;
            seen[slot].value = ~value;
        }
        if (slot >= 0 && seen[slot].value != value && field != 0x4016 && field != 0x4002) {
            static unsigned changes[256];
            if (changes[slot]++ < 8)
                rhv_trace("vmcs write %#x <- %#llx", field, (unsigned long long)value);
            seen[slot].value = value;
        }
    }
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

/*
 * RYZENHV_CONDTRACE=1 (or the file /tmp/rhv-condtrace, for a process whose environment cannot be set): which thread wakes which condition variable, and from where (RYZENHV_TRACE has to be on too).
 * Parallels hands a key press from the thread that receives it to the keyboard device's own thread through a Qt wait
 * condition; this shows whether that wake-up happens. The first wait on each condition names its thread, every signal
 * is logged with the thread that sends it and its call chain as image+offset.
 */
static int g_cond_trace = -1;

static int cond_trace_on(void)
{
    if (g_cond_trace < 0) {
        const char *e = getenv("RYZENHV_CONDTRACE");
        g_cond_trace = g_trace && ((e && *e && *e != '0') || access("/tmp/rhv-condtrace", F_OK) == 0);
    }
    return g_cond_trace;
}

static void cond_thread_name(char *buf, size_t len)
{
    buf[0] = 0;
    pthread_getname_np(pthread_self(), buf, len);
}

static void cond_frames(char *out, size_t len)
{
    void *bt[6];
    int n = backtrace(bt, 6);
    out[0] = 0;
    for (int i = 2; i < n; i++) {                       /* 0: here, 1: the interposer */
        Dl_info info;
        const char *img = "?";
        unsigned long off = (unsigned long)bt[i];
        if (dladdr(bt[i], &info) && info.dli_fname) {
            img = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
            off = (unsigned long)bt[i] - (unsigned long)info.dli_fbase;
        }
        size_t used = strlen(out);
        snprintf(out + used, len - used, " %s+%#lx", img, off);
    }
}

static void cond_note_wait(pthread_cond_t *cond)
{
    static struct { void *cond; char name[32]; } seen[512];
    static int nseen;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nseen; i++)
        if (seen[i].cond == cond) {
            pthread_mutex_unlock(&lock);
            return;
        }
    if (nseen < 512) {
        seen[nseen].cond = cond;
        cond_thread_name(seen[nseen].name, sizeof(seen[nseen].name));
        rhv_trace("cond %p is waited on by '%s'", (void *)cond, seen[nseen].name);
        nseen++;
    }
    pthread_mutex_unlock(&lock);
}

/*
 * Parallels 26.5.0 (57613) libMonitor: the PS/2 controller's state sits at image offset 0x16b8c0 + 0x4a408, and the app
 * hands scancodes over in a ring of a shared page (pointer at +0x4a4f8, head/tail at page +0x33c98/+0x33c9c). Reading it
 * from here shows whether a key press ever lands in that ring and whether the monitor takes it out again.
 */
static void kbd_state_dump(const char *label)
{
    static const uint8_t *base;
    if (!base)
        for (uint32_t i = 0; i < _dyld_image_count(); i++) {
            const char *n = _dyld_get_image_name(i);
            if (n && strstr(n, "libMonitor.dylib"))
                base = (const uint8_t *)_dyld_get_image_header(i);
        }
    if (!base)
        return;
    const uint8_t *st = base + 0x16b8c0;
    const uint8_t *page = *(const uint8_t *const *)(st + 0x4a4f8);
    if (!page) {
        rhv_trace("kbd state (%s): no shared page yet", label);
        return;
    }
    rhv_trace("kbd state (%s): armed %d (blocker %d) status %#x kbd-en %d mouse-en %d kbd-irq %d; ring %d/%d (kbd) %d/%d (mouse); internal ring %d/%d",
              label, st[0x4a550], base[0x8686a0], st[0x4a408], *(const int *)(st + 0x4a418), *(const int *)(st + 0x4a41c), *(const int *)(st + 0x4a424),
              *(const int *)(page + 0x33c98), *(const int *)(page + 0x33c9c), *(const int *)(page + 0x31628),
              *(const int *)(page + 0x3162c), st[0x4a49c], st[0x4a49d]);
}

/* runs of the vCPU still to be followed after a key press: g_kbd_watch, declared above */

static void cond_note_signal(const char *what, pthread_cond_t *cond)
{
    char name[32], frames[400];
    cond_thread_name(name, sizeof(name));
    cond_frames(frames, sizeof(frames));
    rhv_trace("%s %p by '%s' from%s", what, (void *)cond, name, frames);
    if (strcmp(name, "dev.keyboard") == 0) {
        kbd_state_dump("key signalled");
        g_kbd_watch = 6;
    }
}

static int rhv_pthread_cond_signal(pthread_cond_t *cond)
{
    if (cond_trace_on())
        cond_note_signal("cond_signal", cond);
    return pthread_cond_signal(cond);
}
DYLD_INTERPOSE(rhv_pthread_cond_signal, pthread_cond_signal);

static int rhv_pthread_cond_broadcast(pthread_cond_t *cond)
{
    if (cond_trace_on())
        cond_note_signal("cond_broadcast", cond);
    return pthread_cond_broadcast(cond);
}
DYLD_INTERPOSE(rhv_pthread_cond_broadcast, pthread_cond_broadcast);

static int rhv_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    if (cond_trace_on())
        cond_note_wait(cond);
    return pthread_cond_wait(cond, mutex);
}
DYLD_INTERPOSE(rhv_pthread_cond_wait, pthread_cond_wait);

static int rhv_pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime)
{
    if (cond_trace_on())
        cond_note_wait(cond);
    return pthread_cond_timedwait(cond, mutex, abstime);
}
DYLD_INTERPOSE(rhv_pthread_cond_timedwait, pthread_cond_timedwait);
