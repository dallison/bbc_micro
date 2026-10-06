#!/usr/bin/env python3
"""Copy the files on a DFS or ADFS disc image into a folder on this computer.

$.HELLO becomes HELLO in the folder. A DFS directory becomes a subdirectory,
and the second side of a DFS disc is the folder 2. An ADFS comma in a name
becomes a dot, so MAIN,C is saved as MAIN.C. Each file is accompanied by a
NAME.inf holding its load address, execution address, and access byte, which
is the form the file server reads. A file made of tabs, carriage returns,
line feeds, and printable characters is text: each carriage return is saved
as a line feed. Any other byte leaves the file unchanged.

Examples:

  python3 copy_from_disc.py disc.ssd saved
  python3 copy_from_disc.py disc.adl ~/discs/library
"""

import argparse
import os
import sys

from copy_to_hd import DiscError, read_floppy

# The file server's access byte. Low to high: public read, public write,
# owner read, owner write, locked, directory. 0x0D is an ordinary file.
FILE_ACCESS = 0x0D
LOCKED = 0x10
DIR_ACCESS = 0x2D


def host_component(name):
    """A disc name as a single path component. A comma is a dot."""
    text = name.replace(",", ".")
    cleaned = []
    for char in text:
        code = ord(char)
        if char in "/\\" or code < 32 or code == 127:
            cleaned.append("_")
        else:
            cleaned.append(char)
    text = "".join(cleaned)
    if text in ("", ".", ".."):
        raise DiscError("cannot store the name %r on the host" % name)
    return text


def under(root, parts):
    path = os.path.abspath(root)
    for part in parts:
        path = os.path.abspath(os.path.join(path, part))
    root_abs = os.path.abspath(root)
    if os.path.commonpath([root_abs, path]) != root_abs:
        raise DiscError("refusing to write outside %s" % root)
    return path


def is_text(data):
    """True when every byte is a tab, a newline, or a printable character."""
    for byte in data:
        if byte in (9, 10, 13) or 32 <= byte < 127:
            continue
        return False
    return True


def host_bytes(data):
    if is_text(data):
        return data.replace(b"\r", b"\n")
    return data


def write_inf(path, load, exec_addr, access):
    inf = path + ".inf"
    if os.path.isdir(inf):
        raise DiscError("%s is a directory" % inf)
    with open(inf, "w", encoding="ascii") as handle:
        handle.write("%08X %08X %02X\n" % (load & 0xFFFFFFFF, exec_addr & 0xFFFFFFFF, access))


def write_tree(tree, dest):
    seen = set()
    copied = [0]

    def walk(children, parts):
        for item in children:
            component = host_component(item["name"])
            key = tuple(parts + [component])
            if key in seen:
                raise DiscError("two names would both be saved as %s" % "/".join(key))
            seen.add(key)
            path = under(dest, list(key))
            shown = "/".join(key)
            if item["dir"]:
                if os.path.isfile(path):
                    raise DiscError("%s is a file" % shown)
                os.makedirs(path, exist_ok=True)
                write_inf(path, 0, 0, DIR_ACCESS)
                print(shown + "/")
                walk(item["children"], list(key))
                continue
            if os.path.isdir(path):
                raise DiscError("%s is a directory" % shown)
            parent = os.path.dirname(path)
            if parent:
                os.makedirs(parent, exist_ok=True)
            with open(path, "wb") as handle:
                handle.write(host_bytes(item["data"]))
            access = FILE_ACCESS | LOCKED if item.get("locked") else FILE_ACCESS
            write_inf(path, item["load"], item["exec"], access)
            print(shown)
            copied[0] += 1

    walk(tree, [])
    return copied[0]


def main(argv):
    parser = argparse.ArgumentParser(
        description="Copy the files on a DFS or ADFS disc image into a folder.")
    parser.add_argument("image", help="source .ssd, .dsd, .adf, .adm, .adl, or .hd")
    parser.add_argument("folder", help="directory to create on this computer")
    args = parser.parse_args(argv)
    try:
        if os.path.abspath(args.image) == os.path.abspath(args.folder):
            raise DiscError("the disc image and the folder are the same path")
        tree = read_floppy(args.image)
        os.makedirs(args.folder, exist_ok=True)
        if not os.path.isdir(args.folder):
            raise DiscError("%s is not a directory" % args.folder)
        copied = write_tree(tree, args.folder)
    except DiscError as error:
        print("copy_from_disc: %s" % error, file=sys.stderr)
        return 1
    print("%d copied into %s" % (copied, args.folder))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
