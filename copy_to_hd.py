#!/usr/bin/env python3
"""Copy a DFS or ADFS floppy image onto an ADFS hard disc image.

The hard disc is the old-map layout written by blank_disc.py, which is what
ADFS 1.30 reads. DFS directories become ADFS directories, and $ stays the
root. An ADFS floppy is copied as a tree. A second DFS side is placed in
directory 2, the same number DFS uses for the other side of the disc.

Examples:

  python3 copy_to_hd.py disc.ssd disc.hd
  python3 copy_to_hd.py --into saved disc.adl disc.hd
  python3 copy_to_hd.py --list disc.dsd
"""

import argparse
import os
import sys

from blank_disc import adfs_checksum

SECTOR = 256
DIR_BYTES = 5 * SECTOR
DIR_ENTRIES = 47
ENTRY = 26
MAP_ENTRIES = 82
ROOT_SECTOR = 2

# Bit 7 of the first five name bytes, in the order ADFS 1.30 prints them.
ATTR_R = 1 << 0
ATTR_W = 1 << 1
ATTR_L = 1 << 2
ATTR_D = 1 << 3
ATTR_E = 1 << 4


class DiscError(Exception):
    pass


def read_image(path):
    with open(path, "rb") as handle:
        data = bytearray(handle.read())
    if len(data) == 0 or (len(data) % SECTOR) != 0:
        raise DiscError("%s is not a whole number of 256-byte sectors" % path)
    return data


def hugo_at(data, sector):
    base = sector * SECTOR
    if sector < 0 or base + DIR_BYTES > len(data):
        return False
    return (data[base + 1:base + 5] == b"Hugo" and
            data[base + 0x4FB:base + 0x4FF] == b"Hugo")


def directory_ok(data, sector):
    """A directory Hugo may be missing from a blank trailer, as on c14's h."""
    if hugo_at(data, sector):
        return True
    base = sector * SECTOR
    if sector < 0 or base + DIR_BYTES > len(data):
        return False
    if data[base + 1:base + 5] != b"Hugo":
        return False
    return data[base + 0x4CC:base + DIR_BYTES] == bytes(DIR_BYTES - 0x4CC)


def decode_name(raw):
    """Return the visible name and the attribute bits stored in it."""
    chars = []
    attrs = 0
    for index, byte in enumerate(raw[:10]):
        # Bit 7 is an attribute. A short directory name keeps D on the
        # terminator itself, so &8D ends the name just as &0D does.
        if (byte & 0x7F) == 0 or (byte & 0x7F) == 0x0D:
            break
        chars.append(chr(byte & 0x7F))
        if byte & 0x80:
            attrs |= 1 << index
    # Attribute bits past the terminator still sit on the padding bytes.
    for index, byte in enumerate(raw[:5]):
        if index >= len(chars) and (byte & 0x80):
            attrs |= 1 << index
    return "".join(chars), attrs


def encode_name(name, attrs):
    """Ten name bytes. The first &0D ends the name; later bytes hold attributes."""
    raw = bytearray(b"\x0d" * 10)
    for index, char in enumerate(name[:10]):
        raw[index] = ord(char) & 0x7F
    for bit in range(5):
        if (attrs & (1 << bit)) == 0:
            continue
        if bit < len(name):
            raw[bit] |= 0x80
        else:
            raw[bit] = 0x0D | 0x80
    return bytes(raw)


def sort_key(name):
    """The order ADFS searches. It folds each character with ORA #&20."""
    return bytes((ord(char) | 0x20) & 0xFF for char in name)


def object_name(name):
    cleaned = []
    for char in name:
        if char in " .:$\"#*":
            cleaned.append("_")
        elif 32 < ord(char) < 127:
            cleaned.append(char)
    text = "".join(cleaned).strip("._")
    if text == "":
        raise DiscError("cannot store the name %r on ADFS" % name)
    return text[:10]


