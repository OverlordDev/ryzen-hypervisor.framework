#!/bin/bash
#
# Makes Docker Desktop for Mac (Intel) run on an AMD CPU. Its HyperKit VMM uses only the core Hypervisor.framework API, so it
# works through the shim; two checks inside HyperKit and the backend's start-up check have to be dealt with first.
#
#   ./scripts/docker-desktop-shim.sh install      # patches HyperKit in /Applications/Docker.app, wraps HyperKit and the backend
#   ./scripts/docker-desktop-shim.sh uninstall    # puts Docker's own files back
#   ./scripts/docker-desktop-shim.sh status
#
# What it does to /Applications/Docker.app/Contents (nothing needs sudo if you own the app, which is the default):
#   Resources/bin/com.docker.hyperkit.orig   untouched copy of Docker's HyperKit (what uninstall restores)
#   Resources/bin/com.docker.hyperkit.real   the patched copy, ad-hoc signed with only the hypervisor entitlement (the app sandbox
#                                            would keep it away from the RyzenHV kext)
#   Resources/bin/com.docker.hyperkit        a wrapper that starts the .real one with the shim loaded (DYLD_INSERT_LIBRARIES)
#   MacOS/com.docker.backend.real            Docker's backend, moved (its signature stays valid)
#   MacOS/com.docker.backend                 a wrapper that starts it with the shim loaded: the backend refuses to start unless
#                                            hv_vm_create() works
#   ~/Library/Group Containers/group.com.docker/settings-store.json   UseVirtualizationFramework=false (use HyperKit)
# Patches in HyperKit (v0.20210107-12-gadc4ea; each is checked byte for byte first, another build is refused):
#   1. HyperKit refuses a CPU whose CPUID vendor is not "GenuineIntel": that conditional jump becomes nops.
#   2. HyperKit compares the guest linear address of an EPT violation with the one it computes from the instruction; AMD does
#      not report it. The comparison's "equal" branch becomes unconditional.
#
# Needs the kext started with rhv_allow_users=1 and AMFI relaxed (amfi_get_out_of_my_way=0x1), see docs/DOCKER-DESKTOP.md.
# A Docker Desktop update replaces the app: run install again.
set -eu

HERE="$(cd "$(dirname "$0")/.." && pwd)"
APP="${DOCKER_APP:-/Applications/Docker.app}"
BIN="$APP/Contents/Resources/bin"
ORIG="$BIN/com.docker.hyperkit.orig"
REAL="$BIN/com.docker.hyperkit.real"
WRAP="$BIN/com.docker.hyperkit"
BACK="$APP/Contents/MacOS/com.docker.backend"
BACK_REAL="$BACK.real"
OWNER="$(id -un)"
OWNER_HOME="$(eval echo "~$OWNER")"
LIBDIR="$OWNER_HOME/.ryzenhv/lib"
SETTINGS="$OWNER_HOME/Library/Group Containers/group.com.docker/settings-store.json"

is_script() { [ -e "$1" ] && head -c 2 "$1" | grep -q '#!'; }
bytes_at() { od -An -tx1 -j "$2" -N "$3" "$1" | tr -d ' \n'; }

case "${1:-status}" in
install)
    [ -d "$APP" ] || { echo "Docker Desktop is not installed at $APP" >&2; exit 1; }
    pgrep -f "$APP/Contents/MacOS/com.docker.backend" >/dev/null && { echo "quit Docker Desktop first" >&2; exit 1; }
    SHIM=""
    for c in "$HERE/lib/libryzenhv.dylib" "$HERE/build/libryzenhv.dylib"; do [ -f "$c" ] && { SHIM="$c"; break; }; done
    [ -n "$SHIM" ] || { echo "libryzenhv.dylib not found (run make shim)" >&2; exit 1; }

    # --- HyperKit
    if is_script "$WRAP"; then SRC="$ORIG"; else SRC="$WRAP"; fi
    [ -e "$ORIG" ] || cp -p "$SRC" "$ORIG"
    codesign -dv "$ORIG" 2>&1 | grep -q "TeamIdentifier=9BNSXJN65R" || { echo "$ORIG is not Docker's signed binary" >&2; exit 1; }
    cp "$ORIG" "$REAL"
    chmod u+w "$REAL"
    # 1. vendor check: "jne error" (0f 85 d7 1a 00 00) at 0x227b56 -> six nops
    [ "$(bytes_at "$REAL" $((0x227b56)) 6)" = "0f85d71a0000" ] || { echo "this HyperKit is not the build the patches were written for (Docker Desktop 4.97.0, build 244584)" >&2; rm -f "$REAL"; exit 1; }
    printf '\x90\x90\x90\x90\x90\x90' | dd of="$REAL" bs=1 seek=$((0x227b56)) conv=notrunc 2>/dev/null
    # 2. verify_gla: "je ok" (0f 84 db fe ff ff) at 0x20af38 -> "jmp ok" (e9 dc fe ff ff) + nop
    [ "$(bytes_at "$REAL" $((0x20af38)) 6)" = "0f84dbfeffff" ] || { echo "this HyperKit is not the build the patches were written for (Docker Desktop 4.97.0, build 244584)" >&2; rm -f "$REAL"; exit 1; }
    printf '\xe9\xdc\xfe\xff\xff\x90' | dd of="$REAL" bs=1 seek=$((0x20af38)) conv=notrunc 2>/dev/null
    ENT="$(mktemp)"
    cat > "$ENT" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.hypervisor</key><true/></dict></plist>
