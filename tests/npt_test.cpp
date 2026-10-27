/*
 * npt_test.cpp - host-side tests for kext/Source/Npt.hpp.
 *
 * The table memory comes from a fake allocator where "physical address" ==
 * virtual address, so the test can walk the raw tables the way the CPU would,
 * independently of the Table class, and compare. Failure injection makes the
 * N-th allocation fail to check rollback and pruning.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../kext/Source/Npt.hpp"

using namespace rhv::npt;

namespace {

struct Fake {
    long pagesLive = 0;
    long memLive   = 0;
    long failAfter = -1;   /* fail every allocation once this many have succeeded; -1 = never */
    long allocs    = 0;
};

bool fakeAllocPage(void *c, Page *out)
{
    Fake *f = static_cast<Fake *>(c);
    if (f->failAfter >= 0 && f->allocs >= f->failAfter)
        return false;
    void *p = nullptr;
    if (posix_memalign(&p, 4096, 4096))
        return false;
    memset(p, 0, 4096);
    f->allocs++;
    f->pagesLive++;
    out->va     = p;
    out->pa     = (uint64_t)(uintptr_t)p;
    out->handle = nullptr;
    return true;
}

void fakeFreePage(void *c, const Page *p)
{
    static_cast<Fake *>(c)->pagesLive--;
    free(p->va);
}

void *fakeAllocMem(void *c, size_t n)
{
    Fake *f = static_cast<Fake *>(c);
    if (f->failAfter >= 0 && f->allocs >= f->failAfter)
        return nullptr;
    f->allocs++;
    f->memLive++;
    return calloc(1, n);
}

void fakeFreeMem(void *c, void *p, size_t)
{
    static_cast<Fake *>(c)->memLive--;
    free(p);
}

Allocator allocatorFor(Fake *f)
{
    return Allocator{ f, fakeAllocPage, fakeFreePage, fakeAllocMem, fakeFreeMem };
}

/* What the hardware would do: returns false (fault) or the host physical address and rights. */
bool hwWalk(uint64_t rootPa, uint64_t gpa, uint64_t *hpa, uint32_t *perm)
{
    uint64_t table = rootPa;
    for (int lvl = 4; lvl >= 1; lvl--) {
        uint64_t e = ((const uint64_t *)(uintptr_t)table)[indexAt(gpa, lvl)];
        if (lvl > 1) {
            if (!(e & E_P) || !(e & E_RW) || !(e & E_US) || (e & E_NX))
                return false;   /* we only ever build fully permissive upper levels */
            table = e & E_ADDR_MASK;
        } else {
            if (!(e & E_P) || !(e & E_US))
                return false;
            *hpa  = e & E_ADDR_MASK;
            *perm = decodeLeaf(e);
            return true;
        }
    }
    return false;
}

int fails = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                  \
            fails++;                                                             \
        }                                                                        \
    } while (0)

int dummy1, dummy2;
void *const OWN1 = &dummy1;
void *const OWN2 = &dummy2;

void testEncoding()
{
    uint32_t p = PERM_W;
    CHECK(normalizePerm(&p) && p == (PERM_W | PERM_R));
    p = PERM_X;
    CHECK(normalizePerm(&p) && p == (PERM_X | PERM_R));
    p = 8;
    CHECK(!normalizePerm(&p));

    CHECK(decodeLeaf(encodeLeaf(0x1000, 0)) == 0);
    CHECK(decodeLeaf(encodeLeaf(0x1000, PERM_R)) == PERM_R);
    CHECK(decodeLeaf(encodeLeaf(0x1000, PERM_R | PERM_W)) == (PERM_R | PERM_W));
    CHECK(decodeLeaf(encodeLeaf(0x1000, PERM_R | PERM_X)) == (PERM_R | PERM_X));
    CHECK(decodeLeaf(encodeLeaf(0x1000, PERM_MASK)) == PERM_MASK);
    CHECK((encodeLeaf(0x1234000, PERM_R) & E_ADDR_MASK) == 0x1234000);
    CHECK(!(encodeLeaf(0x1000, 0) & E_P));
    CHECK((encodeLeaf(0x1000, PERM_R) & E_US));
    CHECK((encodeLeaf(0x1000, PERM_R) & E_NX));            /* not executable */
    CHECK(!(encodeLeaf(0x1000, PERM_R | PERM_X) & E_NX));

    CHECK(indexAt(0, 4) == 0 && indexAt(0, 1) == 0);
    CHECK(indexAt(0x1000, 1) == 1);
    CHECK(indexAt(0x200000, 2) == 1 && indexAt(0x200000, 1) == 0);
    CHECK(indexAt(0x40000000, 3) == 1);
    CHECK(indexAt(0x8000000000ull, 4) == 1);
    CHECK(indexAt(kGpaLimit - 4096, 4) == 511 && indexAt(kGpaLimit - 4096, 1) == 511);
}

