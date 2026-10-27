#!/bin/bash
#
# Boots a small Linux (Alpine's netboot kernel + initramfs) in QEMU's HVF accelerator, through the RyzenHV shim.
#
#   ./scripts/qemu-hvf-test.sh            # boots; leave QEMU with Ctrl-A then X
#   RYZENHV_TRACE=1 ./scripts/qemu-hvf-test.sh   # also log every Hypervisor.framework call to /tmp/rhv-qemu-trace.txt
#
# Success looks like Linux kernel messages on the serial console and, at the end, "initramfs emergency recovery
# shell launched" and a "~ #" prompt (there is no root disk and no network, so Alpine's init gives up there).
#
#   RYZENHV_STATS=1 ./scripts/qemu-hvf-test.sh   # once a second: exits/s, kernel calls per exit, us per exit -> /tmp/rhv-stats.txt
#
# Extra QEMU options go in RHV_QEMU_EXTRA, e.g. to log what QEMU's interrupt controllers do:
#   RHV_QEMU_EXTRA="-D /tmp/rhv-qemu.log -trace ioapic_* -trace apic_deliver_irq -trace apic_local_deliver"
#
# QEMU and the guest files live in ~/rhv-qemu (see docs/PHASE3.md for how they were built).
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
W="${RHV_QEMU_DIR:-$HOME/rhv-qemu}"
QEMU="$W/src/build-qemu/qemu-system-x86_64"
LIB="$HERE/build/libryzenhv.dylib"
[ -x "$QEMU" ] || { echo "QEMU is not built at $QEMU" >&2; exit 1; }
[ -f "$LIB" ] || { echo "build the shim first: make shim" >&2; exit 1; }
TRACE="${RYZENHV_TRACE:-0}"
sudo rm -f /tmp/rhv-stats.txt /tmp/rhv-qemu-trace.txt /tmp/rhv-mon.sock       # the shim appends; start from empty files
exec sudo env DYLD_INSERT_LIBRARIES="$LIB" RYZENHV_TRACE="$TRACE" RYZENHV_TRACE_FILE=/tmp/rhv-qemu-trace.txt \
    RYZENHV_STATS="${RYZENHV_STATS:-0}" RYZENHV_STATS_FILE=/tmp/rhv-stats.txt \
    "$QEMU" -accel hvf -m 512 -smp 1 -nographic -no-reboot ${RHV_QEMU_EXTRA:-} \
    -kernel "$W/guest/vmlinuz-virt" -initrd "$W/guest/initramfs-virt" -append "console=ttyS0 ${RHV_KERNEL_ARGS:-}"
