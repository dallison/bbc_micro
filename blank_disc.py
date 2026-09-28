#!/usr/bin/env python3
"""Create a blank BBC Micro DFS or ADFS disc image.

The filename extension selects the format:

  .ssd  DFS, one side
  .dsd  DFS, two sides
  .adf  ADFS, one side (40 tracks is S, 80 tracks is M)
  .adm  ADFS, one side, same layout as .adf
  .adl  ADFS, two sides (80 tracks is L)

Examples:

  python3 blank_disc.py blank.ssd
  python3 blank_disc.py --tracks 40 blank.ssd
  python3 blank_disc.py blank.adf
  python3 blank_disc.py blank.adl
"""

import argparse
import sys


def write_dfs_catalogue(data, offset, tracks):
    sectors = tracks * 10
    data[offset:offset + 8] = b" " * 8
    data[offset + 256:offset + 260] = b" " * 4
    data[offset + 262] = (sectors >> 8) & 3
    data[offset + 263] = sectors & 255


def make_dfs(tracks, sides):
    data = bytearray(tracks * sides * 10 * 256)
    for side in range(sides):
        # Double-sided images interleave the two heads of each track.
        write_dfs_catalogue(data, side * 10 * 256, tracks)
    return data


def adfs_checksum(data, base):
    total = 0
    carry = 0
    for index in range(254, -1, -1):
        total = data[base + index] + total + carry
        carry = total >> 8
        total &= 255
    data[base + 255] = total


def make_adfs(sectors):
    data = bytearray(sectors * 256)
    data[0:3] = (7).to_bytes(3, "little")
    data[252:255] = sectors.to_bytes(3, "little")
    adfs_checksum(data, 0)
    data[256:259] = (sectors - 7).to_bytes(3, "little")
    data[256 + 251] = 1
    data[256 + 254] = 3
    adfs_checksum(data, 256)
    root = 512
    data[root + 1:root + 5] = b"Hugo"
    data[root + 0x4FB:root + 0x4FF] = b"Hugo"
    data[root + 0x4CC] = ord("$")
    data[root + 0x4D6] = 2
    data[root + 0x4D9] = ord("$")
    return data


def geometry(path, tracks):
    name = path.lower()
    if name.endswith(".ssd"):
        return "DFS", 1, tracks * 10
    if name.endswith(".dsd"):
        return "DFS", 2, tracks * 10
    if name.endswith(".adf") or name.endswith(".adm"):
        return "ADFS", 1, tracks * 16
    if name.endswith(".adl"):
        return "ADFS", 2, tracks * 32
    return None, 0, 0


def main(argv):
    parser = argparse.ArgumentParser(description="Create a blank BBC DFS or ADFS disc image.")
    parser.add_argument("image", help="output file, named .ssd, .dsd, .adf, .adm, or .adl")
    parser.add_argument("--tracks", type=int, default=80, choices=(40, 80),
                        help="40 or 80 tracks (default 80)")
    args = parser.parse_args(argv)
    kind, sides, sectors = geometry(args.image, args.tracks)
    if kind is None:
        parser.error("filename must end in .ssd, .dsd, .adf, .adm, or .adl")
    if kind == "DFS":
        data = make_dfs(args.tracks, sides)
    else:
        data = make_adfs(sectors)
    with open(args.image, "wb") as out:
        out.write(data)
    print("%s: %s, %d tracks, %d side%s, %d bytes" % (
        args.image, kind, args.tracks, sides, "" if sides == 1 else "s", len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
