#!/bin/sh
# Fetches a MinGW-w64 cross compiler and a portable Wine into .wintools, so
# the Windows build can be compiled and run on Linux without root. Neither
# is installed system-wide; delete .wintools to remove them.
#
#   ./windows_tools.sh
#   export PATH="$PWD/.wintools/llvm-mingw/bin:$PWD/.wintools/wine/bin:$PATH"

set -eu

LLVM_MINGW_VERSION=20260922
WINE_VERSION=11.18

root=$(cd "$(dirname "$0")" && pwd)
tools="$root/.wintools"
mkdir -p "$tools"
cd "$tools"

if [ ! -x llvm-mingw/bin/x86_64-w64-mingw32-gcc ]; then
  name="llvm-mingw-$LLVM_MINGW_VERSION-ucrt-ubuntu-22.04-x86_64"
  curl -fL -o "$name.tar.xz" \
    "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_VERSION/$name.tar.xz"
  tar -xf "$name.tar.xz"
  rm -rf llvm-mingw
  mv "$name" llvm-mingw
  rm "$name.tar.xz"
fi

# The wow64 build runs 32-bit and 64-bit programs without i386 host
# libraries.
if [ ! -x wine/bin/wine ]; then
  name="wine-$WINE_VERSION-amd64-wow64"
  curl -fL -o "$name.tar.xz" \
    "https://github.com/Kron4ek/Wine-Builds/releases/download/$WINE_VERSION/$name.tar.xz"
  tar -xf "$name.tar.xz"
  rm -rf wine
  mv "$name" wine
  rm "$name.tar.xz"
fi

echo "export PATH=\"$tools/llvm-mingw/bin:$tools/wine/bin:\$PATH\""
