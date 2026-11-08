#!/bin/bash
# Build macos/AppIcon.icns from a photo (default: macos/bbc.jpg).
#
#   macos/make_icon.sh [source.jpg] [out.icns]

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="${1:-$ROOT/bbc.jpg}"
OUT="${2:-$ROOT/AppIcon.icns}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/bbc-icon.XXXXXX")"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

if [[ ! -f "$SRC" ]]; then
  echo "missing source image: $SRC" >&2
  exit 1
fi

W="$(sips -g pixelWidth "$SRC" | awk '/pixelWidth/{print $2}')"
H="$(sips -g pixelHeight "$SRC" | awk '/pixelHeight/{print $2}')"
SIDE=$((W < H ? W : H))
XOFF=$(((W - SIDE) / 2))
YOFF=$(((H - SIDE) / 2))
sips --cropToHeightWidth "$SIDE" "$SIDE" --cropOffset "$YOFF" "$XOFF" "$SRC" \
  --out "$WORK/square.jpg" >/dev/null
sips -s format png -z 1024 1024 "$WORK/square.jpg" --out "$WORK/1024.png" >/dev/null

ICONSET="$WORK/AppIcon.iconset"
mkdir "$ICONSET"
for spec in \
  "icon_16x16.png:16" \
  "icon_16x16@2x.png:32" \
  "icon_32x32.png:32" \
  "icon_32x32@2x.png:64" \
  "icon_128x128.png:128" \
  "icon_128x128@2x.png:256" \
  "icon_256x256.png:256" \
  "icon_256x256@2x.png:512" \
  "icon_512x512.png:512" \
  "icon_512x512@2x.png:1024"; do
  name="${spec%%:*}"
  size="${spec##*:}"
  sips -s format png -z "$size" "$size" "$WORK/1024.png" --out "$ICONSET/$name" >/dev/null
done

iconutil -c icns "$ICONSET" -o "$OUT"
echo "wrote $OUT"