class AdfsImage(object):
    def __init__(self, path, data):
        self.path = path
        self.data = data
        if not hugo_at(data, ROOT_SECTOR):
            raise DiscError(
                "%s is not an old-map ADFS disc (no root directory at sector 2)" % path)
        size = int.from_bytes(data[252:255], "little")
        if size != len(data) // SECTOR:
            raise DiscError(
                "%s says it has %d sectors but the file has %d" %
                (path, size, len(data) // SECTOR))
        for base in (0, SECTOR):
            total = 0
            carry = 0
            for index in range(254, -1, -1):
                total = data[base + index] + total + carry
                carry = total >> 8
                total &= 255
            if total != data[base + 255]:
                raise DiscError("%s has a bad free-space checksum" % path)
        self.size = size

    def save(self):
        with open(self.path, "wb") as handle:
            handle.write(self.data)

    def holes(self):
        # Sector 1 byte 254 is the end of the free-space list, three bytes per hole.
        count = self.data[SECTOR + 254] // 3
        found = []
        for index in range(min(count, MAP_ENTRIES)):
            start = int.from_bytes(self.data[index * 3:index * 3 + 3], "little")
            length = int.from_bytes(
                self.data[SECTOR + index * 3:SECTOR + index * 3 + 3], "little")
            if start == 0 and length == 0:
                break
            found.append((start, length))
        return found

    def write_holes(self, holes):
        if len(holes) > MAP_ENTRIES:
            raise DiscError("the free-space map is full")
        for index in range(MAP_ENTRIES):
            if index < len(holes):
                start, length = holes[index]
            else:
                start, length = 0, 0
            self.data[index * 3:index * 3 + 3] = start.to_bytes(3, "little")
            self.data[SECTOR + index * 3:SECTOR + index * 3 + 3] = length.to_bytes(3, "little")
        # ADFS treats this byte as the list length. Leaving the old length in
        # place makes it look for a hole that is no longer there: "Bad FS map".
        self.data[SECTOR + 254] = len(holes) * 3
        adfs_checksum(self.data, 0)
        adfs_checksum(self.data, SECTOR)

    def allocate(self, count):
        if count <= 0:
            return 0
        holes = self.holes()
        for index, (start, length) in enumerate(holes):
            if length < count:
                continue
            if length == count:
                del holes[index]
            else:
                holes[index] = (start + count, length - count)
            self.write_holes(holes)
            return start
        raise DiscError("the hard disc does not have %d free sectors" % count)

    def release(self, start, count):
        if count <= 0 or start <= 0:
            return
        holes = self.holes()
        holes.append((start, count))
        holes.sort()
        merged = []
        for hole_start, hole_len in holes:
            if hole_len <= 0:
                continue
            if not merged:
                merged.append((hole_start, hole_len))
                continue
            prev_start, prev_len = merged[-1]
            prev_end = prev_start + prev_len
            this_end = hole_start + hole_len
            if hole_start <= prev_end:
                merged[-1] = (prev_start, max(prev_end, this_end) - prev_start)
            else:
                merged.append((hole_start, hole_len))
        self.write_holes(merged)

    def read_bytes(self, sector, length):
        if length <= 0:
            return b""
        start = sector * SECTOR
        end = start + length
        if start < 0 or end > len(self.data):
            raise DiscError("file at sector %d runs off the end of the disc" % sector)
        return bytes(self.data[start:end])

    def write_bytes(self, sector, payload):
        if not payload:
            return
        count = (len(payload) + SECTOR - 1) // SECTOR
        start = sector * SECTOR
        end = start + count * SECTOR
        if end > len(self.data):
            raise DiscError("file at sector %d runs off the end of the disc" % sector)
        self.data[start:end] = b"\x00" * (count * SECTOR)
        self.data[start:start + len(payload)] = payload

    def directory(self, sector):
        base = sector * SECTOR
        return self.data[base:base + DIR_BYTES]

    def store_directory(self, sector, directory):
        base = sector * SECTOR
        self.data[base:base + DIR_BYTES] = directory

    def bump(self, directory):
        sequence = (directory[0] + 1) & 255
        directory[0] = sequence
        directory[0x4FA] = sequence

    def entries(self, sector):
        directory = self.directory(sector)
        found = []
        for index in range(DIR_ENTRIES):
            offset = 5 + index * ENTRY
            raw = bytes(directory[offset:offset + ENTRY])
            if raw[0] == 0:
                break
            name, attrs = decode_name(raw[:10])
            if name == "":
                continue
            load = int.from_bytes(raw[10:14], "little")
            exec_addr = int.from_bytes(raw[14:18], "little")
            length = int.from_bytes(raw[18:22], "little")
            start = int.from_bytes(raw[22:25], "little")
            found.append({
                "index": index,
                "name": name,
                "attrs": attrs,
                "load": load,
                "exec": exec_addr,
                "length": length,
                "sector": start,
            })
        return found

    def find(self, sector, name):
        wanted = name.upper()
        for entry in self.entries(sector):
            if entry["name"].upper() == wanted:
                return entry
        return None

    def is_directory(self, entry):
        if not directory_ok(self.data, entry["sector"]):
            return False
        if entry["attrs"] & ATTR_D:
            return True
        return entry["length"] == DIR_BYTES

    def put(self, dir_sector, name, attrs, load, exec_addr, length, sector):
        directory = bytearray(self.directory(dir_sector))
        packed = bytearray(ENTRY)
        packed[:10] = encode_name(name, attrs)
        packed[10:14] = (load & 0xFFFFFFFF).to_bytes(4, "little")
        packed[14:18] = (exec_addr & 0xFFFFFFFF).to_bytes(4, "little")
        packed[18:22] = (length & 0xFFFFFFFF).to_bytes(4, "little")
        packed[22:25] = (sector & 0xFFFFFF).to_bytes(3, "little")
        existing = self.find(dir_sector, name)
        if existing is None:
            # ADFS stops at the first entry that sorts after the name it wants,
            # so a directory has to stay in that order.
            key = sort_key(name)
            present = self.entries(dir_sector)
            insert_at = len(present)
            for index, entry in enumerate(present):
                if sort_key(entry["name"]) > key:
                    insert_at = index
                    break
            if len(present) >= DIR_ENTRIES:
                raise DiscError("directory is full (47 objects)")
            for index in range(len(present), insert_at, -1):
                source = 5 + (index - 1) * ENTRY
                dest = 5 + index * ENTRY
                directory[dest:dest + ENTRY] = directory[source:source + ENTRY]
            slot = 5 + insert_at * ENTRY
        else:
            slot = 5 + existing["index"] * ENTRY
        directory[slot:slot + ENTRY] = packed
        self.bump(directory)
        self.store_directory(dir_sector, directory)

    def make_directory(self, parent, name):
        sector = self.allocate(DIR_BYTES // SECTOR)
        directory = bytearray(DIR_BYTES)
        directory[1:5] = b"Hugo"
        directory[0x4FB:0x4FF] = b"Hugo"
        # The two directory titles are the name followed by zeros, as on a
        # disc made by blank_disc.py. Catalogue entries, not these titles,
        # carry the &0D terminator.
        title = name[:10].encode("latin1")
        directory[0x4CC:0x4CC + len(title)] = title
        directory[0x4D9:0x4D9 + len(title)] = title
        directory[0x4D6:0x4D9] = (parent & 0xFFFFFF).to_bytes(3, "little")
        self.store_directory(sector, directory)
        self.put(parent, name, ATTR_R | ATTR_D, 0, 0, DIR_BYTES, sector)
        return sector

    def ensure_directory(self, parent, name):
        name = object_name(name)
        existing = self.find(parent, name)
        if existing is None:
            return self.make_directory(parent, name)
        if not self.is_directory(existing):
            raise DiscError("%s is a file on the hard disc" % name)
        return existing["sector"]

    def write_file(self, dir_sector, name, load, exec_addr, payload, locked):
        name = object_name(name)
        existing = self.find(dir_sector, name)
        if existing is not None and self.is_directory(existing):
            raise DiscError("%s is a directory on the hard disc" % name)
        attrs = ATTR_R | ATTR_W
        if locked:
            attrs |= ATTR_L
        count = (len(payload) + SECTOR - 1) // SECTOR
        old_sector = 0
        old_count = 0
        if existing is not None and existing["length"] > 0:
            old_sector = existing["sector"]
            old_count = (existing["length"] + SECTOR - 1) // SECTOR
        if count == 0:
            sector = 0
        elif old_count >= count and old_sector > 0:
            sector = old_sector
            if old_count > count:
                self.release(old_sector + count, old_count - count)
            old_count = 0
        else:
            try:
                sector = self.allocate(count)
            except DiscError:
                # The old copy is still in the catalogue until it is replaced
                # below. Give its sectors back only when nothing else will hold
                # the new copy, then try once more.
                if old_count <= 0:
                    raise
                self.release(old_sector, old_count)
                old_count = 0
                sector = self.allocate(count)
        self.write_bytes(sector, payload)
        self.put(dir_sector, name, attrs, load, exec_addr, len(payload), sector)
        if old_count > 0:
            self.release(old_sector, old_count)


def dfs_sector_offset(sector, side, tracks, interleaved):
    if not interleaved:
        return (side * tracks * 10 + sector) * SECTOR
    track, within = divmod(sector, 10)
    return ((track * 2 + side) * 10 + within) * SECTOR


def dfs_catalogue_ok(data, offset, sectors_on_side):
    if offset < 0 or offset + 2 * SECTOR > len(data):
        return False
    count_byte = data[offset + SECTOR + 5]
    if (count_byte & 7) != 0:
        return False
    files = count_byte >> 3
    if files > 31:
        return False
    size = data[offset + SECTOR + 7] | ((data[offset + SECTOR + 6] & 3) << 8)
    if size == 0 or size > sectors_on_side:
        return False
    for index in range(files):
        name = data[offset + 8 + index * 8:offset + 16 + index * 8]
        if not all(32 <= (byte & 0x7F) < 127 for byte in name):
            return False
    return True


def read_dfs_side(data, side, tracks, interleaved):
    """One DFS catalogue. Returns a list of directory dicts and loose $ files."""
    objects = []
    for index in range(data[dfs_sector_offset(1, side, tracks, interleaved) + 5] >> 3):
        name_offset = dfs_sector_offset(0, side, tracks, interleaved) + 8 + index * 8
        info_offset = dfs_sector_offset(1, side, tracks, interleaved) + 8 + index * 8
        name = bytes(byte & 0x7F for byte in data[name_offset:name_offset + 7])
        name = name.decode("latin1").rstrip(" ")
        directory = data[name_offset + 7]
        locked = (directory & 0x80) != 0
        directory = chr(directory & 0x7F)
        info = data[info_offset:info_offset + 8]
        load = info[0] | (info[1] << 8) | (((info[6] >> 2) & 3) << 16)
        exec_addr = info[2] | (info[3] << 8) | (((info[6] >> 6) & 3) << 16)
        length = info[4] | (info[5] << 8) | (((info[6] >> 4) & 3) << 16)
        start = info[7] | ((info[6] & 3) << 8)
        payload = bytearray()
        sector = start
        while len(payload) < length:
            offset = dfs_sector_offset(sector, side, tracks, interleaved)
            if offset + SECTOR > len(data):
                raise DiscError("DFS file %s.%s runs off the end of the floppy" %
                                (directory, name))
            payload += data[offset:offset + SECTOR]
            sector += 1
        objects.append({
            "dir": directory,
            "name": name,
            "load": load,
            "exec": exec_addr,
            "data": bytes(payload[:length]),
            "locked": locked,
        })
    return objects


def read_dfs(path, data):
    sectors = len(data) // SECTOR
    if path.lower().endswith(".dsd"):
        if sectors % 20 != 0:
            raise DiscError("%s is not a double-sided DFS image" % path)
        tracks = sectors // 20
        sequential = tracks * 10
        interleaved_ok = dfs_catalogue_ok(data, 10 * SECTOR, tracks * 10)
        sequential_ok = dfs_catalogue_ok(data, sequential * SECTOR, tracks * 10)
        if sequential_ok and not interleaved_ok:
            interleaved = False
        else:
            interleaved = True
            if not dfs_catalogue_ok(data, 0, tracks * 10):
                raise DiscError("%s does not start with a DFS catalogue" % path)
        sides = 2
    else:
        if sectors % 10 != 0:
            raise DiscError("%s is not a DFS image" % path)
        tracks = sectors // 10
        interleaved = False
        sides = 1
        if not dfs_catalogue_ok(data, 0, tracks * 10):
            raise DiscError("%s does not start with a DFS catalogue" % path)
    tree = []
    for side in range(sides):
        files = read_dfs_side(data, side, tracks, interleaved)
        if side == 1 and not files:
            continue
        side_root = tree
        if side == 1:
            side_root = [{"name": "2", "dir": True, "children": []}]
            tree.append(side_root[0])
            side_root = side_root[0]["children"]
        folders = {}
        for item in files:
            leaf = {
                "name": item["name"],
                "dir": False,
                "load": item["load"],
                "exec": item["exec"],
                "data": item["data"],
                "locked": item["locked"],
            }
            if item["dir"] == "$":
                side_root.append(leaf)
                continue
            folder = folders.get(item["dir"])
            if folder is None:
                folder = {"name": item["dir"], "dir": True, "children": []}
                folders[item["dir"]] = folder
                side_root.append(folder)
            folder["children"].append(leaf)
    return tree


def read_adfs_dir(image, sector, seen):
    if sector in seen:
        raise DiscError("the floppy directories loop")
    if not directory_ok(image.data, sector):
        raise DiscError("broken directory at sector %d" % sector)
    seen.add(sector)
    children = []
    for entry in image.entries(sector):
        if entry["length"] == DIR_BYTES or (entry["attrs"] & ATTR_D) != 0:
            if image.is_directory(entry):
                children.append({
                    "name": entry["name"],
                    "dir": True,
                    "children": read_adfs_dir(image, entry["sector"], seen),
                })
            else:
                print("copy_to_hd: skipping broken directory %s" % entry["name"], file=sys.stderr)
            continue
        children.append({
            "name": entry["name"],
            "dir": False,
            "load": entry["load"],
            "exec": entry["exec"],
            "data": image.read_bytes(entry["sector"], entry["length"]),
            "locked": (entry["attrs"] & ATTR_L) != 0,
        })
    return children


def read_floppy(path):
    data = read_image(path)
    name = path.lower()
    if name.endswith(".ssd") or name.endswith(".dsd"):
        return read_dfs(path, data)
    if hugo_at(data, ROOT_SECTOR):
        return read_adfs_dir(AdfsImage(path, data), ROOT_SECTOR, set())
    if dfs_catalogue_ok(data, 0, max(1, len(data) // SECTOR)):
        return read_dfs(path, data)
    raise DiscError("%s is neither a DFS nor an old-map ADFS disc" % path)


def copy_tree(image, dir_sector, children, prefix):
    copied = 0
    for item in children:
        if item["dir"]:
            name = object_name(item["name"])
            if not item["children"]:
                # An empty directory is created once. A later copy leaves the
                # one already on the hard disc, including anything inside it.
                existing = image.find(dir_sector, name)
                if existing is None:
                    image.make_directory(dir_sector, name)
                    print("%s%s." % (prefix, name))
                    copied += 1
                elif not image.is_directory(existing):
                    raise DiscError("%s is a file on the hard disc" % name)
                continue
            child = image.ensure_directory(dir_sector, name)
            copied += copy_tree(image, child, item["children"], prefix + name + ".")
            continue
        image.write_file(dir_sector, item["name"], item["load"], item["exec"], item["data"],
                         item["locked"])
        copied += 1
        print("%s%s" % (prefix, object_name(item["name"])))
    return copied


def list_tree(children, prefix):
    lines = []
    for item in children:
        if item["dir"]:
            lines.append(prefix + item["name"] + ".")
            lines.extend(list_tree(item["children"], prefix + item["name"] + "."))
        else:
            lines.append("%s%s  %d bytes" % (prefix, item["name"], len(item["data"])))
    return lines


def main(argv):
    parser = argparse.ArgumentParser(
        description="Copy a DFS or ADFS floppy image onto an ADFS hard disc image.")
    parser.add_argument("floppy", help="source .ssd, .dsd, .adf, .adm, or .adl")
    parser.add_argument("harddisc", nargs="?", help="destination .hd, old-map ADFS")
    parser.add_argument("--into", help="ADFS directory to receive the copy (default is $)")
    parser.add_argument("--list", action="store_true", help="show the floppy, and do not write")
    args = parser.parse_args(argv)
    try:
        tree = read_floppy(args.floppy)
        if args.list:
            for line in list_tree(tree, "$."):
                print(line)
            return 0
        if not args.harddisc:
            parser.error("the hard disc image is required")
        if os.path.abspath(args.floppy) == os.path.abspath(args.harddisc):
            raise DiscError("the floppy and the hard disc are the same file")
        image = AdfsImage(args.harddisc, read_image(args.harddisc))
        directory = ROOT_SECTOR
        prefix = "$."
        if args.into:
            directory = image.ensure_directory(ROOT_SECTOR, args.into)
            prefix = "$." + object_name(args.into) + "."
        copied = copy_tree(image, directory, tree, prefix)
        image.save()
    except DiscError as error:
        print("copy_to_hd: %s" % error, file=sys.stderr)
        return 1
    print("%d copied onto %s" % (copied, args.harddisc))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