void testMapLookupUnmap()
{
    Fake f;
    {
        Table t(allocatorFor(&f));
        CHECK(t.init());
        CHECK(t.tablePages() == 1);

        const uint64_t gpas[] = { 0, 0x1000, 0x1FF000, 0x200000, 0x40000000, 0x7FFFFFFFF000ull,
                                  0x8000000000ull, kGpaLimit - 4096 };
        uint64_t hpa = 0x100000;
        for (uint64_t g : gpas) {
            CHECK(t.map(g, hpa, PERM_R | PERM_W, OWN1) == OK);
            hpa += 0x1000;
        }
        CHECK(t.mappedPages() == 8);

        hpa = 0x100000;
        for (uint64_t g : gpas) {
            uint64_t h = 0;
            uint32_t p = 0;
            CHECK(t.lookup(g, &h, &p) == OWN1);
            CHECK(h == hpa && p == (PERM_R | PERM_W));
            /* the hardware view must agree */
            uint64_t hh = 0;
            uint32_t pp = 0;
            CHECK(hwWalk(t.rootPa(), g, &hh, &pp) && hh == hpa && pp == p);
            hpa += 0x1000;
        }
        /* neighbours are not mapped, neither by us nor by the hardware */
        uint64_t hh;
        uint32_t pp;
        CHECK(!t.isMapped(0x2000) && !hwWalk(t.rootPa(), 0x2000, &hh, &pp));
        CHECK(!t.isMapped(0x3000000000ull));

        for (uint64_t g : gpas)
            CHECK(t.unmap(g) == OWN1);
        CHECK(t.mappedPages() == 0);
        CHECK(t.tablePages() == 1);   /* everything pruned back to the root */
        CHECK(t.unmap(0) == nullptr);
    }
    CHECK(f.pagesLive == 0 && f.memLive == 0);
}

void testErrors()
{
    Fake f;
    {
        Table t(allocatorFor(&f));
        CHECK(t.map(0, 0x1000, PERM_R, OWN1) == BAD_ARG);   /* no init yet */
        CHECK(t.init());
        CHECK(t.map(1, 0x1000, PERM_R, OWN1) == BAD_ARG);                 /* unaligned gpa */
        CHECK(t.map(0, 0x1001, PERM_R, OWN1) == BAD_ARG);                 /* unaligned hpa */
        CHECK(t.map(kGpaLimit, 0x1000, PERM_R, OWN1) == BAD_ARG);         /* out of range */
        CHECK(t.map(0, 1ull << 52, PERM_R, OWN1) == BAD_ARG);             /* hpa too wide */
        CHECK(t.map(0, 0x1000, 8, OWN1) == BAD_ARG);                      /* unknown perm bit */
        CHECK(t.map(0, 0x1000, PERM_R, nullptr) == BAD_ARG);              /* owner required */
        CHECK(t.tablePages() == 1);                                       /* nothing was built */

        CHECK(t.map(0x5000, 0x9000, PERM_R, OWN1) == OK);
        CHECK(t.map(0x5000, 0xA000, PERM_R, OWN2) == BUSY);
        void *o = t.lookup(0x5000, nullptr, nullptr);
        CHECK(o == OWN1);                                                 /* unchanged by the failed map */

        CHECK(t.protect(0x6000, PERM_R) == NOT_MAPPED);
        CHECK(t.protect(0x5000, 8) == BAD_ARG);
        CHECK(t.protect(kGpaLimit, PERM_R) == NOT_MAPPED);
        CHECK(t.lookup(kGpaLimit, nullptr, nullptr) == nullptr);
    }
    CHECK(f.pagesLive == 0 && f.memLive == 0);
}

