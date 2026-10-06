#!/bin/bash
# Build a macOS installer package of the emulator, Cumana, the file server,
# the cassette player, the 6502 interpreter, and the disc Python scripts.
#
#   macos/package.sh
#
# The package is written to dist/BBC Micro.pkg. It installs into
# /Applications/BBC Micro. The ROM socket directories are included with
# a README and no ROM images.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ROOT}/build-release"
DIST="${ROOT}/dist"
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/bbc-pkg.XXXXXX")"
APP="${STAGE}/BBC Micro.app"
VERSION="$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)"
PKG_VERSION="1.0.0"

cleanup() {
  rm -rf "$STAGE"
}
trap cleanup EXIT

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --target bbc cumana fileserver cassette davecc6502 bbc_hardware_test \
  -j "$(sysctl -n hw.ncpu)"
"${BUILD}/bbc_hardware_test"
"${BUILD}/bbc" -selftest

mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Resources"
sed "s/@VERSION@/${VERSION}/g" "${ROOT}/macos/Info.plist" > "${APP}/Contents/Info.plist"
cp "${BUILD}/bbc" "${APP}/Contents/MacOS/bbc"
chmod 755 "${APP}/Contents/MacOS/bbc"

cp "${BUILD}/cumana" "${STAGE}/cumana"
cp "${BUILD}/fileserver" "${STAGE}/fileserver"
cp "${BUILD}/cassette" "${STAGE}/cassette"
cp "${BUILD}/6502" "${STAGE}/6502"
chmod 755 "${STAGE}/cumana" "${STAGE}/fileserver" "${STAGE}/cassette" "${STAGE}/6502"
ln -s "BBC Micro.app/Contents/MacOS/bbc" "${STAGE}/bbc"

for script in blank_disc.py blank_tape.py copy_from_disc.py copy_to_hd.py examine_disc.py; do
  cp "${ROOT}/${script}" "${STAGE}/${script}"
  chmod 755 "${STAGE}/${script}"
done

for doc in README.md ECONET.md FILESERVER.md; do
  cp "${ROOT}/${doc}" "${STAGE}/${doc}"
done

write_socket_readme() {
  local dest="$1"
  local title="$2"
  mkdir -p "$dest"
  cat > "${dest}/README.txt" << EOF
${title}

Copy each ROM image into this folder. The emulator reads it at startup.
This file is left in place.

os.rom or os-<name>.rom is the operating system, for example os-1.20.rom
on the Model B and os-3.50.rom on the Master. <socket>.rom or
<socket>-<name>.rom is sideways socket 0-15, for example 15-basic2.rom
and 14-dfs.rom. That file is virtual image 0, the one the machine sees
at startup.

<socket>.<virtual>-<name>.rom is another image in the same socket, for
example 4.1-libc.rom. virtual is 0 to 7. 0 is the same image as the
plain socket file. Bits 4-6 of the ROM select register choose it:
socket 4 image 1 is the value &14. A write of the socket number alone
selects image 0.

An image is 16 KiB. A shorter image fills the rest of the socket with
&FF, which is how an 8 KiB ROM is mapped.

DFS and ADFS may both be present. Break selects the one in the higher
socket. *DISC and *ADFS select the other. When both files name the same
socket, the second is fitted in the highest free socket.
EOF
}
write_socket_readme "${STAGE}/bbc_b_rom_sockets" "Model B ROM images"
write_socket_readme "${STAGE}/bbc_master_rom_sockets" "Master ROM images"

cat > "${STAGE}/README.txt" << EOF
BBC Micro

The installer places this folder in /Applications/BBC Micro.

BBC Micro.app is the machine. Double-click it. From a terminal in this
folder, ./bbc is the same program.

  ./cumana blank.ssd
  ./fileserver ~/econet
  ./cassette blank.uef
  ./6502
  ./blank_disc.py blank.ssd
  ./blank_tape.py blank.uef
  ./copy_from_disc.py disc.ssd saved
  ./copy_to_hd.py disc.ssd disc.hd
  ./examine_disc.py disc.adl

bbc_b_rom_sockets and bbc_master_rom_sockets sit beside the application.
Each one contains a README.txt that explains how to add a ROM image.
The application looks for those directories beside itself.

Discs, tapes, and Econet are described in README.md, ECONET.md, and
FILESERVER.md.
EOF

codesign --force --sign - "$APP"
codesign --force --sign - "${STAGE}/cumana"
codesign --force --sign - "${STAGE}/fileserver"
codesign --force --sign - "${STAGE}/cassette"
codesign --force --sign - "${STAGE}/6502"

mkdir -p "$DIST"
rm -f "${DIST}/BBC Micro.dmg" "${DIST}/BBC Micro.pkg"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/bbc-pkgwork.XXXXXX")"
cleanup() {
  rm -rf "$STAGE" "$WORK"
}
sed "s/@PKG_VERSION@/${PKG_VERSION}/g" "${ROOT}/macos/installer/distribution.xml" > "${WORK}/distribution.xml"
cp "${ROOT}/macos/installer/welcome.txt" "${ROOT}/macos/installer/readme.txt" \
  "${ROOT}/macos/installer/conclusion.txt" "$WORK/"
chmod 755 "${ROOT}/macos/installer/scripts/postinstall"
pkgbuild \
  --root "$STAGE" \
  --identifier com.github.dallison.bbc-micro \
  --version "$PKG_VERSION" \
  --install-location "/Applications/BBC Micro" \
  --component-plist "${ROOT}/macos/installer/components.plist" \
  --scripts "${ROOT}/macos/installer/scripts" \
  --ownership recommended \
  "${WORK}/BBCMicroComponent.pkg"
productbuild \
  --distribution "${WORK}/distribution.xml" \
  --package-path "$WORK" \
  --resources "$WORK" \
  "${DIST}/BBC Micro.pkg"
echo "wrote ${DIST}/BBC Micro.pkg"
