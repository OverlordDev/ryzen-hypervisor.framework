#!/bin/bash
#
# Turns kern.hv_support on by itself at every boot, so Parallels (which looks at it) and QEMU find the native front end without a manual
# `rhv-info hv sysctl 1` after each reboot.
#
#   sudo ./scripts/hv-sysctl-daemon.sh install      # a launchd daemon + a root-owned copy of rhv-info
#   sudo ./scripts/hv-sysctl-daemon.sh remove
#   ./scripts/hv-sysctl-daemon.sh status
#
# What the daemon runs at boot (as root): wait until `rhv-info hv` says the native front end is registered (the kext is loaded by OpenCore
# early, but not necessarily before launchd starts the daemon), then `rhv-info hv sysctl 1`, the same command you would type. That command
# decodes the address of the kernel's hv_support variable from the kernel's own hv_get_support() and refuses if the code is not what it expects;
# it does nothing when the kext was not booted with rhv_hv=1. The log is /var/log/ryzenhv-hvsysctl.log.
#
# rhv-info is COPIED to /usr/local/libexec/ryzenhv (owned by root), not run from this checkout: a root daemon must not execute a file that a
# normal user can replace.
set -eu

LABEL=com.overlorddev.ryzenhv.hvsysctl
T="${RHV_TEST_ROOT:-}"                       # tests write below this directory instead of / and touch no launchd
LIBDIR=/usr/local/libexec/ryzenhv
PLIST=/Library/LaunchDaemons/$LABEL.plist
LOG=/var/log/ryzenhv-hvsysctl.log
HERE="$(cd "$(dirname "$0")" && pwd)"

need_root() { [ -n "$T" ] || [ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }; }

find_info() {
    local c
    for c in "${RHV_INFO:-}" "$HERE/../bin/rhv-info" "$HERE/../build/rhv-info"; do
        [ -n "$c" ] && [ -x "$c" ] && { echo "$c"; return 0; }
    done
    echo "rhv-info not found (looked in ../bin and ../build; set RHV_INFO=/path/to/rhv-info)" >&2
    return 1
}

case "${1:-status}" in
install)
    need_root
    SRC="$(find_info)"
    "$SRC" >/dev/null 2>&1 || { echo "rhv-info does not talk to the kext (is it loaded?); refusing to install a daemon that could not work" >&2; exit 1; }
    mkdir -p "$T$LIBDIR" "$T/Library/LaunchDaemons"
    cp "$SRC" "$T$LIBDIR/rhv-info"
    cat > "$T$LIBDIR/hv-sysctl.sh" <<'WRAPEND'
#!/bin/sh
# Installed by scripts/hv-sysctl-daemon.sh. Waits for the RyzenHV native front end, then turns kern.hv_support on.
BIN="${RHV_BIN:-/usr/local/libexec/ryzenhv/rhv-info}"
i=0
while [ "$i" -lt 90 ]; do
    if "$BIN" hv 2>/dev/null | grep -q "front end: registered"; then
        echo "$(date '+%F %T') front end registered, turning kern.hv_support on"
        "$BIN" hv sysctl 1
        echo "$(date '+%F %T') done, kern.hv_support = $(sysctl -n kern.hv_support)"
        exit 0
    fi
    i=$((i + 1))
    sleep 2
done
echo "$(date '+%F %T') the native front end did not register within 180 s (kext not loaded, or not booted with rhv_hv=1): kern.hv_support left alone"
exit 0
WRAPEND
    chmod 755 "$T$LIBDIR/rhv-info" "$T$LIBDIR/hv-sysctl.sh"
    cat > "$T$PLIST" <<PLEND
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$LABEL</string>
	<key>ProgramArguments</key>
	<array>
		<string>/bin/sh</string>
		<string>$LIBDIR/hv-sysctl.sh</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>StandardOutPath</key>
	<string>$LOG</string>
	<key>StandardErrorPath</key>
	<string>$LOG</string>
</dict>
</plist>
PLEND
    plutil -lint "$T$PLIST" >/dev/null || { echo "the generated plist is not valid" >&2; exit 1; }
    if [ -z "$T" ]; then
        chown -R root:wheel "$LIBDIR"
        chown root:wheel "$PLIST"
        chmod 644 "$PLIST"
        launchctl bootout "system/$LABEL" >/dev/null 2>&1 || true
        launchctl bootstrap system "$PLIST"
        sleep 2
        echo "installed. $(tail -2 "$LOG" 2>/dev/null | tr '\n' ' ')"
        echo "kern.hv_support is now $(sysctl -n kern.hv_support); it is turned on again at every boot. Undo: sudo $0 remove"
    else
        echo "test install below $T"
    fi
    ;;
remove)
    need_root
    if [ -z "$T" ]; then launchctl bootout "system/$LABEL" >/dev/null 2>&1 || true; fi
    rm -f "$T$PLIST" "$T$LIBDIR/hv-sysctl.sh" "$T$LIBDIR/rhv-info"
    rmdir "$T$LIBDIR" 2>/dev/null || true
    echo "removed (kern.hv_support keeps its current value until the next reboot)"
    ;;
status)
    if [ -f "$T$PLIST" ]; then echo "daemon file: $PLIST"; else echo "daemon file: not installed"; fi
    if launchctl print "system/$LABEL" >/dev/null 2>&1; then echo "launchd: loaded"; else echo "launchd: not loaded"; fi
    echo "kern.hv_support: $(sysctl -n kern.hv_support 2>/dev/null)"
    [ -f "$LOG" ] && { echo "log ($LOG):"; tail -4 "$LOG"; }
    exit 0
    ;;
*)
    echo "usage: $0 install|remove|status" >&2
    exit 2
    ;;
esac
