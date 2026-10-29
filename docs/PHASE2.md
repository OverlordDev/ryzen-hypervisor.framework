# Phase 2: guest memory from process memory

Goal: the guest executes and reads memory that belongs to a **userland process** (what
`hv_vm_map` / `hv_vm_unmap` / `hv_vm_protect` do), and permission violations arrive as an NPT fault that the
process can handle and then retry the instruction.

> **Status: done on hardware (5800H): `rhv-map-test` prints `PASS: all stages`.** The host tests of the NPT tree (`tests/npt_test.cpp`) check the logic, while page pinning, NPF and `protect`/`unmap` are confirmed by a real run. `make test`
> runs `tests/npt_test.cpp` (ASan + UBSan). The part about IOMemoryDescriptor, page pinning and
> a real NPF can only be checked on hardware: `sudo rhv-map-test`. Do not run it until phase 1
> (`rhv-guest-test` prints `PASS`) has passed on your machine.

## What was added

| what | where |
|---|---|
| the NPT tree with 4 KiB pages, plain C++ | `kext/Source/Npt.hpp` |
| `map` / `unmap` / `protect` of the VM | `kext/Source/Vm.cpp` |
| the selectors `VmMap`, `VmUnmap`, `VmProtect`, the flag `VmCreate(NO_RAM)` | `include/RyzenHVShared.h`, `RyzenHVUserClient.cpp` |
| decoding the NPF reason (`rhv_npf_*`) | `include/RyzenHVShared.h` |
| tests | `tests/npt_test.cpp` (host), `tools/rhv-map-test` (hardware) |

The ABI is now **version 4**: `VmCreate` gained an input scalar (flags), and `RHVStats` has six new counters.
An old `rhv-info` with a new kext (and vice versa) warns about the version mismatch.

## How it works

```
VmMap(uva, gpa, size, flags)
  └─ IOMemoryDescriptor::withAddressRange(uva, size, task)      the caller's process
       └─ prepare()                                              pages are pinned (wired)
            └─ getPhysicalSegment() piece by piece                physical addresses
                 └─ Npt::map(gpa + i*4K, pa + i*4K, perm, backing)   page by page, all or nothing
```

- **Backing** is a pinned buffer with a counter of live pages. Every NPT entry remembers its
  backing (the pointer sits in the tree leaf). When the last page is removed, the buffer is unpinned
  (`complete`) and released. That is why a **partial `unmap`** of the middle or the edge of a mapping works.
- `map` is atomic: first it checks that the whole range is free, and on any error in the middle
  the pages already placed are removed. An overlap gives `kIOReturnBusy`.
- `unmap` tolerates holes in the range. `protect` requires all pages to be mapped, and then
  changes all of them; otherwise it changes nothing (`kIOReturnNotFound`).
- Empty page tables are freed immediately (`Npt::prune`), so a VM with everything removed
  holds a single root page.
- TLB: `tlbCtl = FLUSH_ALL` on every `VMRUN`, so no separate invalidations are needed after
  `unmap`/`protect`. Tables may be changed only while the guest is not running: `VmRun` holds
  the same lock.
- `VmMap` accepts an address only from the process that opened the connection (`current_task() == owner`).
  Otherwise a process that was handed the connection port could pin other people's addresses.

### Permissions

`RHV_MEM_READ/WRITE/EXEC` match `hv_memory_flags_t`. x86 cannot do "write only" and
"execute only", so `WRITE` and `EXEC` imply `READ`. Permissions `0` are allowed in
`VmProtect`: the page stays mapped, but any access faults. Execution
is forbidden with the NX bit in the NPT, which works when `EFER.NXE=1` on the host (it is on macOS).

### NPF reason

`info1` is an error code like `#PF`'s, and `info2` is the **guest physical address** (checked on Zen 3, 5800H: the address arrives exact, including the offset inside the page):

| `info1` bit | meaning | helper |
|---|---|---|
| 0 | the page was mapped, permissions were insufficient | `rhv_npf_present` |
| 1 | write | `rhv_npf_is_write` |
| 4 | instruction fetch | `rhv_npf_is_fetch` |
| 33 | fault while reading the **guest's** page tables (not data) | `rhv_npf_in_walk` |

The handler in userland: look at the address, `VmMap` / `VmProtect`, run `VmRun` with `skip=0`
(the instruction is repeated).

## How to run

```
make kext tools DEBUG=1
sudo ./build/rhv-map-test
./build/rhv-info stats       # vmMaps, vmUnmaps, vmProtects, vmNptFaults, vmPagesMapped
```

`rhv-map-test` goes through seven stages and prints `ok:` / `FAIL` for every check: the guest's write
is visible in your buffer; `protect` gives a fault; a lazy `map` from the handler; `unmap` and swapping the
buffer; refusals for bad arguments; a VM without built-in memory; the pinned-page counter
after `VmDestroy` returned to its initial value (no leak).

## How to read failures

| symptom | what it means |
|---|---|
| `VmMap` returns `kIOReturnVMError` | the user address is not mapped or not writable; or `getPhysicalSegment` returned 0 |
| `VmMap` returns `kIOReturnBusy` | the range overlaps one already mapped (including the built-in 1 MiB) |
| `VmMap` returns `kIOReturnNoMemory` | not enough pages for NPT tables; for 1 GiB about 4 MiB of tables + bookkeeping memory are used |
| stage 2 `FAIL ... no NPF` | the guest wrote to a read-only page: check that `normalizePerm`/the NPT entry set `RW=0`, and that the host has no `CR0.WP` peculiarities |
| stage 6 `FAIL ... fetch fault` | NX in the NPT has no effect; look at the host's `EFER.NXE` |
| `stage 7 FAIL: counter` | a leak of pinned pages: look for a path where `dropPage` is not called |

## What is not done

- No large pages (2 MiB / 1 GiB): each guest page costs one entry and 8 bytes
  of bookkeeping memory. A VM of 8 GiB means about 32 MiB of tables; acceptable for QEMU, to be optimized later.
- No limit on the total size of mappings per user (only `RHV_MAP_MAX_SIZE` per call).
- The guest cannot yet do paging in long mode with interrupts: the roadmap's acceptance criterion
  ("a guest with paging executes code from mapped memory") will be closed by phase 3 together with interrupt injection and
  FPU state. The `rhv-map-test` stage covers what is required of the NPT for that.
