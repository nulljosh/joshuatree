#!/bin/bash
# Build + ad-hoc sign + relaunch the menu bar monitor. Signing (even ad-hoc)
# gives the binary a stable identity so macOS stops treating every rebuild as
# a brand-new unidentified app and re-prompting for permission.
set -e
cd "$(dirname "$0")"

mkdir -p JoshuaTreeMonitor.app/Contents/MacOS
cat > JoshuaTreeMonitor.app/Contents/Info.plist << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>JoshuaTreeMonitor</string>
    <key>CFBundleIdentifier</key><string>com.nulljosh.joshuatreemonitor</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>LSUIElement</key><true/>
</dict>
</plist>
EOF

swiftc -O -parse-as-library main.swift -o JoshuaTreeMonitor.app/Contents/MacOS/JoshuaTreeMonitor
codesign --force --deep --sign - JoshuaTreeMonitor.app
xattr -cr JoshuaTreeMonitor.app

pkill -f JoshuaTreeMonitor 2>/dev/null || true
sleep 0.5
# Launch the binary directly rather than `open` -- `open` routes through
# LaunchServices, which re-runs Gatekeeper's "unidentified developer" check
# on every rebuild (new hash = never seen this before), even after ad-hoc signing.
nohup ./JoshuaTreeMonitor.app/Contents/MacOS/JoshuaTreeMonitor > /tmp/joshuatree_monitor.log 2>&1 &
disown
