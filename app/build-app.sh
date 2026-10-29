#!/bin/bash
#
# Builds build/RyzenHV.app (SwiftUI, x86_64) and build/RyzenHV-app.zip. Needs the Command Line Tools (swift), nothing else.
#   ./build-app.sh [version]
set -eu
cd "$(dirname "$0")"
VER="${1:-0.1.0}"

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
codesign --force --sign - "$APP" >/dev/null
( cd build && rm -f RyzenHV-app.zip && ditto -c -k --keepParent RyzenHV.app RyzenHV-app.zip )
echo "built: $APP"
