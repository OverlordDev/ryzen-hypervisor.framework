/*
 * PhysMem.hpp - zeroed, wired, physically contiguous, 4K-aligned kernel memory
 * with both a kernel virtual address and a physical address. Used for the
 * VMCB, the permission bitmaps and the nested page tables.
 */
#ifndef RYZENHV_PHYSMEM_HPP
#define RYZENHV_PHYSMEM_HPP

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <stdint.h>

namespace rhv {

struct PhysMem {
    IOBufferMemoryDescriptor *md;
    void                     *va;
    uint64_t                  pa;
    size_t                    size;
};

/* `size` is rounded up to a multiple of 4096. Returns false (and logs) on failure. */
bool physAlloc(PhysMem *m, size_t size);
void physFree(PhysMem *m);

} // namespace rhv

#endif /* RYZENHV_PHYSMEM_HPP */
