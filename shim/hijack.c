/*
 * hijack.c - lets the shim into a process that does not honor DYLD_INSERT_LIBRARIES.
 *
 * Parallels' VM process (prl_vm_app) is a hardened-runtime binary: dyld ignores DYLD_INSERT_LIBRARIES for it, and its
 * dispatcher refuses a process whose signature is not Parallels' own, so neither inserting nor re-signing works.
 * What is left is a library the process loads by itself. The shim is built under the name of one of them
 * (libPrlXmlModel.1.dylib, loaded at launch from the app's Frameworks directory), re-exporting the real library, so
 * the process keeps working. This constructor then does by hand what dyld would have done for an inserted library: for
 * every image of the process (the ones loaded now, and each one loaded later, such as Parallels' libMonitor) it
 * rewrites the import pointers (lazy and non-lazy symbol pointers) of the functions the shim replaces, by name. The
 * technique is the one Facebook's fishhook uses; dyld_dynamic_interpose was tried first and does not reach the
 * bindings of images that are already loaded.
 *
 * Only the process named in RHV_HIJACK_PROCESS gets any of this; in every other process that loads the library (the
 * dispatcher, the GUI, ...) it does nothing.
 */
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef RHV_HIJACK_PROCESS
#define RHV_HIJACK_PROCESS "prl_vm_app"
#endif

struct rhv_bind {
    const char *name;           /* the function, without the leading underscore */
    const void *replacement;
};

extern const struct mach_header_64 _mh_dylib_header;
void rhv_shim_init(void);

static const struct rhv_bind *g_binds;
static size_t g_nbinds;

static const void *replacement_for(const char *symbol)
{
    if (symbol[0] == '_')
        symbol++;
    for (size_t i = 0; i < g_nbinds; i++)
        if (strcmp(g_binds[i].name, symbol) == 0)
            return g_binds[i].replacement;
    return NULL;
}

static void rewrite_section(const struct section_64 *sect, intptr_t slide, const struct nlist_64 *symtab,
                            const char *strtab, const uint32_t *indirect, int is_const)
{
    const uint32_t *indices = indirect + sect->reserved1;
    void **ptrs = (void **)(sect->addr + slide);
    size_t count = (size_t)(sect->size / sizeof(void *));
    for (size_t i = 0; i < count; i++) {
        uint32_t idx = indices[i];
        if (idx == INDIRECT_SYMBOL_ABS || idx == INDIRECT_SYMBOL_LOCAL || idx == (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS))
            continue;
        const char *name = strtab + symtab[idx].n_un.n_strx;
        const void *repl = replacement_for(name);
        if (!repl || ptrs[i] == repl)
            continue;
        if (is_const) {                       /* __DATA_CONST is read-only once dyld is done with it */
            vm_address_t page = (vm_address_t)&ptrs[i] & ~(vm_address_t)(vm_page_size - 1);
            if (vm_protect(mach_task_self(), page, vm_page_size, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS)
                continue;
        }
        ptrs[i] = (void *)repl;
    }
}

static void on_image(const struct mach_header *mh, intptr_t slide)
{
    if (mh == (const struct mach_header *)&_mh_dylib_header || mh->magic != MH_MAGIC_64)
        return;
    /* The system's own libraries live in the shared cache and are not where the calls we replace come from. */
    Dl_info info;
    if (dladdr(mh, &info) && info.dli_fname &&
        (strncmp(info.dli_fname, "/System/", 8) == 0 || strncmp(info.dli_fname, "/usr/lib/", 9) == 0))
        return;

    const struct mach_header_64 *h = (const struct mach_header_64 *)mh;
    const struct symtab_command *symtab_cmd = NULL;
    const struct dysymtab_command *dysymtab_cmd = NULL;
    const struct segment_command_64 *linkedit = NULL;
    const struct load_command *lc = (const struct load_command *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd == LC_SYMTAB)
            symtab_cmd = (const struct symtab_command *)lc;
        else if (lc->cmd == LC_DYSYMTAB)
            dysymtab_cmd = (const struct dysymtab_command *)lc;
        else if (lc->cmd == LC_SEGMENT_64 && strcmp(((const struct segment_command_64 *)lc)->segname, SEG_LINKEDIT) == 0)
            linkedit = (const struct segment_command_64 *)lc;
    }
    if (!symtab_cmd || !dysymtab_cmd || !linkedit || !dysymtab_cmd->nindirectsyms)
        return;

    uintptr_t linkedit_base = (uintptr_t)slide + linkedit->vmaddr - linkedit->fileoff;
    const struct nlist_64 *symtab = (const struct nlist_64 *)(linkedit_base + symtab_cmd->symoff);
    const char *strtab = (const char *)(linkedit_base + symtab_cmd->stroff);
    const uint32_t *indirect = (const uint32_t *)(linkedit_base + dysymtab_cmd->indirectsymoff);

    lc = (const struct load_command *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64)
            continue;
        const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
        if (strcmp(seg->segname, SEG_DATA) != 0 && strcmp(seg->segname, "__DATA_CONST") != 0 &&
            strcmp(seg->segname, "__AUTH_CONST") != 0 && strcmp(seg->segname, "__AUTH") != 0)
            continue;
        const struct section_64 *sect = (const struct section_64 *)(seg + 1);
        for (uint32_t j = 0; j < seg->nsects; j++, sect++) {
            uint32_t type = sect->flags & SECTION_TYPE;
            if (type == S_LAZY_SYMBOL_POINTERS || type == S_NON_LAZY_SYMBOL_POINTERS)
                rewrite_section(sect, slide, symtab, strtab, indirect,
                                strcmp(seg->segname, SEG_DATA) != 0 /* *_CONST */);
        }
    }
}

