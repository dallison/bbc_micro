#!/usr/bin/env python3
# Clean-room client for the ARM Tube second processor.
#
# The host still speaks the 6502 Tube protocol: a short banner on register
# 1, then 256-byte copies of the language, then register 2 value 0x80.
# This client drains those copies and then offers a small monitor. It does
# not enter the language image, which is 6502 code.

import struct
import os

TUBE = 0x03000000
WORK = 0x10000
STACK = 0x20000
BUF = 0x100  # WORK + BUF is the line buffer


class Asm:
    def __init__(self):
        self.words = []
        self.labels = {}
        self.branches = []
        self.pool = []
        self.pool_uses = []

    def label(self, name):
        if name in self.labels:
            raise SystemExit("duplicate label " + name)
        self.labels[name] = len(self.words) * 4

    def emit(self, word):
        self.words.append(word & 0xFFFFFFFF)

    def b(self, name, cond=0xE, link=False):
        self.branches.append((len(self.words), name, cond, link))
        self.emit(0)

    def bl(self, name):
        self.b(name, link=True)

    def pool_load(self, rd, name):
        self.pool_uses.append((len(self.words), name, rd))
        if name not in self.pool:
            self.pool.append(name)
        self.emit(0)

    def finish(self):
        for index, name, cond, link in self.branches:
            if name not in self.labels:
                raise SystemExit("missing label " + name)
            pc = index * 4
            offset = self.labels[name] - pc - 8
            if offset & 3:
                raise SystemExit("unaligned branch " + name)
            if offset < -(1 << 25) or offset >= (1 << 25):
                raise SystemExit("branch out of range " + name)
            bit = 1 if link else 0
            self.words[index] = (cond << 28) | (0x5 << 25) | (bit << 24) | ((offset >> 2) & 0xFFFFFF)
        base = len(self.words)
        for index, name, rd in self.pool_uses:
            if name not in self.labels:
                raise SystemExit("missing pool " + name)
            pc = index * 4
            slot = base + self.pool.index(name)
            offset = slot * 4 - pc - 8
            if offset < 0 or offset > 0xFFF:
                raise SystemExit("pool offset for " + name)
            self.words[index] = 0xE59F0000 | (rd << 12) | offset
        for name in self.pool:
            self.words.append(self.labels[name])
        return self.words


def imm(value):
    value &= 0xFFFFFFFF
    for rot in range(16):
        amount = rot * 2
        if amount == 0:
            rolled = value
        else:
            rolled = ((value << amount) | (value >> (32 - amount))) & 0xFFFFFFFF
        if rolled <= 0xFF:
            return rot, rolled
    raise SystemExit("not an ARM immediate: %#x" % value)


def dp_imm(op, s, rd, rn, value, cond=0xE):
    rot, byte = imm(value)
    return (cond << 28) | (1 << 25) | (op << 21) | ((1 if s else 0) << 20) | (rn << 16) | (rd << 12) | (rot << 8) | byte


def dp_reg(op, s, rd, rn, rm, cond=0xE):
    return (cond << 28) | (op << 21) | ((1 if s else 0) << 20) | (rn << 16) | (rd << 12) | rm


def dp_lsl(op, s, rd, rn, rm, amount, cond=0xE):
    return (cond << 28) | (op << 21) | ((1 if s else 0) << 20) | (rn << 16) | (rd << 12) | ((amount & 31) << 7) | rm


def mov_i(rd, value, cond=0xE):
    return dp_imm(0xD, False, rd, 0, value, cond)


def mov(rd, rm, cond=0xE):
    return dp_reg(0xD, False, rd, 0, rm, cond)


def movs_pc_lr():
    return dp_reg(0xD, True, 15, 0, 14)


def cmp_i(rn, value, cond=0xE):
    return dp_imm(0xA, True, 0, rn, value, cond)


def cmp_r(rn, rm):
    return dp_reg(0xA, True, 0, rn, rm)


def tst_i(rn, value):
    return dp_imm(0x8, True, 0, rn, value)


def add_i(rd, rn, value, cond=0xE):
    return dp_imm(0x4, False, rd, rn, value, cond)


def sub_i(rd, rn, value):
    return dp_imm(0x2, False, rd, rn, value)


def subs_i(rd, rn, value):
    return dp_imm(0x2, True, rd, rn, value)


def ldrb(rd, rn, offset):
    return 0xE5D00000 | (rn << 16) | (rd << 12) | (offset & 0xFFF)


def ldrb_post(rd, rn, offset):
    return 0xE4D00000 | (rn << 16) | (rd << 12) | (offset & 0xFFF)


