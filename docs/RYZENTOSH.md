# Ryzentosh: preparation, booting, debugging

Target machine: Ryzen 7 5800H, macOS Sequoia, OpenCore.

## 0. Safety net (do this before anything else)

A bug in a virtualization kext can bring the kernel down at boot.

- Save a working copy of `EFI/` to a USB stick.
- Keep a **second entry** in OpenCore (a second `config.plist` or a profile) **without** RyzenHV.kext.
- Do not enable `rhv_enable_svm=1` right away. Probe only at first.

## 1. BIOS

Enable SVM (on laptops it is often called `SVM Mode` and may be hidden in advanced menus).
If there is no such option, `rhv-info` shows `BIOS disable: YES`, and the kext cannot fix that.

## 2. Building

```
make test           # the pure logic, must pass
make kext tools     # on the hackintosh itself, needs Xcode / Command Line Tools
```

`kext/Makefile` is best effort. If it fails, send the error or build the kext with an Xcode project.

## 3. OpenCore: Kernel → Add

Copy `build/RyzenHV.kext` to `EFI/OC/Kexts/`. Add an entry to `config.plist`:

| field | value |
|---|---|
| Arch | Any |
| BundlePath | RyzenHV.kext |
| Comment | RyzenHV |
| Enabled | true |
| ExecutablePath | Contents/MacOS/RyzenHV |
| MinKernel / MaxKernel | empty |
| PlistPath | Contents/Info.plist |

Order: after Lilu and the other kernel kexts. There is no dependency on Lilu at the moment.

## 4. Checking

```
./build/rhv-info
./build/rhv-info log
```

In the log expect lines like `svm=1 disabledByBios=0 ...`, then `probe only; add boot-arg rhv_enable_svm=1 ...`.
Everything about logs, counters and the self-test is in `docs/DEBUGGING.md`. If something is wrong, `./scripts/collect-debug.sh`
gathers a report for analysis.

How to read the output:

| output | means |
|---|---|
| `SVM: NOT supported` | the CPU/firmware does not show SVM; check that it is not masked in CPUID |
| `BIOS disable: YES` | enable SVM in the BIOS |
| `NPT: [ ]` | without nested paging the project makes no sense (this should not happen on Zen 3) |
| `usable N < CPUs` | an asymmetric configuration; the kext refuses to enable SVM |

## 5. Enabling SVM

Add the boot-args `rhv_enable_svm=1 rhv_selftest=1` (`Misc → Boot → boot-args`). The log should contain
`SVM enabled on N CPUs` and `selftest: N of N CPUs ok`; `sudo ./build/rhv-info selftest` gives the same.
If the system keeps working stably, phase 0 is done. If something breaks after enabling it, add `-rhv_off`.

## 6. Ryzentosh specifics

- **Spoofed CPUID.** The AMD Vanilla patches make the kernel believe an Intel CPU is installed. We read `cpuid` directly
  in the kext, so we see the real AMD. Still, check in `rhv-info` that it says `CPU: AMD family 0x19 model 0x50`.
- **Sleep/wake.** SVM state is lost after S3, so the kext turns SVM off before sleep and on again after it.
  This logic has not been verified on hardware: after sleep look at `rhv-info stats` and `sudo rhv-info selftest`.
- **Unloading.** `kextunload` calls `disableAll()`, but with an OpenCore-injected kext unloading is usually unavailable. To reset, just reboot.
