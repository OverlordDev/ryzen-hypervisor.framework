/*
 * rhv_patch.c - see rhv_patch.h.
 */
#include "rhv_patch.h"

#include <string.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach-o/loader.h>
#endif

/*
 * Parallels 26.5.0 (57613), prl_vm_app and libMonitor.dylib:
 *     xor rdx, "ntel" ; or rdx, rax ; je <Intel path>      -> jmp
 */
const struct rhv_patch rhv_patch_prl_vendor = {
    "Parallels CPUID vendor check",
    "48 81 f2 6e 74 65 6c  48 09 c2  74 ??",
    "?? ?? ?? ?? ?? ?? ??  ?? ?? ??  eb ??",
};

/*
 * HyperKit v0.20210107-12-gadc4ea (Docker Desktop 4.97.0):
 *     movabs rcx, "neIntel\0" ; xor rcx, [rbp-x] ; or rcx, rax ; jne <not Intel>    -> six nops
 */
const struct rhv_patch rhv_patch_hyperkit_vendor = {
    "HyperKit CPUID vendor check",
    "48 b9 6e 65 49 6e 74 65 6c 00  48 33 4d ??  48 09 c1  0f 85 ?? ?? ?? ??",
    "?? ?? ?? ?? ?? ?? ?? ?? ?? ??  ?? ?? ?? ??  ?? ?? ??  90 90 90 90 90 90",
};

/*
 * HyperKit verify_gla: after comparing the guest linear address with the one it decoded, `je ok`. AMD does not report that
 * address, so the branch is always taken. `je rel32` (6 bytes) becomes `nop ; jmp rel32`: the same target, no arithmetic.
 */
const struct rhv_patch rhv_patch_hyperkit_gla = {
    "HyperKit verify_gla",
    "48 3b 9d ?? ?? ?? ??  4c 8b 7d ??  0f 84 ?? ?? ?? ??  48 83 ec 08  48 8d 3d",
    "?? ?? ?? ?? ?? ?? ??  ?? ?? ?? ??  90 e9 ?? ?? ?? ??  ?? ?? ?? ??  ?? ?? ??",
};

const char *rhv_patch_result_name(rhv_patch_result r)
{
    switch (r) {
    case RHV_PATCH_APPLIED:     return "patched";
    case RHV_PATCH_ALREADY:     return "already patched";
    case RHV_PATCH_NOT_FOUND:   return "pattern not found (another build of the application), left alone";
    case RHV_PATCH_AMBIGUOUS:   return "pattern found more than once, left alone";
    case RHV_PATCH_BAD_PATTERN: return "bad pattern";
    case RHV_PATCH_NO_ACCESS:   return "could not make the code writable";
    }
    return "?";
}

#define MAXPAT 64

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "48 ?? 0f" -> bytes + wildcard flags. Returns the length, or -1. */
static int parse(const char *s, uint8_t *val, uint8_t *wild)
{
    int n = 0;
    while (*s) {
        if (*s == ' ') { s++; continue; }
        if (n >= MAXPAT) return -1;
        if (s[0] == '?' && s[1] == '?') {
            val[n] = 0; wild[n++] = 1; s += 2;
        } else {
            int hi = hexval(s[0]), lo = hi < 0 ? -1 : hexval(s[1]);
            if (hi < 0 || lo < 0) return -1;
            val[n] = (uint8_t)(hi << 4 | lo); wild[n++] = 0; s += 2;
        }
    }
    return n;
}

