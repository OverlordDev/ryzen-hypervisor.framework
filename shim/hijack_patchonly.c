/*
 * hijack_patchonly.c - the stand-in library for native mode (rhv_hv=1): it patches Parallels' CPUID vendor check and arms its keyboard.
 *
 * In native mode the kext is the hypervisor provider, so prl_vm_app uses the real Hypervisor.framework: only two hv_* calls (hv_vcpu_run and hv_vcpu_run_until) get a wrapper here
 * (compare shim/hijack.c, which replaces them all). Two things are still needed inside the process:
 *
 * 1. The branch that refuses a CPU whose vendor is not GenuineIntel. Parallels runs that check while it sets up its VM configuration, before its
 *    first hv_vm_create, so the kext's own client patcher (kext/Source/HvPatch.cpp, which runs at hv_vm_create) comes too late.
 * 2. The keyboard. libMonitor drops every key press until the guest writes 0 to the POST code port 0x80 (Parallels' own firmware does that under
 *    Parallels' own hypervisor, here it never does). So hv_vcpu_run, and only that function, is replaced in libMonitor's imports: after each run
 *    it looks at the exit, and on the first guest access to the keyboard controller (ports 0x60/0x64) it calls the monitor's own port handler with
 *    0 (the same as parallels_arm_keyboard in shim/hv_shim.c). Only for the Parallels build whose layout this is; any other build is left alone.
 *
 * Like hijack.c this is built under the name of a library the process loads at launch (libPrlXmlModel.1.dylib), re-exporting the real one. Its
 * constructor patches prl_vm_app (the main image) and then libMonitor.dylib, now or whenever it is loaded (Parallels unloads and loads it again at
 * another address after a guest reset). Only the process named in RHV_HIJACK_PROCESS is touched. The patches go through shim/rhv_patch.c: found by
 * their bytes, applied only if the pattern is there exactly once.
 *
 * What it logs, one line per event: ~/.ryzenhv/prl-patch.log. RYZENHV_NO_KBD_ARM=1 turns the keyboard part off.
 */
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rhv_patch.h"

#ifndef RHV_HIJACK_PROCESS
#define RHV_HIJACK_PROCESS "prl_vm_app"
#endif

extern const struct mach_header_64 _mh_dylib_header;

/* Hypervisor.framework's functions, by their C types (hv_return_t is int, hv_vcpuid_t is unsigned int), found at run time. */
typedef int (*run_fn)(unsigned vcpu);
typedef int (*run_until_fn)(unsigned vcpu, uint64_t deadline);
typedef int (*read_vmcs_fn)(unsigned vcpu, unsigned field, uint64_t *value);
static run_fn g_real_run;
static run_until_fn g_real_run_until;
static read_vmcs_fn g_read_vmcs;

static void plog(const char *line)
{
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
    }
    if (!home)
        return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/.ryzenhv/prl-patch.log", home);
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    time_t now = time(NULL);
    struct tm tm;
    char stamp[32] = "";
    if (localtime_r(&now, &tm))
        strftime(stamp, sizeof(stamp), "%m-%d %H:%M:%S", &tm);
    fprintf(f, "%s pid %d %s\n", stamp, (int)getpid(), line);
    fclose(f);
}

static void patch_vendor_check(const struct mach_header *mh, const char *what)
{
    rhv_patch_result r = rhv_patch_image_any(mh, rhv_patch_prl_vendor, rhv_patch_prl_vendor_count);
    char msg[160];
    snprintf(msg, sizeof(msg), "vendor check in %s: %s", what, rhv_patch_result_name(r));
    plog(msg);
}

/*
 * The monitor drops keys until a flag in its saved state is set, which only its port-0x80 handler (the POST code value 0) does. Run that handler
 * ourselves, with 0, the first time the guest touches the keyboard controller (and again after a VM reset, which clears the flag). The handler and
 * the two flags are found in the monitor's code by their bytes (rhv_find_monitor_kbd in shim/rhv_patch.c), not by address, and only if the code
 * looks as expected. The monitor is looked up on every use, not remembered: after a reset it is unloaded and loaded again at another address.
 */
static unsigned long g_runs;                     /* hv_vcpu_run returns since libMonitor was loaded (again), for the log only */
static uint64_t g_load_ns, g_last_check_ns;      /* when libMonitor was loaded (again), and when the safety net last looked */

/*
 * `periodic`: not the first-access trigger but a safety net. A VM restored from a saved state (suspend/resume) never touches the keyboard controller
 * again, because the guest initialised it in its first life, while the monitor's flag starts at 0; so every few thousand runs, once the guest has
 * had time to start, the flag is looked at and set if it is clear.
 */
/* Where the handler and the flags are in the monitor loaded at `base`; the result of the search is logged once per load. */
static int monitor_kbd(const uint8_t *base, struct rhv_monitor_kbd *kbd)
{
    int fresh = 0;
    rhv_patch_result r = rhv_find_monitor_kbd_cached((const struct mach_header *)base, kbd, &fresh);
    if (fresh) {
        char msg[160];
        if (r == RHV_PATCH_APPLIED)
            snprintf(msg, sizeof(msg), "keyboard: handler at +%#zx, flags at +%#zx and +%#zx (found by their bytes)", kbd->handler, kbd->armed, kbd->second);
        else
            snprintf(msg, sizeof(msg), "keyboard: POST code handler: %s", rhv_patch_result_name(r));
        plog(msg);
    }
    return r == RHV_PATCH_APPLIED;
}

