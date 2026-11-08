#!/usr/bin/env python3
"""Install a ROM image into bbc_b_rom_sockets or bbc_master_rom_sockets.

The destination name matches what the emulator loads:

  os.rom / os-<name>.rom
  <socket>.rom / <socket>-<name>.rom          image 0
  <socket>.<virtual>-<name>.rom               image 1-7 (virtual 0 also ok)

SOCKET is 0-15, or SOCKET.VIRTUAL for another image in that socket
(virtual 0-7). Use os for the MOS.

Examples:

  python3 install_rom.py -m master os ~/roms/os-3.50.rom
  python3 install_rom.py -m master 15 ~/roms/terminal.rom
  python3 install_rom.py -m master --name libc 8 ~/roms/libc.rom
  python3 install_rom.py -m master --name libm 8.1 ~/roms/libm.rom
  python3 install_rom.py --roms /path/to/sockets 14 ~/roms/dfs.rom
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import sys


SOCKET_DIRS = {
    "b": "bbc_b_rom_sockets",
    "master": "bbc_master_rom_sockets",
    "master128": "bbc_master_rom_sockets",
    "master256": "bbc_master_rom_sockets",
}


def sanitize_name(name: str) -> str:
    name = name.strip()
    if not name:
        raise ValueError("name is empty")
    if name.lower().endswith(".rom"):
        name = name[:-4]
    # Keep the label readable in the socket directory listing.
    cleaned = re.sub(r"[^A-Za-z0-9._+-]+", "-", name).strip("-.")
    if not cleaned:
        raise ValueError("name has no usable characters")
    return cleaned


def default_name(source: str) -> str:
    base = os.path.basename(source)
    if base.lower().endswith(".rom"):
        base = base[:-4]
    # Strip a leading socket/virtual prefix the user may already have used.
    stripped = re.sub(r"^(?:os-|(?:\d{1,2})(?:\.\d)?-)", "", base, flags=re.I)
    return sanitize_name(stripped or base)


def parse_socket(text: str) -> tuple[str, int | None, int]:
    """Return (kind, slot, virt). kind is 'os' or 'sideways'."""
    text = text.strip().lower()
    if text in ("os", "mos"):
        return "os", None, 0
    match = re.fullmatch(r"(\d{1,2})(?:\.(\d{1,2}))?", text)
    if not match:
        raise ValueError("socket must be os, 0-15, or 0-15.0-7")
    slot = int(match.group(1))
    if slot > 15:
        raise ValueError("socket must be 0-15")
    virt = 0
    if match.group(2) is not None:
        virt = int(match.group(2))
        if virt > 7:
            raise ValueError("virtual image must be 0-7")
    return "sideways", slot, virt


def classify_existing(name: str) -> tuple[str, int | None, int] | None:
    """Match ClassifyRomName in bbc_hardware.c. Returns kind/slot/virt."""
    if not name.lower().endswith(".rom") or name.startswith("."):
        return None
    stem = name[:-4]
    lower = stem.lower()
    if lower == "os" or lower.startswith("os-"):
        return "os", None, 0
    match = re.fullmatch(r"(\d{1,2})(?:\.(\d{1,2})-(.+)|(?:-(.+))?)?", stem)
    if not match:
        return None
    slot = int(match.group(1))
    if slot > 15:
        return None
    if match.group(2) is not None:
        virt = int(match.group(2))
        if virt > 7 or not match.group(3):
            return None
        return "sideways", slot, virt
    return "sideways", slot, 0


def destination_name(kind: str, slot: int | None, virt: int, name: str | None) -> str:
    if kind == "os":
        return "os.rom" if not name else f"os-{name}.rom"
    assert slot is not None
    if virt != 0:
        if not name:
            raise ValueError("virtual images need --name (for example 8.1-libm.rom)")
        return f"{slot}.{virt}-{name}.rom"
    if name:
        return f"{slot}-{name}.rom"
    return f"{slot}.rom"


def find_conflicts(roms_dir: str, kind: str, slot: int | None, virt: int) -> list[str]:
    conflicts = []
    for entry in os.listdir(roms_dir):
        parsed = classify_existing(entry)
        if parsed is None:
            continue
        existing_kind, existing_slot, existing_virt = parsed
        if existing_kind == kind and existing_slot == slot and existing_virt == virt:
            conflicts.append(entry)
    return sorted(conflicts)


def resolve_roms_dir(args: argparse.Namespace) -> str:
    if args.roms:
        return args.roms
    if not args.machine:
        raise SystemExit("say -m b|master or --roms DIR")
    root = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(root, SOCKET_DIRS[args.machine])


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Install a ROM image into a Model B or Master socket directory."
    )
    parser.add_argument(
        "-m",
        "--machine",
        choices=sorted(SOCKET_DIRS),
        help="which socket directory beside this script to use",
    )
    parser.add_argument(
        "--roms",
        metavar="DIR",
        help="socket directory (overrides -m)",
    )
    parser.add_argument(
        "--name",
        help="label in the installed filename (default: from the source name)",
    )
    parser.add_argument(
        "--plain",
        action="store_true",
        help="install as os.rom or <socket>.rom with no label (image 0 only)",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="replace any ROM already in that socket/image",
    )
    parser.add_argument(
        "socket",
        help="os, socket 0-15, or socket.virtual (virtual 0-7)",
    )
    parser.add_argument(
        "source",
        help="path to the .rom image to install",
    )
    args = parser.parse_args(argv)

    try:
        kind, slot, virt = parse_socket(args.socket)
    except ValueError as err:
        parser.error(str(err))

    source = os.path.abspath(args.source)
    if not os.path.isfile(source):
        parser.error(f"source ROM not found: {source}")

    if args.plain and args.name:
        parser.error("use either --plain or --name, not both")
    if args.plain and virt != 0:
        parser.error("--plain only applies to os or image 0")

    label: str | None
    if args.plain:
        label = None
    else:
        try:
            label = sanitize_name(args.name) if args.name else default_name(source)
        except ValueError as err:
            parser.error(str(err))
        if kind == "os" and not args.name and label.lower() in ("os", "mos"):
            label = None

    try:
        dest_name = destination_name(kind, slot, virt, label)
    except ValueError as err:
        parser.error(str(err))

    roms_dir = resolve_roms_dir(args)
    os.makedirs(roms_dir, exist_ok=True)

    conflicts = find_conflicts(roms_dir, kind, slot, virt)
    dest_path = os.path.join(roms_dir, dest_name)
    # Installing under a new name still collides with the same socket/image.
    others = [c for c in conflicts if c != dest_name]
    if (os.path.exists(dest_path) or others) and not args.force:
        shown = ", ".join(conflicts) if conflicts else dest_name
        parser.error(
            f"socket already occupied by {shown}; pass --force to replace"
        )

    for old in others:
        old_path = os.path.join(roms_dir, old)
        os.remove(old_path)
        print(f"removed {old_path}")

    size = os.path.getsize(source)
    if size == 0:
        parser.error("source ROM is empty")
    if size > 16384:
        print(
            f"warning: {source} is {size} bytes; the emulator loads only the first 16 KiB",
            file=sys.stderr,
        )
    elif size not in (8192, 16384):
        print(
            f"warning: {source} is {size} bytes; shorter images are padded with &FF",
            file=sys.stderr,
        )

    shutil.copy2(source, dest_path)
    where = "OS" if kind == "os" else f"socket {slot} image {virt}"
    print(f"installed {source} -> {dest_path} ({where})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