EOF
    codesign --force --sign - --entitlements "$ENT" "$REAL" >/dev/null 2>&1
    rm -f "$ENT"

    # --- the shim
    mkdir -p "$LIBDIR"
    cp "$SHIM" "$LIBDIR/libryzenhv.dylib"

    # --- wrappers
    cat > "$WRAP" <<WRAP
#!/bin/bash
# RyzenHV wrapper, see scripts/docker-desktop-shim.sh in the ryzen-hypervisor repository.
export DYLD_INSERT_LIBRARIES="$LIBDIR/libryzenhv.dylib"
# A HyperKit that was killed leaves its pid file behind, and the next one then fails with "Failed to set up pidfile".
prev=""
for a in "\$@"; do
    if [ "\$prev" = "-F" ] && [ -f "\$a" ]; then
        pid="\$(cat "\$a" 2>/dev/null)"
        if [ -z "\$pid" ] || ! ps -p "\$pid" >/dev/null 2>&1; then rm -f "\$a"; fi
    fi
    prev="\$a"
done
# HyperKit's own messages (Docker's log keeps only its exit code) also go to $OWNER_HOME/.ryzenhv/hyperkit.log
exec "\$(dirname "\$0")/com.docker.hyperkit.real" "\$@" 2> >(tee -a "$OWNER_HOME/.ryzenhv/hyperkit.log" >&2)
WRAP
    chmod 755 "$WRAP"
    if ! is_script "$BACK"; then
        mv "$BACK" "$BACK_REAL"
    fi
    cat > "$BACK" <<WRAP
#!/bin/bash
# RyzenHV wrapper, see scripts/docker-desktop-shim.sh in the ryzen-hypervisor repository.
export DYLD_INSERT_LIBRARIES="$LIBDIR/libryzenhv.dylib"
exec "\$(dirname "\$0")/com.docker.backend.real" "\$@"
WRAP
    chmod 755 "$BACK"

    # --- settings: HyperKit instead of Apple's Virtualization framework
    mkdir -p "$(dirname "$SETTINGS")"
    [ -f "$SETTINGS" ] || echo '{}' > "$SETTINGS"
    plutil -replace UseVirtualizationFramework -bool NO "$SETTINGS"
    echo "installed: Docker Desktop in $APP now runs HyperKit through the shim"
    echo "start it as usual (Launchpad, Dock, open -a Docker)"
    ;;
uninstall)
    pgrep -f "$APP/Contents/MacOS/com.docker.backend" >/dev/null && { echo "quit Docker Desktop first" >&2; exit 1; }
    if [ -e "$ORIG" ]; then
        rm -f "$WRAP" "$REAL"
        mv "$ORIG" "$WRAP"
    fi
    if [ -e "$BACK_REAL" ]; then
        rm -f "$BACK"
        mv "$BACK_REAL" "$BACK"
    fi
    echo "restored Docker's own HyperKit and backend (the UseVirtualizationFramework setting is left as it is)"
    ;;
status)
    if is_script "$WRAP" && is_script "$BACK"; then echo "installed (HyperKit and backend wrapped)"
    elif is_script "$WRAP" || is_script "$BACK"; then echo "half installed: run install again"
    else echo "Docker's own files in place"; fi
    ;;
*)
    echo "usage: $0 install|uninstall|status" >&2
    exit 2
    ;;
esac
