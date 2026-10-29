# Debugging

Kernel code is hard to debug: a bug means a panic or a hang, not a nice stack trace. So the project has logs, counters,
an emergency switch and a self-test built in from the start. This page is about how to use them.

## Boot-args

Set them in `Misc → Boot → boot-args` in the OpenCore `config.plist` (or through `nvram`).

| boot-arg | meaning |
|---|---|
| `-rhv_off` | **emergency switch.** The kext loads but does nothing. The first thing to try if something broke after the kext. |
| `rhv_enable_svm=1` | enable SVM on all cores. Without it the kext only reads. |
| `rhv_selftest=1` | after enabling (and after wake) check EFER and `VM_HSAVE_PA` on every core |
| `rhv_loglevel=N` | kernel ring log level, 0..5 (none, error, warn, info, debug, trace). Default 3 |
| `rhv_iolog=N` | up to which level to duplicate into the system log (IOLog). Default 3 |
| `rhv_stage=N` | **finding the step at which boot crashes**, see below. Without the flag all steps run |
| `rhv_slow=N` | wait N seconds (at most 10) after announcing each step, to read the screen |

### OpenCore prints `Prelinked injection RyzenHV.kext - Invalid Parameter`

The kext is not injected and will not get into the system, so there will not be a single `RyzenHV` line in the macOS log.
The OpenCore log is in the root of the EFI partition (`opencore-YYYY-MM-DD-HHMMSS.txt`, enabled with `Misc → Debug → Target`).
The most common cause: the kext imports a symbol that does not exist in the kernel (an API written from memory).
`make kext` now checks this itself (`scripts/check-kext-symbols.sh`, compared against the running kernel);
run it separately after a macOS update: `./scripts/check-kext-symbols.sh build/RyzenHV.kext`.
Confirmed in practice: `ml_get_max_cpus` does not exist in the Sequoia kernel, and OpenCore rejected the kext because of it.

### If the machine reboots right after booting

First add `-v keepsyms=1 debug=0x100`: without `debug=0x100` the kernel reboots right after the panic and
the text has no time to appear. Then go step by step, one step further on every boot, and look at the
**last `RyzenHV: [stage N]` line on the screen** (it is printed BEFORE the step runs):

| `rhv_stage` | what runs |
|---|---|
| `0` | the kext only announces `start() reached` and refuses to start. If it already crashes here, the cause is not our code but the loading of the kext through OpenCore itself |
| `1` | + `IOService::start`, the ring log |
| `2` | + `CPUID` on every core (MSRs are not read) |
| `3` | + reading `VM_CR` and `EFER` on every core, i.e. the full read-only probe |
| `4` | + sleep notifications and `registerService` (`rhv-info` works from this step on) |
| `5` | + enabling SVM by `rhv_enable_svm=1` (the default behavior) |

Example: `rhv_stage=2 rhv_slow=3 -v debug=0x100`. After a boot without a crash read
`log show --last 5m --predicate 'eventMessage CONTAINS "RyzenHV"'`.

For the kernel itself, when debugging, people usually add `-v` (verbose boot) and `keepsyms=1` (symbols in panics).
`debug=0x100` is also common, so that the system does not reboot right after a panic and the screen can be
photographed. Check the exact value of the flags for your macOS version.

## Logs

The kext writes to **its own 64 KiB ring buffer** (it can be read without `log show` and it is not lost if
the system log overflows) and duplicates the important lines into the system log.

```
./build/rhv-info log          # the whole buffer
./build/rhv-info log -f       # read continuously (like tail -f)
sudo ./build/rhv-info level 5 # turn trace on the fly, no reboot
log show --last boot --predicate 'eventMessage CONTAINS "RyzenHV"'
```

Line format:

```
[   12.345678] I cpu3 SVM enabled on 16 CPUs
 ^ seconds      ^ level (E W I D T)   ^ number of the core that made the call
```

If the buffer overflowed before you read it, `log` prints how many bytes were lost.

### Rules for the kext code

- In `mp_rendezvous` and any context with interrupts disabled **do not log**. Put results into arrays and print afterwards.
- Before an operation that can bring the kernel down, use `RHV_SYNC(...)`: the line goes straight to IOLog.
  The last such line before a panic shows where it failed (see `enableAll()`).
- `RHV_ASSERT(cond)` does not panic: it writes an error to the log and increments the `assertFailures` counter.

## Counters

```
./build/rhv-info stats
```

They answer "did this code run at all?": `probeRuns`, `enableCalls`, `enableFailures`,
`sleepEvents`/`wakeEvents`, `clientCalls`, `assertFailures`, `selftestCpusOk`, and for VMs `vmCreates`, `vmRuns`,
`vmExits` (all exits, including host interrupts) and `vmInvalidExits` (VMRUN rejected the VMCB). See also `docs/PHASE1.md`.

## SVM self-test

```
# boot-args: rhv_enable_svm=1
sudo ./build/rhv-info selftest
./build/rhv-info log | tail
```

For every core it reads `EFER` and `VM_HSAVE_PA` and compares them with what we wrote. An example of a good result:
`selftest: 16 of 16 CPUs ok`. Any mismatch is printed as `selftest cpu N: MISMATCH ...`.

## Sleep and wake

SVM is turned off before sleep (`sleep: turning SVM off`) and turned on again after wake
(`wake: SVM re-enabled on N CPUs`). To check: put the machine to sleep, wake it, then `rhv-info stats`
(`sleepEvents` and `wakeEvents` must grow) and `sudo rhv-info selftest`.

If sleep does not work at all on a Ryzentosh laptop regardless of this kext, fix that first, it is a separate topic.

## If booting is broken

1. Boot from the fallback EFI (without the kext), or add the boot-arg `-rhv_off` in the OpenCore menu.
2. After booting: `./scripts/collect-debug.sh`. The script collects the CPU, boot-args, the RyzenHV logs and the last panic into one file.
3. If you did not manage to see the panic: add `-v keepsyms=1 debug=0x100`, repeat, photograph the screen.
4. Send the script's file and the photo of the panic; we start from the last `RHV_SYNC` line in the log.

## Fast iteration

```
make kext tools DEBUG=1        # -O0 -g, debug log level by default
./scripts/install-to-efi.sh /Volumes/EFI
# reboot
```

`DEBUG=1` builds only the kext (`kext/Makefile`). For kernel development with minimal risk keep
`rhv_enable_svm` off until the probe looks right.
