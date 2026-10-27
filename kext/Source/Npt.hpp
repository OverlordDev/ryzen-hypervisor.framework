/*
 * Npt.hpp - nested page tables (guest physical -> host physical), 4 KiB pages.
 *
 * Pure C++: only <stdint.h>/<stddef.h>/<string.h>, memory comes from an
 * allocator the caller provides. In the kext that is wired contiguous kernel
 * memory, in tests/npt_test.cpp it is plain malloc plus failure injection. This
 * is what lets the tree logic (growth, pruning, rollback) be checked on any host.
 *
 * Format: NPT entries are ordinary x86-64 long-mode page table entries
 * (AMD APM Vol. 2, 15.25). A guest access is always treated as a "user" access
 * by the nested walk, so US must be set at every level. Permissions are the AND
 * of all levels, so upper levels stay fully permissive and leaves carry the
 * real rights: P (read), RW (write), NX (execute, honoured when host EFER.NXE=1).
 *
 * Every leaf slot also remembers an opaque `owner` pointer. That is how the
 * kext knows which pinned user buffer a page belongs to when it is unmapped.
 * "Mapped" means "owner != nullptr"; a mapped page with no rights has P=0.
 *
 * Not thread safe: the caller serialises (the Vm lock).
 */
#ifndef RYZENHV_NPT_HPP
#define RYZENHV_NPT_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace rhv {
namespace npt {

/* Same values as the hv_memory_flags_t of Hypervisor.framework. */
enum : uint32_t { PERM_R = 1u << 0, PERM_W = 1u << 1, PERM_X = 1u << 2, PERM_MASK = 7u };

const uint64_t kPageSize = 4096;
const uint64_t kGpaLimit = 1ull << 48;   /* 4-level walk */

const uint64_t E_P         = 1ull << 0;
const uint64_t E_RW        = 1ull << 1;
const uint64_t E_US        = 1ull << 2;
const uint64_t E_NX        = 1ull << 63;
const uint64_t E_ADDR_MASK = 0x000FFFFFFFFFF000ull;

/* x86 cannot express write-only or execute-only, so W and X imply R. Returns false for unknown bits. */
static inline bool normalizePerm(uint32_t *p)
{
    if (*p & ~PERM_MASK)
        return false;
    if (*p & (PERM_W | PERM_X))
        *p |= PERM_R;
    return true;
}

static inline uint64_t encodeLeaf(uint64_t hpa, uint32_t perm)
{
    uint64_t e = hpa & E_ADDR_MASK;
    if (!(perm & PERM_R))
        return e;                       /* tracked but not present: every access faults */
    e |= E_P | E_US;
    if (perm & PERM_W)
        e |= E_RW;
    if (!(perm & PERM_X))
        e |= E_NX;
    return e;
}

static inline uint32_t decodeLeaf(uint64_t e)
{
    if (!(e & E_P))
        return 0;
    uint32_t p = PERM_R;
    if (e & E_RW)
        p |= PERM_W;
    if (!(e & E_NX))
        p |= PERM_X;
    return p;
}

/* Index into the table of `level` (4 = PML4 ... 1 = PT) for a guest physical address. */
static inline unsigned indexAt(uint64_t gpa, int level)
{
    return (unsigned)((gpa >> (12 + 9 * (level - 1))) & 0x1FF);
}

struct Page {
    void    *va;       /* zeroed, 4 KiB, physically contiguous */
    uint64_t pa;
    void    *handle;   /* the allocator's own bookkeeping, passed back to freePage */
};

struct Allocator {
    void *ctx;
    bool (*allocPage)(void *ctx, Page *out);
    void (*freePage)(void *ctx, const Page *p);
    void *(*allocMem)(void *ctx, size_t bytes);              /* zeroed */
    void (*freeMem)(void *ctx, void *p, size_t bytes);
};

enum Status { OK = 0, BAD_ARG, NO_MEMORY, BUSY, NOT_MAPPED, BAD_SEGMENT };

/*
 * Describes where byte `offset` of a buffer lives: *pa is the physical address of that byte and *len how many
 * bytes follow contiguously. Returns false when the buffer has no such byte. This is what
 * IOMemoryDescriptor::getPhysicalSegment gives the kext; tests supply fake, fragmented ones.
 */
typedef bool (*SegmentFn)(void *ctx, uint64_t offset, uint64_t *pa, uint64_t *len);

class Table {
public:
    Table() : fA(), fRoot(nullptr), fPages(0), fMapped(0) {}
    explicit Table(const Allocator &a) : fA(a), fRoot(nullptr), fPages(0), fMapped(0) {}
    ~Table() { shutdown(nullptr, nullptr); }
    Table(const Table &) = delete;
    Table &operator=(const Table &) = delete;

