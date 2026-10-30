# Parallels Desktop on a Ryzen: installation and use

State: **Windows 11 (Home) installs and runs in Parallels Desktop 26.5.0 (57613) on a Ryzen 7 5800H,
macOS Sequoia 15.8.1, OpenCore, up to 4 vCPUs.** Boot to the Setup window takes ~45–50 s, the keyboard, network and Parallels Tools
(`prlctl exec`) work. Everything is done without editing files inside `/Applications/Parallels Desktop.app`, except one
replacement of `prl_vm_app` with a wrapper (reversible).

> From the ready-made release, everything below is done by `./rhv kext` and `./rhv parallels` (steps 4–5 and installing the wrapper); this document describes
> what exactly happens.

> This is an experiment, not a product. It works on the **exact Parallels build 26.5.0 (57613)**: the patches are found by the bytes around them, not by address, but only this build is tested; on another build they may not apply (the VM then simply does not start). After a Parallels update
> repeat steps 4–5. Patching a proprietary application on your own machine is your responsibility; the repository contains
> nothing from Parallels, the copy is built from the application installed on your machine.

## What you need

| | |
|---|---|
| CPU | an AMD Ryzen with SVM and NPT (tested: Ryzen 7 5800H, Zen 3). SVM enabled in the BIOS |
| System | macOS Sequoia, OpenCore (tested 15.8.1), Xcode/Command Line Tools for building |
| Parallels Desktop | **26.5.0 (57613)**, the trial version is fine |
| Guest | tested: Windows 11 Home (1–4 vCPUs). Linux and macOS guests were not tested |
| Fallback EFI | a USB stick with a working copy of `EFI/` **without** the kext (see `docs/RYZENTOSH.md`) |

## Installation

1. **Build** (on the hackintosh itself):
   ```
   make test            # the pure logic, must pass completely
   make kext tools shim
   ```
2. **The kext in OpenCore.** Mount the EFI, `./scripts/install-to-efi.sh /Volumes/EFI`, and on the first installation add the
   `Kernel → Add` entry (the table is in `docs/RYZENTOSH.md`).
