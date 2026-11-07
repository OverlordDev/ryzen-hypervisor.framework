#!/bin/bash
#
# Makes Docker Desktop for Mac (Intel) run on an AMD CPU. Its HyperKit VMM uses only the core Hypervisor.framework API, so it
# works through the shim; two checks inside HyperKit and the backend's start-up check have to be dealt with first.
#
#   ./scripts/docker-desktop-shim.sh install         # shim mode: disk-patches HyperKit, wraps HyperKit and the backend
#   ./scripts/docker-desktop-shim.sh install-native  # native mode: the same disk patch, no shim (the kext is the hypervisor
#                                                    #   provider, e.g. after `./rhv kext --native --no-amfi`)
#   ./scripts/docker-desktop-shim.sh uninstall       # puts Docker's own files back
#   ./scripts/docker-desktop-shim.sh status
#
# What it does to /Applications/Docker.app/Contents (nothing needs sudo if you own the app, which is the default):
#   Resources/bin/com.docker.hyperkit.orig   untouched copy of Docker's HyperKit (what uninstall restores)
#   Resources/bin/com.docker.hyperkit.real   a copy of it with the two CPU checks patched on disk (rhv-hkpatch), then
#                                            ad-hoc signed with only the hypervisor entitlement (the app sandbox
#                                            would keep it away from the RyzenHV kext)
#   Resources/bin/com.docker.hyperkit        a wrapper that starts the .real one
#   MacOS/com.docker.backend.real            Docker's backend, moved (its signature stays valid; shim mode only)
#   MacOS/com.docker.backend                 a wrapper that starts it with the shim loaded: the backend refuses to start
#                                            unless hv_vm_create() works (shim mode only)
#   ~/Library/Group Containers/group.com.docker/settings-store.json   UseVirtualizationFramework=false (use HyperKit)
#
# The checks live in HyperKit (v0.20210107-12-gadc4ea) and are patched by their bytes (tools/rhv-hkpatch, shim/rhv_patch.c):
#   1. HyperKit refuses a CPU whose CPUID vendor is not "GenuineIntel": that conditional jump becomes nops.
#   2. HyperKit compares the guest linear address of an EPT violation with the one it computes from the instruction; AMD does
#      not report it. The comparison's "equal" branch becomes unconditional.
#
# Patching the FILE and signing afterwards means the signature describes the patched bytes: HyperKit's pages all validate,
# nothing modifies it while it runs, and AMFI has nothing to kill -- amfi_get_out_of_my_way=0x1 is NOT needed, code signing
# stays on for Firefox and everything else. The in-memory patchers (shim/rhv_patch.c, kext/Source/HvPatch.cpp) find their
# patterns in the patched form and write nothing ("already in place"). Only a HyperKit build whose patterns do not fit
# (reported by install and in ~/.ryzenhv/hyperkit.log) is left for the in-memory patching, which then needs the AMFI
# boot-arg or the kext's per-process signing relax.
#
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

find_hkpatch() {
    local c
    for c in "$HERE/bin/rhv-hkpatch" "$HERE/build/rhv-hkpatch" "$HERE/../build/rhv-hkpatch"; do
        [ -x "$c" ] && { echo "$c"; return 0; }
    done
    echo "" ; return 1
}

# The wrapper both install modes share. "$1" is the extra shim line ("" in native mode).
write_wrapper() {
    local shim_line="$1"
    {
        echo '#!/bin/bash'
        echo '# RyzenHV wrapper, see scripts/docker-desktop-shim.sh in the ryzen-hypervisor repository.'
        [ -n "$shim_line" ] && echo "$shim_line"
        cat <<'PIDEND'
# A HyperKit that was killed leaves its pid file behind, and the next one then fails with "Failed to set up pidfile".
# The number must belong to a running HyperKit, not just to any process: pid numbers are reused within hours on a busy
# machine, and a stale file naming a recycled pid is exactly what wedged every restart after a crash. A running
# HyperKit keeps its file.
prev=""
for a in "$@"; do
    if [ "$prev" = "-F" ] && [ -f "$a" ]; then
        pid="$(tr -dc '0-9' < "$a" 2>/dev/null)"
        if [ -z "$pid" ] || ! ps -p "$pid" -o comm= 2>/dev/null | grep -q hyperkit; then rm -f "$a"; fi
    fi
    prev="$a"
done
PIDEND
        echo "# HyperKit's own messages (Docker's log keeps only its exit code) also go to $OWNER_HOME/.ryzenhv/hyperkit.log"
        echo 'exec "$(dirname "$0")/com.docker.hyperkit.real" "$@" 2> >(tee -a "'"$OWNER_HOME"'/.ryzenhv/hyperkit.log" >&2)'
    } > "$WRAP"
    chmod 755 "$WRAP"
}

