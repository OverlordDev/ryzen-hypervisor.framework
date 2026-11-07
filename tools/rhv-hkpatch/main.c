/*
 * rhv-hkpatch - applies the HyperKit patches to a copy on disk, so no patching in memory is needed at all.
 *
 *   rhv-hkpatch <hyperkit-copy>                (patches the file in place)
 *
 * Docker's HyperKit carries two checks an AMD CPU cannot pass (shim/rhv_patch.c: the CPUID vendor
 * check and verify_gla). Until now the shim and the kext patched them in memory after the process
 * started; with code signing enforced for the client, a modified page is rejected at page-in and the
 * process is killed ("Code Signature Invalid") -- that is why amfi_get_out_of_my_way=0x1 was needed.
 *
 * Patching the file instead and signing it afterwards (scripts/docker-desktop-shim.sh install does
 * that) makes the signature describe the patched bytes: every page validates, nothing is ever killed,
 * and AMFI stays on for the whole system. At run time both patchers then find their patterns in the
 * patched form and write nothing ("already in place").
 *
 * The patterns must occur exactly once in the __TEXT segment (the same place the in-memory patchers
 * scan), in every x86_64 slice of the file. A copy where they do not is left unchanged and the exit
 * status says so: the caller falls back to the in-memory patching, which then needs the AMFI boot-arg
 * (or the kext's per-process relax, kext/Source/HvPatch.cpp).
 *
 * The same engine as the in-memory patchers (shim/rhv_patch.c), so all three agree on what "already
 * patched" looks like. The Mach-O reading is done with byte readers, so the host tests
 * (tests/hkpatch_test.c) build on any OS.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../shim/rhv_patch.h"
#include "hkpatch.h"

#define MH_MAGIC_64   0xfeedfacfu
#define LC_SEGMENT_64 0x19u
#define FAT_MAGIC     0xcafebabeu
#define CPU_TYPE_X86_64 0x01000007u

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd64le(const uint8_t *p)
{
    return (uint64_t)rd32le(p) | (uint64_t)rd32le(p + 4) << 32;
}

static uint32_t rd32be(const uint8_t *p)
{
    return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}

/* Patches one Mach-O 64 slice at buf[off..off+size). Returns 0 when both patches are applied or were
 * already in place, 1 when the slice must not be used (pattern missing or ambiguous), -1 when the
 * slice is not a little-endian Mach-O 64 (the caller skips it). */
int hkpatch_slice(uint8_t *buf, size_t len, size_t off, size_t size, const char *label)
{
    if (off + 32 > len || size < 32)
        return -1;
    if (rd32le(buf + off) != MH_MAGIC_64)
        return -1;

    const uint32_t ncmds = rd32le(buf + off + 16), sizeofcmds = rd32le(buf + off + 20);
    if (sizeofcmds > size - 32)                      /* load commands cannot reach past the slice */
        return -1;

    /* The __TEXT segment: the in-memory patchers scan its mapped bytes (rhv_patch_image), so the file
     * patch scans exactly the same range. */
    const uint8_t *lc = buf + off + 32, *lcEnd = lc + sizeofcmds;
    const uint8_t *text = NULL;
    for (uint32_t i = 0; i < ncmds && lc + 8 <= lcEnd; i++) {
        const uint32_t cmd = rd32le(lc), cmdsize = rd32le(lc + 4);
        if (cmdsize < 8 || lc + cmdsize > lcEnd)
            break;
        if (cmd == LC_SEGMENT_64 && cmdsize >= 72 && memcmp(lc + 8, "__TEXT", 7) == 0)
            text = lc;
        lc += cmdsize;
    }
    if (!text) {
        printf("%s: no __TEXT segment, left alone\n", label);
        return 1;
    }
    const uint64_t fileoff = rd64le(text + 40), filesize = rd64le(text + 48);
    if (fileoff > size || filesize > size - fileoff) {
        printf("%s: __TEXT segment outside the file, left alone\n", label);
        return 1;
    }

    uint8_t *base = buf + off + (size_t)fileoff;
    int bad = 0;
    const struct rhv_patch *patches[2] = { &rhv_patch_hyperkit_vendor, &rhv_patch_hyperkit_gla };
    for (unsigned i = 0; i < 2; i++) {
        size_t at = 0;
        rhv_patch_result r = rhv_patch_locate(base, (size_t)filesize, patches[i], &at);
        switch (r) {
        case RHV_PATCH_APPLIED:
            rhv_patch_apply(base + at, patches[i]);
            printf("%s: %s: patched at %#llx\n", label, patches[i]->name, (unsigned long long)at);
            break;
        case RHV_PATCH_ALREADY:
            printf("%s: %s: already patched at %#llx\n", label, patches[i]->name, (unsigned long long)at);
            break;
        default:
            printf("%s: %s: %s\n", label, patches[i]->name, rhv_patch_result_name(r));
            bad = 1;
        }
    }
    return bad;
}

/* Patches every x86_64 slice of buf (thin or universal). Returns 0 only when every x86_64 slice is
 * patched (or was already) and at least one was seen. */
int hkpatch_buffer(uint8_t *buf, size_t len, const char *label)
{
    if (len >= 4 && rd32le(buf) == MH_MAGIC_64)          /* a thin x86_64 file */
        return hkpatch_slice(buf, len, 0, len, label) == 0 ? 0 : 1;

    if (len >= 8 && rd32be(buf) == FAT_MAGIC) {          /* a universal file, arch entries big-endian */
        const uint32_t n = rd32be(buf + 4);
        int seen = 0, bad = 0;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *e = buf + 8 + (size_t)i * 20;
            if ((size_t)(e - buf) + 20 > len)
                break;
            const uint32_t cputype = rd32be(e), aoff = rd32be(e + 8), asize = rd32be(e + 12);
            if (cputype != CPU_TYPE_X86_64) {
                printf("%s: skipping the slice at %#x (cputype %#x, not x86_64)\n", label, aoff, cputype);
                continue;
            }
            seen = 1;
            char sub[96];
            snprintf(sub, sizeof(sub), "%s (x86_64 slice at %#x)", label, aoff);
            if (aoff > len || asize > len - aoff || hkpatch_slice(buf, len, aoff, asize, sub) != 0)
                bad = 1;
        }
        if (!seen) {
            printf("%s: no x86_64 slice, left alone\n", label);
            return 1;
        }
        return bad ? 1 : 0;
    }

    printf("%s: not a Mach-O 64 file (thin or universal), left alone\n", label);
    return 1;
}

#ifndef RHV_HKPATCH_NO_MAIN    /* tests/hkpatch_test.c builds this file without main() */
int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: rhv-hkpatch <hyperkit-copy>   (patches the file in place)\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r+b");
    if (!f) {
        fprintf(stderr, "%s: cannot open: %s\n", argv[1], strerror(errno));
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fprintf(stderr, "%s: empty file\n", argv[1]);
        fclose(f);
        return 2;
    }
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "%s: cannot read\n", argv[1]);
        free(buf);
        fclose(f);
        return 2;
    }
    fclose(f);

    int rc = hkpatch_buffer(buf, (size_t)n, argv[1]);
    if (rc == 0) {
        f = fopen(argv[1], "wb");
        if (!f || fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            fprintf(stderr, "%s: cannot write\n", argv[1]);
            if (f)
                fclose(f);
            free(buf);
            return 2;
        }
        fclose(f);
    }
    free(buf);
    return rc;
}
#endif
