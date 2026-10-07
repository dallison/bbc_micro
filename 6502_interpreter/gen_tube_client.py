#!/usr/bin/env python3
# Assemble the parasite ROM. Register 1 carries characters. Register 2 carries
# ordinary OS calls. Registers 3 and 4 carry the block transfers the host uses
# to copy the current language into this processor.

import pathlib

BASE = 0xF800
SIZE = 0x800

R1S, R1D = 0xFEF8, 0xFEF9
R2S, R2D = 0xFEFA, 0xFEFB
R3S, R3D = 0xFEFC, 0xFEFD
R4S, R4D = 0xFEFE, 0xFEFF
NMILO, NMIHI = 0xF000, 0xF001
ADDR = 0xF002
EXECF, EXEC = 0xF007, 0xF008
BLOCK = 0xF010
NAME = 0xF012
SCRATCH = 0xF020
COUNT, LAST = 0xF030, 0xF031
# Zero page is saved here, not on the stack: an RTS would pop the saved
# bytes and return to address 0. The NMI uses the second pair so it can
# interrupt a transfer that already saved the foreground pair.
SAVED, SAVED_NMI, IN_NMI = 0xF00A, 0xF00C, 0xF00E

# Bytes the host expects for OSWORD 1 to 20: how many to send, then how many
# come back. OSWORD 0 and calls numbered &80 and above are handled apart.
OSWORD_IO = [
    (0, 5), (5, 0), (0, 5), (5, 0), (2, 5),
    (5, 0), (8, 0), (14, 0), (4, 5), (1, 9),
    (1, 5), (5, 0), (0, 8), (8, 25), (25, 1),
    (16, 13), (13, 13), (0, 128), (8, 8), (128, 128),
]


class Asm:
    def __init__(self):
        self.pc = BASE
        self.mem = bytearray([0xFF] * SIZE)
        self.used = set()
        self.labels = {}
        self.fixups = []

    def _off(self, addr):
        if not BASE <= addr < BASE + SIZE:
            raise SystemExit(f"${addr:04X} is outside the client ROM")
        return addr - BASE

    def org(self, addr):
        self.pc = addr

    def label(self, name):
        if name in self.labels:
            raise SystemExit(f"duplicate label {name}")
        self.labels[name] = self.pc

    def emit(self, *raw):
        for byte in raw:
            off = self._off(self.pc)
            if off in self.used:
                raise SystemExit(f"overlap at ${self.pc:04X}")
            self.mem[off] = byte & 0xFF
            self.used.add(off)
            self.pc += 1

    def abs_addr(self, opcode, addr):
        self.emit(opcode, addr & 0xFF, (addr >> 8) & 0xFF)

    def fix(self, at, name, kind):
        self.fixups.append((at, name, kind))

    def branch(self, opcode, name):
        at = self.pc + 1
        self.emit(opcode, 0)
        self.fix(at, name, "rel")

    def jsr(self, name):
        at = self.pc + 1
        self.emit(0x20, 0, 0)
        self.fix(at, name, "word")

    def jmp(self, name):
        at = self.pc + 1
        self.emit(0x4C, 0, 0)
        self.fix(at, name, "word")

    def resolve(self):
        for at, name, kind in self.fixups:
            if name not in self.labels:
                raise SystemExit(f"undefined label {name}")
            target = self.labels[name]
            off = self._off(at)
            if kind == "word":
                self.mem[off] = target & 0xFF
                self.mem[off + 1] = (target >> 8) & 0xFF
            elif kind == "lo":
                self.mem[off] = target & 0xFF
            elif kind == "hi":
                self.mem[off] = (target >> 8) & 0xFF
            else:
                rel = target - (at + 1)
                if not -128 <= rel <= 127:
                    raise SystemExit(f"branch {name} is {rel} bytes away")
                self.mem[off] = rel & 0xFF


