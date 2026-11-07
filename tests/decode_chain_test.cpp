// Tests the decode chain that kext/Source/HvPatch.cpp (relaxSigning) uses, against functions
// compiled from the shapes in xnu's bsd/kern/kern_proc.c and kern_cs.c (macOS 15 / 26 sources).
//
//   csproc_mark_invalid_allowed (kern_cs.c)  --mov esi,0x20; call-->  proc_csflags_set (kern_proc.c)
//   proc_csflags_set                          --call-->               proc_csflags_update
//
// Compiled with -O2 like the kernel, the calls stay real calls. The "kernel text" the decoder
// walks is the text of this very binary, so the test checks the byte patterns, the rel32 math
// and the probe-by-effect loop end to end.

#include <cstdint>
#include <cstdio>
#include <cstring>

// --- the flags machinery, with the semantics of xnu -----------------------------

struct proc { unsigned pad; };
typedef struct proc *proc_t;

static struct proc g_client;                    /* the process whose flags are relaxed */
static struct proc g_kernel_task;               /* kernproc: proc_csflags_update skips writes for it */
proc_t kernproc = &g_kernel_task;

static uint32_t g_ro_csflags = 0;               /* stands in for the read-only proc_ro field */

extern "C" uint64_t proc_getcsflags(proc_t p) { (void)p; return g_ro_csflags; }
extern "C" __attribute__((noinline)) void proc_csflags_update(proc_t p, uint64_t flags)
{
    if (p != kernproc) g_ro_csflags = (uint32_t)flags;
}
extern "C" __attribute__((noinline)) void proc_csflags_set(proc_t p, uint64_t flags)
{
    proc_csflags_update(p, proc_getcsflags(p) | (uint32_t)flags);
}
extern "C" int proc_csflags(proc_t p, uint64_t *flags)          /* the getter the kext links */
{
    if (p && flags) { *flags = proc_getcsflags(p); return 0; }
    return 22; /* EINVAL */
}

constexpr unsigned CS_INVALID_ALLOWED = 0x20;

// The kern_cs.c side. In the kernel these two are in different translation units; with default
// visibility the compiler has to keep the call a call either way, which is what the decoder walks.
extern "C" void csproc_mark_invalid_allowed(proc_t p)
{
    if (p != nullptr) {
        proc_csflags_set(p, CS_INVALID_ALLOWED);
    }
}

// --- the decoder, copied from kext/Source/HvPatch.cpp -----------------------------

typedef void (*proc_csflags_update_t)(proc_t, uint64_t);

uint8_t *callAfterMovEsiImm(const uint8_t *code, size_t span, uint32_t imm)
{
    for (size_t i = 0; i + 10 <= span && code[i] != 0xc3; i++) {
        if (code[i] == 0xbe && code[i + 1] == (uint8_t)(imm & 0xff) && code[i + 2] == (uint8_t)((imm >> 8) & 0xff) &&
            code[i + 3] == (uint8_t)((imm >> 16) & 0xff) && code[i + 4] == (uint8_t)((imm >> 24) & 0xff)) {
            for (size_t j = i + 5; j < i + 5 + 12 && j + 5 <= span && code[j] != 0xc3; j++) {
                int32_t rel;
                size_t len = 0;
                if (code[j] == 0xe8 || code[j] == 0xe9) {          /* call rel32 / jmp rel32 */
                    __builtin_memcpy(&rel, code + j + 1, sizeof(rel));
                    len = 5;
                } else if (code[j] == 0xeb) {                      /* jmp rel8 */
                    rel = (int8_t)code[j + 1];
                    len = 2;
                } else
                    continue;
                if (rel > -(int32_t)0x1000000 && rel < (int32_t)0x1000000)
                    return const_cast<uint8_t *>(code + j + len + rel);      /* rip after the transfer is code + j + len */
            }
            return nullptr;                    /* the transfer of this argument setup is not in reach */
        }
    }
    return nullptr;
}

unsigned allCalls(const uint8_t *code, size_t span, const uint8_t *out[], unsigned max)
{
    unsigned n = 0;
    for (size_t i = 0; i + 5 <= span && code[i] != 0xc3 && n < max; i++) {
        int32_t rel;
        size_t len = 0;
        if (code[i] == 0xe8 || code[i] == 0xe9) {
            __builtin_memcpy(&rel, code + i + 1, sizeof(rel));
            len = 5;
        } else if (code[i] == 0xeb && i + 2 <= span) {
            rel = (int8_t)code[i + 1];
            len = 2;
        } else
            continue;
        if (rel <= -(int32_t)0x1000000 || rel >= (int32_t)0x1000000)
            continue;
        out[n++] = code + i + len + rel;
    }
    return n;
}

