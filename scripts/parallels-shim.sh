#!/bin/bash
#
# Runs Parallels' VM process (prl_vm_app) with the Hypervisor.framework shim loaded.
#
#   ./scripts/parallels-shim.sh prepare          # builds a private copy of the VM app with the shim built in (no sudo)
#   PATCHONLY=1 ./scripts/parallels-shim.sh prepare   # the same for native mode (kext booted with rhv_hv=1): the vendor check is patched and the keyboard armed
#   sudo ./scripts/parallels-shim.sh install     # prl_vm_app in /Applications becomes a wrapper that starts that copy
#   sudo ./scripts/parallels-shim.sh uninstall   # puts Parallels' own prl_vm_app back
#   ./scripts/parallels-shim.sh fix-vms           # turns 3D acceleration off in every stopped VM (it crashes prl_vm_app here)
#   ./scripts/parallels-shim.sh status
#
# Why this much: Parallels' dispatcher (a root daemon) starts the VM process, so an environment variable set in a shell
# never reaches it, and prl_vm_app is a hardened-runtime binary that ignores DYLD_INSERT_LIBRARIES anyway. The dispatcher
# also checks that the process is signed by Parallels, so it cannot be re-signed. What works: a copy of the app (own
# signature intact) in ~/.ryzenhv/prl, laid out exactly like /Applications/Parallels Desktop.app, in which the library
# libPrlXmlModel.1.dylib (loaded at launch from Contents/Frameworks) is the shim re-exporting the real one (shim/hijack.c).
# Nothing inside the installed Parallels app changes except the small wrapper that replaces prl_vm_app.
#
# Needs the kext started with rhv_allow_users=1 if the VM process runs as you, and AMFI relaxed on this machine
# (amfi_get_out_of_my_way=0x1) so the stand-in library passes library validation. Parallels updates replace the wrapper:
# run install again (and prepare, if the app changed).
set -eu

HERE="$(cd "$(dirname "$0")/.." && pwd)"
APP="/Applications/Parallels Desktop.app"
DIR="$APP/Contents/MacOS/Parallels VM.app/Contents/MacOS"
WRAPPER="$DIR/prl_vm_app"
REAL="$DIR/prl_vm_app.real"
ORIG="$DIR/prl_vm_app.orig"      # untouched copy of Parallels' binary, what uninstall puts back
OWNER="${SUDO_USER:-$(id -un)}"
OWNER_HOME="$(eval echo "~$OWNER")"
FARM="$OWNER_HOME/.ryzenhv/prl/Parallels Desktop.app"
FARM_BIN="$FARM/Contents/MacOS/Parallels VM.app/Contents/MacOS/prl_vm_app"

is_wrapper() { [ -e "$WRAPPER" ] && head -c 2 "$WRAPPER" | grep -q '#!'; }

# The pristine VM binary: Parallels' own, whatever state a previous install left things in.
pristine() {
    if [ -e "$ORIG" ]; then echo "$ORIG"
    elif [ -e "$WRAPPER" ] && ! is_wrapper; then echo "$WRAPPER"
    else echo "$REAL"; fi
}

