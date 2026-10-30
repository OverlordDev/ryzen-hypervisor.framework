/*
 * hijack_patchonly.c - the stand-in library for native mode (rhv_hv=1): it patches Parallels' CPUID vendor check and nothing else.
 *
 * In native mode the kext is the hypervisor provider, so prl_vm_app can use the real Hypervisor.framework: no hv_* call is
 * replaced here (compare shim/hijack.c, which does that). What is still needed is the one branch that refuses a CPU whose
 * vendor is not GenuineIntel. Parallels runs that check while it sets up its VM configuration, before its first
 * hv_vm_create, so the kext's own client patcher (kext/Source/HvPatch.cpp, which runs at hv_vm_create) comes too late.
 *
 * Like hijack.c this is built under the name of a library the process loads at launch (libPrlXmlModel.1.dylib), re-exporting
 * the real one. Its constructor patches prl_vm_app (the main image) and then libMonitor.dylib, now or whenever it is loaded
 * (Parallels unloads and loads it again at another address after a guest reset). Only the process named in
 * RHV_HIJACK_PROCESS is touched. The patches go through shim/rhv_patch.c: found by their bytes, applied only if the
 * pattern is there exactly once, a build that does not match is left alone and the log says so.
 *
 * What it logs, one line per image: ~/.ryzenhv/prl-patch.log.
 *
 * This is a stepping stone: the Parallels-specific fixes that the full shim does (keyboard, CPUID topology, #UD, APIC page,
 * TPR) are not here, and native mode may need some of them in the kext.
 */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pwd.h>
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

static void plog(const char *what, const char *result)
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
    fprintf(f, "%s pid %d vendor check in %s: %s\n", stamp, (int)getpid(), what, result);
    fclose(f);
}

static void patch_vendor_check(const struct mach_header *mh, const char *what)
{
    rhv_patch_result r = rhv_patch_image(mh, &rhv_patch_prl_vendor);
    plog(what, rhv_patch_result_name(r));
}

static void on_image(const struct mach_header *mh, intptr_t slide)
{
    (void)slide;
    if (mh == (const struct mach_header *)&_mh_dylib_header || mh->magic != MH_MAGIC_64)
        return;
    Dl_info info;
    if (!dladdr(mh, &info) || !info.dli_fname)
        return;
    if (strstr(info.dli_fname, "/libMonitor.dylib"))
        patch_vendor_check(mh, "libMonitor");
}

__attribute__((constructor)) static void rhv_patchonly_init(void)
{
    const char *prog = getprogname();
    if (!prog || strcmp(prog, RHV_HIJACK_PROCESS) != 0)
        return;
    patch_vendor_check(_dyld_get_image_header(0), "prl_vm_app");
    _dyld_register_func_for_add_image(on_image);     /* reports the images already loaded, then every new one */
}