static size_t count_matches(const uint8_t *buf, size_t len, const uint8_t *val, const uint8_t *wild, int n, size_t *first)
{
    size_t found = 0;
    if (len < (size_t)n)
        return 0;
    /* Every pattern here starts with a fixed byte: memchr skips most of a 30 MB code segment quickly. */
    int lead = 0;
    while (lead < n && wild[lead]) lead++;
    if (lead == n)
        return 0;
    const uint8_t *p = buf + lead, *end = buf + len - (n - lead) + 1;
    while (p < end && (p = memchr(p, val[lead], (size_t)(end - p))) != NULL) {
        const uint8_t *start = p - lead;
        if (start >= buf) {
            int ok = 1;
            for (int i = 0; i < n && ok; i++)
                ok = wild[i] || start[i] == val[i];
            if (ok) {
                if (!found) *first = (size_t)(start - buf);
                found++;
            }
        }
        p++;
    }
    return found;
}

rhv_patch_result rhv_patch_locate(const uint8_t *buf, size_t len, const struct rhv_patch *p, size_t *offset)
{
    uint8_t fv[MAXPAT], fw[MAXPAT], pv[MAXPAT], pw[MAXPAT];
    int n = parse(p->find, fv, fw), m = parse(p->patched, pv, pw);
    if (n <= 0 || n != m)
        return RHV_PATCH_BAD_PATTERN;

    /* The patched form is the original pattern with the changed bytes replaced. */
    uint8_t av[MAXPAT], aw[MAXPAT];
    for (int i = 0; i < n; i++) {
        av[i] = pw[i] ? fv[i] : pv[i];
        aw[i] = pw[i] ? fw[i] : 0;
    }

    size_t at_orig = 0, at_done = 0;
    size_t orig = count_matches(buf, len, fv, fw, n, &at_orig);
    size_t done = count_matches(buf, len, av, aw, n, &at_done);
    if (orig + done > 1)
        return RHV_PATCH_AMBIGUOUS;
    if (orig == 1) { *offset = at_orig; return RHV_PATCH_APPLIED; }
    if (done == 1) { *offset = at_done; return RHV_PATCH_ALREADY; }
    return RHV_PATCH_NOT_FOUND;
}

int rhv_patch_apply(uint8_t *at, const struct rhv_patch *p)
{
    uint8_t pv[MAXPAT], pw[MAXPAT];
    int m = parse(p->patched, pv, pw);
    if (m <= 0)
        return -1;
    for (int i = 0; i < m; i++)
        if (!pw[i])
            at[i] = pv[i];
    return 0;
}

#ifdef __APPLE__
rhv_patch_result rhv_patch_image(const struct mach_header *mh, const struct rhv_patch *p)
{
    if (!mh || mh->magic != MH_MAGIC_64)
        return RHV_PATCH_NOT_FOUND;
    const struct mach_header_64 *h = (const struct mach_header_64 *)mh;
    const struct segment_command_64 *text = NULL;
    const struct load_command *lc = (const struct load_command *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize))
        if (lc->cmd == LC_SEGMENT_64 && strcmp(((const struct segment_command_64 *)lc)->segname, SEG_TEXT) == 0)
            text = (const struct segment_command_64 *)lc;
    if (!text)
        return RHV_PATCH_NOT_FOUND;

    /* The image is mapped from its __TEXT segment's vmaddr; the header sits at the start of that segment. */
    uint8_t *base = (uint8_t *)mh;
    size_t offset = 0;
    rhv_patch_result r = rhv_patch_locate(base, (size_t)text->vmsize, p, &offset);
    if (r != RHV_PATCH_APPLIED)
        return r;

    vm_address_t page = (vm_address_t)(base + offset) & ~(vm_address_t)(vm_page_size - 1);
    vm_size_t span = (vm_address_t)(base + offset + 64) - page;      /* a pattern may straddle a page */
    span = (span + vm_page_size - 1) & ~(vm_size_t)(vm_page_size - 1);
    if (vm_protect(mach_task_self(), page, span, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE | VM_PROT_COPY) != KERN_SUCCESS)
        return RHV_PATCH_NO_ACCESS;
    rhv_patch_apply(base + offset, p);
    vm_protect(mach_task_self(), page, span, 0, VM_PROT_READ | VM_PROT_EXECUTE);
    return RHV_PATCH_APPLIED;
}
#endif
