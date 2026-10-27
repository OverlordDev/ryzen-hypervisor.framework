#!/bin/bash
#
# Runs a program against the RyzenHV shim instead of Apple's Hypervisor.framework:
#
#   ./scripts/run-with-shim.sh ./build/hvf-smoke
#
# Needs root (the kext only lets root create VMs for now) and `env` (sudo drops DYLD_* from the environment).
# RYZENHV_TRACE=1 prints every Hypervisor.framework call the program makes.
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
LIB="$HERE/build/libryzenhv.dylib"
[ -f "$LIB" ] || { echo "build it first: make shim" >&2; exit 1; }
[ $# -ge 1 ] || { echo "usage: $0 <program> [args...]" >&2; exit 2; }
exec sudo env DYLD_INSERT_LIBRARIES="$LIB" RYZENHV_TRACE="${RYZENHV_TRACE:-0}" "$@"
