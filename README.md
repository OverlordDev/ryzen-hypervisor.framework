# ryzen-hypervisor.framework

An attempt to make `Hypervisor.framework` work on a **Ryzentosh** (a hackintosh on an AMD Ryzen).

The native `Hypervisor.framework` on Intel Macs is built on Intel VT-x (VMX), so on a Ryzen
`kern.hv_support` is 0 and everything that depends on it (QEMU/HVF, Docker Desktop, Android
emulators, current VMware/Parallels) does not start. This project builds a layer on **AMD-V (SVM)**
that provides the same API.

> **Status: Docker Desktop and Windows 11 in Parallels Desktop run on a Ryzen** (Ryzen 7 5800H, Sequoia 15.8.1, OpenCore, up to 4 vCPUs,
> guest installation and use, keyboard, network). Phases 0–2 are done (SVM on all cores, a guest executes code, process memory
> as guest memory, NPT) and so is phase 3 (the VMX API compatibility layer: QEMU/HVF boots Linux, Parallels boots Windows).
> **Docker Desktop (Intel build 4.97.0) runs through HyperKit: `docs/DOCKER-DESKTOP.md`.**
> **New in 0.7 (experimental, opt-in): native mode.** With the boot-arg `rhv_hv=1` the kext is the hypervisor provider itself and
> Docker Desktop runs with Docker's own unmodified files: no shim, no wrappers, nothing re-signed. See `docs/NATIVE.md`.
> **New in 0.7.1 (experimental): Parallels in native mode** (`./rhv parallels native`): Windows 10 boots on Apple's own Hypervisor.framework with 1 and
> 4 vCPUs (no keyboard yet in 0.7.1). Also: the kext checks `EFER.SVME` on the CPU before every `VMRUN` (a panic report on macOS 26,
> issue 1), and `./rhv doctor` says when it is not run from the unpacked archive. See `docs/NATIVE.md`.
> **New in 0.7.3: native mode offers the TPR shadow** (Parallels boots Windows 10 to the desktop in ~20 s, VirtualBox 7.x runs through the kext, see `docs/NATIVE.md`).
> **New in 0.7.2: the keyboard works in a native Parallels VM** (the kext is unchanged), **`./rhv autostart install`** turns `kern.hv_support` on at every
> boot (a launchd service), and the RyzenHV app has an "Automatic start" page and a native-mode button for Parallels.
> **How to install Parallels with this kext: `docs/PARALLELS.md`.** It works on the exact Parallels build 26.5.0 (57613).
> **A ready-made build without compiling: the archive from Releases and the `./rhv` installer** (kext into the EFI, Parallels, Docker, `./rhv doctor`),
> see `START-HERE.md` inside the archive.
> Not verified: sleep/wake, `kextunload`, an hour of stability, guests other than Windows 11. Parallels 3D acceleration crashes
> inside NVIDIA's Metal driver on the author's machine, keep it off.
> If something crashes at boot, start with `docs/DEBUGGING.md` (`rhv_stage`, the OpenCore log, `check-kext-symbols.sh`).

Target hardware: Ryzen 7 5800H (Zen 3, Cezanne, family 19h model 50h), macOS Sequoia (15), OpenCore.

## Layout

```
include/RyzenHVShared.h   ABI between the kext, the shim and the tools (selectors, RHVInfo)
include/RHVRing.h         ring buffer for the log (plain C, tested on the host)
kext/                     RyzenHV.kext: SVM, log, counters, sleep/wake, VMCB, VMRUN, NPT (Npt.hpp), IOUserClient
shim/                     userland Hypervisor.framework layer: hv_shim.c (hv_* interposition), rhv_core.c (VMX API semantics, cache,
                          multiple vCPUs), rhv_iokit.c (talks to the kext), hijack.c (injection into Parallels)
tools/rhv-info/           tool: info, log, counters, self-test
tools/rhv-guest-test/     phase 1 acceptance test: a guest prints a string, runs CPUID, halts
tools/rhv-map-test/       phase 2 acceptance test: guest memory = process memory, protect/unmap, NPF
tools/rhv-vmx-test/       phase 3: a guest driven through VMCS fields, the way a Hypervisor.framework client does it
tools/rhv-long-test/      phase 3: a 64-bit guest with paging, x87/SSE/AVX, host interrupts
tools/hvf-smoke/          phase 3: a mini-VMM on the PUBLIC Hypervisor.framework API, runs through the shim (scripts/run-with-shim.sh)
scripts/                  collect-debug.sh (report for analysis), install-to-efi.sh, parallels-shim.sh (Parallels), docker-desktop-shim.sh (Docker Desktop), qemu-hvf-test.sh, docker-hvf-test.sh
tests/host_test.c         unit tests of the pure functions, run anywhere
docs/                     architecture, roadmap, Ryzentosh, debugging, reverse engineering
```

