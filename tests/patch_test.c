/*
 * Host-side test of the signature patcher (shim/rhv_patch.c). Synthetic buffers always; the applications' real binaries too
 * when they are installed (an unpatched copy is needed: Parallels' libMonitor.dylib, prl_vm_app.orig, HyperKit's .orig).
 *   make test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../shim/rhv_patch.h"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static const struct rhv_patch je_to_jmp = { "t", "aa bb ?? 74 ??", "?? ?? ?? eb ??" };
static const struct rhv_patch nops = { "t", "0f 85 ?? ?? ?? ??  c3", "90 90 90 90 90 90  ??" };
static const struct rhv_patch jmp32 = { "t", "0f 84 ?? ?? ?? ??  c3", "90 e9 ?? ?? ?? ??  ??" };

static void test_basic(void)
{
    uint8_t buf[64];
    memset(buf, 0x11, sizeof(buf));
    memcpy(buf + 20, "\xaa\xbb\x42\x74\x5d", 5);
    size_t off = 0;

    CHECK(rhv_patch_locate(buf, sizeof(buf), &je_to_jmp, &off) == RHV_PATCH_APPLIED && off == 20);
    CHECK(rhv_patch_apply(buf + off, &je_to_jmp) == 0);
    CHECK(memcmp(buf + 20, "\xaa\xbb\x42\xeb\x5d", 5) == 0);          /* wildcards keep their bytes */
    CHECK(rhv_patch_locate(buf, sizeof(buf), &je_to_jmp, &off) == RHV_PATCH_ALREADY && off == 20);

    /* a different build: the pattern is simply not there, nothing is reported as found */
    memset(buf, 0x11, sizeof(buf));
    CHECK(rhv_patch_locate(buf, sizeof(buf), &je_to_jmp, &off) == RHV_PATCH_NOT_FOUND);
    memcpy(buf + 20, "\xaa\xbb\x42\x75\x5d", 5);                      /* jne, not je */
    CHECK(rhv_patch_locate(buf, sizeof(buf), &je_to_jmp, &off) == RHV_PATCH_NOT_FOUND);
}

static void test_ambiguous_and_edges(void)
{
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    size_t off;
    memcpy(buf + 3, "\xaa\xbb\x01\x74\x02", 5);
    memcpy(buf + 40, "\xaa\xbb\x01\x74\x02", 5);
    CHECK(rhv_patch_locate(buf, sizeof(buf), &je_to_jmp, &off) == RHV_PATCH_AMBIGUOUS);

    /* a match at the very start and at the very end of the buffer */
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "\xaa\xbb\x01\x74\x02", 5);
    CHECK(rhv_patch_locate(buf, sizeof(buf), &je_to_jmp, &off) == RHV_PATCH_APPLIED && off == 0);
    memset(buf, 0, sizeof(buf));
    memcpy(buf + sizeof(buf) - 5, "\xaa\xbb\x01\x74\x02", 5);
    CHECK(rhv_patch_locate(buf, sizeof(buf), &je_to_jmp, &off) == RHV_PATCH_APPLIED && off == sizeof(buf) - 5);
    CHECK(rhv_patch_locate(buf, 4, &je_to_jmp, &off) == RHV_PATCH_NOT_FOUND);       /* buffer shorter than the pattern */

    struct rhv_patch bad = { "t", "aa zz", "?? ??" };
    CHECK(rhv_patch_locate(buf, sizeof(buf), &bad, &off) == RHV_PATCH_BAD_PATTERN);
    struct rhv_patch mismatch = { "t", "aa bb", "?? ?? ??" };
    CHECK(rhv_patch_locate(buf, sizeof(buf), &mismatch, &off) == RHV_PATCH_BAD_PATTERN);
}

static void test_branches(void)
{
    uint8_t buf[32] = { 0 };
    size_t off;
    memcpy(buf + 4, "\x0f\x85\x11\x22\x33\x44\xc3", 7);
    CHECK(rhv_patch_locate(buf, sizeof(buf), &nops, &off) == RHV_PATCH_APPLIED);
    rhv_patch_apply(buf + off, &nops);
    CHECK(memcmp(buf + 4, "\x90\x90\x90\x90\x90\x90\xc3", 7) == 0);

    /* je rel32 -> nop; jmp rel32: the rel32 is untouched, and the jump lands where the je would have */
    memset(buf, 0, sizeof(buf));
    memcpy(buf + 4, "\x0f\x84\xdb\xfe\xff\xff\xc3", 7);
    CHECK(rhv_patch_locate(buf, sizeof(buf), &jmp32, &off) == RHV_PATCH_APPLIED);
    rhv_patch_apply(buf + off, &jmp32);
    CHECK(memcmp(buf + 4, "\x90\xe9\xdb\xfe\xff\xff\xc3", 7) == 0);
    CHECK(rhv_patch_locate(buf, sizeof(buf), &jmp32, &off) == RHV_PATCH_ALREADY);
}

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

/* expect_off: the file offset the old hard-coded patches used (the pattern starts before it), 0 = do not check */
static void real(const char *path, const struct rhv_patch *p, size_t expect_off)
{
    size_t len, off = 0;
    unsigned char *b = slurp(path, &len);
    if (!b) {
        printf("  skip %s (not installed)\n", p->name);
        return;
    }
    rhv_patch_result r = rhv_patch_locate(b, len, p, &off);
    printf("  %s: %s at %#zx\n", p->name, rhv_patch_result_name(r), off);
    CHECK(r == RHV_PATCH_APPLIED);
    if (expect_off) {
        /* the branch the old patch changed lies inside the pattern, after the part that identifies it */
        CHECK(expect_off >= off && expect_off < off + 40);
    }
    free(b);
}

int main(void)
{
    test_basic();
    test_ambiguous_and_edges();
    test_branches();

    const char *prl = "/Applications/Parallels Desktop.app/Contents/MacOS";
    char path[1024];
    snprintf(path, sizeof(path), "%s/libMonitor.dylib", prl);
    real(path, &rhv_patch_prl_vendor, 0x207b5);
    snprintf(path, sizeof(path), "%s/Parallels VM.app/Contents/MacOS/prl_vm_app.orig", prl);
    real(path, &rhv_patch_prl_vendor, 0xc1c61);
    const char *hk = "/Applications/Docker.app/Contents/Resources/bin/com.docker.hyperkit.orig";
    real(hk, &rhv_patch_hyperkit_vendor, 0x227b56);
    real(hk, &rhv_patch_hyperkit_gla, 0x20af38);

    printf("patch_test: PASS\n");
    return 0;
}