# Copies Docker's HyperKit aside, patches the copy on disk, signs it. Returns 0 when the copy is usable; *1* when the
# patterns do not fit this build and the copy stays unpatched (the caller decides whether that is acceptable).
prepare_hyperkit() {
    if is_script "$WRAP"; then SRC="$ORIG"; else SRC="$WRAP"; fi
    [ -e "$ORIG" ] || cp -p "$SRC" "$ORIG"
    codesign -dv "$ORIG" 2>&1 | grep -q "TeamIdentifier=9BNSXJN65R" || { echo "$ORIG is not Docker's signed binary" >&2; return 2; }
    cp "$ORIG" "$REAL"
    chmod u+w "$REAL"

    local HKPATCH
    HKPATCH="$(find_hkpatch)" || HKPATCH=""
    if [ -z "$HKPATCH" ]; then
        echo "rhv-hkpatch not found (run make; it patches the copy on disk, the in-memory patching would need the AMFI boot-arg)" >&2
        return 1
    fi
    if "$HKPATCH" "$REAL"; then
        echo "the two CPU checks are now in the copy's own bytes: nothing patches HyperKit while it runs"
    else
        echo "this HyperKit build does not match the patterns; the copy stays unpatched" >&2
        return 1
    fi

    local ENT
    ENT="$(mktemp)"
    cat > "$ENT" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.hypervisor</key><true/></dict></plist>
EOF
    if ! codesign --force --sign - --entitlements "$ENT" "$REAL" >/dev/null 2>&1; then
        echo "cannot re-sign the copy ad-hoc (codesign failed); HyperKit would not start without a valid signature" >&2
        rm -f "$ENT"
        return 2
    fi
    rm -f "$ENT"
    return 0
}

# Set a boolean key in Docker's settings-store.json. That file is JSON, not a plist, and a plain
# `plutil -replace` fails on macOS 26 (Tahoe): plutil reads the JSON through its OpenStep plist reader
# and then cannot write it back ("Conversion to OpenStep format is not supported" -- OpenStep is the one
# plist format CoreFoundation does not write). Edit it as JSON: first osascript (JavaScript for
# Automation), then perl (JSON::PP is in the base system). Same helper as in release/rhv.
json_set_bool() {                       # json_set_bool FILE KEY true|false
    local f="$1" key="$2" val="$3" out
    if [ "$(head -c 8 "$f" 2>/dev/null)" = "bplist00" ] || [ "$(head -c 5 "$f" 2>/dev/null)" = "<?xml" ]; then
        plutil -convert json "$f" || return 1        # a plist from an older run: make it JSON first
    fi
    out="$(osascript -l JavaScript -e '
        ObjC.import("Foundation");
        function run(argv) {
            const p = argv[0], k = argv[1], v = (argv[2] === "true");
            const raw = $.NSString.stringWithContentsOfFileEncodingError(p, $.NSUTF8StringEncoding, $());
            if (raw.isNil()) return "ERR: cannot read " + p;
            const t = raw.js;
            let o;
            try { o = (t.trim() === "") ? {} : JSON.parse(t); }
            catch (e) { return "ERR: not JSON: " + e.message; }
            if (!o || typeof o !== "object" || Array.isArray(o)) return "ERR: not a JSON object";
            o[k] = v;
            const s = $.NSString.alloc.initWithUTF8String(JSON.stringify(o, null, 2) + "\n");
            if (!s.writeToFileAtomicallyEncodingError(p, false, $.NSUTF8StringEncoding, $())) return "ERR: cannot write " + p;
            return "OK";
        }' "$f" "$key" "$val" 2>/dev/null)" && [ "$out" = "OK" ] && return 0
    /usr/bin/perl -MJSON::PP -e '
        my ($f, $k, $v) = @ARGV;
        open(my $in, "<", $f) or die "cannot read $f: $!\n";
        local $/; my $txt = <$in>; close $in;
        $txt = "{}" if !defined($txt) || $txt =~ /^\s*$/;
        my $d = JSON::PP->new->decode($txt);
        die "not a JSON object\n" unless ref($d) eq "HASH";
        $d->{$k} = $v eq "true" ? JSON::PP::true() : JSON::PP::false();
        open(my $out, ">", $f) or die "cannot write $f: $!\n";
        print $out JSON::PP->new->canonical(1)->encode($d);
        close $out;
    ' "$f" "$key" "$val" 2>/dev/null
}

