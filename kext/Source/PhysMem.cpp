#include <IOKit/IOLib.h>

#include "Log.hpp"
#include "PhysMem.hpp"

namespace rhv {

bool physAlloc(PhysMem *m, size_t size)
{
    bzero(m, sizeof(*m));
    size = (size + 4095) & ~(size_t)4095;

    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, size,
        0x000FFFFFFFFFF000ULL);
    if (!md) {
        RHV_ERR("physAlloc(%zu): out of contiguous memory", size);
        return false;
    }

    void *va = md->getBytesNoCopy();
    bzero(va, size);

    IOByteCount len = 0;
    addr64_t pa = md->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
    if (!pa || (pa & 0xFFF) || len < size) {
        RHV_ERR("physAlloc(%zu): bad physical segment pa=0x%llx len=%llu", size,
                (unsigned long long)pa, (unsigned long long)len);
        md->release();
        return false;
    }

    m->md   = md;
    m->va   = va;
    m->pa   = pa;
    m->size = size;
    return true;
}

void physFree(PhysMem *m)
{
    if (m->md)
        m->md->release();
    bzero(m, sizeof(*m));
}

} // namespace rhv