// --- the test ---------------------------------------------------------------------

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) printf("  ok: %s\n", msg); else { printf("  FAIL: %s\n", msg); failures++; } } while (0)

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const uint8_t *mark = reinterpret_cast<const uint8_t *>(&csproc_mark_invalid_allowed);

    printf("csproc_mark_invalid_allowed bytes:");
    for (int i = 0; i < 48 && mark[i] != 0xc3; i++) printf(" %02x", mark[i]);
    printf("\n");

    // 1. the mov esi,0x20 + call pattern is there, and it points at proc_csflags_set
    uint8_t *via = callAfterMovEsiImm(mark, 128, CS_INVALID_ALLOWED);
    CHECK(via == reinterpret_cast<uint8_t *>(&proc_csflags_set),
          "the call after `mov esi, CS_INVALID_ALLOWED` lands on proc_csflags_set");

    printf("  via=%p  &proc_csflags_set=%p  &proc_csflags_update=%p  &getter=%p\n",
           (void *)via, (void *)&proc_csflags_set, (void *)&proc_csflags_update, (void *)&proc_getcsflags);
    if (!via) return 1;

    // 2. inside proc_csflags_set the calls are the getter (no effect) and proc_csflags_update;
    //    the probe-by-effect finds the update and cannot confuse it with the setter or getter.
    const uint64_t kCsKill = 0x200, kCsHard = 0x100, kCsDebugged = 0x10000000;
    g_ro_csflags = 0x00003301;          /* VALID|KILL|HARD|ENFORCEMENT|REQUIRE_LV: the flags of the Tahoe crash */

    const uint64_t before = proc_getcsflags(&g_client);
    const uint64_t want = (before & ~(kCsKill | kCsHard)) | kCsDebugged;
    CHECK((before & (kCsKill | kCsHard)) != 0, "the mock starts with the kill bits set");

    proc_csflags_update_t found = nullptr;
    const uint8_t *cands[6];
    unsigned n = allCalls(via, 128, cands, 6);
    for (unsigned i = 0; i < n; i++)
        printf("    cand[%u]=%p (set=%p update=%p getter=%p)\n", i, (void *)cands[i],
               (void *)&proc_csflags_set, (void *)&proc_csflags_update, (void *)&proc_getcsflags);
    printf("  proc_csflags_set has %u call(s)\n", n);
    for (unsigned i = 0; i < n && !found; i++) {
        if (cands[i] == reinterpret_cast<const uint8_t *>(&proc_csflags_set))
            continue;                   /* cannot happen, kept from the kext code for clarity */
        // probe by effect, exactly like tryCandidate()
        reinterpret_cast<proc_csflags_update_t>(const_cast<uint8_t *>(cands[i]))(&g_client, want);
        if (proc_getcsflags(&g_client) == want)
            found = reinterpret_cast<proc_csflags_update_t>(const_cast<uint8_t *>(cands[i]));
        else
            g_ro_csflags = (uint32_t)before;    /* undo whatever the probe did (getter probe: nothing) */
    }
    CHECK(found == reinterpret_cast<proc_csflags_update_t>(&proc_csflags_update),
          "the probe-by-effect finds proc_csflags_update, not the getter and not the setter");
    CHECK(proc_getcsflags(&g_client) == (found ? want : before),
          found ? "the successful probe left exactly the wanted flags" : "the failed probes left the flags as they were");

    // 3. the real call through the found pointer produces exactly the wanted state
    g_ro_csflags = (uint32_t)before;
    found(&g_client, want);
    const uint64_t done = proc_getcsflags(&g_client);
    CHECK(done == want, "the call through the decoded pointer sets exactly the wanted flags");
    CHECK(!(done & kCsKill) && !(done & kCsHard), "CS_KILL and CS_HARD are gone");
    CHECK((done & kCsDebugged) != 0, "CS_DEBUGGED is set");
    CHECK((done & 0x2000) != 0, "CS_REQUIRE_LV is kept (the debugger state keeps it)");

    if (failures) { printf("FAILED (%d)\n", failures); return 1; }
    printf("decode-chain test: OK\n");
    return 0;
}