3. **boot-args** (`Misc → Boot → boot-args`) and a reboot:
   ```
   rhv_enable_svm=1 rhv_allow_users=1 amfi_get_out_of_my_way=0x1
   ```
   - `rhv_enable_svm=1`: enable SVM on all cores;
   - `rhv_allow_users=1`: the Parallels VM runs as an ordinary user, not as root;
   - AMFI is relaxed so that the process with our stand-in library can start (the author's machine also has `amfi=0x80`).
   Check after booting: `./build/rhv-info` must print `RyzenHV kext (ABI 11)` and `SVM usable on N, active on N`.
4. **A copy of Parallels with the shim** (no sudo, lives in `~/.ryzenhv/prl`):
   ```
   ./scripts/parallels-shim.sh prepare
   ```
5. **A wrapper instead of `prl_vm_app`** (once, and after every Parallels update):
   ```
   sudo ./scripts/parallels-shim.sh install
   ./scripts/parallels-shim.sh status
   ```
6. **The VM.** Create a VM in Parallels as usual (Windows 11 from an ISO), then:
   ```
   prlctl set "Windows 11" --cpus 4                      # tested with 1 and 4 (the kext allows up to 16 vCPUs per VM)
   prlctl set "Windows 11" --3d-accelerate off --high-resolution off --high-resolution-in-guest off --vertical-sync off
   ```
   The hypervisor type must be Apple (`prlctl list -i "Windows 11" | grep Hypervisor` → `apple`; otherwise
   `prlctl set "Windows 11" --hypervisor-type apple`).
7. Start the VM. At "Press any key to boot from CD or DVD" press a key.

To remove: `sudo ./scripts/parallels-shim.sh uninstall`, then remove the kext from `Kernel → Add` and from `EFI/OC/Kexts`.

## Tips for the guest

- **Do not kill the VM** (`prlctl stop --kill`, closing the window with "Stop") during installation and the first start (OOBE): after
  a few unplanned restarts Windows shows "Why did my PC restart?" and goes into automatic recovery.
- Shut Windows down **completely**: `shutdown /s /full /t 0`. "Fast startup" stores the old number of processors.
- Windows 11 **Home** runs one physical processor. The shim itself gives the guest the right topology (4 cores in one
  package, cache), so all vCPUs work.
- `prlctl exec "Windows 11" cmd /c ...` works when Parallels Tools are installed (you can read the event log, the registry).
- 3D acceleration (`--3d-accelerate`) crashes `prl_vm_app` within seconds on the author's machine (RTX 3070 with the third-party NVIDIA Metal driver NVMTL, nullmoth/nvidia-macos-driver,
  issue 376 there; not Apple's own driver and not RyzenHV): keep it off with that driver. Cause found (2026-10-30): Parallels draws through its own `AsyncDrawable` class (an `MTLDrawable`).
  Apple's `-[_MTLCommandBuffer presentDrawable:]` (and the `atTime:`, `afterMinimumDuration:` and `options:` variants) first gives the drawable a block through the private
  `addPresentScheduledHandler:`, and `AsyncDrawable.present` calls that block without a nil check. NVMTL's `presentDrawable:` never sets it, so the first `present` reads a NULL block (fault at
  address 0x10, in `nvmtlFinishExecution`). With a stand-in library that calls `addPresentScheduledHandler:` with an empty block before the driver's `present`, a Windows 10 VM with `--3d-accelerate highest`
  ran with the display adapter "Parallels Display Adapter (WDDM)" for minutes with 1600+ presents and no crash. The fix belongs in the NVIDIA driver, not in RyzenHV. On an AMD GPU (a user's RX 6800) 3D works.

## What the shim does inside Parallels

| problem | solution |
|---|---|
| `kern.hv_support` = 0, Parallels falls back to its own engine | `sysctlbyname` returns 1, `hv_*` are replaced with calls into the kext |
| `prl_vm_app` is hardened, `DYLD_INSERT_LIBRARIES` does not work, the dispatcher checks the signature | a mirror of the application in `~/.ryzenhv/prl` in which `libPrlXmlModel.1.dylib` = the shim, imports rewritten by name (`shim/hijack.c`) |
| the `GenuineIntel` vendor check (the CPUID instruction) | one branch patched in memory, in `prl_vm_app` and in `libMonitor.dylib`, found by the bytes around it (`shim/rhv_patch.c`) |
| the keyboard does not work | the Parallels monitor drops keys until the guest writes 0 to port 0x80: the shim calls its handler itself |
| triple fault when the Windows kernel starts | the Hyper-V hypercall `VMCALL` is a #UD on AMD: the shim intercepts #UD and turns it into an ordinary VMX exit |
| the guest "sleeps" after the kernel boots | the APIC page (0xFEE00000) is left unmapped, accesses go to the monitor; the hypervisor is hidden from the guest |
| ~60% of exits are CR8 | TPR shadow in the kernel (a CR8 write does not exit while there is no threshold), `HV_X86_TPR` is 8-bit |
| QPC is negative, OOBE "restarts" | `RDTSC exiting` intercepts `RDTSCP` too |
| Windows Home runs 1 CPU | CPUID: `0x80000008`, `0x80000001`, `0x80000005/6`, leaf 4 (cores per package, cache) |
| several vCPUs | VM = a shared part (NPT, memory) + an object per vCPU; the shim opens one connection per vCPU |

Details and the debugging history: `docs/PHASE3.md` (stage 8).

## Debugging

Trace and statistics are **off**: they produce hundreds of thousands of lines per second and noticeably load the system. To turn them on:

```
touch /tmp/rhv-debug          # (or ~/.ryzenhv/debug) and restart the VM
# files: /tmp/rhv-prl-<pid>.trace and .stats
rm /tmp/rhv-debug
```

Other switches (files in `/tmp`, effective at the next VM start):

| file / variable | effect |
|---|---|
| `/tmp/rhv-no-vtpr` | do not offer TPR shadow (brings CR8 exits back) |
| `/tmp/rhv-show-hv`, `RYZENHV_SHOW_HYPERVISOR=1` | do not hide the hypervisor from the guest (and do not patch CPUID) |
| `/tmp/rhv-map-apic` | map the APIC page as memory |
| `RYZENHV_NO_KBD_ARM=1` | do not "arm" the monitor's keyboard |
| `/tmp/rhv-dump`, `/tmp/rhv-madt` | (with the trace) a dump of guest registers/code once a second; the MADT table |

With the trace on, a guest reset request searches for the header of a Windows crash dump (`BUGCHECK`), and on a triple fault
the guest state is printed (`TRIPLE FAULT`). For `prl_vm_app` crashes look in `/Library/Logs/DiagnosticReports`
(`prl_vm_app-*.ips`), the VM log is `~/Parallels/<VM>.pvm/parallels.log`.

## Known limitations

- Only Parallels build 26.5.0 (57613); after an update run `prepare` and `install` again, and on another
  build expect the address-based patches not to apply (the shim says so in the trace and touches nothing).
- Not verified: sleep/wake, `kextunload`, sustained load, guests other than Windows 11, snapshot/suspend
  with several vCPUs, the `hv_vm_space_*` address spaces.
- The "Why did my PC restart?" window after unplanned stops: see the tips above.
- `rhv_allow_users=1` gives any local user access to the hypervisor: enable it on a personal machine only.
