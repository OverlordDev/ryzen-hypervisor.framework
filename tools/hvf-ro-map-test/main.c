/* A read-only mapping, like a firmware image the Virtualization framework maps from a file (issue 14):
 * mmap(PROT_READ) of a file, then hv_vm_map(READ|EXEC). The kext used to fail it with "cannot pin the buffer: 0x2".
 *   make tools; build/hvf-ro-map-test
 */
#include <Hypervisor/Hypervisor.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    char path[] = "/tmp/rhv-ro-map-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 2; }
    unlink(path);
    char page[4096];
    memset(page, 0x90, sizeof(page));
    for (int i = 0; i < 16; i++)                      /* 64 KiB */
        if (write(fd, page, sizeof(page)) != sizeof(page)) { perror("write"); return 2; }
    void *ro = mmap(NULL, 16 * 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (ro == MAP_FAILED) { perror("mmap"); return 2; }

    hv_return_t r = hv_vm_create(HV_VM_DEFAULT);
    if (r != HV_SUCCESS) { printf("hv_vm_create: 0x%x\n", r); return 2; }
    /* What a program asks for: read+execute, and read+write+execute (the kext then maps it without write). */
    static const struct { hv_memory_flags_t flags; const char *name; } cases[] = {
        { HV_MEMORY_READ | HV_MEMORY_EXEC, "READ|EXEC" },
        { HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC, "READ|WRITE|EXEC" },
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        r = hv_vm_map(ro, 0xfff00000, 16 * 4096, cases[i].flags);
        printf("read-only file mapping, %-16s: hv_vm_map = 0x%x (%s)\n", cases[i].name, r, r == HV_SUCCESS ? "ok" : "FAILED");
        if (r != HV_SUCCESS) { bad++; continue; }
        if (hv_vm_unmap(0xfff00000, 16 * 4096) != HV_SUCCESS) { printf("hv_vm_unmap failed\n"); bad++; }
    }
    hv_vm_destroy();
    return bad ? 1 : 0;
}
