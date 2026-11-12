#!/bin/bash
# make_dmg.sh -- the Mac disk image: SentinelWatch.app next to a shortcut to /Applications, so it can be dragged over.
#   usage (on a Mac, after ./build.sh):  bash installer/macos/make_dmg.sh [build-mac/SentinelWatch.app] [SentinelWatch-macOS.dmg]
# The app is only ad-hoc signed (no Apple developer certificate): the first start is right-click -> Open (README section 12).
set -eu
APP=${1:-build-mac/SentinelWatch.app}
OUT=${2:-SentinelWatch-macOS.dmg}
[ -d "$APP" ] || { echo "not found: $APP (run ./build.sh on a Mac first)"; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cp -R "$APP" "$T/"
ln -s /Applications "$T/Applications"
rm -f "$OUT"
hdiutil create -volname "Sentinel Watch" -srcfolder "$T" -format UDZO "$OUT"
echo "made $OUT"
