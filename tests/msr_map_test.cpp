/* msr_map_test.cpp - host tests for kext/Source/MsrMap.hpp. */
#include <stdio.h>
#include <string.h>

#include "../kext/Source/MsrMap.hpp"

using namespace rhv::msr;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main()
{
    /* Where the MSRs live in the VMCB (offsets from the AMD manual: save area starts at 0x400). */
    Info i = info(MSR_LSTAR);            CHECK(i.kind == K_VMCB && i.vmcbOff == 0x400 + 0x208 && i.vmcbSize == 8);
    i = info(MSR_STAR);                  CHECK(i.kind == K_VMCB && i.vmcbOff == 0x400 + 0x200);
    i = info(MSR_CSTAR);                 CHECK(i.kind == K_VMCB && i.vmcbOff == 0x400 + 0x210);
    i = info(MSR_SFMASK);                CHECK(i.kind == K_VMCB && i.vmcbOff == 0x400 + 0x218);
    i = info(MSR_KERNEL_GS_BASE);        CHECK(i.kind == K_VMCB && i.vmcbOff == 0x400 + 0x220);
    i = info(MSR_SYSENTER_CS);           CHECK(i.kind == K_VMCB && i.vmcbOff == 0x400 + 0x228 && i.valueBits == 32);
    i = info(MSR_SYSENTER_ESP);          CHECK(i.kind == K_VMCB && i.vmcbOff == 0x400 + 0x230);
    i = info(MSR_SYSENTER_EIP);          CHECK(i.kind == K_VMCB && i.vmcbOff == 0x400 + 0x238);
    i = info(MSR_PAT);                   CHECK(i.kind == K_VMCB && i.vmcbOff == 0x400 + 0x268);
    i = info(MSR_FS_BASE);               CHECK(i.kind == K_VMCB && i.vmcbOff == 0x400 + 0x040 + 8);
    i = info(MSR_GS_BASE);               CHECK(i.kind == K_VMCB && i.vmcbOff == 0x400 + 0x050 + 8);
    i = info(MSR_EFER);                  CHECK(i.kind == K_EFER && i.vmcbOff == 0x400 + 0x0D0);
    i = info(MSR_TSC);                   CHECK(i.kind == K_TSC);
    CHECK(info(0x1B).kind == K_UNKNOWN); /* APIC base: the client emulates it */
    CHECK(info(0xC0000103).kind == K_UNKNOWN); /* TSC_AUX: the host uses it, never native */
    CHECK(info(0x80).kind == K_UNKNOWN);

    /* MSRPM layout, checked against the manual's examples. */
    uint32_t off; unsigned bit;
    CHECK(msrpmBit(0x00000000, &off, &bit) && off == 0x0000 && bit == 0);
    CHECK(msrpmBit(0x00000001, &off, &bit) && off == 0x0000 && bit == 2);
    CHECK(msrpmBit(0x00000004, &off, &bit) && off == 0x0001 && bit == 0);
    CHECK(msrpmBit(0x00001FFF, &off, &bit) && off == 0x07FF && bit == 6);
    CHECK(msrpmBit(0xC0000000, &off, &bit) && off == 0x0800 && bit == 0);
    CHECK(msrpmBit(0xC0000080, &off, &bit) && off == 0x0800 + 0x20 && bit == 0);    /* EFER */
    CHECK(msrpmBit(0xC0000081, &off, &bit) && off == 0x0800 + 0x20 && bit == 2);    /* STAR */
    CHECK(msrpmBit(0xC0000082, &off, &bit) && off == 0x0800 + 0x20 && bit == 4);    /* LSTAR */
    CHECK(msrpmBit(0xC0000102, &off, &bit) && off == 0x0800 + 0x40 + 0 && bit == 4);  /* KERNEL_GS_BASE: (0x102*2)=0x204 -> byte 0x40, bit 4 */
    CHECK(msrpmBit(0xC0000103, &off, &bit) && off == 0x0800 + 0x40 && bit == 6);    /* TSC_AUX */
    CHECK(msrpmBit(0xC0010000, &off, &bit) && off == 0x1000 && bit == 0);
    CHECK(msrpmBit(0xC0011FFF, &off, &bit) && off == 0x17FF && bit == 6);
    CHECK(!msrpmBit(0x2000, &off, &bit) && !msrpmBit(0xBFFFFFFF, &off, &bit) && !msrpmBit(0xC0002000, &off, &bit) &&
          !msrpmBit(0xC0012000, &off, &bit) && !msrpmBit(0xFFFFFFFF, &off, &bit));

    /* Making an MSR native clears exactly its two bits. */
    uint8_t map[kMsrpmSize];
    memset(map, 0xFF, sizeof(map));
    CHECK(!isNative(map, MSR_LSTAR));
    CHECK(setNative(map, MSR_LSTAR, true));
    CHECK(isNative(map, MSR_LSTAR));
    CHECK(!isNative(map, MSR_STAR) && !isNative(map, MSR_CSTAR) && !isNative(map, MSR_EFER));   /* neighbours untouched */
    size_t cleared = 0;
    for (size_t b = 0; b < sizeof(map); b++)
        for (int k = 0; k < 8; k++)
            if (!((map[b] >> k) & 1))
                cleared++;
    CHECK(cleared == 2);
    CHECK(setNative(map, MSR_LSTAR, false));
    CHECK(!isNative(map, MSR_LSTAR));
    for (size_t b = 0; b < sizeof(map); b++)
        CHECK(map[b] == 0xFF);
    CHECK(!setNative(map, 0x2000, true) && !setNative(map, 0x40000000, true));

    /* The whole supported set at once. */
    static const uint32_t all[] = { MSR_SYSENTER_CS, MSR_SYSENTER_ESP, MSR_SYSENTER_EIP, MSR_PAT, MSR_STAR, MSR_LSTAR,
                                    MSR_CSTAR, MSR_SFMASK, MSR_FS_BASE, MSR_GS_BASE, MSR_KERNEL_GS_BASE };
    for (uint32_t m : all)
        CHECK(setNative(map, m, true) && isNative(map, m));
    cleared = 0;
    for (size_t b = 0; b < sizeof(map); b++)
        for (int k = 0; k < 8; k++)
            if (!((map[b] >> k) & 1))
                cleared++;
    CHECK(cleared == 2 * (sizeof(all) / sizeof(all[0])));

    if (fails) { printf("msr_map_test: %d FAILED\n", fails); return 1; }
    printf("msr_map_test: OK\n");
    return 0;
}
