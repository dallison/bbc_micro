#!/usr/bin/env python3
"""Look through a DFS or ADFS disc image from the host.

  python3 examine_disc.py disc.adl
  python3 examine_disc.py disc.ssd type $.!BOOT
  python3 examine_disc.py disc.adl dump c.dasm

With only the image name, it prompts for commands:

  cat [path]     list a directory
  dir path       move to a directory ($ is the root)
  info path      load address, execution address, and length
  type path      show the file as text
  dump path      show the file in hex
  help
  quit
"""

import sys

from copy_to_hd import DiscError, read_floppy

HELP = """\
cat [path]   list a directory
dir path     move to a directory. $ is the root
info path    load address, execution address, and length
type path    show the file as text. BBC returns become new lines.
             Zero bytes are left out, as they are on the BBC.
dump path    show the file in hex
help
quit
"""


class DiscBrowser(object):
    def __init__(self, tree):
        self.root = {"name": "$", "dir": True, "children": tree}
        self.stack = [self.root]

    def cwd(self):
        return self.stack[-1]

    def where(self):
        return ".".join(node["name"] for node in self.stack)

    def resolve(self, text, want_dir=False):
        parts = [part for part in text.strip().split(".") if part != ""]
        if not parts:
            node = self.cwd()
            stack = list(self.stack)
        elif parts[0] == "$":
            node = self.root
            stack = [self.root]
            parts = parts[1:]
        else:
            node = self.cwd()
            stack = list(self.stack)
        for part in parts:
            if not node.get("dir"):
                raise DiscError("%s is a file" % self._join(stack))
            found = None
            for child in node["children"]:
                if child["name"].upper() == part.upper():
                    found = child
                    break
            if found is None:
                raise DiscError("%s not found" % text.strip())
            node = found
            stack.append(node)
        if want_dir and not node.get("dir"):
            raise DiscError("%s is a file" % self._join(stack))
        return node, stack

    def _join(self, stack):
        return ".".join(node["name"] for node in stack)

    def cat(self, text):
        node, stack = self.resolve(text, want_dir=True)
        print(self._join(stack))
        children = sorted(node["children"], key=lambda item: item["name"].upper())
        if not children:
            print("(empty)")
            return
        for child in children:
            if child["dir"]:
                print("  %s." % child["name"])
            else:
                mark = " L" if child["locked"] else ""
                print("  %-12s %8d  %08X %08X%s" % (
                    child["name"], len(child["data"]), child["load"] & 0xFFFFFFFF,
                    child["exec"] & 0xFFFFFFFF, mark))

    def chdir(self, text):
        if text.strip() == "":
            raise DiscError("dir needs a directory")
        node, stack = self.resolve(text, want_dir=True)
        self.stack = stack
        print(self.where())

    def info(self, text):
        node, stack = self.resolve(text)
        path = self._join(stack)
        if node.get("dir"):
            print("%s  directory, %d entries" % (path, len(node["children"])))
            return
        locked = " locked" if node["locked"] else ""
        print("%s  %d bytes  load %08X  exec %08X%s" % (
            path, len(node["data"]), node["load"] & 0xFFFFFFFF,
            node["exec"] & 0xFFFFFFFF, locked))

    def type_file(self, text):
        node, stack = self.resolve(text)
        if node.get("dir"):
            raise DiscError("%s is a directory" % self._join(stack))
        sys.stdout.write(as_text(node["data"]))
        if node["data"] and node["data"][-1] not in (10, 13):
            sys.stdout.write("\n")

    def dump(self, text):
        node, stack = self.resolve(text)
        if node.get("dir"):
            raise DiscError("%s is a directory" % self._join(stack))
        data = node["data"]
        print("%s  %d bytes" % (self._join(stack), len(data)))
        if not data:
            return
        for offset in range(0, len(data), 16):
            chunk = data[offset:offset + 16]
            words = " ".join("%02X" % byte for byte in chunk)
            letters = "".join(chr(byte) if 32 <= byte < 127 else "." for byte in chunk)
            print("%06X  %-47s  %s" % (offset, words, letters))


def as_text(data):
    out = []
    index = 0
    while index < len(data):
        byte = data[index]
        if byte == 13:
            out.append("\n")
            if index + 1 < len(data) and data[index + 1] == 10:
                index += 1
        elif byte == 10:
            out.append("\n")
        elif byte == 0:
            # A BBC *TYPE prints a zero as nothing.
            pass
        elif byte == 9 or 32 <= byte < 127:
            out.append(chr(byte))
        else:
            out.append(".")
        index += 1
    return "".join(out)


def run_command(browser, line):
    parts = line.strip().split(None, 1)
    if not parts:
        return True
    command = parts[0].lower()
    arg = parts[1] if len(parts) > 1 else ""
    if command in ("quit", "q", "exit"):
        return False
    if command in ("help", "?"):
        sys.stdout.write(HELP)
    elif command == "cat":
        browser.cat(arg)
    elif command == "dir":
        browser.chdir(arg)
    elif command == "info":
        browser.info(arg)
    elif command in ("type", "ascii"):
        browser.type_file(arg)
    elif command in ("dump", "hex"):
        browser.dump(arg)
    else:
        raise DiscError("unknown command %s" % command)
    return True


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print("usage: examine_disc.py image [cat|dir|info|type|dump [path]]")
        print("       examine_disc.py image")
        return 0 if len(argv) > 1 else 1
    try:
        browser = DiscBrowser(read_floppy(argv[1]))
    except DiscError as error:
        print("examine_disc: %s" % error, file=sys.stderr)
        return 1
    if len(argv) > 2:
        try:
            run_command(browser, " ".join(argv[2:]))
        except DiscError as error:
            print("examine_disc: %s" % error, file=sys.stderr)
            return 1
        return 0
    print(argv[1])
    print("type help for commands")
    while True:
        try:
            line = input("%s> " % browser.where())
        except EOFError:
            print()
            return 0
        try:
            if not run_command(browser, line):
                return 0
        except DiscError as error:
            print("examine_disc: %s" % error, file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