static void arm_keyboard(unsigned vcpu, int periodic)
{
    static int disabled = -1;
    if (disabled < 0) {
        const char *e = getenv("RYZENHV_NO_KBD_ARM");
        disabled = e && *e && *e != '0';
    }
    if (disabled || !g_read_vmcs)
        return;
    if (!periodic) {
        uint64_t reason = 0, qual = 0;
        if (g_read_vmcs(vcpu, 0x4402, &reason) != 0 || (reason & 0xFFFF) != 30)
            return;
        if (g_read_vmcs(vcpu, 0x6400, &qual) != 0)
            return;
        unsigned port = (unsigned)((qual >> 16) & 0xFFFF);
        if (port != 0x60 && port != 0x64)
            return;
    }

    const uint8_t *base = NULL;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *n = _dyld_get_image_name(i);
        if (n && strstr(n, "libMonitor.dylib"))
            base = (const uint8_t *)_dyld_get_image_header(i);
    }
    struct rhv_monitor_kbd kbd;
    if (!base || !monitor_kbd(base, &kbd))
        return;                                        /* not a monitor this was written for: leave it alone */
    volatile uint8_t *armed = (volatile uint8_t *)base + kbd.armed;
    volatile uint8_t *second = (volatile uint8_t *)base + kbd.second;
    if (periodic) {
        static int shown;
        if (shown++ < 3) {
            char msg[160];
            snprintf(msg, sizeof(msg), "keyboard: periodic check after %lu runs: armed flag %d, second flag %d", g_runs, *armed, *second);
            plog(msg);
        }
    }
    /* Both flags: the first is part of the monitor's saved state, so a restored VM has it set while this process's second one (a global of the library) is
     * still clear, and the keys stay dropped until the handler has run in this process. */
    if (*armed && *second)
        return;
    static volatile int busy;                    /* several vCPU threads may get here at once: one calls the handler */
    if (__sync_lock_test_and_set(&busy, 1))
        return;
    uint8_t zero = 0;
    int (*post_code)(uint32_t port, uint32_t size, uint64_t count, void *data) =
        (int (*)(uint32_t, uint32_t, uint64_t, void *))(base + kbd.handler);
    int rc = post_code(0x80, 1, 1, &zero);
    __sync_lock_release(&busy);

    static int logged;
    if (logged++ < 8) {
        char msg[120];
        snprintf(msg, sizeof(msg), "keyboard: POST code 0 handed to the monitor (%s, rc %d, armed %d)", periodic ? "periodic" : "first access", rc, *armed);
        plog(msg);
    }
}

static void after_run(unsigned vcpu, const char *which);

/* What libMonitor calls instead of hv_vcpu_run and hv_vcpu_run_until: the real one, then a look at the exit. */
static int hooked_vcpu_run(unsigned vcpu)
{
    int r = g_real_run(vcpu);
    if (r == 0)
        after_run(vcpu, "hv_vcpu_run");
    return r;
}

static int hooked_vcpu_run_until(unsigned vcpu, uint64_t deadline)
{
    int r = g_real_run_until(vcpu, deadline);
    if (r == 0)
        after_run(vcpu, "hv_vcpu_run_until");
    return r;
}

static void after_run(unsigned vcpu, const char *which)
{
    static volatile int seen_run, seen_until;
    volatile int *seen = which[11] == '_' && which[12] == 'u' ? &seen_until : &seen_run;
    if (!*seen) {
        *seen = 1;
        char msg[100];
        snprintf(msg, sizeof(msg), "keyboard: %s is called by the monitor (vcpu %u)", which, vcpu);
        plog(msg);
    }
    {
        __sync_add_and_fetch(&g_runs, 1);
        /* The safety net: once the guest has had 3 s, and then once a second, look at the flag (an idle guest makes only a few hundred runs a second). */
        uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        int periodic = g_load_ns && now - g_load_ns > 3000000000ull && now - g_last_check_ns >= 1000000000ull;
        if (periodic)
            g_last_check_ns = now;
        arm_keyboard(vcpu, periodic);
    }
}

/*
 * Points libMonitor's import pointers for hv_vcpu_run (lazy and non-lazy) at hooked_vcpu_run. The technique is Facebook's fishhook, the same as
 * hijack.c: dyld_dynamic_interpose does not reach images that are already loaded.
 */
