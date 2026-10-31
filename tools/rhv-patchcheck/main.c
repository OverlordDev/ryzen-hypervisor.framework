/*
 * rhv-patchcheck - does the installed Parallels have the code RyzenHV patches? Reads the files, patches nothing.
 *
 *   rhv-patchcheck <prl_vm_app> <libMonitor.dylib>
 *
 * Looks for the three things the stand-in library needs, by the bytes (shim/rhv_patch.c): the CPUID vendor check in prl_vm_app, the same check in
 * libMonitor, and libMonitor's POST-code handler with its two flags (the keyboard). Exit status 0: all three found exactly once.
 */
#include <stdio.h>
#include <stdlib.h>

#include "../../shim/rhv_patch.h"

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = b ? (size_t)n : 0;
    return b;
}

static int check_vendor(const char *label, const char *path)
{
    size_t len, off = 0;
    unsigned which = 0;
    unsigned char *b = slurp(path, &len);
    if (!b) {
        printf("  [bad]  %s: cannot read %s\n", label, path);
        return 1;
    }
    rhv_patch_result r = rhv_patch_locate_any(b, len, rhv_patch_prl_vendor, rhv_patch_prl_vendor_count, &off, &which);
    free(b);
    if (r == RHV_PATCH_APPLIED || r == RHV_PATCH_ALREADY) {
        printf("  [ok]   %s: CPUID vendor check found (%s)\n", label, rhv_patch_prl_vendor[which].name);
        return 0;
    }
    printf("  [bad]  %s: CPUID vendor check: %s\n", label, rhv_patch_result_name(r));
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: rhv-patchcheck <prl_vm_app> <libMonitor.dylib>\n");
        return 2;
    }
    int bad = 0;
    bad += check_vendor("prl_vm_app", argv[1]);
    bad += check_vendor("libMonitor", argv[2]);
    size_t len;
    unsigned char *b = slurp(argv[2], &len);
    if (!b) {
        printf("  [bad]  libMonitor: cannot read %s\n", argv[2]);
        return 1;
    }
    struct rhv_monitor_kbd k;
    rhv_patch_result r = rhv_find_monitor_kbd(b, len, &k);
    free(b);
    if (r == RHV_PATCH_APPLIED) {
        printf("  [ok]   libMonitor: keyboard handler found (+%#zx, flags +%#zx and +%#zx)\n", k.handler, k.armed, k.second);
    } else {
        printf("  [bad]  libMonitor: keyboard handler: %s\n", rhv_patch_result_name(r));
        bad++;
    }
    return bad ? 1 : 0;
}
