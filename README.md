# RyzenHV

Virtual machines and Docker on a **Hackintosh with an AMD Ryzen**.

On an Intel Mac, Docker Desktop, Parallels, VirtualBox and QEMU use Apple's `Hypervisor.framework`, which is built on Intel's VT-x. On a Ryzen it
reports "not supported" (`kern.hv_support` is 0) and they do not start. RyzenHV is a kernel extension that gives them the same API on top of AMD-V (SVM),
so the unmodified apps run.

> **This is an experimental project.** Test it on a USB stick with its own copy of your OpenCore EFI first, and always keep a working EFI **without** the kext.
> A mistake in kernel code is a panic.

## What works

| software | state |
|---|---|
| **Docker Desktop 4.97.0** (the Intel build) | works with Docker's own, unmodified files: engine, `docker run`, builds, 16 vCPUs |
| **Parallels Desktop 26.5.0** (build 57613 only) | **Windows 10 works**: 1–4 vCPUs, keyboard, network, shared folders, Parallels Tools, suspend and resume. A boot takes about 20 seconds. Windows 11 worked in the older (shim) mode |
| **VirtualBox 7.x** | experimental: version 7.2.22 runs a Windows 10 installation onto a disk (reported by a user, a second report would help). VirtualBox 6.x does **not** work (see below) |
| **VMware Fusion 26** | **not working yet**: with 0.7.5 the BIOS and its SMM entry work and Windows starts, but the installer stops at the Windows boot logo |
| **QEMU** with `-accel hvf` | boots Linux |

Tested on one machine: **Ryzen 7 5800H, macOS Sequoia 15.8.1, OpenCore**. macOS 26 (Tahoe) has not been tried by the author. A user reported (issue 1) that on macOS 26.7.1 with a Ryzen 9 5900X (24 logical CPUs) only one CPU core was online
when the kext started, so SVM was switched on for that core only and VMs did not run. Version 0.7.4 looks for the late cores again when a VM starts; that user confirmed that
Parallels (the older shim mode) then runs a fresh Windows 11 LTSC installation, also with 3D acceleration on an RX 6800.

**Not verified:** sleep and wake of the Mac, runs of many hours, 3D acceleration in Parallels (it crashes with the third-party NVIDIA Metal driver on the author's machine, keep it
off there; it works on an AMD GPU according to a user), Windows 11 or Linux guests in Parallels' native mode, other Parallels builds.

## Install (the ready-made build)

You need **SVM Mode switched on in the BIOS**. Nothing has to be compiled: no Xcode, no Homebrew.

1. Download the archive `ryzenhv-<version>-macos-x86_64.tar.gz` from the Releases page of the project.
2. Unpack it and open a terminal there:
   ```bash
   tar xzf ryzenhv-*-macos-x86_64.tar.gz && cd ryzenhv-*/
   xattr -dr com.apple.quarantine .      # if you downloaded it with a browser
   ```
3. Put the kext into your OpenCore EFI (use the **test EFI** first). This also adds the boot-args and keeps a backup of `config.plist`:
   ```bash
   ./rhv kext --native
   ```
   Restart from that EFI.
4. Check everything: `./rhv doctor`. It says what to fix.
5. Make `kern.hv_support` turn on by itself at every start (Parallels needs it; it is 0 after each restart):
   ```bash
   ./rhv autostart install
   ```
6. Then whatever you use:
   - **Docker Desktop:** `./rhv docker-desktop get` (downloads exactly version 4.97.0 from docker.com; skip it if you have that build), quit Docker if it runs, then
     `./rhv docker-desktop`, then start Docker as usual.
   - **Parallels Desktop:** `./rhv parallels native`. To undo: `./rhv parallels undo`.
   - **VirtualBox 7.x:** nothing to install, it uses the kext by itself.

There is also a small app, **RyzenHV**, with buttons for the same steps (kext, automatic start, Parallels, Docker); download it from the same Releases page.

## Creating virtual machines the right way

### Parallels Desktop

1. Create the VM in Parallels as usual (a Windows 10 ISO works best; it is what was tested).
2. **Before the first start**, with the VM stopped, set three things (replace the name):
   ```bash
   prlctl set "My Windows" --hypervisor-type apple
   prlctl set "My Windows" --cpus 4
   prlctl set "My Windows" --3d-accelerate off --high-resolution off --high-resolution-in-guest off --vertical-sync off
   ```
   The hypervisor type must be **Apple**. 1 to 4 vCPUs were tested. Parallels switches 3D acceleration on again in every **new** VM and it crashes here, so run
   `./rhv parallels fix-vms` (or the button in the app) after you create each VM.
3. **Do not hard-stop the VM** (kill, "Stop" with force) during the installation or the first start. Windows may land in "Why did my PC restart?" and automatic repair.
4. Install **Parallels Tools** in the guest: they give you shared folders, the clipboard and `prlctl exec`.
5. Give the VM only the memory it needs (4–8 GB is plenty). The kext keeps the guest's memory in RAM while the VM runs.
6. **Keep the Parallels Desktop app open** when you suspend a VM. With only its background service running, suspending hung twice in the author's tests and the VM
   could not be stopped until a restart. With the app open it worked every time. If you are not sure, shut Windows down instead of suspending.
