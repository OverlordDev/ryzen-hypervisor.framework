#!/bin/bash
#
# Copies build/RyzenHV.kext into a mounted OpenCore EFI partition and keeps a
# backup of whatever was there. Does NOT edit config.plist.
#
#   1. mount your EFI partition (MountEFI, or: diskutil list; sudo diskutil mount diskXsY)
#   2. ./scripts/install-to-efi.sh /Volumes/EFI
#   3. first time only: add the Kernel -> Add entry from docs/RYZENTOSH.md
#   4. reboot

set -eu

EFI="${1:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
KEXT="$HERE/../build/RyzenHV.kext"

if [ -z "$EFI" ]; then
    echo "usage: $0 <mounted EFI volume, e.g. /Volumes/EFI>" >&2
    exit 2
fi
if [ ! -d "$EFI/EFI/OC" ]; then
    echo "error: $EFI/EFI/OC not found, is this the OpenCore EFI partition?" >&2
    exit 1
fi
if [ ! -d "$KEXT" ]; then
    echo "error: $KEXT not built (run: make kext)" >&2
    exit 1
fi

DEST="$EFI/EFI/OC/Kexts/RyzenHV.kext"
if [ -d "$DEST" ]; then
    BACKUP="$EFI/EFI/OC/Kexts/RyzenHV.kext.bak-$(date +%Y%m%d-%H%M%S)"
    echo "backing up old kext to $BACKUP"
    # A .bak directory inside Kexts/ is ignored by OpenCore (it only loads what config.plist lists).
    mv "$DEST" "$BACKUP"
fi

cp -R "$KEXT" "$DEST"
echo "installed $DEST"

if ! grep -q "RyzenHV.kext" "$EFI/EFI/OC/config.plist" 2>/dev/null; then
    echo
    echo "NOTE: config.plist has no RyzenHV.kext entry yet."
    echo "      Add it under Kernel -> Add, see docs/RYZENTOSH.md."
fi
echo
echo "Reminder: keep a working fallback EFI without this kext on a USB stick."
