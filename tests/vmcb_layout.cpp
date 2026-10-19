/*
 * Host-side check of the VMCB layout. The real work is the static_asserts in
 * kext/Source/VMCB.hpp: if this file compiles, every offset matches the AMD
 * manual. The runtime part only checks the attribute helper.
 */
#include <stdio.h>
#include <stdlib.h>

#include "../kext/Source/VMCB.hpp"

using namespace rhv::vmcb;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

int main()
{
    /* VMX access rights for a 16-bit real-mode code segment: type 0xB, S, P = 0x9B; no AVL/L/D/G. */
    CHECK(segAttrib(0x9B) == ATTR_REAL_CODE);

    /* A flat 32-bit code segment in VMX layout: 0xC09B (G=1, D=1) -> VMCB 0xC9B. */
    CHECK(segAttrib(0xC09B) == 0xC9B);

    /* 64-bit code segment (L=1, G=1): VMX 0xA09B -> VMCB 0xA9B. */
    CHECK(segAttrib(0xA09B) == 0xA9B);

    puts("vmcb_layout: OK");
    return 0;
}
