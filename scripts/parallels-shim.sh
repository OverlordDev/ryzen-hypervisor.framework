#!/bin/bash
#
# Runs Parallels' VM process (prl_vm_app) with the Hypervisor.framework shim loaded.
#
#   sudo ./scripts/parallels-shim.sh install     # prl_vm_app becomes a small wrapper script, the real one is kept
#   sudo ./scripts/parallels-shim.sh uninstall   # puts the original back
#   ./scripts/parallels-shim.sh status
#
# Why a wrapper: Parallels' dispatcher (a root daemon) starts the VM process, so an environment variable set in a
# shell never reaches it. The wrapper sets DYLD_INSERT_LIBRARIES and then execs the real binary (same pid, same
# signature on the process that connects back to the dispatcher). Parallels updates replace the file: run install again.
#
# Needs the kext started with boot-arg rhv_allow_users=1 (the VM process runs as you, not as root), and AMFI relaxed
# (amfi_get_out_of_my_way=0x1 on this machine) because prl_vm_app is a hardened-runtime binary.
set -eu

HERE="$(cd "$(dirname "$0")/.." && pwd)"
LIB="$HERE/build/libryzenhv.dylib"
DIR="/Applications/Parallels Desktop.app/Contents/MacOS/Parallels VM.app/Contents/MacOS"
APP="$DIR/prl_vm_app"
REAL="$DIR/prl_vm_app.real"

case "${1:-status}" in
install)
    [ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
    [ -f "$LIB" ] || { echo "build the shim first: make shim" >&2; exit 1; }
    [ -e "$APP" ] || { echo "Parallels is not installed at $APP" >&2; exit 1; }
    if [ ! -e "$REAL" ]; then
        mv "$APP" "$REAL"
    elif ! head -c 2 "$APP" | grep -q '#!'; then
        # Parallels was updated and put a new binary in place: keep that one as the real one.
        mv "$APP" "$REAL"
    fi
    cat > "$APP" <<WRAP
#!/bin/bash
# RyzenHV wrapper, see scripts/parallels-shim.sh in the ryzen-hypervisor repository.
export DYLD_INSERT_LIBRARIES="$LIB"
export RYZENHV_TRACE=\${RYZENHV_TRACE:-2} RYZENHV_TRACE_FILE=/tmp/rhv-prl-trace.txt
export RYZENHV_STATS=1 RYZENHV_STATS_FILE=/tmp/rhv-prl-stats.txt
exec "\$(dirname "\$0")/prl_vm_app.real" "\$@"
WRAP
    chmod 755 "$APP"
    chown root:wheel "$APP"
    echo "installed: $APP -> wrapper (original: $REAL)"
    ;;
uninstall)
    [ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
    if [ -e "$REAL" ]; then
        rm -f "$APP"
        mv "$REAL" "$APP"
        echo "restored the original prl_vm_app"
    else
        echo "nothing to restore"
    fi
    ;;
status)
    if [ -e "$REAL" ] && head -c 2 "$APP" | grep -q '#!'; then echo "wrapper installed"; else echo "original prl_vm_app in place"; fi
    ;;
*)
    echo "usage: $0 install|uninstall|status" >&2
    exit 2
    ;;
esac
