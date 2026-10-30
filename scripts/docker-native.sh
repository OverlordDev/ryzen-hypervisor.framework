#!/bin/bash
#
# Docker Desktop (Intel build) on the RyzenHV kext's native Hypervisor.framework, with NO shim and NO DYLD injection.
# The only change to Docker.app is one file: HyperKit is replaced by a copy with two branches patched (found by their bytes) and
# re-signed ad hoc with only the hypervisor entitlement (Docker's own signature carries the app sandbox, which keeps it from the kext).
#
#   ./scripts/docker-native.sh install     # patches a copy of HyperKit, keeps Docker's own as com.docker.hyperkit.orig
#   ./scripts/docker-native.sh uninstall   # puts Docker's own HyperKit back
#   ./scripts/docker-native.sh status
#
# Needs the kext started with rhv_hv=1 and rhv_allow_users=1 and AMFI relaxed (see docs/DOCKER-DESKTOP.md). The backend is untouched.
# A Docker Desktop update replaces HyperKit: run install again.
set -eu

HERE="$(cd "$(dirname "$0")/.." && pwd)"
APP="${DOCKER_APP:-/Applications/Docker.app}"
HK="$APP/Contents/Resources/bin/com.docker.hyperkit"
ORIG="$HK.orig"

signed_by_docker() { codesign -dv "$1" 2>&1 | grep -q "TeamIdentifier=9BNSXJN65R"; }

case "${1:-status}" in
install)
    [ -d "$APP" ] || { echo "Docker Desktop is not installed at $APP" >&2; exit 1; }
    pgrep -f "$APP/Contents/MacOS/com.docker.backend" >/dev/null && { echo "quit Docker Desktop first" >&2; exit 1; }
    if [ -e "$ORIG" ]; then
        signed_by_docker "$ORIG" || { echo "$ORIG is not Docker's signed binary" >&2; exit 1; }
    else
        signed_by_docker "$HK" || { echo "$HK is not Docker's signed binary (run docker-desktop-shim.sh uninstall first?)" >&2; exit 1; }
        cp -p "$HK" "$ORIG"
    fi
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    python3 -I "$HERE/scripts/patch-file.py" "$ORIG" "$TMP/hyperkit" vendor gla
    chmod 755 "$TMP/hyperkit"
    cat > "$TMP/ent.plist" <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.hypervisor</key><true/></dict></plist>
PL
    codesign --force --sign - --entitlements "$TMP/ent.plist" "$TMP/hyperkit" >/dev/null 2>&1
    cp "$TMP/hyperkit" "$HK"
    echo "installed: HyperKit in $APP is the patched copy (Docker's own: $ORIG)"
    ;;
uninstall)
    pgrep -f "$APP/Contents/MacOS/com.docker.backend" >/dev/null && { echo "quit Docker Desktop first" >&2; exit 1; }
    [ -e "$ORIG" ] || { echo "nothing to restore ($ORIG is missing)"; exit 0; }
    mv -f "$ORIG" "$HK"
    echo "restored Docker's own HyperKit"
    ;;
status)
    if [ -e "$ORIG" ]; then echo "native: HyperKit is the patched copy"; elif signed_by_docker "$HK"; then echo "Docker's own HyperKit in place"; else echo "HyperKit is not Docker's (shim wrapper?)"; fi
    ;;
*) echo "usage: $0 install|uninstall|status" >&2; exit 2 ;;
esac