case "${1:-status}" in
prepare)
    [ "$(id -u)" != 0 ] || { echo "run prepare as yourself, not with sudo" >&2; exit 1; }
    [ -d "$APP" ] || { echo "Parallels is not installed at $APP" >&2; exit 1; }
    SRC_BIN="$(pristine)"
    # The file to copy must be a real executable, not a wrapper left by an install of ours (its first bytes are "#!"). Whose signature it carries does not matter.
    if [ ! -f "$SRC_BIN" ] || head -c 2 "$SRC_BIN" | grep -q '#!'; then
        echo "$SRC_BIN is not Parallels' own executable but a script (a leftover wrapper of an older install?); run: sudo $0 uninstall" >&2; exit 1
    fi
    rm -rf "$OWNER_HOME/.ryzenhv/prl"
    mkdir -p "$FARM/Contents/Frameworks" "$FARM/Contents/MacOS"
    # Everything that is not ours points at the real app.
    for e in "$APP/Contents"/*; do
        n="$(basename "$e")"
        case "$n" in Frameworks|MacOS) ;; *) ln -s "$e" "$FARM/Contents/$n" ;; esac
    done
    for e in "$APP/Contents/Frameworks"/*; do
        n="$(basename "$e")"
        [ "$n" = "libPrlXmlModel.1.dylib" ] || ln -s "$e" "$FARM/Contents/Frameworks/$n"
    done
    for e in "$APP/Contents/MacOS"/*; do
        n="$(basename "$e")"
        [ "$n" = "Parallels VM.app" ] || ln -s "$e" "$FARM/Contents/MacOS/$n"
    done
    # The VM app is a real copy, with Parallels' binary and signature back in place.
    cp -R "$DIR/../.." "$FARM/Contents/MacOS/Parallels VM.app"
    rm -f "$FARM/Contents/MacOS/Parallels VM.app/Contents/MacOS/prl_vm_app.real" \
          "$FARM/Contents/MacOS/Parallels VM.app/Contents/MacOS/prl_vm_app.orig"
    cp -p "$SRC_BIN" "$FARM_BIN"
    # The real library, untouched (its signature stays Parallels'), next to the stand-in.
    cp -p "$APP/Contents/Frameworks/libPrlXmlModel.1.dylib" "$FARM/Contents/Frameworks/libPrlXmlModel.1.orig.dylib"
    if [ -f "$HERE/lib/libPrlXmlModel.1.dylib" ]; then
        # Release bundle: the stand-in library comes prebuilt (it only re-exports ...orig.dylib next to it), no compiler needed.
        # PATCHONLY=1 (native mode, the kext booted with rhv_hv=1): the variant that patches Parallels' vendor check and arms its keyboard.
        LIB="$HERE/lib/libPrlXmlModel.1.dylib"
        if [ -n "${PATCHONLY:-}" ]; then
            LIB="$HERE/lib/libPrlXmlModel.1.native.dylib"
            [ -f "$LIB" ] || { echo "this bundle has no native-mode library ($LIB)" >&2; exit 1; }
        fi
        cp "$LIB" "$FARM/Contents/Frameworks/libPrlXmlModel.1.dylib"
    else
        # For the linker only: a copy whose install name says where the real library will be.
        mkdir -p "$HERE/build/hijack-link"
        cp "$APP/Contents/Frameworks/libPrlXmlModel.1.dylib" "$HERE/build/hijack-link/libPrlXmlModel.1.dylib"
        install_name_tool -id "@loader_path/libPrlXmlModel.1.orig.dylib" "$HERE/build/hijack-link/libPrlXmlModel.1.dylib" 2>/dev/null
        # PATCHONLY=1: for native mode (kext booted with rhv_hv=1). The library patches the vendor check and arms the keyboard, the hv_* calls stay Apple's.
        if [ -n "${PATCHONLY:-}" ]; then make -C "$HERE" hijack-patchonly >/dev/null; else make -C "$HERE" hijack >/dev/null; fi
        cp "$HERE/build/libPrlXmlModel.1.dylib" "$FARM/Contents/Frameworks/libPrlXmlModel.1.dylib"
    fi
    codesign --force --sign - "$FARM/Contents/Frameworks/libPrlXmlModel.1.dylib" 2>/dev/null
    # libMonitor (Parallels' hypervisor backend) also refuses a CPU whose CPUID vendor is not GenuineIntel. The shim finds that
    # branch by its bytes and patches it in memory when the library loads (shim/rhv_patch.c), so the library stays Parallels' own.
    if [ -n "${PATCHONLY:-}" ]; then echo native > "$OWNER_HOME/.ryzenhv/prl/MODE"; else echo shim > "$OWNER_HOME/.ryzenhv/prl/MODE"; fi
    echo "prepared: $FARM ($(cat "$OWNER_HOME/.ryzenhv/prl/MODE") mode)"
    ;;
install)
    [ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
    [ -x "$FARM_BIN" ] || { echo "run ./scripts/parallels-shim.sh prepare first (as yourself)" >&2; exit 1; }
    [ -e "$WRAPPER" ] || { echo "Parallels is not installed at $WRAPPER" >&2; exit 1; }
    if ! is_wrapper; then                       # first install, or Parallels was updated
        [ -e "$ORIG" ] && rm -f "$ORIG"
        cp -p "$WRAPPER" "$ORIG"
        mv "$WRAPPER" "$REAL"
    fi
    [ -e "$ORIG" ] || cp -p "$REAL" "$ORIG"
    cp -p "$ORIG" "$REAL"                       # drop any older re-signed copy
    cat > "$WRAPPER" <<WRAP
#!/bin/bash
# RyzenHV wrapper, see scripts/parallels-shim.sh in the ryzen-hypervisor repository.
# Starts the private copy of the VM app that has the Hypervisor.framework shim built in.
# One trace file per process: /tmp/rhv-prl-<pid>.trace
export RYZENHV_TRACE=\${RYZENHV_TRACE:-2} RYZENHV_TRACE_FILE=/tmp/rhv-prl-\$\$.trace
export RYZENHV_STATS=1 RYZENHV_STATS_FILE=/tmp/rhv-prl-\$\$.stats
if [ -x "$FARM_BIN" ]; then exec "$FARM_BIN" "\$@"; fi
exec "\$(dirname "\$0")/prl_vm_app.real" "\$@"
WRAP
    chmod 755 "$WRAPPER"
    chown root:wheel "$WRAPPER"
    echo "installed: $WRAPPER now starts $FARM_BIN"
    ;;
uninstall)
    [ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
    if [ -e "$ORIG" ]; then
        rm -f "$WRAPPER" "$REAL"
        mv "$ORIG" "$WRAPPER"
        echo "restored the original prl_vm_app"
    else
        echo "nothing to restore"
    fi
    ;;
fix-vms)
    # Parallels turns 3D acceleration on for every new VM; on this machine it crashes prl_vm_app inside the Metal driver, and
    # Windows in a VM does not need it here. Turns it off for every stopped VM (a running one cannot be changed).
    command -v prlctl >/dev/null || { echo "prlctl not found" >&2; exit 1; }
    prlctl list -a -o uuid,status 2>/dev/null | tail -n +2 | while read -r uuid status; do
        [ "$status" = "stopped" ] || { echo "$uuid is $status, skipped"; continue; }
        prlctl set "$uuid" --3d-accelerate off --high-resolution off --high-resolution-in-guest off --vertical-sync off >/dev/null \
            && echo "$uuid: 3D acceleration off"
    done
    ;;
status)
    if is_wrapper; then echo "wrapper installed (private copy: $([ -x "$FARM_BIN" ] && echo present || echo missing), mode: $(cat "$OWNER_HOME/.ryzenhv/prl/MODE" 2>/dev/null || echo unknown))"
    else echo "original prl_vm_app in place"; fi
    ;;
*)
    echo "usage: $0 prepare|install|uninstall|fix-vms|status" >&2
    exit 2
    ;;
esac