    /* Allocates the root. Must succeed before anything else is called. */
    bool init()
    {
        if (fRoot)
            return true;
        fRoot = newNode();
        return fRoot != nullptr;
    }

    bool init(const Allocator &a)
    {
        if (fRoot)
            return true;
        fA = a;
        return init();
    }

    uint64_t rootPa() const { return fRoot ? fRoot->page.pa : 0; }
    size_t   tablePages() const { return fPages; }      /* hardware table pages incl. the root */
    uint64_t mappedPages() const { return fMapped; }

    Status map(uint64_t gpa, uint64_t hpa, uint32_t perm, void *owner)
    {
        if (!fRoot || !owner || (gpa & 0xFFF) || (hpa & 0xFFF) || gpa >= kGpaLimit ||
            (hpa & ~E_ADDR_MASK) || !normalizePerm(&perm))
            return BAD_ARG;

        Node *path[4];
        unsigned idx[4];
        Node *n = fRoot;
        for (int lvl = 4; lvl > 1; lvl--) {
            int k = 4 - lvl;
            path[k] = n;
            idx[k]  = indexAt(gpa, lvl);
            Node *c = static_cast<Node *>(n->slot[idx[k]]);
            if (!c) {
                c = newNode();
                if (!c) {
                    prune(path, idx, k);
                    return NO_MEMORY;
                }
                n->slot[idx[k]]  = c;
                n->table[idx[k]] = c->page.pa | E_P | E_RW | E_US;
                n->used++;
            }
            n = c;
        }
        path[3] = n;
        idx[3]  = indexAt(gpa, 1);
        if (n->slot[idx[3]])
            return BUSY;            /* the leaf existed, so every level above did too */

        n->slot[idx[3]]  = owner;
        n->table[idx[3]] = encodeLeaf(hpa, perm);
        n->used++;
        fMapped++;
        return OK;
    }

    /*
     * Maps `size` bytes of a buffer (offset 0 .. size) at guest physical `gpa`, one page at a time, following
     * the buffer's physical segments. The range must be free. On any failure the pages mapped so far are left in
     * place and *mapped says how many bytes that is (always a multiple of the page size), so the caller can
     * unmap exactly those and release the buffer.
     */
    Status mapRange(uint64_t gpa, uint64_t size, uint32_t perm, void *owner, SegmentFn segment, void *ctx,
                    uint64_t *mapped)
    {
        *mapped = 0;
        if ((gpa & 0xFFF) || (size & 0xFFF) || size == 0)
            return BAD_ARG;
        for (uint64_t off = 0; off < size; off += kPageSize) {
            if (isMapped(gpa + off))
                return BUSY;
        }
        while (*mapped < size) {
            uint64_t pa = 0, len = 0;
            if (!segment(ctx, *mapped, &pa, &len) || (pa & 0xFFF))
                return BAD_SEGMENT;
            if (len > size - *mapped)
                len = size - *mapped;
            len &= ~(uint64_t)0xFFF;
            if (len == 0)
                return BAD_SEGMENT;
            const uint64_t base = *mapped;     // where this segment starts in the buffer
            for (uint64_t p = 0; p < len; p += kPageSize) {
                Status st = map(gpa + base + p, pa + p, perm, owner);
                if (st != OK)
                    return st;
                *mapped = base + p + kPageSize;
            }
        }
        return OK;
    }

    /* Returns the owner the page was mapped with, or nullptr if it was not mapped. */
    void *unmap(uint64_t gpa)
    {
        Node *path[4];
        unsigned idx[4];
        Node *leaf = walk(gpa, path, idx);
        if (!leaf || !leaf->slot[idx[3]])
            return nullptr;
        void *owner       = leaf->slot[idx[3]];
        leaf->slot[idx[3]]  = nullptr;
        leaf->table[idx[3]] = 0;
        leaf->used--;
        fMapped--;
        prune(path, idx, 3);
        return owner;
    }

    Status protect(uint64_t gpa, uint32_t perm)
    {
        if (!normalizePerm(&perm))
            return BAD_ARG;
        Node *path[4];
        unsigned idx[4];
        Node *leaf = walk(gpa, path, idx);
        if (!leaf || !leaf->slot[idx[3]])
            return NOT_MAPPED;
        leaf->table[idx[3]] = encodeLeaf(leaf->table[idx[3]], perm);
        return OK;
    }