## Quick start

1. Read `docs/RYZENTOSH.md`: enable SVM in the BIOS, make a **fallback OpenCore entry without the kext**.
2. `make test`: check that the pure logic builds (works on any OS).
3. `make kext tools`: build `build/RyzenHV.kext` and `build/rhv-info`.
4. Put the kext into `EFI/OC/Kexts`, add an entry under `Kernel -> Add`, reboot.
5. `./build/rhv-info`: see the CPU, SVM, ASIDs and features (NPT, NRIPS, VmcbClean, ...).
6. To turn SVM on for all cores add the boot-arg `rhv_enable_svm=1`. By default the kext only reads.
7. If something goes wrong, `./scripts/collect-debug.sh` gathers everything needed for analysis into one file.
   If the system does not boot, add the boot-arg `-rhv_off` (the kext does nothing).

## Diagnostics

```
rhv-info                  CPU / SVM
rhv-info log [-f]         kernel ring log (-f: follow)
rhv-info stats            event counters
sudo rhv-info level 0-5   log level on the fly
sudo rhv-info selftest    check EFER/VM_HSAVE_PA on every core
```

Boot-args: `-rhv_off`, `rhv_enable_svm`, `rhv_selftest`, `rhv_loglevel`, `rhv_iolog`, `rhv_stage`, `rhv_slow`, and for native mode `rhv_hv`, `rhv_hv_sysctl`, `rhv_clientpatch`, `rhv_tpr_shadow`. Details in `docs/DEBUGGING.md`.

If step 5 says `SVM: supported`, `BIOS disable: no`, `NPT: [x]`, phase 0 is done
and you can write a VMCB and `VMRUN` (see `docs/ROADMAP.md`).

## Documentation

- `docs/ARCHITECTURE.md`: how the layers work and why
- `docs/ROADMAP.md`: phases of work and their acceptance criteria
- `docs/RYZENTOSH.md`: BIOS, OpenCore, how not to kill the system
- `docs/DEBUGGING.md`: boot-args, logs, counters, self-test, what to do on a panic
- `docs/PHASE1.md`: how the first guest works, how to run it and how to read failures
- `docs/PHASE2.md`: mapping process memory into the guest, NPF, how to run it and how to read failures
- `docs/PARALLELS.md`: **installation and use with Parallels Desktop**, debugging, limits
- `docs/NATIVE.md`: **native mode** (0.7, experimental): the kext as the hypervisor provider, Docker Desktop without the shim, limits
- `docs/DOCKER-DESKTOP.md`: **Docker Desktop** on an AMD CPU (HyperKit through the shim)
- `docs/DOCKER.md`: a small Linux guest with dockerd in QEMU/HVF (`scripts/docker-hvf-test.sh`)
- `docs/PHASE3.md`: the VMX API compatibility layer and the progress on QEMU and Parallels
- `docs/REVERSING.md`: how to extract and study the native `Hypervisor.framework`

## License

MIT, see `LICENSE`. You may use, change and redistribute the code, also in your own projects, as long as the copyright notice
and the license text stay with it, so the author is always credited. The bundle ships QEMU, which has its own licenses
(`LICENSES/`). The repository contains nothing from Parallels or Docker. If you port code from other projects, keep their
copyright: bhyve (FreeBSD, BSD) can be ported with its copyright kept; Linux KVM is GPL: copying code is not allowed,
reading it as a reference is.