def strb(rd, rn, offset):
    return 0xE5C00000 | (rn << 16) | (rd << 12) | (offset & 0xFFF)


def strb_post(rd, rn, offset):
    return 0xE4C00000 | (rn << 16) | (rd << 12) | (offset & 0xFFF)


def str_w(rd, rn, offset):
    return 0xE5800000 | (rn << 16) | (rd << 12) | (offset & 0xFFF)


def str_pre_down(rd, rn, offset):
    # STR rd, [rn, #-offset]!
    return 0xE5200000 | (rn << 16) | (rd << 12) | (offset & 0xFFF)


def ldr_post(rd, rn, offset):
    # LDR rd, [rn], #offset
    return 0xE4900000 | (rn << 16) | (rd << 12) | (offset & 0xFFF)


def ldr_w(rd, rn, offset):
    return 0xE5900000 | (rn << 16) | (rd << 12) | (offset & 0xFFF)


def build():
    a = Asm()
    # Vectors are instructions. Reset starts in supervisor mode at address 0.
    a.b("reset")
    a.b("hang")
    a.b("swi")
    a.b("hang")
    a.b("hang")
    a.b("hang")
    a.b("irq")
    a.b("fiq")

    a.label("reset")
    a.emit(mov_i(13, STACK))
    a.emit(mov_i(8, WORK))
    a.emit(mov_i(0, 0))
    a.emit(str_w(0, 8, 0))
    a.bl("banner")
    a.b("service")

    a.label("banner")
    a.emit(str_pre_down(14, 13, 4))
    a.pool_load(5, "banner_text")
    a.label("banner_loop")
    a.emit(ldrb_post(0, 5, 1))
    a.bl("r1put")
    a.emit(cmp_i(0, 0))
    a.b("banner_loop", cond=1)
    a.emit(ldr_post(14, 13, 4))
    a.emit(mov(15, 14))

    def wait_put(name, status, data):
        a.label(name)
        a.emit(mov_i(4, TUBE))
        a.label(name + "_w")
        a.emit(ldrb(1, 4, status))
        a.emit(tst_i(1, 0x40))
        a.b(name + "_w", cond=0)
        a.emit(strb(0, 4, data))
        a.emit(mov(15, 14))

    def wait_get(name, status, data):
        a.label(name)
        a.emit(mov_i(4, TUBE))
        a.label(name + "_w")
        a.emit(ldrb(0, 4, status))
        a.emit(tst_i(0, 0x80))
        a.b(name + "_w", cond=0)
        a.emit(ldrb(0, 4, data))
        a.emit(mov(15, 14))

    wait_put("r1put", 0, 1)
    wait_put("r2put", 2, 3)
    wait_get("r2get", 2, 3)
    wait_get("r4get", 6, 7)
    wait_put("r4put", 6, 7)

    a.label("read_addr")
    a.emit(str_pre_down(14, 13, 4))
    a.emit(mov_i(5, 0))
    a.emit(mov_i(6, 4))
    a.label("addr_loop")
    a.bl("r4get")
    a.emit(dp_lsl(0xC, False, 5, 0, 5, 8))
    a.emit(subs_i(6, 6, 1))
    a.b("addr_loop", cond=1)
    a.emit(ldr_post(14, 13, 4))
    a.emit(mov(15, 14))

    a.label("service")
    a.emit(mov_i(4, TUBE))
    a.emit(ldrb(0, 4, 6))
    a.emit(tst_i(0, 0x80))
    a.b("got_r4", cond=1)
    a.emit(ldrb(0, 4, 2))
    a.emit(tst_i(0, 0x80))
    a.b("service", cond=0)
    a.emit(ldrb(0, 4, 3))
    a.emit(cmp_i(0, 0x80))
    a.b("service", cond=1)
    a.b("monitor")

    a.label("got_r4")
    a.bl("r4get")
    a.emit(mov(7, 0))
    a.emit(cmp_i(7, 0x80))
    a.b("r4_error", cond=2)
    a.emit(cmp_i(7, 8))
    a.b("service", cond=2)
    a.bl("r4get")
    a.emit(cmp_i(7, 0))
    a.b("type0", cond=0)
    a.emit(cmp_i(7, 1))
    a.b("type1", cond=0)
    a.emit(cmp_i(7, 2))
    a.b("type2", cond=0)
    a.emit(cmp_i(7, 3))
    a.b("type3", cond=0)
    a.emit(cmp_i(7, 4))
    a.b("type4", cond=0)
    a.emit(cmp_i(7, 5))
    a.b("type5", cond=0)
    a.emit(cmp_i(7, 6))
    a.b("type6", cond=0)
    a.emit(cmp_i(7, 7))
    a.b("type7", cond=0)
    a.b("service")

    # A type of 0x80 or more is a host error. Two bytes, then text up to a 0.
    a.label("r4_error")
    a.bl("r2get")
    a.bl("r2get")
    a.label("err_loop")
    a.bl("r2get")
    a.emit(cmp_i(0, 0))
    a.b("err_loop", cond=1)
    a.b("service")

    # Types 0 to 3 carry an address and a sync byte. The boot copy uses 7.
    for name in ("type0", "type1", "type2", "type3"):
        a.label(name)
        a.bl("read_addr")
        a.bl("r4get")
        a.b("service")

    a.label("type4")
    a.bl("read_addr")
    a.bl("r4get")
    a.emit(mov_i(8, WORK))
    a.emit(str_w(5, 8, 4))
    a.emit(mov_i(0, 1))
    a.emit(str_w(0, 8, 0))
    a.b("service")

    a.label("type5")
    a.b("service")

    def copy_page(name, to_host):
        a.label(name)
        a.bl("read_addr")
        a.bl("r4get")
        a.emit(mov_i(4, TUBE))
        a.emit(mov_i(6, 256))
        a.label(name + "_loop")
        a.emit(ldrb(0, 4, 4))
        if to_host:
            a.emit(tst_i(0, 0x40))
            a.b(name + "_loop", cond=0)
            a.emit(ldrb(0, 5, 0))
            a.emit(strb(0, 4, 5))
            a.emit(add_i(5, 5, 1))
        else:
            a.emit(tst_i(0, 0x80))
            a.b(name + "_loop", cond=0)
            a.emit(ldrb(0, 4, 5))
            a.emit(strb_post(0, 5, 1))
        a.emit(subs_i(6, 6, 1))
        a.b(name + "_loop", cond=1)

    copy_page("type6", True)
    a.emit(mov_i(0, 0))
    a.bl("r4put")
    a.b("service")
    copy_page("type7", False)
    a.b("service")

    a.label("readline")
    a.emit(str_pre_down(14, 13, 4))
    a.emit(mov_i(8, WORK))
    a.emit(mov_i(7, 0))
    a.emit(mov_i(0, 0x0A))
    a.bl("r2put")
    a.emit(mov_i(0, 0x7F))
    a.bl("r2put")
    a.emit(mov_i(0, 0x20))
    a.bl("r2put")
    a.emit(mov_i(0, 0x7E))
    a.bl("r2put")
    a.emit(mov_i(0, 7))
    a.bl("r2put")
    a.emit(mov_i(0, 0))
    a.bl("r2put")
    a.bl("r2get")
    a.emit(cmp_i(0, 0x80))
    a.b("line_esc", cond=2)
    a.emit(add_i(5, 8, BUF))
    a.label("line_loop")
    a.bl("r2get")
    a.emit(strb_post(0, 5, 1))
    a.emit(cmp_i(0, 0x0D))
    a.b("line_loop", cond=1)
    a.b("line_done")
    a.label("line_esc")
    a.emit(mov_i(7, 1))
    a.emit(add_i(5, 8, BUF))
    a.emit(mov_i(0, 0x0D))
    a.emit(strb(0, 5, 0))
    a.label("line_done")
    a.emit(ldr_post(14, 13, 4))
    a.emit(mov(15, 14))

    a.label("oscli")
    a.emit(str_pre_down(14, 13, 4))
    a.emit(mov_i(8, WORK))
    a.emit(mov_i(0, 2))
    a.bl("r2put")
    a.emit(add_i(5, 8, BUF))
    a.label("cli_loop")
    a.emit(ldrb_post(0, 5, 1))
    a.bl("r2put")
    a.emit(cmp_i(0, 0x0D))
    a.b("cli_loop", cond=1)
    a.bl("r2get")
    a.emit(ldr_post(14, 13, 4))
    a.emit(mov(15, 14))

    a.label("is_go")
    a.emit(mov_i(8, WORK))
    a.emit(add_i(5, 8, BUF))
    a.emit(ldrb(0, 5, 0))
    a.emit(cmp_i(0, ord("G")))
    a.emit(mov(15, 14, cond=1))
    a.emit(ldrb(0, 5, 1))
    a.emit(cmp_i(0, ord("O")))
    a.emit(mov(15, 14, cond=1))
    a.emit(ldrb(0, 5, 2))
    a.emit(cmp_i(0, 0x20))
    a.emit(cmp_i(0, 0x0D, cond=1))
    a.emit(mov(15, 14, cond=1))
    a.emit(mov(15, 14))

    a.label("monitor")
    a.emit(mov_i(8, WORK))
    a.emit(mov_i(0, ord("*")))
    a.bl("r1put")
    a.bl("readline")
    a.emit(cmp_i(7, 0))
    a.b("monitor", cond=1)
    a.bl("is_go")
    a.b("do_go", cond=0)
    a.bl("oscli")
    a.b("monitor")

    a.label("do_go")
    a.emit(mov_i(8, WORK))
    a.emit(add_i(5, 8, BUF))
    a.emit(add_i(5, 5, 2))
    a.emit(mov_i(6, 0))
    a.emit(mov_i(7, 0))
    a.label("go_skip")
    a.emit(ldrb(0, 5, 0))
    a.emit(cmp_i(0, 0x20))
    a.emit(add_i(5, 5, 1, cond=0))
    a.b("go_skip", cond=0)
    a.label("go_hex")
    a.emit(ldrb_post(0, 5, 1))
    a.emit(cmp_i(0, ord("0")))
    a.b("go_jump", cond=11)
    a.emit(cmp_i(0, ord("9")))
    a.b("go_digit", cond=13)
    a.emit(cmp_i(0, ord("A")))
    a.b("go_lower", cond=11)
    a.emit(cmp_i(0, ord("F")))
    a.b("go_lower", cond=12)
    a.emit(sub_i(0, 0, ord("A") - 10))
    a.b("go_shift")
    a.label("go_lower")
    a.emit(cmp_i(0, ord("a")))
    a.b("go_jump", cond=11)
    a.emit(cmp_i(0, ord("f")))
    a.b("go_jump", cond=12)
    a.emit(sub_i(0, 0, ord("a") - 10))
    a.b("go_shift")
    a.label("go_digit")
    a.emit(sub_i(0, 0, ord("0")))
    a.label("go_shift")
    a.emit(dp_lsl(0xD, False, 6, 0, 6, 4))
    a.emit(dp_reg(0xC, False, 6, 6, 0))
    a.emit(add_i(7, 7, 1))
    a.b("go_hex")
    a.label("go_jump")
    a.emit(cmp_i(7, 0))
    a.b("monitor", cond=0)
    a.emit(mov(15, 6))

    # SWI 0 writes the character in R0. Other numbers return.
    a.label("swi")
    a.emit(str_pre_down(14, 13, 4))
    a.emit(str_pre_down(0, 13, 4))
    a.emit(dp_imm(0xE, False, 1, 14, 0xFC000000))
    a.emit(dp_imm(0xE, False, 1, 1, 3))
    a.emit(sub_i(1, 1, 4))
    a.emit(ldr_w(1, 1, 0))
    a.emit(dp_imm(0xE, False, 1, 1, 0xFF000000))
    a.emit(cmp_i(1, 0))
    a.b("swi_out", cond=1)
    a.emit(ldr_w(0, 13, 0))
    a.bl("r1put")
    a.label("swi_out")
    a.emit(ldr_post(0, 13, 4))
    a.emit(ldr_post(14, 13, 4))
    a.emit(movs_pc_lr())

    a.label("irq")
    a.emit(subs_i(15, 14, 4))
    a.label("fiq")
    a.emit(subs_i(15, 14, 4))
    a.label("hang")
    a.b("hang")

    text = bytes([0x0A]) + b"Acorn ARM3 4MB" + bytes([0x0A, 0x0A, 0x0D, 0])
    while len(text) % 4:
        text += b"\x00"
    a.label("banner_text")
    for i in range(0, len(text), 4):
        word = text[i] | (text[i + 1] << 8) | (text[i + 2] << 16) | (text[i + 3] << 24)
        a.emit(word)
    return a.finish(), text


def main():
    words, text = build()
    data = b"".join(struct.pack("<I", word) for word in words)
    # The terminating 0 is inside the text. Padding after it is not sent.
    if b"Acorn ARM3 4MB" not in data:
        raise SystemExit("banner missing")
    if len(text) > 24:
        raise SystemExit("banner longer than the register 1 queue")
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "arm_client.c")
    lines = [
        "// Generated by gen_arm_client.py. Do not edit by hand.",
        "",
        "#include <stdint.h>",
        "",
        "const uint8_t kArmClient[] = {",
    ]
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        lines.append("  " + ", ".join("%d" % b for b in chunk) + ",")
    lines.append("};")
    lines.append("const int kArmClientLength = %d;" % len(data))
    lines.append("")
    with open(path, "w", encoding="utf-8") as out:
        out.write("\n".join(lines))
    print("wrote %s (%d bytes)" % (path, len(data)))


if __name__ == "__main__":
    main()