    /* Owner of the page, or nullptr when unmapped. hpa/perm are optional outputs. */
    void *lookup(uint64_t gpa, uint64_t *hpa, uint32_t *perm) const
    {
        Node *path[4];
        unsigned idx[4];
        Node *leaf = const_cast<Table *>(this)->walk(gpa, path, idx);
        if (!leaf || !leaf->slot[idx[3]])
            return nullptr;
        if (hpa)
            *hpa = leaf->table[idx[3]] & E_ADDR_MASK;
        if (perm)
            *perm = decodeLeaf(leaf->table[idx[3]]);
        return leaf->slot[idx[3]];
    }

    bool isMapped(uint64_t gpa) const { return lookup(gpa, nullptr, nullptr) != nullptr; }

    /*
     * Drops everything. `release` is called once per still-mapped page with its
     * owner, so the kext can unpin buffers. The root is kept (and emptied).
     */
    void clear(void (*release)(void *ctx, void *owner), void *ctx)
    {
        if (!fRoot)
            return;
        freeChildren(fRoot, 4, release, ctx);
        fRoot->used = 0;
        memset(fRoot->table, 0, kPageSize);
    }

    /* clear() and also give the root back. The table is unusable until init() again. Safe to call twice. */
    void shutdown(void (*release)(void *ctx, void *owner), void *ctx)
    {
        if (!fRoot)
            return;
        clear(release, ctx);
        freeNode(fRoot);
        fRoot = nullptr;
    }

private:
    struct Node {
        Page      page;
        uint64_t *table;       /* == page.va */
        uint16_t  used;        /* live slots */
        void     *slot[512];   /* levels 4..2: Node*, level 1: owner */
    };

    Node *newNode()
    {
        Node *n = static_cast<Node *>(fA.allocMem(fA.ctx, sizeof(Node)));
        if (!n)
            return nullptr;
        memset(n, 0, sizeof(Node));
        if (!fA.allocPage(fA.ctx, &n->page)) {
            fA.freeMem(fA.ctx, n, sizeof(Node));
            return nullptr;
        }
        n->table = static_cast<uint64_t *>(n->page.va);
        fPages++;
        return n;
    }

    void freeNode(Node *n)
    {
        fA.freePage(fA.ctx, &n->page);
        fA.freeMem(fA.ctx, n, sizeof(Node));
        fPages--;
    }

    /* Follows the existing tables. path/idx are filled as far as the walk got; returns the leaf node or nullptr. */
    Node *walk(uint64_t gpa, Node **path, unsigned *idx)
    {
        if (!fRoot || gpa >= kGpaLimit)
            return nullptr;
        Node *n = fRoot;
        for (int lvl = 4; lvl > 1; lvl--) {
            int k = 4 - lvl;
            path[k] = n;
            idx[k]  = indexAt(gpa, lvl);
            n = static_cast<Node *>(n->slot[idx[k]]);
            if (!n)
                return nullptr;
        }
        path[3] = n;
        idx[3]  = indexAt(gpa, 1);
        return n;
    }

    /* Frees empty tables from path[depth] upwards (never the root) and unhooks them from their parent. */
    void prune(Node **path, const unsigned *idx, int depth)
    {
        for (int k = depth; k >= 1; k--) {
            Node *n = path[k];
            if (n->used != 0)
                break;
            Node *parent = path[k - 1];
            parent->slot[idx[k - 1]]  = nullptr;
            parent->table[idx[k - 1]] = 0;
            parent->used--;
            freeNode(n);
        }
    }

    void freeChildren(Node *n, int level, void (*release)(void *, void *), void *ctx)
    {
        for (unsigned i = 0; i < 512; i++) {
            void *s = n->slot[i];
            if (!s)
                continue;
            if (level == 1) {
                fMapped--;
                if (release)
                    release(ctx, s);
            } else {
                Node *c = static_cast<Node *>(s);
                freeChildren(c, level - 1, release, ctx);
                freeNode(c);
            }
            n->slot[i] = nullptr;
        }
    }

    Allocator fA;
    Node     *fRoot;
    size_t    fPages;
    uint64_t  fMapped;
};

} // namespace npt
} // namespace rhv

#endif /* RYZENHV_NPT_HPP */
