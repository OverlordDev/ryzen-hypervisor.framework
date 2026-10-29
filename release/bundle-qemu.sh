#!/bin/bash
#
# Turns a QEMU build tree into a relocatable directory for the release bundle.
#
#   release/bundle-qemu.sh <qemu-build-dir> <qemu-source-dir> <out-dir>
#
# out-dir gets bin/qemu-system-x86_64, lib/*.dylib (glib, pixman, libslirp and what they need) and share/qemu/ (firmware).
# All library references become @executable_path / @loader_path relative, the binaries are stripped of debug maps (which
# hold build directories) and signed ad hoc again. The checks at the end fail if a path of the build machine is left.
set -eu
BUILD="${1:?usage: bundle-qemu.sh <qemu-build-dir> <qemu-source-dir> <out-dir>}"
SRC="${2:?}"
OUT="${3:?}"
rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/share/qemu"
cp "$BUILD/qemu-system-x86_64" "$OUT/bin/"

# Firmware the PC machine loads from the data directory (-L); EFI images and other architectures are not needed.
for f in bios-256k.bin bios.bin vgabios.bin vgabios-stdvga.bin vgabios-cirrus.bin vgabios-vmware.bin vgabios-qxl.bin vgabios-virtio.bin \
         vgabios-ramfb.bin vgabios-bochs-display.bin kvmvapic.bin linuxboot.bin linuxboot_dma.bin multiboot.bin multiboot_dma.bin \
         pvh.bin sgabios.bin efi-virtio.rom efi-e1000.rom efi-rtl8139.rom efi-pcnet.rom efi-ne2k_pci.rom efi-eepro100.rom efi-e1000e.rom \
         efi-vmxnet3.rom; do
    [ -f "$SRC/pc-bios/$f" ] && cp "$SRC/pc-bios/$f" "$OUT/share/qemu/"
done
cp -R "$SRC/pc-bios/keymaps" "$OUT/share/qemu/"

# Copy the closure of non-system libraries.
deps() { otool -L "$1" | tail -n +2 | awk '{print $1}' | grep -v -e '^/usr/lib/' -e '^/System/' -e '^@' || true; }
queue="$(deps "$OUT/bin/qemu-system-x86_64")"
seen=""
while [ -n "$queue" ]; do
    next=""
    for lib in $queue; do
        base="$(basename "$lib")"
        case " $seen " in *" $base "*) continue ;; esac
        seen="$seen $base"
        real="$(cd "$(dirname "$lib")" && pwd -P)/$base"
        cp -L "$real" "$OUT/lib/$base"
        chmod u+w "$OUT/lib/$base"
        next="$next $(deps "$OUT/lib/$base")"
    done
    queue="$next"
done

fix() {   # file, prefix for references to bundled libraries
    local f="$1" pre="$2" ref
    for ref in $(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -v -e '^/usr/lib/' -e '^/System/' -e '^@'); do
        install_name_tool -change "$ref" "$pre/$(basename "$ref")" "$f"
    done
    # drop absolute rpaths of the build machine
    local rp
    for rp in $(otool -l "$f" | awk '/cmd LC_RPATH/ {getline; getline; print $2}'); do install_name_tool -delete_rpath "$rp" "$f"; done
}
fix "$OUT/bin/qemu-system-x86_64" "@executable_path/../lib"
for l in "$OUT"/lib/*.dylib; do
    install_name_tool -id "@loader_path/$(basename "$l")" "$l"
    fix "$l" "@loader_path"
done

strip -S -x "$OUT/bin/qemu-system-x86_64" 2>/dev/null || true
for l in "$OUT"/lib/*.dylib; do strip -S -x "$l" 2>/dev/null || true; done

# __FILE__ strings of the compiled sources still name the build directory (../../../Users/<name>/...). Same-length
# replacement keeps every offset intact; the strings are only used in assertion messages.
u="$(id -un)"; pad="$(printf '%*s' "${#u}" '' | tr ' ' x)"
for f in "$OUT/bin/qemu-system-x86_64" "$OUT"/lib/*.dylib; do LC_ALL=C perl -pi -e "s#/Users/$u/#/Users/$pad/#g" "$f"; done

ENT="$(mktemp)"
cat > "$ENT" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.hypervisor</key><true/></dict></plist>
EOF
for l in "$OUT"/lib/*.dylib; do codesign --force --sign - "$l" >/dev/null 2>&1; done
codesign --force --sign - --entitlements "$ENT" "$OUT/bin/qemu-system-x86_64" >/dev/null 2>&1
rm -f "$ENT"

# Checks.
fail=0
for f in "$OUT/bin/qemu-system-x86_64" "$OUT"/lib/*.dylib; do
    if otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -v -e '^/usr/lib/' -e '^/System/' -e '^@executable_path/' -e '^@loader_path/' | grep -q .; then
        echo "unrelocated reference in $f:"; otool -L "$f" | grep -v -e '/usr/lib/' -e '/System/' -e '@'; fail=1
    fi
    if otool -l "$f" | grep -q LC_RPATH; then echo "rpath left in $f"; fail=1; fi
    if strings "$f" | grep -q -e "$u" -e '/private/tmp/claude'; then echo "user name or scratch path inside $f:"; strings "$f" | grep -m3 -e "$u" -e '/private/tmp/claude'; fail=1; fi
done
[ $fail = 0 ] || { echo "bundle-qemu: checks failed" >&2; exit 1; }
"$OUT/bin/qemu-system-x86_64" -L "$OUT/share/qemu" --version | head -1
du -sh "$OUT"
