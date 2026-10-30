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

bool nameMatches(const char *name, const char *prefix)
{
    return strncmp(name, prefix, strlen(prefix)) == 0;
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

    for (unsigned i = 0; i < nWanted; i++) {
        const Patch &pt = kPatches[wanted[i]];
        Search &s = srch[i];
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
