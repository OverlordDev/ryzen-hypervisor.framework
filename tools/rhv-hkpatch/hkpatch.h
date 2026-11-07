/* Shared by tools/rhv-hkpatch/main.c and tests/hkpatch_test.c (which build this file with -DRHV_HKPATCH_NO_MAIN). */
#ifndef RHV_HKPATCH_H
#define RHV_HKPATCH_H

#include <stddef.h>
#include <stdint.h>

/* Patches one Mach-O 64 slice at buf[off..off+size). 0 = both patches applied or already in place, 1 = the
 * slice must not be used (a pattern missing or ambiguous), -1 = not a little-endian Mach-O 64 slice. */
int hkpatch_slice(uint8_t *buf, size_t len, size_t off, size_t size, const char *label);

/* Patches every x86_64 slice of buf (thin or universal). 0 = every x86_64 slice patched and at least one seen. */
int hkpatch_buffer(uint8_t *buf, size_t len, const char *label);

#endif
