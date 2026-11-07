/*
 * Host-side test of the on-disk HyperKit patcher (tools/rhv-hkpatch/main.c). Synthetic Mach-O images only: a thin x86_64
 * file, a universal file with an x86_64 and an arm64 slice, the already-patched forms, an ambiguous image, and images the
 * tool must refuse. The real binary is checked by tests/patch_test.c (the patterns) and on the Mac by the install.
 *   cc -DRHV_HKPATCH_NO_MAIN -o hkpatch_test tests/hkpatch_test.c tools/rhv-hkpatch/main.c shim/rhv_patch.c && ./hkpatch_test
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../shim/rhv_patch.h"
#include "../tools/rhv-hkpatch/hkpatch.h"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

/* The bytes of a find pattern ("48 b9 ..." with "??"), parsed locally so the synthetic images carry the real patterns. */
static int parse_hex(const char *s, uint8_t *out, uint8_t *wild, int max)
{
    int n = 0;
    while (*s) {
        if (*s == ' ') { s++; continue; }
        if (n >= max) return -1;
        if (s[0] == '?' && s[1] == '?') { out[n] = 0; wild[n++] = 1; s += 2; continue; }
        static const char *h = "0123456789abcdef";
        const char *a = strchr(h, (char)tolower((unsigned char)s[0]));
        const char *b = strchr(h, (char)tolower((unsigned char)s[1]));
        if (!a || !b) return -1;
        out[n] = (uint8_t)((a - h) << 4 | (b - h)); wild[n++] = 0; s += 2;
    }
    return n;
}

static int embed(const uint8_t *v, const uint8_t *w, int n, uint8_t *buf, size_t len, size_t at)
{
    if (at + (size_t)n > len)
        return -1;
    memcpy(buf + at, v, (size_t)n);
    for (int i = 0; i < n; i++)
        if (w[i])
            buf[at + i] = (uint8_t)i;          /* a filler that differs per byte; the tool must keep it */
    return n;
}

static void put32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put32be(uint8_t *p, uint32_t v)
{
    p[3] = (uint8_t)v; p[2] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v >> 16); p[0] = (uint8_t)(v >> 24);
}

static void put64le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* A little-endian Mach-O 64 image with one __TEXT segment covering the whole buffer; the two HyperKit
 * patterns are placed after the load commands. Returns the size used. */
static size_t make_thin(uint8_t *buf, size_t len, uint32_t magic, uint32_t cputype)
{
    CHECK(len >= 0x400);
    memset(buf, 0xcc, len);
    put32le(buf, magic);                     /* mach_header_64 */
    put32le(buf + 4, cputype);
    put32le(buf + 8, 3);                     /* cpusubtype */
    put32le(buf + 12, 2);                    /* filetype MH_EXECUTE */
    put32le(buf + 16, 1);                    /* ncmds */
    put32le(buf + 20, 72);                   /* sizeofcmds */
    put32le(buf + 24, 0);                    /* flags */
    put32le(buf + 28, 0);                    /* reserved */
    put32le(buf + 32, 0x19);                 /* LC_SEGMENT_64 */
    put32le(buf + 36, 72);                   /* cmdsize */
    memcpy(buf + 40, "__TEXT", 7);           /* segname */
    put64le(buf + 56, 0);                    /* vmaddr */
    put64le(buf + 64, len);                  /* vmsize */
    put64le(buf + 72, 0);                    /* fileoff */
    put64le(buf + 80, len);                  /* filesize */
    put32le(buf + 88, 5);                    /* maxprot r-x */
    put32le(buf + 92, 5);                    /* initprot */
    put32le(buf + 96, 0);                    /* nsects */
    put32le(buf + 100, 0);                   /* flags */

    uint8_t v[64], w[64];
    int n = parse_hex(rhv_patch_hyperkit_vendor.find, v, w, sizeof(v));
    CHECK(n > 0);
    CHECK(embed(v, w, n, buf, len, 0x200) == n);
    n = parse_hex(rhv_patch_hyperkit_gla.find, v, w, sizeof(v));
    CHECK(n > 0);
    CHECK(embed(v, w, n, buf, len, 0x300) == n);
    return len;
}

/* The first byte of each patched branch inside the embedded patterns: the `0f 85` of the vendor jne sits at
 * pattern offset 17, the `0f 84` of the verify_gla je at offset 11. The wildcard bytes are filled with their
 * index by embed(), so the kept rel32 bytes are 13, 14, 15, 16. */
#define VENDOR_JNE_AT  (0x200 + 17)
#define GLA_JE_AT      (0x300 + 11)