7. After you change the number of vCPUs, shut Windows down completely: `shutdown /s /full /t 0` (Windows' "fast startup" would remember the old count).
8. After a Parallels update run `./rhv parallels native` again. Only build 57613 (26.5.0) is supported.

### VirtualBox 7.x

- Use **7.x** (7.2.22 was tested). VirtualBox 6.x was reported to stop with `VERR_SVM_IN_USE` (issue 2): it loads its own driver, which most likely wants to switch AMD-V on
  itself while RyzenHV already keeps it on. 7.x has no driver on macOS and goes through the kext.
- In the "New Virtual Machine" window **tick "Skip Unattended Installation"**. The unattended helper adds its own auxiliary ISO; with a custom Windows 10 image that
  failed with `VERR_ALREADY_EXISTS` and the VM did not even start (it is a VirtualBox setting, not a RyzenHV problem).
- Type: Windows 10 (64-bit), EFI on, **2 or more vCPUs**, 4 GB of RAM, a new virtual disk of 50 GB or more, and the Windows ISO in the optical drive.
- At the start the installer says "Press any key to boot from CD or DVD": click into the VM window and press a key quickly.
- Leave nested virtualization **off** (not tested).
- VirtualBox's log says `Turtle execution mode`: that is normal here, it means it runs slower than on a real Intel Mac. The installer reaches the disk page in about a
  minute; a complete installation onto a disk has not been checked yet.

### Docker Desktop

- Only the **Intel build 4.97.0** is supported; `./rhv docker-desktop get` installs it. Turn off automatic updates in Docker's settings; after an update run
  `./rhv docker-desktop` again.
- Quit Docker Desktop before running `./rhv docker-desktop`. The step also switches Docker to its HyperKit engine (not "Virtualization framework"); that is on purpose.
- If Docker does not start after a restart of the Mac, delete the leftover file `~/Library/Containers/com.docker.docker/Data/vms/0/hyperkit.pid` and start it again.

### For every VM

- Shut the VMs down before you restart the Mac. Sleep and wake of the Mac with a running VM has not been tested.
- One VM program at a time is the safest. Docker next to a running Parallels VM worked in short tests.
- If a VM does not start, first check `sysctl kern.hv_support`: it must print **1**. If it prints 0, run `./rhv autostart install` (once) or `./bin/rhv-info hv sysctl 1`.

## If something goes wrong

- `./rhv doctor` shows what is missing.
- `./bin/rhv-info` shows the CPU and SVM state; `./bin/rhv-info log` is the kernel log of the kext. Attach it when you open an issue.
- If the Mac does not boot: choose the **fallback EFI without the kext** in OpenCore, or add the boot-arg `-rhv_off` (then the kext does nothing).
- Run `./rhv` from the unpacked **archive**, not from a copy of the source code: the checks need the files that only the archive has.

## For developers

Build from source (needs Xcode's command line tools):

```bash
make test                # the pure logic, runs anywhere
make kext tools shim     # build/RyzenHV.kext, build/rhv-info, the shim
./scripts/install-to-efi.sh /Volumes/EFI    # copies the kext into a mounted OpenCore EFI
```

Boot-args: `rhv_enable_svm=1` (turn SVM on, off by default), `rhv_allow_users=1` (VMs run as a normal user), `rhv_hv=1` (native mode: the kext is the hypervisor provider),
`rhv_tpr_shadow=0` (do not offer the TPR shadow), `rhv_selftest`, `rhv_loglevel`, `rhv_stage`, `-rhv_off`. Diagnostics:

```
rhv-info                  CPU / SVM
rhv-info log [-f]         kernel ring log (-f: follow)
rhv-info stats            event counters
rhv-info hv               native mode state; "hv sysctl 0|1", "hv tpr 0|1"
sudo rhv-info selftest    check EFER/VM_HSAVE_PA on every core
```

Layout of the sources:

```
kext/                     the kext: SVM, VMCB, VMRUN, nested page tables, the native front end for Hypervisor.framework
shim/                     the userland layer for the older (shim) mode and the stand-in library used by native Parallels
tools/                    rhv-info and the acceptance tests of each phase (hvf-smoke is a test VMM on the public API)
scripts/                  installers and helpers (Parallels, Docker Desktop, the boot service, EFI install)
release/                  the `rhv` installer and the packaging of the archive
app/                      the RyzenHV app (SwiftUI)
tests/                    unit tests, run by `make test`
docs/                     the technical documentation (architecture, native mode, debugging)
```

## License

MIT, see `LICENSE`. You may use, change and redistribute the code, also in your own projects, as long as the copyright notice
and the license text stay with it, so the author is always credited. The archive ships QEMU, which has its own licenses
(`LICENSES/`). The repository contains nothing from Parallels, Docker or VirtualBox. If you port code from other projects, keep their
copyright: bhyve (FreeBSD, BSD) can be ported with its copyright kept; Linux KVM is GPL: copying code is not allowed,
reading it as a reference is.