void testProtect()
{
    Fake f;
    {
        Table t(allocatorFor(&f));
        CHECK(t.init());
        CHECK(t.map(0x7000, 0xB000, PERM_R | PERM_W | PERM_X, OWN1) == OK);

        const uint32_t steps[] = { PERM_R, PERM_R | PERM_W, 0, PERM_R | PERM_X, PERM_MASK };
        for (uint32_t want : steps) {
            CHECK(t.protect(0x7000, want) == OK);
            uint64_t h = 0;
            uint32_t p = 99;
            CHECK(t.lookup(0x7000, &h, &p) == OWN1);        /* still mapped, even with no rights */
            CHECK(h == 0xB000 && p == want);
            uint64_t hh;
            uint32_t pp;
            CHECK(hwWalk(t.rootPa(), 0x7000, &hh, &pp) == (want != 0));
        }
        /* W alone is promoted to R+W */
        CHECK(t.protect(0x7000, PERM_W) == OK);
        uint32_t p = 0;
        t.lookup(0x7000, nullptr, &p);
        CHECK(p == (PERM_R | PERM_W));
        /* a page with no rights can still be unmapped, which returns the owner */
        CHECK(t.protect(0x7000, 0) == OK);
        CHECK(t.unmap(0x7000) == OWN1);
        CHECK(t.tablePages() == 1);
    }
    CHECK(f.pagesLive == 0 && f.memLive == 0);
}

void testPartialPrune()
{
    Fake f;
    {
        Table t(allocatorFor(&f));
        CHECK(t.init());
        /* two pages in the same PT, one far away */
        CHECK(t.map(0x1000, 0x10000, PERM_R, OWN1) == OK);
        CHECK(t.map(0x2000, 0x11000, PERM_R, OWN1) == OK);
        CHECK(t.map(0x8000000000ull, 0x12000, PERM_R, OWN2) == OK);
        CHECK(t.tablePages() == 1 + 3 + 3);                      /* root + two chains of PDPT/PD/PT */

        CHECK(t.unmap(0x8000000000ull) == OWN2);
        CHECK(t.tablePages() == 1 + 3);                          /* far chain gone, near chain intact */
        CHECK(t.unmap(0x1000) == OWN1);
        CHECK(t.tablePages() == 1 + 3);                          /* PT still holds 0x2000 */
        CHECK(t.isMapped(0x2000));
        CHECK(t.unmap(0x2000) == OWN1);
        CHECK(t.tablePages() == 1);
    }
    CHECK(f.pagesLive == 0 && f.memLive == 0);
}

void releaseCb(void *ctx, void *owner)
{
    (*static_cast<long *>(ctx))++;
    (void)owner;
}

void testClear()
{
    Fake f;
    {
        Table t(allocatorFor(&f));
        CHECK(t.init());
        for (uint64_t i = 0; i < 1500; i++)
            CHECK(t.map(i * 4096 + 0x100000, i * 4096 + 0x7000000, PERM_R | PERM_W, OWN1) == OK);
        CHECK(t.mappedPages() == 1500);
        long released = 0;
        t.clear(releaseCb, &released);
        CHECK(released == 1500);
        CHECK(t.mappedPages() == 0 && t.tablePages() == 1);
        CHECK(!t.isMapped(0x100000));
        /* still usable after clear */
        CHECK(t.map(0x1000, 0x2000, PERM_R, OWN2) == OK);
        /* shutdown reports the remaining page and returns the root too */
        released = 0;
        t.shutdown(releaseCb, &released);
        CHECK(released == 1 && f.pagesLive == 0 && f.memLive == 0);
        t.shutdown(releaseCb, &released);        /* idempotent */
        CHECK(released == 1);
        CHECK(t.map(0x1000, 0x2000, PERM_R, OWN2) == BAD_ARG);   /* needs init() again */
        CHECK(t.init(allocatorFor(&f)) && t.map(0x1000, 0x2000, PERM_R, OWN2) == OK);
        /* the destructor releases a still-populated tree without callbacks */
    }
    CHECK(f.pagesLive == 0 && f.memLive == 0);
}