use_hyperkit_engine() {
    mkdir -p "$(dirname "$SETTINGS")"
    [ -f "$SETTINGS" ] || echo '{}' > "$SETTINGS"
    json_set_bool "$SETTINGS" UseVirtualizationFramework false || { echo "cannot edit $SETTINGS" >&2; return 1; }
}

case "${1:-status}" in
install)
    [ -d "$APP" ] || { echo "Docker Desktop is not installed at $APP" >&2; exit 1; }
    pgrep -f "$APP/Contents/MacOS/com.docker.backend" >/dev/null && { echo "quit Docker Desktop first" >&2; exit 1; }
    SHIM=""
    for c in "$HERE/lib/libryzenhv.dylib" "$HERE/build/libryzenhv.dylib"; do [ -f "$c" ] && { SHIM="$c"; break; }; done
    [ -n "$SHIM" ] || { echo "libryzenhv.dylib not found (run make shim)" >&2; exit 1; }

    HKRC=0
    prepare_hyperkit || HKRC=$?
    [ "$HKRC" -eq 2 ] && exit 1                  # not Docker's signed binary, or the re-sign failed: no fallback for that
    if [ "$HKRC" -ne 0 ]; then
        echo "continuing with an unpatched copy: the shim patches HyperKit in memory when it starts, which needs" >&2
        echo "the kext's per-process signing relax, and on a kernel where that does not decode (see rhv-info log) the" >&2
        echo "amfi_get_out_of_my_way=0x1 boot-arg" >&2
    fi

    # --- the shim
    mkdir -p "$LIBDIR"
    cp "$SHIM" "$LIBDIR/libryzenhv.dylib"

    # --- wrappers
    write_wrapper "export DYLD_INSERT_LIBRARIES=\"$LIBDIR/libryzenhv.dylib\""
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
    use_hyperkit_engine
    echo "installed: Docker Desktop in $APP now runs HyperKit through the shim"
    echo "start it as usual (Launchpad, Dock, open -a Docker)"
    ;;
install-native)
    # Native mode: the kext is the hypervisor provider (kern.hv_support on, e.g. `./rhv kext --native --no-amfi`), so
    # HyperKit talks to the system Hypervisor.framework and no shim is loaded. The disk patch is required here: without
    # it the kext would patch HyperKit in memory, and on a kernel where the per-process signing relax does not decode
    # that ends in "Code Signature Invalid" unless the whole system runs with amfi_get_out_of_my_way=0x1.
    [ -d "$APP" ] || { echo "Docker Desktop is not installed at $APP" >&2; exit 1; }
    pgrep -f "$APP/Contents/MacOS/com.docker.backend" >/dev/null && { echo "quit Docker Desktop first" >&2; exit 1; }
    HKRC=0
    prepare_hyperkit || HKRC=$?
    if [ "$HKRC" -ne 0 ]; then
        [ "$HKRC" -eq 2 ] && exit 1              # not Docker's signed binary, or the re-sign failed
        echo "native mode needs the disk patch (there is no shim to patch in memory). Keep the amfi_get_out_of_my_way=0x1" >&2
        echo "boot-arg and use install, or check ~/.ryzenhv and rhv-info log for why the patterns did not fit." >&2
        exit 1
    fi

    # --- the wrapper, without the shim: HyperKit uses the real Hypervisor.framework, which the kext serves
    write_wrapper ""

    # --- a backend wrapper from an earlier shim install is not needed: hv_vm_create() works as Docker ships it
    if is_script "$BACK" && [ -e "$BACK_REAL" ]; then
        rm -f "$BACK"
        mv "$BACK_REAL" "$BACK"
        echo "restored Docker's own backend (the shim wrapper is only needed in shim mode)"
    fi

    # --- settings: HyperKit instead of Apple's Virtualization framework
    use_hyperkit_engine
    echo "installed: Docker Desktop in $APP runs HyperKit with its CPU checks already patched on disk (native mode)"
    echo "start it as usual (Launchpad, Dock, open -a Docker); no AMFI boot-arg is needed"
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
    if is_script "$WRAP"; then
        if grep -q "DYLD_INSERT_LIBRARIES" "$WRAP"; then echo "installed (shim mode: HyperKit and backend wrapped)"
        else echo "installed (native mode: HyperKit patched on disk, no shim)"; fi
    elif is_script "$BACK"; then echo "half installed: run install again"
    else echo "Docker's own files in place"; fi
    ;;
*)
    echo "usage: $0 install|install-native|uninstall|status" >&2
    exit 2
    ;;
esac
