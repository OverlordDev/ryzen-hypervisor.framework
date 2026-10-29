/*
 * rhv_patch.h - patches of the applications' own code, found by the bytes around them instead of by an address.
 *
 * Parallels and HyperKit check the CPUID vendor (GenuineIntel), and HyperKit compares the guest linear address of an EPT
 * violation, which AMD does not report. Those are single branches in the applications. A patch here is a pattern of
 * bytes (with `??` for the bytes that change from build to build: displacements, stack offsets), the place in it to
 * change, and what the pattern looks like afterwards. The code is searched in memory, the patch is applied only if the
 * pattern is found exactly once, and a build where it is not found is left alone and says so: no addresses to update
 * for every release, and no way to patch the wrong place.
 *
 * rhv_patch_locate / rhv_patch_apply are plain C and are tested on the host (tests/patch_test.c).
 */
#ifndef RHV_PATCH_H
#define RHV_PATCH_H

#include <stddef.h>
#include <stdint.h>

struct rhv_patch {
    const char *name;
    const char *find;      /* hex bytes, "??" matches any byte, spaces are ignored */
    const char *patched;   /* the same bytes after patching, same length; "??" keeps the byte as it is */
};

typedef enum {
    RHV_PATCH_APPLIED = 0,   /* found once and changed */
    RHV_PATCH_ALREADY,       /* found once in its patched form (a file patched by an older install) */
    RHV_PATCH_NOT_FOUND,     /* neither form is in the code: another build of the application */
    RHV_PATCH_AMBIGUOUS,     /* more than one place matches, nothing is touched */
    RHV_PATCH_BAD_PATTERN,
    RHV_PATCH_NO_ACCESS      /* could not make the code writable */
} rhv_patch_result;

const char *rhv_patch_result_name(rhv_patch_result r);

/* Looks for the patch in buf[0..len). On RHV_PATCH_APPLIED / RHV_PATCH_ALREADY, *offset is where the pattern starts. */
rhv_patch_result rhv_patch_locate(const uint8_t *buf, size_t len, const struct rhv_patch *p, size_t *offset);

/* Writes the patched bytes over the pattern at buf + offset (the "??" bytes stay). */
int rhv_patch_apply(uint8_t *at, const struct rhv_patch *p);

/* Patches the code of a loaded image (the __TEXT segment of `mh`), in memory. Returns what rhv_patch_locate found. */
struct mach_header;
rhv_patch_result rhv_patch_image(const struct mach_header *mh, const struct rhv_patch *p);

/* The patches themselves. */
extern const struct rhv_patch rhv_patch_prl_vendor;        /* prl_vm_app and libMonitor.dylib: CPUID vendor check */
extern const struct rhv_patch rhv_patch_hyperkit_vendor;
extern const struct rhv_patch rhv_patch_hyperkit_gla;

#endif
