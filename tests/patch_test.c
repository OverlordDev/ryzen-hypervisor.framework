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

/* The POST code handler of Parallels' monitor, as in build 57613 (image offset 0x47b30), placed at `at` in a buffer of zeros whose rip-relative operands
 * are made to point at the flags `second` and `armed_base` (+ the 0x4a550 of the mov), so the test knows what the finder has to answer. */
static const unsigned char kbd_fn[] = {
    0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0xbb,0x02,0x00,0x00,0x00,0x48,0x83,0xfa,0x01,0x0f,0x85,0x85,0x00,0x00,0x00,0x83,0xfe,0x02,0x74,0x10,0xbb,0x01,0x00,0x00,0x00,
    0x83,0xfe,0x01,0x75,0x76,0x44,0x0f,0xb6,0x31,0xeb,0x04,0x44,0x0f,0xb7,0x31,0x48,0x8d,0x05,0xd2,0x28,0x12,0x00,0x83,0x38,0x03,0x7c,0x24,0x48,0x8d,0x3d,0x66,0x3a,
    0x0c,0x00,0x48,0x8d,0x35,0xa2,0x70,0x0b,0x00,0x48,0x8d,0x0d,0x67,0xde,0x0c,0x00,0xba,0x03,0x00,0x00,0x00,0x45,0x89,0xf0,0x31,0xc0,0xe8,0x51,0x3b,0x0a,0x00,0x31,
    0xdb,0x45,0x84,0xf6,0x75,0x35,0x80,0x3d,0x03,0x0b,0x82,0x00,0x00,0x75,0x2c,0xe8,0x4c,0x82,0x00,0x00,0x48,0x8d,0x05,0x15,0x3d,0x12,0x00,0xc6,0x80,0x50,0xa5,0x04,
    0x00,0x01,0x31,0xdb,0xbf,0x84,0x03,0x00,0x00,0x31,0xf6,0x31,0xd2,0x31,0xc9,0xe8,0x5c,0xdb,0xfd,0xff,0xc6,0x05,0xd5,0x0a,0x82,0x00,0x01,0x89,0xd8,0x5b,0x41,0x5e,
    0x5d,0xc3,
};
#define KBD_CMP 0x66          /* offset of the cmp byte [rip+d],0 inside kbd_fn */
#define KBD_LEA 0x74          /* offset of the lea rax,[rip+d] */

static void put32(unsigned char *p, int32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }

static void test_monitor_kbd(void)
{
    unsigned char buf[0x2000];
    struct rhv_monitor_kbd k;
    const size_t at = 0x300, second = 0x1800, armed_base = 0x1000, mov_off = 0x4a0;
    memset(buf, 0x90, sizeof(buf));
    memcpy(buf + at, kbd_fn, sizeof(kbd_fn));
    /* the same code with other displacements: the finder follows them */
    put32(buf + at + KBD_CMP + 2, (int32_t)(second - (at + KBD_CMP + 7)));
    put32(buf + at + KBD_LEA + 3, (int32_t)(armed_base - (at + KBD_LEA + 7)));
    put32(buf + at + KBD_LEA + 9, (int32_t)mov_off);
    CHECK(rhv_find_monitor_kbd(buf, sizeof(buf), &k) == RHV_PATCH_APPLIED);
    CHECK(k.handler == at && k.second == second && k.armed == armed_base + mov_off);

    /* the branch distances and the call target may change */
    buf[at + 36] = 0x7a;                   /* jne rel8 after cmp esi,1 */
    buf[at + 0x70] = 0x11;                 /* the call's operand */
    CHECK(rhv_find_monitor_kbd(buf, sizeof(buf), &k) == RHV_PATCH_APPLIED && k.handler == at);

    /* another build: the code is different (cmp esi,2 became cmp esi,3) -> not found */
    buf[at + 24] = 0x03;
    CHECK(rhv_find_monitor_kbd(buf, sizeof(buf), &k) == RHV_PATCH_NOT_FOUND);
    buf[at + 24] = 0x02;
    CHECK(rhv_find_monitor_kbd(buf, sizeof(buf), &k) == RHV_PATCH_APPLIED);

    /* the head is there but the flag part is not: the immediate of "mov byte [rax+off],1" is 0 */
    buf[at + KBD_LEA + 13] = 0x00;
    CHECK(rhv_find_monitor_kbd(buf, sizeof(buf), &k) == RHV_PATCH_NOT_FOUND);
    buf[at + KBD_LEA + 13] = 0x01;
    CHECK(rhv_find_monitor_kbd(buf, sizeof(buf), &k) == RHV_PATCH_APPLIED);

    /* two copies of the head: ambiguous, nothing answered */
    memcpy(buf + at, kbd_fn, sizeof(kbd_fn));
    memcpy(buf + 0x900, kbd_fn, sizeof(kbd_fn));
    CHECK(rhv_find_monitor_kbd(buf, sizeof(buf), &k) == RHV_PATCH_AMBIGUOUS);
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
    test_monitor_kbd();

    const char *prl = "/Applications/Parallels Desktop.app/Contents/MacOS";
    char path[1024];
    snprintf(path, sizeof(path), "%s/libMonitor.dylib", prl);
    real(path, &rhv_patch_prl_vendor, 0x207b5);
    snprintf(path, sizeof(path), "%s/Parallels VM.app/Contents/MacOS/prl_vm_app.orig", prl);
    real(path, &rhv_patch_prl_vendor, 0xc1c61);
    {
        size_t len;
        snprintf(path, sizeof(path), "%s/libMonitor.dylib", prl);
        unsigned char *b = slurp(path, &len);
        if (b) {
            struct rhv_monitor_kbd k;
            rhv_patch_result r = rhv_find_monitor_kbd(b, len, &k);
            printf("  Parallels POST code handler: %s, handler %#zx armed %#zx second %#zx\n", r == RHV_PATCH_APPLIED ? "found once" : rhv_patch_result_name(r), k.handler, k.armed, k.second);
            /* what the shim used before, as fixed offsets (build 57613) */
            CHECK(r == RHV_PATCH_APPLIED && k.handler == 0x47b30 && k.armed == 0x16b8c0 + 0x4a550 && k.second == 0x8686a0);
            free(b);
        }
    }
    const char *hk = "/Applications/Docker.app/Contents/Resources/bin/com.docker.hyperkit.orig";
    real(hk, &rhv_patch_hyperkit_vendor, 0x227b56);
    real(hk, &rhv_patch_hyperkit_gla, 0x20af38);

    printf("patch_test: PASS\n");
    return 0;
}