static void rewrite_section(const struct section_64 *sect, intptr_t slide, const struct nlist_64 *symtab, const char *strtab,
                            const uint32_t *indirect, int is_const)
{
    const uint32_t *indices = indirect + sect->reserved1;
    void **ptrs = (void **)(sect->addr + slide);
    size_t count = (size_t)(sect->size / sizeof(void *));
    for (size_t i = 0; i < count; i++) {
        uint32_t idx = indices[i];
        if (idx == INDIRECT_SYMBOL_ABS || idx == INDIRECT_SYMBOL_LOCAL || idx == (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS))
            continue;
        const char *name = strtab + symtab[idx].n_un.n_strx;
        if (name[0] == '_')
            name++;
        void *replacement = NULL;
        if (strcmp(name, "hv_vcpu_run") == 0 && g_real_run)
            replacement = (void *)hooked_vcpu_run;
        else if (strcmp(name, "hv_vcpu_run_until") == 0 && g_real_run_until)
            replacement = (void *)hooked_vcpu_run_until;
        if (!replacement || ptrs[i] == replacement)
            continue;
        if (is_const) {                       /* __DATA_CONST is read-only once dyld is done with it */
            vm_address_t page = (vm_address_t)&ptrs[i] & ~(vm_address_t)(vm_page_size - 1);
            if (vm_protect(mach_task_self(), page, vm_page_size, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS)
                continue;
        }
        ptrs[i] = replacement;
    }
}

static int hook_vcpu_run_in(const struct mach_header *mh, intptr_t slide)
{
    if (!g_real_run || !g_real_run_until || mh->magic != MH_MAGIC_64)
        return -1;
    const struct mach_header_64 *h = (const struct mach_header_64 *)mh;
    const struct symtab_command *symtab_cmd = NULL;
    const struct dysymtab_command *dysymtab_cmd = NULL;
    const struct segment_command_64 *linkedit = NULL;
    const struct load_command *lc = (const struct load_command *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd == LC_SYMTAB)
            symtab_cmd = (const struct symtab_command *)lc;
        else if (lc->cmd == LC_DYSYMTAB)
            dysymtab_cmd = (const struct dysymtab_command *)lc;
        else if (lc->cmd == LC_SEGMENT_64 && strcmp(((const struct segment_command_64 *)lc)->segname, SEG_LINKEDIT) == 0)
            linkedit = (const struct segment_command_64 *)lc;
    }
    if (!symtab_cmd || !dysymtab_cmd || !linkedit || !dysymtab_cmd->nindirectsyms)
        return -1;

    uintptr_t linkedit_base = (uintptr_t)slide + linkedit->vmaddr - linkedit->fileoff;
    const struct nlist_64 *symtab = (const struct nlist_64 *)(linkedit_base + symtab_cmd->symoff);
    const char *strtab = (const char *)(linkedit_base + symtab_cmd->stroff);
    const uint32_t *indirect = (const uint32_t *)(linkedit_base + dysymtab_cmd->indirectsymoff);

    lc = (const struct load_command *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64)
            continue;
        const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
        if (strcmp(seg->segname, SEG_DATA) != 0 && strcmp(seg->segname, "__DATA_CONST") != 0 &&
            strcmp(seg->segname, "__AUTH_CONST") != 0 && strcmp(seg->segname, "__AUTH") != 0)
            continue;
        const struct section_64 *sect = (const struct section_64 *)(seg + 1);
        for (uint32_t j = 0; j < seg->nsects; j++, sect++) {
            uint32_t type = sect->flags & SECTION_TYPE;
            if (type == S_LAZY_SYMBOL_POINTERS || type == S_NON_LAZY_SYMBOL_POINTERS)
                rewrite_section(sect, slide, symtab, strtab, indirect, strcmp(seg->segname, SEG_DATA) != 0 /* *_CONST */);
        }
    }
    return 0;
}

static void on_image(const struct mach_header *mh, intptr_t slide)
{
    if (mh == (const struct mach_header *)&_mh_dylib_header || mh->magic != MH_MAGIC_64)
        return;
    Dl_info info;
    if (!dladdr(mh, &info) || !info.dli_fname)
        return;
    if (strstr(info.dli_fname, "/libMonitor.dylib")) {
        patch_vendor_check(mh, "libMonitor");
        g_runs = 0;
        g_last_check_ns = 0;
        g_load_ns = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        int rc = hook_vcpu_run_in(mh, slide);
        plog(rc == 0 ? "hv_vcpu_run and hv_vcpu_run_until hooked in libMonitor (keyboard arming)" : "hv_vcpu_run* NOT hooked in libMonitor");
    }
}

__attribute__((constructor)) static void rhv_patchonly_init(void)
{
    const char *prog = getprogname();
    if (!prog || strcmp(prog, RHV_HIJACK_PROCESS) != 0)
        return;
    /* hv_vcpu_run and the VMCS reader live in Hypervisor.framework (the shared cache); this library does not define them, so this finds the real ones. */
    g_real_run = (run_fn)dlsym(RTLD_DEFAULT, "hv_vcpu_run");
    g_real_run_until = (run_until_fn)dlsym(RTLD_DEFAULT, "hv_vcpu_run_until");
    g_read_vmcs = (read_vmcs_fn)dlsym(RTLD_DEFAULT, "hv_vmx_vcpu_read_vmcs");
    patch_vendor_check(_dyld_get_image_header(0), "prl_vm_app");
    _dyld_register_func_for_add_image(on_image);     /* reports the images already loaded, then every new one */
}
