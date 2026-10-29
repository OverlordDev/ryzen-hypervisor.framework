# Reverse engineering the native Hypervisor.framework

Goal: understand which calls the native framework makes into the kernel, to decide whether kernel-side interception is realistic
(front end A in `ARCHITECTURE.md`) and which ABI has to be repeated.

All paths and commands below are written from memory and may differ on your macOS version. Check them.

## What to extract

Frameworks in macOS 11+ are not separate files but live in the dyld shared cache. On Intel Sequoia it is usually here:

```
ls /System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/
```

A file like `dyld_shared_cache_x86_64h`.

It can be extracted with, for example, `dyld-shared-cache-extractor` or `ipsw dyld extract`
(installed separately, not part of macOS):

```
mkdir -p reversing/dsc
dyld-shared-cache-extractor /System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h reversing/dsc
```

The binary you need: `.../System/Library/Frameworks/Hypervisor.framework/Versions/A/Hypervisor`.
The `reversing/` folder is in `.gitignore`: **Apple binaries are not committed to the repository.**

## What to look at

1. `nm`/`otool -tV`: the list of `_hv_*` exports and where they jump to.
2. In a disassembler (Ghidra or Hopper) find how `hv_vm_create` and `hv_vcpu_run` reach the kernel:
   a mach trap, a syscall, an ioctl or an IOKit user client. The whole of option A depends on the answer.
3. The structures passed into the kernel (sizes, offsets) and the return codes.
4. Then the kernel: the Kernel Debug Kit for your build (if available) or `kmutil`/`nm` on the kernel,
   searching for symbols with `hv`. This shows the name of the implementation on the kernel side.

## What to record

Put findings in `docs/reversing-notes/<macos-build>.md` (without pieces of binaries): the list of calls,
signatures, trap/syscall numbers, the macOS version, where it differs from the previous version.

## Legal note

Studying an interface for compatibility on your own hardware is usually permissible, but do not copy
Apple's code into the project and do not publish extracted binaries. For an exact assessment in your jurisdiction,
ask a lawyer.
