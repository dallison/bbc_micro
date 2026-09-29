#!/usr/bin/env python3
"""Create a BBC Micro cassette image.

With only a filename, the image is a blank UEF. The emulator's cassette
player can record onto it. With --data, the file is stored as one or more
Acorn cassette blocks that *LOAD and *RUN can read.

Examples:

  python3 blank_tape.py blank.uef
  python3 blank_tape.py --name HELLO --load 0xE00 --exec 0xE00 --data prog.bin hello.uef
"""

import argparse
import sys


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def chunk(kind, payload):
    return kind.to_bytes(2, "little") + len(payload).to_bytes(4, "little") + payload


def block(name, load, exec_addr, number, data, last):
    head = name.encode("ascii", "replace")[:10] + b"\x00"
    head += load.to_bytes(4, "little")
    head += exec_addr.to_bytes(4, "little")
    head += number.to_bytes(2, "little")
    head += len(data).to_bytes(2, "little")
    head += bytes([0x80 if last else 0x00, 0, 0, 0, 0])
    header_crc = crc16(head)
    data_crc = crc16(data)
    return (
        bytes([0x2A])
        + head
        + bytes([header_crc >> 8, header_crc & 0xFF])
        + data
        + bytes([data_crc >> 8, data_crc & 0xFF])
    )


def program_bytes(name, load, exec_addr, data):
    parts = []
    offset = 0
    number = 0
    if not data:
        data = b""
    while True:
        piece = data[offset:offset + 256]
        offset += len(piece)
        last = offset >= len(data)
        if parts:
            parts.append(b"\xAA" * 32)
        parts.append(block(name, load, exec_addr, number, piece, last))
        number += 1
        if last:
            break
    return b"".join(parts)


def uef(data_bytes):
    # Version 0.10. A short carrier, then the cassette bytes.
    out = bytearray(b"UEF File!\x00\x0A\x00")
    if data_bytes:
        out += chunk(0x0110, (32 * 20).to_bytes(2, "little"))
        out += chunk(0x0100, data_bytes)
    return bytes(out)


def address(text):
    try:
        value = int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError("address must be a number")
    if value < 0 or value > 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("address is out of range")
    return value


def main():
    parser = argparse.ArgumentParser(description="Create a BBC Micro cassette image.")
    parser.add_argument("tape", help="output .uef file")
    parser.add_argument("--name", default="FILE", help="cassette file name")
    parser.add_argument("--load", type=address, default=0x0E00, help="load address")
    parser.add_argument("--exec", dest="exec_addr", type=address, default=None,
                        help="execution address (defaults to the load address)")
    parser.add_argument("--data", help="bytes to store on the tape")
    args = parser.parse_args()
    if args.exec_addr is None:
        args.exec_addr = args.load
    payload = b""
    if args.data is not None:
        try:
            with open(args.data, "rb") as handle:
                payload = handle.read()
        except OSError as error:
            print("Unable to read %s: %s" % (args.data, error), file=sys.stderr)
            return 1
        payload = program_bytes(args.name, args.load, args.exec_addr, payload)
    try:
        with open(args.tape, "wb") as handle:
            handle.write(uef(payload))
    except OSError as error:
        print("Unable to write %s: %s" % (args.tape, error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
