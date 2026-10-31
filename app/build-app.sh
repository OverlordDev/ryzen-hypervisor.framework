#!/bin/bash
#
# Builds build/RyzenHV.app (SwiftUI, x86_64) and build/RyzenHV-app.zip. Needs the Command Line Tools (swift), nothing else.
#   ./build-app.sh [version] [bundle-dir]
#
# With a bundle-dir (an unpacked release folder made by release/package.sh, e.g. ../dist/ryzenhv-0.7.8) the release is put INSIDE the app
# (Contents/Resources/bundle): the app then needs no download, and a user needs no terminal.
set -eu
cd "$(dirname "$0")"
VER="${1:-0.1.0}"
EMBED="${2:-}"

swift build -c release --arch x86_64
BIN="$(swift build -c release --arch x86_64 --show-bin-path)/RyzenHVManager"

APP="build/RyzenHV.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/RyzenHV"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>RyzenHV</string>
    <key>CFBundleIdentifier</key><string>dev.ryzenhv.manager</string>
    <key>CFBundleName</key><string>RyzenHV</string>
    <key>CFBundleDisplayName</key><string>RyzenHV</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VER</string>
    <key>CFBundleVersion</key><string>$VER</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSPrincipalClass</key><string>NSApplication</string>
</dict>
</plist>
PLIST
if [ -n "$EMBED" ]; then
    [ -x "$EMBED/rhv" ] || { echo "$EMBED is not an unpacked release (no rhv)" >&2; exit 1; }
    mkdir -p "$APP/Contents/Resources/bundle"
    ditto "$EMBED" "$APP/Contents/Resources/bundle/$(basename "$EMBED")"
    echo "embedded: $(basename "$EMBED") ($(du -sh "$APP/Contents/Resources/bundle" | cut -f1))"
fi
codesign --force --deep --sign - "$APP" >/dev/null
( cd build && rm -f RyzenHV-app.zip && ditto -c -k --keepParent RyzenHV.app RyzenHV-app.zip )
echo "built: $APP"