static void test_thin(void)
{
    static uint8_t buf[0x1000];
    make_thin(buf, sizeof(buf), 0xfeedfacfU /* MH_MAGIC_64 */, 0x01000007U /* CPU_TYPE_X86_64 */);
    CHECK(hkpatch_buffer(buf, sizeof(buf), "thin") == 0);
    CHECK(buf[VENDOR_JNE_AT] == 0x90 && buf[VENDOR_JNE_AT + 1] == 0x90);
    for (int i = 2; i < 6; i++)
        CHECK(buf[VENDOR_JNE_AT + i] == 0x90);              /* rel32 -> nops */
    CHECK(buf[GLA_JE_AT] == 0x90 && buf[GLA_JE_AT + 1] == 0xe9);   /* je rel32 -> nop; jmp rel32 */
    CHECK(buf[GLA_JE_AT + 2] == 13 && buf[GLA_JE_AT + 3] == 14);   /* the rel32 itself stays */
    /* a wildcard byte that the tool must have kept */
    CHECK(buf[0x200 + 13] == 13);

    /* already patched: reported, nothing changed, still success */
    static uint8_t copy[0x1000];
    memcpy(copy, buf, sizeof(copy));
    CHECK(hkpatch_buffer(copy, sizeof(copy), "thin-already") == 0);
    CHECK(memcmp(copy, buf, sizeof(copy)) == 0);
}

static void test_refusals(void)
{
    static uint8_t buf[0x1000];
    /* two copies of the vendor pattern: ambiguous, nothing written */
    make_thin(buf, sizeof(buf), 0xfeedfacfU, 0x01000007U);
    uint8_t v[64], w[64];
    int n = parse_hex(rhv_patch_hyperkit_vendor.find, v, w, sizeof(v));
    CHECK(n > 0);
    embed(v, w, n, buf, sizeof(buf), 0x800);
    CHECK(hkpatch_buffer(buf, sizeof(buf), "ambiguous") == 1);

    /* the pattern is only searched in __TEXT: the same bytes below the segment's end but placed after a
     * smaller vmsize are not found -- make a segment that covers only the first 0x400 bytes */
    memset(buf, 0xcc, sizeof(buf));
    put32le(buf, 0xfeedfacfU); put32le(buf + 4, 0x01000007U); put32le(buf + 8, 3);
    put32le(buf + 12, 2); put32le(buf + 16, 1); put32le(buf + 20, 72); put32le(buf + 24, 0); put32le(buf + 28, 0);
    put32le(buf + 32, 0x19); put32le(buf + 36, 72); memcpy(buf + 40, "__TEXT", 7);
    put64le(buf + 56, 0); put64le(buf + 64, 0x400); put64le(buf + 72, 0); put64le(buf + 80, 0x400);
    put32le(buf + 88, 5); put32le(buf + 92, 5); put32le(buf + 96, 0); put32le(buf + 100, 0);
    n = parse_hex(rhv_patch_hyperkit_vendor.find, v, w, sizeof(v));
    embed(v, w, n, buf, sizeof(buf), 0x800);            /* outside __TEXT */
    n = parse_hex(rhv_patch_hyperkit_gla.find, v, w, sizeof(v));
    embed(v, w, n, buf, sizeof(buf), 0x300);            /* inside, but the vendor check is missing */
    CHECK(hkpatch_buffer(buf, sizeof(buf), "outside-text") == 1);

    /* not a Mach-O at all */
    memset(buf, 0x22, sizeof(buf));
    CHECK(hkpatch_buffer(buf, sizeof(buf), "junk") == 1);

    /* a universal file with no x86_64 slice */
    static uint8_t fat[0x40];
    memset(fat, 0, sizeof(fat));
    put32be(fat, 0xcafebabeU); put32be(fat + 4, 1);
    put32be(fat + 8, 0x0100000cU); put32be(fat + 12, 0); put32be(fat + 16, 0); put32be(fat + 20, 4); put32be(fat + 24, 12);
    CHECK(hkpatch_buffer(fat, sizeof(fat), "fat-no-x86") == 1);
}

static void test_universal(void)
{
    static uint8_t thin[0x1000], fat[0x1400];
    const size_t tlen = make_thin(thin, sizeof(thin), 0xfeedfacfU, 0x01000007U);
    memset(fat, 0, sizeof(fat));
    put32be(fat, 0xcafebabeU);                 /* FAT_MAGIC, header fields big-endian */
    put32be(fat + 4, 2);                       /* nfat_arch */
    /* arch entry: cputype, cpusubtype, offset, size, align */
    put32be(fat + 8, 0x0100000cU); put32be(fat + 12, 0); put32be(fat + 16, 48); put32be(fat + 20, 4); put32be(fat + 24, 12);
    put32be(fat + 28, 0x01000007U); put32be(fat + 32, 3); put32be(fat + 36, 0x400); put32be(fat + 40, (uint32_t)tlen);
    put32be(fat + 44, 12);
    memcpy(fat + 0x400, thin, tlen);           /* the x86_64 slice (the arm64 slice at 48 is skipped unread) */
    CHECK(hkpatch_buffer(fat, sizeof(fat), "fat") == 0);
    CHECK(fat[0x400 + VENDOR_JNE_AT] == 0x90);
    CHECK(fat[0x400 + GLA_JE_AT] == 0x90 && fat[0x400 + GLA_JE_AT + 1] == 0xe9);
}

int main(void)
{
    test_thin();
    test_refusals();
    test_universal();
    printf("hkpatch_test: PASS\n");
    return 0;
}