void rhv_hijack_log(const char *msg);

/*
 * Parallels refuses a host whose CPUID vendor is not GenuineIntel, in prl_vm_app itself: a `je` after the comparison
 * (and the same code in libMonitor, which the private copy patches in its file). prl_vm_app cannot be changed on disk,
 * its signature is what the dispatcher checks, so the branch is made unconditional in memory instead. The bytes are
 * checked first: a different build of Parallels is left alone.
 */
static void patch_vendor_check(void)
{
    const uintptr_t kJe = 0x1000bdc65ull;                       /* Parallels 26.5.0 (57613), x86_64 */
    uint8_t *p = (uint8_t *)(kJe + (uintptr_t)_dyld_get_image_vmaddr_slide(0));
    static const uint8_t expect[] = { 0x48, 0x09, 0xc2, 0x74, 0x5d };       /* or %rax,%rdx ; je +0x5d */
    if (memcmp(p - 3, expect, sizeof(expect)) != 0) {
        rhv_hijack_log("vendor check: not the expected code, left alone");
        return;
    }
    vm_address_t page = (vm_address_t)p & ~(vm_address_t)(vm_page_size - 1);
    kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size, 0,
                                  VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) {
        rhv_hijack_log("vendor check: could not make the code writable");
        return;
    }
    *p = 0xEB;                                                  /* je -> jmp */
    vm_protect(mach_task_self(), page, vm_page_size, 0, VM_PROT_READ | VM_PROT_EXECUTE);
    rhv_hijack_log("vendor check: patched, every CPU vendor takes the Intel path");
}

__attribute__((constructor)) static void rhv_hijack_init(void)
{
    const char *prog = getprogname();
    if (!prog || strcmp(prog, RHV_HIJACK_PROCESS) != 0)
        return;

    unsigned long size = 0;
    g_binds = (const struct rhv_bind *)getsectiondata(&_mh_dylib_header, "__DATA", "__rhvtuples", &size);
    g_nbinds = size / sizeof(struct rhv_bind);
    if (!g_binds || !g_nbinds)
        return;

    rhv_shim_init();
    patch_vendor_check();
    _dyld_register_func_for_add_image(on_image);     /* reports the images already loaded, then every new one */
}