def build():
    a = Asm()

    def lda_i(v):
        a.emit(0xA9, v)

    def ldx_i(v):
        a.emit(0xA2, v)

    def ldy_i(v):
        a.emit(0xA0, v)

    def lda_abs(addr):
        a.abs_addr(0xAD, addr)

    def sta_abs(addr):
        a.abs_addr(0x8D, addr)

    def stx_abs(addr):
        a.abs_addr(0x8E, addr)

    def sty_abs(addr):
        a.abs_addr(0x8C, addr)

    def bit_abs(addr):
        a.abs_addr(0x2C, addr)

    def inc_abs(addr):
        a.abs_addr(0xEE, addr)

    def cmp_i(v):
        a.emit(0xC9, v)

    def cpy_i(v):
        a.emit(0xC0, v)

    def and_i(v):
        a.emit(0x29, v)

    def bne(name):
        a.branch(0xD0, name)

    def beq(name):
        a.branch(0xF0, name)

    def bpl(name):
        a.branch(0x10, name)

    def bmi(name):
        a.branch(0x30, name)

    def bcc(name):
        a.branch(0x90, name)

    def bcs(name):
        a.branch(0xB0, name)

    def bvc(name):
        a.branch(0x50, name)

    def imm_lo(name):
        a.emit(0xA9, 0)
        a.fix(a.pc - 1, name, "lo")

    def imm_hi(name):
        a.emit(0xA2, 0)
        a.fix(a.pc - 1, name, "hi")

    def arm(handler):
        a.jsr("read_addr")
        imm_lo(handler)
        imm_hi(handler)
        a.jsr("set_nmi")
        a.jsr("r4get")

    a.label("reset")
    a.emit(0x78, 0xD8)  # SEI, CLD
    ldx_i(0xFF)
    a.emit(0x9A)  # TXS
    imm_lo("nmi_rti")
    sta_abs(NMILO)
    imm_hi("nmi_rti")
    stx_abs(NMIHI)
    lda_i(0)
    sta_abs(EXECF)
    sta_abs(0x00FF)
    a.emit(0x58)  # CLI
    a.jsr("plant_vectors")
    ldx_i(0)
    a.label("banner_loop")
    a.abs_addr(0xBD, 0)
    a.fix(a.pc - 2, "banner", "word")
    beq("banner_done")
    a.jsr("r1put")
    a.emit(0xE8)
    bne("banner_loop")
    a.label("banner_done")
    lda_i(0)
    a.jsr("r1put")
    a.label("wait80")
    bit_abs(R2S)
    bpl("wait80")
    lda_abs(R2D)
    cmp_i(0x80)
    bne("wait80")
    lda_abs(EXECF)
    beq("wait80")
    a.abs_addr(0x6C, EXEC)

    # The MOS enters OSWRCH, OSBYTE and the filing calls through the
    # vectors in page 2. BASIC also jumps through WRCHV directly.
    vectors = (
        (0x0208, "oscli"),
        (0x020A, "osbyte"),
        (0x020C, "osword"),
        (0x020E, "oswrch"),
        (0x0210, "osrdch"),
        (0x0212, "osfile"),
        (0x0214, "osargs"),
        (0x0216, "osbget"),
        (0x0218, "osbput"),
        (0x021A, "osgbpb"),
        (0x021C, "osfind"),
    )

    a.label("plant_vectors")
    for addr, name in vectors:
        imm_lo(name)
        sta_abs(addr)
        imm_hi(name)
        stx_abs(addr + 1)
    a.emit(0x60)

    # The external 6502 client prints this before the host reads it, so the
    # bytes have to fit in the 24-deep register 1 queue, including the final
    # 0. A line feed, the title, two line feeds and a carriage return leave
    # the cursor under a blank line, where the host prints the filing system.
    a.label("banner")
    a.emit(0x0A, *b"Acorn TUBE 6502 64K", 0x0A, 0x0A, 0x0D, 0)

    def poll(name, stat, data, writing):
        a.label(name)
        bit_abs(stat)
        (bvc if writing else bpl)(name)
        (sta_abs if writing else lda_abs)(data)
        a.emit(0x60)

    poll("r1put", R1S, R1D, True)
    poll("r1get", R1S, R1D, False)
    poll("r2put", R2S, R2D, True)
    poll("r2get", R2S, R2D, False)
    poll("r3put", R3S, R3D, True)
    poll("r3get", R3S, R3D, False)
    poll("r4put", R4S, R4D, True)
    poll("r4get", R4S, R4D, False)

    # A is the low byte and X is the high byte.
    a.label("set_nmi")
    sta_abs(NMILO)
    stx_abs(NMIHI)
    a.emit(0x60)

    # Four address bytes arrive most significant first. They land
    # little-endian at ADDR.
    a.label("read_addr")
    ldx_i(3)
    a.label("addr_loop")
    a.jsr("r4get")
    a.abs_addr(0x9D, ADDR)
    a.emit(0xCA)
    bpl("addr_loop")
    a.emit(0x60)

    a.label("borrow00")
    lda_abs(IN_NMI)
    bne("borrow_nmi")
    lda_abs(0x0000)
    sta_abs(SAVED)
    lda_abs(0x0001)
    sta_abs(SAVED + 1)
    a.emit(0x60)
    a.label("borrow_nmi")
    lda_abs(0x0000)
    sta_abs(SAVED_NMI)
    lda_abs(0x0001)
    sta_abs(SAVED_NMI + 1)
    a.emit(0x60)

    a.label("point_addr")
    a.jsr("borrow00")
    lda_abs(ADDR)
    sta_abs(0x0000)
    lda_abs(ADDR + 1)
    sta_abs(0x0001)
    a.emit(0x60)

    a.label("point_block")
    a.jsr("borrow00")
    lda_abs(BLOCK)
    sta_abs(0x0000)
    lda_abs(BLOCK + 1)
    sta_abs(0x0001)
    a.emit(0x60)

    a.label("restore00")
    lda_abs(IN_NMI)
    bne("restore_nmi")
    lda_abs(SAVED + 1)
    sta_abs(0x0001)
    lda_abs(SAVED)
    sta_abs(0x0000)
    a.emit(0x60)
    a.label("restore_nmi")
    lda_abs(SAVED_NMI + 1)
    sta_abs(0x0001)
    lda_abs(SAVED_NMI)
    sta_abs(0x0000)
    a.emit(0x60)

    # Send A bytes at ($00), last byte first, on register 2.
    a.label("send_a")
    cmp_i(0)
    beq("send_a_done")
    a.emit(0xA8)  # TAY
    a.label("send_a_loop")
    a.emit(0x88)  # DEY
    a.emit(0xB1, 0x00)
    a.jsr("r2put")
    a.emit(0x98)  # TYA
    bne("send_a_loop")
    a.label("send_a_done")
    a.emit(0x60)

    a.label("recv_a")
    cmp_i(0)
    beq("recv_a_done")
    a.emit(0xA8)
    a.label("recv_a_loop")
    a.emit(0x88)
    a.jsr("r2get")
    a.emit(0x91, 0x00)
    a.emit(0x98)
    bne("recv_a_loop")
    a.label("recv_a_done")
    a.emit(0x60)

    a.label("nmi_bounce")
    a.abs_addr(0x6C, NMILO)

    a.label("nmi_rti")
    a.emit(0x40)

    def nmi_one(name, to_host):
        a.label(name)
        lda_i(1)
        sta_abs(IN_NMI)
        a.emit(0x48, 0x98, 0x48)
        a.jsr("point_addr")
        ldy_i(0)
        if to_host:
            a.emit(0xB1, 0x00)
            sta_abs(R3D)
        else:
            bit_abs(R3S)
            bpl(name + "_skip")
            lda_abs(R3D)
            a.emit(0x91, 0x00)
        inc_abs(ADDR)
        bne(name + "_page")
        inc_abs(ADDR + 1)
        a.label(name + "_page")
        a.label(name + "_skip")
        a.jsr("restore00")
        lda_i(0)
        sta_abs(IN_NMI)
        a.emit(0x68, 0xA8, 0x68, 0x40)

    nmi_one("nmi_to_host", True)
    nmi_one("nmi_from_host", False)

    def nmi_pair(name, to_host):
        a.label(name)
        lda_i(1)
        sta_abs(IN_NMI)
        a.emit(0x48, 0x98, 0x48)
        a.jsr("point_addr")
        for i in range(2):
            ldy_i(0)
            if to_host:
                a.emit(0xB1, 0x00)
                sta_abs(R3D)
            else:
                lda_abs(R3D)
                a.emit(0x91, 0x00)
            inc_abs(ADDR)
            bne(f"{name}_p{i}")
            inc_abs(ADDR + 1)
            a.label(f"{name}_p{i}")
        a.jsr("restore00")
        lda_i(0)
        sta_abs(IN_NMI)
        a.emit(0x68, 0xA8, 0x68, 0x40)

    nmi_pair("nmi_to_host2", True)
    nmi_pair("nmi_from_host2", False)

    a.label("irq")
    a.emit(0x48, 0x8A, 0x48, 0x98, 0x48)
    bit_abs(R4S)
    bpl("irq_r1")
    a.jsr("r4_service")
    a.jmp("irq_out")
    a.label("irq_r1")
    bit_abs(R1S)
    bpl("irq_out")
    lda_abs(R1D)
    bpl("irq_event")
    and_i(0x40)
    beq("irq_esc0")
    lda_i(0xFF)
    sta_abs(0x00FF)
    a.jmp("irq_out")
    a.label("irq_esc0")
    lda_i(0)
    sta_abs(0x00FF)
    a.jmp("irq_out")
    a.label("irq_event")
    a.jsr("r1get")
    a.jsr("r1get")
    a.jsr("r1get")
    a.label("irq_out")
    a.emit(0x68, 0xA8, 0x68, 0xAA, 0x68, 0x40)

    a.label("r4_service")
    lda_abs(R4D)
    inc_abs(COUNT)
    sta_abs(LAST)
    cmp_i(0x80)
    bcs("r4_error")
    cmp_i(8)
    bcs("r4_done")
    a.emit(0x48)
    a.jsr("r4get")
    a.emit(0x68)
    a.emit(0x0A)  # ASL
    a.emit(0xA8)  # TAY
    a.abs_addr(0xB9, 0)  # LDA table,Y
    a.fix(a.pc - 2, "r4_table", "word")
    sta_abs(SCRATCH)
    a.emit(0xC8)
    a.abs_addr(0xB9, 0)
    a.fix(a.pc - 2, "r4_table", "word")
    sta_abs(SCRATCH + 1)
    a.abs_addr(0x6C, SCRATCH)
    a.label("r4_done")
    a.emit(0x60)
    a.label("r4_error")
    a.jsr("r2get")
    a.jsr("r2get")
    a.label("r4_err_loop")
    a.jsr("r2get")
    bne("r4_err_loop")
    a.emit(0x60)
    a.label("r4_table")
    for name in ("type0", "type1", "type2", "type3", "type4", "type5", "type6", "type7"):
        at = a.pc
        a.emit(0, 0)
        a.fix(at, name, "word")

    a.label("type0")
    arm("nmi_to_host")
    a.emit(0x60)
    a.label("type1")
    arm("nmi_from_host")
    a.emit(0x60)
    a.label("type2")
    arm("nmi_to_host2")
    a.emit(0x60)
    a.label("type3")
    arm("nmi_from_host2")
    a.emit(0x60)
    a.label("type4")
    a.jsr("read_addr")
    a.jsr("r4get")
    lda_abs(ADDR)
    sta_abs(EXEC)
    lda_abs(ADDR + 1)
    sta_abs(EXEC + 1)
    lda_i(1)
    sta_abs(EXECF)
    a.emit(0x60)
    # Release carries no address. The identity byte was already taken.
    a.label("type5")
    imm_lo("nmi_rti")
    imm_hi("nmi_rti")
    a.jsr("set_nmi")
    a.emit(0x60)

    # Polled transfers. The NMI vector is pointed at RTI before the host
    # enables M, and the data loop is inline so it finishes inside the
    # host's three NOPs.
    def polled(name, to_host):
        a.label(name)
        imm_lo("nmi_rti")
        imm_hi("nmi_rti")
        a.jsr("set_nmi")
        a.jsr("read_addr")
        a.jsr("r4get")
        a.jsr("point_addr")
        ldy_i(0)
        a.label(name + "_page")
        if to_host:
            a.emit(0xB1, 0x00)
            bit_abs(R3S)
            bvc(name + "_page")
            sta_abs(R3D)
        else:
            bit_abs(R3S)
            bpl(name + "_page")
            lda_abs(R3D)
            a.emit(0x91, 0x00)
        a.emit(0xC8)
        bne(name + "_page")
        a.jsr("restore00")

    polled("type6", True)
    lda_i(0)
    a.jsr("r4put")
    a.emit(0x60)

    polled("type7", False)
    a.emit(0x60)

    a.label("save_xy")
    stx_abs(BLOCK)
    sty_abs(BLOCK + 1)
    a.emit(0x60)

    a.label("put_cr")
    a.label("cr_loop")
    a.emit(0xB1, 0x00)
    a.jsr("r2put")
    cmp_i(0x0D)
    beq("cr_done")
    a.emit(0xC8)
    bne("cr_loop")
    a.label("cr_done")
    a.emit(0x60)

    a.label("oswrch")
    a.emit(0x58)
    a.jmp("r1put")

    a.label("osasci")
    a.emit(0x20, 0xEE, 0xFF)
    a.emit(0x60)

    a.label("osnewl")
    a.emit(0x48)
    lda_i(0x0D)
    a.emit(0x20, 0xEE, 0xFF)
    a.emit(0x68, 0x60)

    a.label("osrdch")
    a.emit(0x58)
    lda_i(0x00)
    a.jsr("r2put")
    a.jsr("r2get")
    a.emit(0xAA)
    a.jsr("r2get")
    a.emit(0x48)
    a.emit(0x8A)
    bpl("osrdch_ok")
    a.emit(0x38)
    a.emit(0x68, 0x60)
    a.label("osrdch_ok")
    a.emit(0x18)
    a.emit(0x68, 0x60)

    a.label("osbyte")
    cmp_i(0x82)
    beq("byte82")
    cmp_i(0x83)
    beq("byte83")
    cmp_i(0x84)
    beq("byte84")
    cmp_i(0x80)
    bcc("byte_short")
    sta_abs(SCRATCH)
    a.emit(0x58)
    lda_i(0x06)
    a.jsr("r2put")
    a.emit(0x8A)
    a.jsr("r2put")
    a.emit(0x98)
    a.jsr("r2put")
    lda_abs(SCRATCH)
    a.jsr("r2put")
    cmp_i(0x9D)
    beq("byte_keep")
    a.jsr("r2get")
    sta_abs(SCRATCH + 1)
    a.jsr("r2get")
    a.emit(0x48)
    a.jsr("r2get")
    a.emit(0xAA)
    a.emit(0x68)
    a.emit(0xA8)
    lda_abs(SCRATCH + 1)
    bmi("byte_sec")
    a.emit(0x18)
    a.jmp("byte_keep")
    a.label("byte_sec")
    a.emit(0x38)
    a.label("byte_keep")
    lda_abs(SCRATCH)
    a.emit(0x60)
    a.label("byte_short")
    sta_abs(SCRATCH)
    a.emit(0x58)
    lda_i(0x04)
    a.jsr("r2put")
    a.emit(0x8A)
    a.jsr("r2put")
    lda_abs(SCRATCH)
    a.jsr("r2put")
    a.jsr("r2get")
    a.emit(0xAA)
    lda_abs(SCRATCH)
    a.emit(0x60)
    a.label("byte82")
    ldx_i(0)
    ldy_i(0)
    a.emit(0x18, 0x60)
    a.label("byte83")
    ldx_i(0)
    ldy_i(0x08)
    a.emit(0x18, 0x60)
    a.label("byte84")
    ldx_i(0)
    ldy_i(0x80)
    a.emit(0x18, 0x60)

    a.label("osword")
    a.emit(0x58)
    cmp_i(0)
    beq("osword0")
    sta_abs(SCRATCH)
    a.jsr("save_xy")
    a.jsr("point_block")
    lda_abs(SCRATCH)
    cmp_i(0x80)
    bcs("osword_hi")
    cmp_i(21)
    bcs("osword_16")
    a.emit(0xAA)
    a.emit(0xCA)
    a.abs_addr(0xBD, 0)
    a.fix(a.pc - 2, "ow_send", "word")
    sta_abs(SCRATCH + 1)
    a.abs_addr(0xBD, 0)
    a.fix(a.pc - 2, "ow_recv", "word")
    sta_abs(SCRATCH + 2)
    a.jmp("osword_go")
    a.label("osword_16")
    lda_i(16)
    sta_abs(SCRATCH + 1)
    sta_abs(SCRATCH + 2)
    a.jmp("osword_go")
    a.label("osword_hi")
    ldy_i(0)
    a.emit(0xB1, 0x00)
    sta_abs(SCRATCH + 1)
    ldy_i(1)
    a.emit(0xB1, 0x00)
    sta_abs(SCRATCH + 2)
    a.label("osword_go")
    lda_i(0x08)
    a.jsr("r2put")
    lda_abs(SCRATCH)
    a.jsr("r2put")
    lda_abs(SCRATCH + 1)
    a.jsr("r2put")
    lda_abs(SCRATCH + 1)
    a.jsr("send_a")
    lda_abs(SCRATCH + 2)
    a.jsr("r2put")
    lda_abs(SCRATCH + 2)
    a.jsr("recv_a")
    a.jsr("restore00")
    a.emit(0x60)

    a.label("osword0")
    a.jsr("save_xy")
    a.jsr("point_block")
    lda_i(0x0A)
    a.jsr("r2put")
    ldy_i(4)
    a.label("ow0_send")
    a.emit(0xB1, 0x00)
    a.jsr("r2put")
    a.emit(0x88)
    cpy_i(1)
    bne("ow0_send")
    lda_i(0x07)
    a.jsr("r2put")
    lda_i(0x00)
    a.jsr("r2put")
    a.jsr("r2get")
    cmp_i(0x80)
    bcs("ow0_esc")
    ldy_i(0)
    a.emit(0xB1, 0x00)
    a.emit(0x48)
    ldy_i(1)
    a.emit(0xB1, 0x00)
    sta_abs(0x0001)
    a.emit(0x68)
    sta_abs(0x0000)
    ldy_i(0)
    a.label("ow0_line")
    a.jsr("r2get")
    a.emit(0x91, 0x00)
    cmp_i(0x0D)
    beq("ow0_ok")
    a.emit(0xC8)
    bne("ow0_line")
    a.label("ow0_ok")
    a.jsr("restore00")
    a.emit(0x18, 0x60)
    a.label("ow0_esc")
    a.jsr("restore00")
    lda_i(0x1B)
    a.emit(0x38, 0x60)

    a.label("oscli")
    a.emit(0x58)
    a.jsr("save_xy")
    lda_i(0x02)
    a.jsr("r2put")
    a.jsr("point_block")
    ldy_i(0)
    a.jsr("put_cr")
    a.jsr("restore00")
    a.jsr("r2get")
    cmp_i(0x80)
    bne("oscli_done")
    a.abs_addr(0x6C, EXEC)
    a.label("oscli_done")
    a.emit(0x60)

    a.label("osfind")
    a.emit(0x58)
    sta_abs(SCRATCH)
    a.jsr("save_xy")
    lda_i(0x12)
    a.jsr("r2put")
    lda_abs(SCRATCH)
    a.jsr("r2put")
    beq("osfind_close")
    a.jsr("point_block")
    ldy_i(0)
    a.jsr("put_cr")
    a.jsr("restore00")
    a.jmp("osfind_result")
    a.label("osfind_close")
    lda_abs(BLOCK + 1)
    a.jsr("r2put")
    a.label("osfind_result")
    a.jsr("r2get")
    a.emit(0x60)

    a.label("osbput")
    a.emit(0x58)
    sta_abs(SCRATCH)
    lda_i(0x10)
    a.jsr("r2put")
    a.emit(0x98)
    a.jsr("r2put")
    lda_abs(SCRATCH)
    a.jsr("r2put")
    a.emit(0x60)

    a.label("osbget")
    a.emit(0x58)
    lda_i(0x0E)
    a.jsr("r2put")
    a.emit(0x98)
    a.jsr("r2put")
    a.jsr("r2get")
    a.emit(0xAA)
    a.jsr("r2get")
    a.emit(0x48)
    a.emit(0x8A)
    bpl("osbget_ok")
    a.emit(0x38)
    a.emit(0x68, 0x60)
    a.label("osbget_ok")
    a.emit(0x18)
    a.emit(0x68, 0x60)

    a.label("osargs")
    a.emit(0x58)
    sta_abs(SCRATCH)
    sty_abs(SCRATCH + 1)
    stx_abs(SCRATCH + 3)
    lda_i(0x0C)
    a.jsr("r2put")
    lda_abs(SCRATCH + 1)
    a.jsr("r2put")
    lda_abs(SCRATCH + 3)
    a.emit(0x18, 0x69, 0x03)  # CLC, ADC #3
    a.emit(0xAA)
    ldy_i(4)
    a.label("args_send")
    a.emit(0xB5, 0x00)  # LDA $00,X
    a.jsr("r2put")
    a.emit(0xCA)
    a.emit(0x88)
    bne("args_send")
    lda_abs(SCRATCH)
    a.jsr("r2put")
    a.jsr("r2get")
    sta_abs(SCRATCH)
    lda_abs(SCRATCH + 3)
    a.emit(0x18, 0x69, 0x03)
    a.emit(0xAA)
    ldy_i(4)
    a.label("args_recv")
    a.jsr("r2get")
    a.emit(0x95, 0x00)  # STA $00,X
    a.emit(0xCA)
    a.emit(0x88)
    bne("args_recv")
    lda_abs(SCRATCH)
    ldx_i(0)
    a.abs_addr(0xAE, SCRATCH + 3)  # LDX scratch+3, the base
    a.emit(0x60)

    a.label("osgbpb")
    a.emit(0x58)
    sta_abs(SCRATCH)
    a.jsr("save_xy")
    lda_i(0x16)
    a.jsr("r2put")
    a.jsr("point_block")
    lda_i(13)
    a.jsr("send_a")
    lda_abs(SCRATCH)
    a.jsr("r2put")
    lda_i(13)
    a.jsr("recv_a")
    a.jsr("r2get")
    sta_abs(SCRATCH + 1)
    a.jsr("r2get")
    sta_abs(SCRATCH)
    lda_abs(SCRATCH + 1)
    bmi("gbpb_sec")
    a.jsr("restore00")
    lda_abs(SCRATCH)
    a.emit(0x18, 0x60)
    a.label("gbpb_sec")
    a.jsr("restore00")
    lda_abs(SCRATCH)
    a.emit(0x38, 0x60)

    a.label("osfile")
    a.emit(0x58)
    sta_abs(SCRATCH)
    a.jsr("save_xy")
    lda_i(0x14)
    a.jsr("r2put")
    a.jsr("point_block")
    ldy_i(17)
    a.label("file_send")
    a.emit(0xB1, 0x00)
    a.jsr("r2put")
    a.emit(0x88)
    cpy_i(1)
    bne("file_send")
    ldy_i(0)
    a.emit(0xB1, 0x00)
    sta_abs(NAME)
    ldy_i(1)
    a.emit(0xB1, 0x00)
    sta_abs(NAME + 1)
    lda_abs(NAME)
    sta_abs(0x0000)
    lda_abs(NAME + 1)
    sta_abs(0x0001)
    ldy_i(0)
    a.jsr("put_cr")
    lda_abs(SCRATCH)
    a.jsr("r2put")
    lda_abs(BLOCK)
    sta_abs(0x0000)
    lda_abs(BLOCK + 1)
    sta_abs(0x0001)
    ldy_i(17)
    a.label("file_recv")
    a.jsr("r2get")
    a.emit(0x91, 0x00)
    a.emit(0x88)
    cpy_i(1)
    bne("file_recv")
    a.jsr("r2get")
    sta_abs(SCRATCH)
    a.jsr("restore00")
    lda_abs(SCRATCH)
    a.emit(0x60)

    a.label("ow_send")
    a.emit(*[send for send, _recv in OSWORD_IO])
    a.label("ow_recv")
    a.emit(*[recv for _send, recv in OSWORD_IO])

    if a.pc > 0xFFCE:
        raise SystemExit(f"client code overlaps the OS entries at ${a.pc:04X}")

    entries = [
        (0xFFCE, "osfind"),
        (0xFFD1, "osgbpb"),
        (0xFFD4, "osbput"),
        (0xFFD7, "osbget"),
        (0xFFDA, "osargs"),
        (0xFFDD, "osfile"),
        (0xFFE0, "osrdch"),
        (0xFFE3, "osasci"),
        (0xFFE7, "osnewl"),
        (0xFFEE, "oswrch"),
        (0xFFF1, "osword"),
        (0xFFF4, "osbyte"),
        (0xFFF7, "oscli"),
    ]
    entry_vector = {
        "osfind": 0x021C,
        "osgbpb": 0x021A,
        "osbput": 0x0218,
        "osbget": 0x0216,
        "osargs": 0x0214,
        "osfile": 0x0212,
        "osrdch": 0x0210,
        "oswrch": 0x020E,
        "osword": 0x020C,
        "osbyte": 0x020A,
        "oscli": 0x0208,
    }
    for addr, name in entries:
        a.org(addr)
        if name in entry_vector:
            vec = entry_vector[name]
            a.emit(0x6C, vec & 0xFF, (vec >> 8) & 0xFF)
        else:
            a.jmp(name)
    a.org(0xFFFA)
    at = a.pc
    a.emit(0, 0)
    a.fix(at, "nmi_bounce", "word")
    at = a.pc
    a.emit(0, 0)
    a.fix(at, "reset", "word")
    at = a.pc
    a.emit(0, 0)
    a.fix(at, "irq", "word")
    a.resolve()
    return a


def main():
    image = build()
    out = pathlib.Path(__file__).with_name("tube_client.c")
    lines = [
        "// Generated by gen_tube_client.py. Do not edit by hand.",
        "",
        "#include <stdint.h>",
        "",
        "const uint8_t kTubeClient[2048] = {",
    ]
    data = image.mem
    for i in range(0, SIZE, 16):
        chunk = ", ".join(f"0x{byte:02x}" for byte in data[i : i + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")
    out.write_text("\n".join(lines))
    print(f"wrote {out} ({SIZE} bytes)")
    for name in ("reset", "irq", "nmi_bounce", "oswrch", "osbyte", "banner"):
        print(f"  {name:12} ${image.labels[name]:04X}")


if __name__ == "__main__":
    main()
