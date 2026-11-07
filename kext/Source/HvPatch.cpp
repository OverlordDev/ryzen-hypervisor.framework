#include <IOKit/IOLib.h>
#include <mach/mach_types.h>
#include <mach/mach_vm.h>
#include <mach/vm_map.h>
#include <mach/vm_region.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <string.h>

#include "HvPatch.hpp"
#include "Log.hpp"

extern "C" vm_map_t get_task_map(task_t);
extern "C" proc_t current_proc(void);
extern "C" void csproc_disable_enforcement(proc_t p);        /* com.apple.kpi.private, sys/codesign.h */
extern "C" void csproc_mark_invalid_allowed(proc_t p);       /* com.apple.kpi.private, sys/codesign.h */

namespace rhv {
namespace hv {
namespace patch {

namespace {

constexpr int kMaxPat = 64;
constexpr size_t kChunk = 64 * 1024;
constexpr mach_vm_address_t kScanFrom = 0x100000000ull;           /* above the 4 GiB page zero */
constexpr mach_vm_address_t kScanBelow = 0x7f0000000000ull;       /* the dyld shared cache and the system start here */
constexpr mach_vm_size_t kMaxRegion = 128ull << 20;
constexpr unsigned kMaxRegions = 4096;

struct Patch {
    const char *name;
    const char *procPrefix;        /* the first characters of the process name (p_comm) */
    const char *find;              /* hex bytes, "??" = any byte */
    const char *patched;           /* same length; "??" keeps the byte */
};

/* The same patterns as shim/rhv_patch.c (Docker Desktop 4.97.0, HyperKit v0.20210107-12-gadc4ea). */
const Patch kPatches[] = {
    {"HyperKit CPUID vendor check", "com.docker.hyper",
     "48 b9 6e 65 49 6e 74 65 6c 00  48 33 4d ??  48 09 c1  0f 85 ?? ?? ?? ??",
     "?? ?? ?? ?? ?? ?? ?? ?? ?? ??  ?? ?? ?? ??  ?? ?? ??  90 90 90 90 90 90"},
    {"HyperKit verify_gla", "com.docker.hyper",
     "48 3b 9d ?? ?? ?? ??  4c 8b 7d ??  0f 84 ?? ?? ?? ??  48 83 ec 08  48 8d 3d",
     "?? ?? ?? ?? ?? ?? ??  ?? ?? ?? ??  90 e9 ?? ?? ?? ??  ?? ?? ?? ??  ?? ?? ??"},
};
constexpr unsigned kNumPatches = sizeof(kPatches) / sizeof(kPatches[0]);

struct Pat {
    uint8_t v[kMaxPat], w[kMaxPat];     /* value, wildcard */
    int n;
};

int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse(const char *s, Pat *p)
{
    p->n = 0;
    while (*s) {
        if (*s == ' ') { s++; continue; }
        if (p->n >= kMaxPat) return false;
        if (s[0] == '?' && s[1] == '?') {
            p->v[p->n] = 0; p->w[p->n++] = 1; s += 2;
        } else {
            int hi = hexval(s[0]), lo = hi < 0 ? -1 : hexval(s[1]);
            if (hi < 0 || lo < 0) return false;
            p->v[p->n] = (uint8_t)(hi << 4 | lo); p->w[p->n++] = 0; s += 2;
        }
    }
    return p->n > 0;
}

/* What one patch looks for: the original form, and the form it has once patched. */
struct Search {
    Pat orig, done, apply;              /* apply: the new bytes, wildcard = keep */
    unsigned nOrig, nDone;
    mach_vm_address_t atOrig, atDone;
    bool valid;
};

bool prepare(const Patch &patch, Search *s)
{
    Pat f, p;
    s->valid = false;
    s->nOrig = s->nDone = 0;
    s->atOrig = s->atDone = 0;
    if (!parse(patch.find, &f) || !parse(patch.patched, &p) || f.n != p.n)
        return false;
    s->orig = f;
    s->apply = p;
    s->done.n = f.n;
    for (int i = 0; i < f.n; i++) {
        s->done.v[i] = p.w[i] ? f.v[i] : p.v[i];
        s->done.w[i] = p.w[i] ? f.w[i] : 0;
    }
    s->valid = true;
    return true;
}

/* Matches of `pat` in buf[0..len) that START before `startLimit`, counted into *count (the first address into *first). */
void scanBuf(const uint8_t *buf, size_t len, size_t startLimit, const Pat &pat, mach_vm_address_t base, unsigned *count, mach_vm_address_t *first)
{
    if (len < (size_t)pat.n)
        return;
    size_t last = len - pat.n;
    if (last >= startLimit)
        last = startLimit ? startLimit - 1 : 0;
    for (size_t i = 0; i <= last; i++) {
        bool ok = true;
        for (int j = 0; j < pat.n && ok; j++)
            ok = pat.w[j] || buf[i + j] == pat.v[j];
        if (ok) {
            if (!*count)
                *first = base + i;
            (*count)++;
        }
    }
}

/* The client's code-signing state, logged once before its first patch (one log line holds 256 characters, so the messages are short). Seen so far: on a machine
 * with AMFI relaxed (amfi_get_out_of_my_way=0x1, macOS 15.8.1) CS_ENFORCEMENT is clear for HyperKit; on a Tahoe 26.7.1 machine it was set and the process was killed
 * with "Code Signature Invalid" right after the patch (issue 1). Whether that is the whole story is not known. */
constexpr uint64_t kCsHard = 0x100, kCsKill = 0x200, kCsEnforcement = 0x1000, kCsRequireLv = 0x2000;

void logSigningState(const char *name)
{
    uint64_t flags = 0;
    const int rc = proc_csflags(current_proc(), &flags);
    if (rc) {
        RHV_INFO("client fix-ups: signing flags of %s not readable (%d)", name, rc);
        return;
    }
    const bool enforced = (flags & kCsEnforcement) != 0;
    RHV_INFO("client fix-ups: signing flags of %s: %#llx, enforcement %s, hard %s, kill %s, lv %s", name, (unsigned long long)flags, enforced ? "ON" : "off",
             (flags & kCsHard) ? "on" : "off", (flags & kCsKill) ? "on" : "off", (flags & kCsRequireLv) ? "on" : "off");
    if (enforced) {
        RHV_WARN("client fix-ups: code signing is enforced for %s; such a process was killed after the patch on macOS 26.7.1 (issue 1)", name);
        RHV_WARN("client fix-ups: AMFI should be relaxed for it (amfi_get_out_of_my_way=0x1), see docs/NATIVE.md");
    }
}

bool nameMatches(const char *name, const char *prefix)
{
    return strncmp(name, prefix, strlen(prefix)) == 0;
}

/* --- Relaxing the client's own code-signing state, so the fix-ups survive without amfi_get_out_of_my_way=0x1 (macOS 26, ROADMAP 13) ---
 *
 * With code signing enforced for the client, the page a fix-up modified is rejected at page-in and the
 * process is killed with "Code Signature Invalid": cs_invalid_page() (bsd/kern/kern_cs.c) sends SIGKILL
 * when the process carries CS_KILL and refuses the page under CS_HARD. That is why the global AMFI
 * boot-arg was needed so far.
 *
 * The kill bits belong to the process, not to AMFI. A traced process gets exactly the state needed here:
 * ptrace(PT_TRACE_ME) makes the kernel call cs_allow_invalid() for it, which clears CS_KILL and CS_HARD
 * and sets CS_DEBUGGED (bsd/kern/mach_process.c), and modified code then runs. The client gets the same
 * per-process state from the kext:
 *
 *   - proc_csflags (com.apple.kpi.private, already used above) reads the flags of the client,
 *   - csproc_disable_enforcement (com.apple.kpi.private) clears CS_ENFORCEMENT and the enforcement flag of
 *     the address space -- the per-process part of what the AMFI boot-arg does for every process,
 *   - CS_KILL and CS_HARD are cleared through proc_csflags_update, which is NOT exported to kexts. Its
 *     address is found by walking two calls: csproc_mark_invalid_allowed (exported) calls proc_csflags_set
 *     right after the `mov esi, CS_INVALID_ALLOWED (0x20)` argument; proc_csflags_set (or, when the kernel
 *     inlined it, that same call) calls proc_csflags_update. Every candidate is verified by effect: it is
 *     called with the wanted flags and the flags are read back. A wrong candidate is the pure flags getter
 *     (no effect) or proc_csflags_set, which can only OR bits in and so cannot produce a value with a kill
 *     bit cleared, so a wrong decode cannot damage the flags.
 *
 * AMFI, library validation and notarization stay on for every other process on the system, which
 * amfi_get_out_of_my_way=0x1 switches off globally. Boot-arg rhv_csrelax=0 restores the old behavior
 * (patch only; the client may then be killed on an enforced kernel, see ROADMAP 13).
 */
constexpr uint64_t kCsDebugged = 0x10000000;    /* cs_blobs.h: set by cs_allow_invalid() for a traced process */

typedef void (*proc_csflags_update_t)(proc_t, uint64_t);

/* The rel32 target of the first `call` (or tail `jmp`) that follows `mov esi, imm32` within a few
 * bytes (here: the CS_INVALID_ALLOWED argument of proc_csflags_set; the compiler may set up other
 * arguments between the two, and the last call may be a jmp). The scan stops at the first `ret`,
 * so only this one function is read, and address ranges are bounded so that a mismatch cannot
 * send us far away. Every target is verified by effect by the caller. */
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

/* The rel32 targets of every `call` or tail `jmp` in a small function (scan stops at the first
 * `ret`), at most six. A `jmp` here is a tail call, which lands on the callee just like a call. */
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

/* Calls one candidate for proc_csflags_update with the wanted flags and reads them back. Returns the
 * candidate when it produced exactly them. See the comment above for why a wrong candidate cannot
 * remove or corrupt a flag here. */
proc_csflags_update_t tryCandidate(proc_t p, void *addr, uint64_t want, uint64_t before)
{
    if (!addr)
        return nullptr;
    reinterpret_cast<proc_csflags_update_t>(addr)(p, want);
    uint64_t after = 0;
    if (proc_csflags(p, &after) != 0)
        return nullptr;
    if (after != want) {
        if (after != before)                       /* proc_csflags_set probed: it only added CS_DEBUGGED */
            RHV_INFO("client fix-ups: probe of %#llx added CS_DEBUGGED only (flags now %#llx), not the setter",
                     (unsigned long long)(uintptr_t)addr, (unsigned long long)after);
        return nullptr;
    }
    return reinterpret_cast<proc_csflags_update_t>(addr);
}

/* Clears CS_KILL and CS_HARD of the calling process (and CS_ENFORCEMENT with it) and sets CS_DEBUGGED,
 * the state a debugged process has. Runs in the client's own context, in the hv_vm_create trap. */
void relaxSigning(const char *name)
{
    static int disabled = -1;
    if (disabled < 0) {
        uint32_t v = 1;
        disabled = PE_parse_boot_argn("rhv_csrelax", &v, sizeof(v)) && v == 0;
    }
    if (disabled)
        return;

    proc_t p = current_proc();
    uint64_t flags = 0;
    if (proc_csflags(p, &flags) != 0)
        return;
    if (!(flags & (kCsKill | kCsHard)) && !(flags & kCsEnforcement))
        return;                                    /* nothing enforced for it: an AMFI-relaxed machine */

    const uint64_t want = (flags & ~(kCsKill | kCsHard)) | kCsDebugged;

    /* The exported part of the state: CS_ENFORCEMENT off, in the process and in its address space. */
    csproc_disable_enforcement(p);
    uint64_t now = 0;
    if (proc_csflags(p, &now) != 0)
        return;
    if (now & kCsEnforcement)
        RHV_WARN("client fix-ups: csproc_disable_enforcement left CS_ENFORCEMENT set for %s (not the expected kernel)", name);

    proc_csflags_update_t update = nullptr;
    uint8_t *via = callAfterMovEsiImm(reinterpret_cast<const uint8_t *>(csproc_mark_invalid_allowed), 128, 0x20);
    if (via) {
        update = tryCandidate(p, via, want, now);  /* proc_csflags_update, when the kernel inlined it there */
        if (!update) {
            const uint8_t *cands[6];
            unsigned n = allCalls(via, 128, cands, 6);
            for (unsigned i = 0; i < n && !update; i++)
                update = tryCandidate(p, const_cast<uint8_t *>(cands[i]), want, now);
        }
    }
    uint64_t done = 0;
    if (update && proc_csflags(p, &done) != 0)
        return;                                    /* flags were changed; just skip the log line */
    if (!update)
        RHV_WARN("client fix-ups: could not relax the signing state of %s (kernel not as expected); without "
                 "amfi_get_out_of_my_way=0x1 it may be killed after the patch (rhv_csrelax=0 silences this)", name);
    else
        RHV_INFO("client fix-ups: signing of %s relaxed for this process only (%#llx -> %#llx); AMFI stays on "
                 "for the rest of the system, no amfi_get_out_of_my_way boot-arg needed", name,
                 (unsigned long long)flags, (unsigned long long)done);
}

} // namespace

void fixCurrentClient()
{
    static int disabled = -1;
    if (disabled < 0) {
        uint32_t v = 1;
        disabled = PE_parse_boot_argn("rhv_clientpatch", &v, sizeof(v)) && v == 0;
    }
    if (disabled)
        return;

    char name[32] = {0};
    proc_selfname(name, sizeof(name));
    unsigned wanted[kNumPatches], nWanted = 0;
    for (unsigned i = 0; i < kNumPatches; i++)
        if (nameMatches(name, kPatches[i].procPrefix))
            wanted[nWanted++] = i;
    if (!nWanted)
        return;

    task_t task = current_task();
    vm_map_t map = task ? get_task_map(task) : nullptr;
    if (!map) {
        RHV_WARN("client fix-ups: no address space for %s", name);
        return;
    }

    Search *srch = (Search *)IOMallocZero(sizeof(Search) * nWanted);
    uint8_t *buf = (uint8_t *)IOMalloc(kChunk + kMaxPat);
    if (!srch || !buf) {
        if (srch) IOFree(srch, sizeof(Search) * nWanted);
        if (buf) IOFree(buf, kChunk + kMaxPat);
        return;
    }
    for (unsigned i = 0; i < nWanted; i++)
        prepare(kPatches[wanted[i]], &srch[i]);

    /* Every executable region of the process below the shared cache: the main executable and the libraries loaded next to it. */
    mach_vm_size_t scanned = 0;
    unsigned regions = 0;
    mach_vm_address_t addr = kScanFrom;
    while (regions++ < kMaxRegions) {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t objName = 0;
        if (mach_vm_region(map, &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &objName) != KERN_SUCCESS || !size)
            break;
        if (addr >= kScanBelow)
            break;
        if ((info.protection & VM_PROT_EXECUTE) && size <= kMaxRegion) {
            for (mach_vm_size_t off = 0; off < size; off += kChunk) {
                size_t want = (size_t)(size - off < kChunk + kMaxPat ? size - off : kChunk + kMaxPat);
                if (copyin((user_addr_t)(addr + off), buf, want))
                    break;
                bool lastChunk = off + kChunk >= size;
                size_t limit = lastChunk ? want : kChunk;       /* starts at or beyond the chunk belong to the next one */
                for (unsigned i = 0; i < nWanted; i++) {
                    if (!srch[i].valid)
                        continue;
                    scanBuf(buf, want, limit, srch[i].orig, addr + off, &srch[i].nOrig, &srch[i].atOrig);
                    scanBuf(buf, want, limit, srch[i].done, addr + off, &srch[i].nDone, &srch[i].atDone);
                }
            }
            scanned += size;
        }
        addr += size;
        if (!addr)
            break;
    }

    bool signingLogged = false;
    for (unsigned i = 0; i < nWanted; i++) {
        const Patch &pt = kPatches[wanted[i]];
        Search &s = srch[i];
        if (!signingLogged && s.valid && s.nOrig == 1 && !s.nDone) {
            signingLogged = true;
            logSigningState(name);
            relaxSigning(name);            /* drop the client's kill bits before the write, so the patch survives without the AMFI boot-arg */
        }
        if (!s.valid) {
            RHV_WARN("client fix-ups: bad pattern for %s", pt.name);
        } else if (s.nOrig + s.nDone != 1) {
            RHV_WARN("client fix-ups: %s: %u original and %u patched matches in %llu KiB of code of %s, left alone", pt.name, s.nOrig, s.nDone,
                     (unsigned long long)(scanned >> 10), name);
        } else if (s.nDone == 1) {
            RHV_INFO("client fix-ups: %s: already in place at %#llx (%s)", pt.name, (unsigned long long)s.atDone, name);
        } else {
            uint8_t cur[kMaxPat], nw[kMaxPat];
            const mach_vm_address_t at = s.atOrig;
            if (copyin((user_addr_t)at, cur, (user_size_t)s.orig.n)) {
                RHV_WARN("client fix-ups: %s: cannot read the code again", pt.name);
                continue;
            }
            for (int k = 0; k < s.orig.n; k++)
                nw[k] = s.apply.w[k] ? cur[k] : s.apply.v[k];
            const mach_vm_address_t page = at & ~(mach_vm_address_t)0xfff;
            const mach_vm_size_t span = ((at + (mach_vm_address_t)s.orig.n + 0xfff) & ~(mach_vm_address_t)0xfff) - page;
            RHV_SYNC("client fix-ups: %s at %#llx in %s (pid %d)", pt.name, (unsigned long long)at, name, proc_selfpid());
            kern_return_t kr = vm_protect(map, (vm_offset_t)page, (vm_size_t)span, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
            if (kr != KERN_SUCCESS) {
                RHV_WARN("client fix-ups: %s: could not make the code writable (0x%x), left alone", pt.name, kr);
                continue;
            }
            int cerr = copyout(nw, (user_addr_t)at, (user_size_t)s.orig.n);
            kr = vm_protect(map, (vm_offset_t)page, (vm_size_t)span, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
            if (cerr || kr != KERN_SUCCESS)
                RHV_WARN("client fix-ups: %s: write %d, protect back 0x%x", pt.name, cerr, kr);
            else
                RHV_INFO("client fix-ups: %s: patched", pt.name);
        }
    }

    IOFree(buf, kChunk + kMaxPat);
    IOFree(srch, sizeof(Search) * nWanted);
}

} // namespace patch
} // namespace hv
} // namespace rhv