/* Fail the N-th allocation for every N and check that a failed map leaves no trace. */
void testAllocationFailure()
{
    for (long limit = 0; limit < 12; limit++) {
        Fake f;
        {
            Table t(allocatorFor(&f));
            bool ok = t.init();
            if (!ok) {
                CHECK(f.pagesLive == 0 && f.memLive == 0);
                continue;
            }
            /* one page that exists before the injected failure, so pruning has something to keep */
            CHECK(t.map(0x1000, 0x1000, PERM_R, OWN1) == OK);
            size_t pagesBefore = t.tablePages();

            f.failAfter = f.allocs + limit;
            Status s = t.map(0x8000000000ull, 0x2000, PERM_R, OWN2);   /* needs 3 new tables, 6 allocs */
            if (s == OK) {
                CHECK(t.isMapped(0x8000000000ull));
                CHECK(limit >= 6);
            } else {
                CHECK(s == NO_MEMORY);
                CHECK(t.tablePages() == pagesBefore);        /* partial chain pruned again */
                CHECK(!t.isMapped(0x8000000000ull));
                CHECK(t.isMapped(0x1000));
                uint64_t h;
                uint32_t p;
                CHECK(!hwWalk(t.rootPa(), 0x8000000000ull, &h, &p));
                CHECK(hwWalk(t.rootPa(), 0x1000, &h, &p));
                /* and it works once memory is back */
                f.failAfter = -1;
                CHECK(t.map(0x8000000000ull, 0x2000, PERM_R, OWN2) == OK);
            }
        }
        CHECK(f.pagesLive == 0 && f.memLive == 0);
    }
}

/* A fake buffer: a list of physically contiguous runs, like IOMemoryDescriptor::getPhysicalSegment sees. */
struct Run { uint64_t pa; uint64_t pages; };
struct FakeBuf { const Run *runs; unsigned n; unsigned calls; };

bool fakeSegment(void *c, uint64_t offset, uint64_t *pa, uint64_t *len)
{
    FakeBuf *b = static_cast<FakeBuf *>(c);
    b->calls++;
    uint64_t base = 0;
    for (unsigned i = 0; i < b->n; i++) {
        uint64_t bytes = b->runs[i].pages * 4096;
        if (offset < base + bytes) {
            *pa  = b->runs[i].pa + (offset - base);
            *len = bytes - (offset - base);
            return true;
        }
        base += bytes;
    }
    return false;
}

uint64_t expectedPa(const Run *runs, unsigned n, uint64_t offset)
{
    uint64_t base = 0;
    for (unsigned i = 0; i < n; i++) {
        uint64_t bytes = runs[i].pages * 4096;
        if (offset < base + bytes)
            return runs[i].pa + (offset - base);
        base += bytes;
    }
    return ~0ull;
}

/* The bug this guards against: pages of a multi-page physical run landing at the wrong guest address. */
void testMapRange()
{
    Fake f;
    {
        Table t(allocatorFor(&f));
        CHECK(t.init());

        /* Runs of 1, 2, 16, 3 and 7 pages at scattered physical addresses: 29 pages. */
        const Run runs[] = { { 0x10000000, 1 }, { 0x30000000, 2 }, { 0x50000000, 16 }, { 0x20000000, 3 }, { 0x70000000, 7 } };
        const unsigned n = sizeof(runs) / sizeof(runs[0]);
        uint64_t total = 0;
        for (unsigned i = 0; i < n; i++) total += runs[i].pages * 4096;
        FakeBuf buf = { runs, n, 0 };

        uint64_t mapped = 99;
        CHECK(t.mapRange(0x100000, total, PERM_R | PERM_W, OWN1, fakeSegment, &buf, &mapped) == OK);
        CHECK(mapped == total);
        for (uint64_t off = 0; off < total; off += 4096) {
            uint64_t h = 0;
            uint32_t p = 0;
            CHECK(t.lookup(0x100000 + off, &h, &p) == OWN1);
            CHECK(h == expectedPa(runs, n, off));          /* every page where it belongs, none skipped */
            CHECK(p == (PERM_R | PERM_W));
            uint64_t hh;
            uint32_t pp;
            CHECK(hwWalk(t.rootPa(), 0x100000 + off, &hh, &pp) && hh == h);
        }
        CHECK(!t.isMapped(0x100000 + total));                /* and nothing beyond the end */
        CHECK(t.mappedPages() == total / 4096);

        /* The same range again is refused before anything changes. */
        FakeBuf buf2 = { runs, n, 0 };
        CHECK(t.mapRange(0x100000, total, PERM_R, OWN2, fakeSegment, &buf2, &mapped) == BUSY && mapped == 0);
        CHECK(t.mapRange(0x100000 + 5 * 4096, 4096, PERM_R, OWN2, fakeSegment, &buf2, &mapped) == BUSY);
        CHECK(t.lookup(0x100000, nullptr, nullptr) == OWN1);

        /* Argument checks. */
        CHECK(t.mapRange(0x100001, 4096, PERM_R, OWN2, fakeSegment, &buf2, &mapped) == BAD_ARG);
        CHECK(t.mapRange(0x900000, 4097, PERM_R, OWN2, fakeSegment, &buf2, &mapped) == BAD_ARG);
        CHECK(t.mapRange(0x900000, 0, PERM_R, OWN2, fakeSegment, &buf2, &mapped) == BAD_ARG);

        /* The buffer is shorter than the range we ask for: BAD_SEGMENT, and `mapped` tells what to roll back. */
        FakeBuf shortBuf = { runs, 2, 0 };                    /* 3 pages */
        CHECK(t.mapRange(0x800000, 8 * 4096, PERM_R, OWN2, fakeSegment, &shortBuf, &mapped) == BAD_SEGMENT);
        CHECK(mapped == 3 * 4096);
        for (uint64_t q = 0; q < mapped; q += 4096)
            CHECK(t.unmap(0x800000 + q) == OWN2);
        CHECK(!t.isMapped(0x800000));
    }
    CHECK(f.pagesLive == 0 && f.memLive == 0);
}

