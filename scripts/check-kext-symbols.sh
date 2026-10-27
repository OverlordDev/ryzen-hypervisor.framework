#!/bin/bash
#
# Fails when the kext imports a symbol that the RUNNING kernel does not export to kexts.
#
# Why: such a kext cannot be linked. OpenCore refuses to inject it with the unhelpful
#      "OC: Prelinked injection X.kext - Invalid Parameter", and the kernel would refuse to load it.
#      A kernel API written from memory (we once used ml_get_max_cpus) is the usual cause.
#
#   ./scripts/check-kext-symbols.sh build/RyzenHV.kext
#
# Checked against the kernel collection of the macOS you are running, so re-run it after an update.

set -eu

KEXT="${1:-}"
if [ -z "$KEXT" ] || [ ! -d "$KEXT" ]; then
    echo "usage: $0 <path to .kext>" >&2
    exit 2
fi
if ! command -v kmutil >/dev/null 2>&1; then
    echo "check-kext-symbols: kmutil not found, skipping the symbol check" >&2
    exit 0
fi

EXE="$KEXT/Contents/MacOS/$(plutil -extract CFBundleExecutable raw -o - "$KEXT/Contents/Info.plist")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

nm -u "$EXE" | sort -u > "$TMP/undefined"
kmutil libraries -p "$KEXT" --arch x86_64 --undef-symbols 2>/dev/null \
    | awk '/KernelCollections/ {print $1}' | sort -u > "$TMP/resolved"

if [ ! -s "$TMP/resolved" ]; then
    echo "check-kext-symbols: kmutil resolved nothing, cannot check (is this an x86_64 Mac?)" >&2
    exit 0
fi

MISSING="$(comm -23 "$TMP/undefined" "$TMP/resolved")"
if [ -n "$MISSING" ]; then
    echo "ERROR: the running kernel does not export these symbols to kexts:" >&2
    echo "$MISSING" | sed 's/^/    /' >&2
    echo "A kext importing them cannot link; OpenCore reports it as 'Prelinked injection ... Invalid Parameter'." >&2
    exit 1
fi
echo "check-kext-symbols: all $(wc -l < "$TMP/undefined" | tr -d ' ') imported symbols resolve against this kernel"
