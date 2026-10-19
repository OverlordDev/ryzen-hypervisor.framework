#!/bin/bash
#
# Collects everything needed to debug RyzenHV remotely into one text file.
# Run on the Ryzentosh:   ./scripts/collect-debug.sh
# Then send me ~/rhv-debug-<timestamp>.txt (read it first if you like: it holds
# CPU info, boot-args, kext/panic info and the RyzenHV logs, no serial numbers).
#
# Safe to run as a normal user; some sections are richer with sudo.

OUT="${1:-$HOME/rhv-debug-$(date +%Y%m%d-%H%M%S).txt}"
HERE="$(cd "$(dirname "$0")" && pwd)"
RHV_INFO="$HERE/../build/rhv-info"

section() {
    {
        echo
        echo "################################################################"
        echo "### $1"
        echo "################################################################"
    } >> "$OUT"
}

run() {
    section "$*"
    "$@" >> "$OUT" 2>&1 || echo "(exit status $?)" >> "$OUT"
}

runsh() {
    section "$1"
    bash -c "$1" >> "$OUT" 2>&1 || echo "(exit status $?)" >> "$OUT"
}

echo "RyzenHV debug report, $(date)" > "$OUT"

run sw_vers
run uname -a
runsh "sysctl machdep.cpu.brand_string machdep.cpu.family machdep.cpu.model machdep.cpu.stepping kern.hv_support hw.logicalcpu hw.physicalcpu"
runsh "sysctl machdep.cpu.features machdep.cpu.extfeatures"
runsh "nvram boot-args"
runsh "csrutil status"

# Is the kext loaded? (kextstat is deprecated but still there; kmutil is the new tool)
runsh "kextstat 2>/dev/null | grep -i -E 'ryzen|overlord|Lilu|AMD'"
runsh "kmutil showloaded 2>/dev/null | grep -i -E 'ryzen|overlord'"

if [ -x "$RHV_INFO" ]; then
    run "$RHV_INFO" info
    run "$RHV_INFO" stats
    run "$RHV_INFO" log
else
    section "rhv-info"
    echo "$RHV_INFO not built (run: make tools)" >> "$OUT"
fi

runsh "log show --last boot --style compact --predicate 'eventMessage CONTAINS \"RyzenHV\"'"
runsh "log show --last boot --style compact --predicate 'process == \"kernel\" AND eventMessage CONTAINS[c] \"panic\"' | tail -50"

# Newest kernel panic reports, if any (they only exist if the panic was saved before reboot).
section "kernel panic reports"
ls -lt /Library/Logs/DiagnosticReports/ 2>/dev/null | grep -i -E 'panic|Kernel' | head -5 >> "$OUT"
LATEST="$(ls -t /Library/Logs/DiagnosticReports/*.panic /Library/Logs/DiagnosticReports/Kernel*.ips 2>/dev/null | head -1)"
if [ -n "$LATEST" ]; then
    section "newest panic report: $LATEST (first 150 lines)"
    head -150 "$LATEST" >> "$OUT" 2>&1
else
    echo "none found" >> "$OUT"
fi

echo "Wrote $OUT"