/* Many random fragmentations, same invariant each time. */
void testMapRangeRandom()
{
    unsigned seed = 12345;
    auto rnd = [&seed]() { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7FFF; };
    Fake f;
    {
        Table t(allocatorFor(&f));
        CHECK(t.init());
        for (int round = 0; round < 200; round++) {
            Run runs[40];
            unsigned n = 1 + rnd() % 40;
            uint64_t total = 0;
            for (unsigned i = 0; i < n; i++) {
                runs[i].pages = 1 + rnd() % 40;
                runs[i].pa    = (0x100000000ull + (uint64_t)(rnd() % 4096) * 0x1000000ull) & E_ADDR_MASK;
                total += runs[i].pages * 4096;
            }
            FakeBuf buf = { runs, n, 0 };
            uint64_t gpa = ((uint64_t)(rnd() % 1000)) * 0x4000000ull;     /* spread over the address space */
            uint64_t mapped = 0;
            CHECK(t.mapRange(gpa, total, PERM_R | PERM_X, OWN1, fakeSegment, &buf, &mapped) == OK && mapped == total);
            for (uint64_t off = 0; off < total; off += 4096) {
                uint64_t h = 0;
                if (t.lookup(gpa + off, &h, nullptr) != OWN1 || h != expectedPa(runs, n, off)) {
                    CHECK(false);
                    break;
                }
            }
            for (uint64_t off = 0; off < total; off += 4096)
                CHECK(t.unmap(gpa + off) == OWN1);
            CHECK(t.tablePages() == 1);
        }
    }
    CHECK(f.pagesLive == 0 && f.memLive == 0);
}

/* An allocation failure in the middle of a range: `mapped` must match what is really mapped. */
void testMapRangeFailureAccounting()
{
    for (long limit = 1; limit < 40; limit += 3) {
        Fake f;
        {
            Table t(allocatorFor(&f));
            CHECK(t.init());
            const Run runs[] = { { 0x10000000, 300 } };           /* one run spanning a page-table boundary soon */
            FakeBuf buf = { runs, 1, 0 };
            f.failAfter = f.allocs + limit;
            uint64_t mapped = 0;
            Status s = t.mapRange(0x1FF000, 300 * 4096, PERM_R, OWN1, fakeSegment, &buf, &mapped);
            if (s == OK) {
                CHECK(mapped == 300 * 4096);
            } else {
                CHECK(s == NO_MEMORY);
                CHECK(t.mappedPages() == mapped / 4096);
                for (uint64_t q = 0; q < mapped; q += 4096)
                    CHECK(t.unmap(0x1FF000 + q) == OWN1);
                CHECK(t.mappedPages() == 0 && t.tablePages() == 1);
            }
        }
        CHECK(f.pagesLive == 0 && f.memLive == 0);
    }
}

} // namespace

int main()
{
    testEncoding();
    testMapLookupUnmap();
    testErrors();
    testProtect();
    testPartialPrune();
    testClear();
    testAllocationFailure();
    testMapRange();
    testMapRangeRandom();
    testMapRangeFailureAccounting();
    if (fails) {
        printf("npt_test: %d FAILED\n", fails);
        return 1;
    }
    printf("npt_test: OK\n");
    return 0;
}
