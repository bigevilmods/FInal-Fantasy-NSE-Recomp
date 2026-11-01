"""Minimal, complete NMOS 6502 decoder (official opcodes + illegal NOPs)."""

# mode -> (size, base cycles)
MODES = {
    "imp": (1, 2), "acc": (1, 2), "imm": (2, 2), "zp": (2, 3),
    "zpx": (2, 4), "zpy": (2, 4), "abs": (3, 4), "absx": (3, 4),
    "absy": (3, 4), "indx": (2, 6), "indy": (2, 5), "ind": (3, 5),
    "rel": (2, 2),
}

OPS = {}


def _op(code, mnem, mode, cycles=None):
    OPS[code] = (mnem, mode, cycles if cycles is not None else MODES[mode][1])


# --- official opcodes -------------------------------------------------------
for code, mnem, mode in [
    (0x00, "BRK", "imp"), (0x01, "ORA", "indx"), (0x05, "ORA", "zp"),
    (0x06, "ASL", "zp"), (0x08, "PHP", "imp"), (0x09, "ORA", "imm"),
    (0x0A, "ASL", "acc"), (0x0D, "ORA", "abs"), (0x0E, "ASL", "abs"),
    (0x10, "BPL", "rel"), (0x11, "ORA", "indy"), (0x15, "ORA", "zpx"),
    (0x16, "ASL", "zpx"), (0x18, "CLC", "imp"), (0x19, "ORA", "absy"),
    (0x1D, "ORA", "absx"), (0x1E, "ASL", "absx"),
    (0x20, "JSR", "abs"), (0x21, "AND", "indx"), (0x24, "BIT", "zp"),
    (0x25, "AND", "zp"), (0x26, "ROL", "zp"), (0x28, "PLP", "imp"),
    (0x29, "AND", "imm"), (0x2A, "ROL", "acc"), (0x2C, "BIT", "abs"),
    (0x2D, "AND", "abs"), (0x2E, "ROL", "abs"),
    (0x30, "BMI", "rel"), (0x31, "AND", "indy"), (0x35, "AND", "zpx"),
    (0x36, "ROL", "zpx"), (0x38, "SEC", "imp"), (0x39, "AND", "absy"),
    (0x3D, "AND", "absx"), (0x3E, "ROL", "absx"),
    (0x40, "RTI", "imp"), (0x41, "EOR", "indx"), (0x45, "EOR", "zp"),
    (0x46, "LSR", "zp"), (0x48, "PHA", "imp"), (0x49, "EOR", "imm"),
    (0x4A, "LSR", "acc"), (0x4C, "JMP", "abs"), (0x4D, "EOR", "abs"),
    (0x4E, "LSR", "abs"),
    (0x50, "BVC", "rel"), (0x51, "EOR", "indy"), (0x55, "EOR", "zpx"),
    (0x56, "LSR", "zpx"), (0x58, "CLI", "imp"), (0x59, "EOR", "absy"),
    (0x5D, "EOR", "absx"), (0x5E, "LSR", "absx"),
    (0x60, "RTS", "imp"), (0x61, "ADC", "indx"), (0x65, "ADC", "zp"),
    (0x66, "ROR", "zp"), (0x68, "PLA", "imp"), (0x69, "ADC", "imm"),
    (0x6A, "ROR", "acc"), (0x6C, "JMP", "ind"), (0x6D, "ADC", "abs"),
    (0x6E, "ROR", "abs"),
    (0x70, "BVS", "rel"), (0x71, "ADC", "indy"), (0x75, "ADC", "zpx"),
    (0x76, "ROR", "zpx"), (0x78, "SEI", "imp"), (0x79, "ADC", "absy"),
    (0x7D, "ADC", "absx"), (0x7E, "ROR", "absx"),
    (0x81, "STA", "indx"), (0x84, "STY", "zp"), (0x85, "STA", "zp"),
    (0x86, "STX", "zp"), (0x88, "DEY", "imp"), (0x8A, "TXA", "imp"),
    (0x8C, "STY", "abs"), (0x8D, "STA", "abs"), (0x8E, "STX", "abs"),
    (0x90, "BCC", "rel"), (0x91, "STA", "indy"), (0x94, "STY", "zpx"),
    (0x95, "STA", "zpx"), (0x96, "STX", "zpy"), (0x98, "TYA", "imp"),
    (0x99, "STA", "absy"), (0x9A, "TXS", "imp"), (0x9D, "STA", "absx"),
    (0xA0, "LDY", "imm"), (0xA1, "LDA", "indx"), (0xA2, "LDX", "imm"),
    (0xA4, "LDY", "zp"), (0xA5, "LDA", "zp"), (0xA6, "LDX", "zp"),
    (0xA8, "TAY", "imp"), (0xA9, "LDA", "imm"), (0xAA, "TAX", "imp"),
    (0xAC, "LDY", "abs"), (0xAD, "LDA", "abs"), (0xAE, "LDX", "abs"),
    (0xB0, "BCS", "rel"), (0xB1, "LDA", "indy"), (0xB4, "LDY", "zpx"),
    (0xB5, "LDA", "zpx"), (0xB6, "LDX", "zpy"), (0xB8, "CLV", "imp"),
    (0xB9, "LDA", "absy"), (0xBA, "TSX", "imp"), (0xBC, "LDY", "absx"),
    (0xBD, "LDA", "absx"), (0xBE, "LDX", "absy"),
    (0xC0, "CPY", "imm"), (0xC1, "CMP", "indx"), (0xC4, "CPY", "zp"),
    (0xC5, "CMP", "zp"), (0xC6, "DEC", "zp"), (0xC8, "INY", "imp"),
    (0xC9, "CMP", "imm"), (0xCA, "DEX", "imp"), (0xCC, "CPY", "abs"),
    (0xCD, "CMP", "abs"), (0xCE, "DEC", "abs"),
    (0xD0, "BNE", "rel"), (0xD1, "CMP", "indy"), (0xD5, "CMP", "zpx"),
    (0xD6, "DEC", "zpx"), (0xD8, "CLD", "imp"), (0xD9, "CMP", "absy"),
    (0xDD, "CMP", "absx"), (0xDE, "DEC", "absx"),
    (0xE0, "CPX", "imm"), (0xE1, "SBC", "indx"), (0xE4, "CPX", "zp"),
    (0xE5, "SBC", "zp"), (0xE6, "INC", "zp"), (0xE8, "INX", "imp"),
    (0xE9, "SBC", "imm"), (0xEA, "NOP", "imp"), (0xEC, "CPX", "abs"),
    (0xED, "SBC", "abs"), (0xEE, "INC", "abs"),
    (0xF0, "BEQ", "rel"), (0xF1, "SBC", "indy"), (0xF5, "SBC", "zpx"),
    (0xF6, "INC", "zpx"), (0xF8, "SED", "imp"), (0xF9, "SBC", "absy"),
    (0xFD, "SBC", "absx"), (0xFE, "INC", "absx"),
]:
    _op(code, mnem, mode)

# --- illegal NOPs used by NES code ------------------------------------------
for code in (0x1A, 0x3A, 0x5A, 0x7A, 0xDA, 0xFA):
    _op(code, "NOP", "imp")
for code in (0x80, 0x82, 0x89, 0xC2, 0xE2):
    _op(code, "NOP", "imm")
for code in (0x04, 0x44, 0x64):
    _op(code, "NOP", "zp")
for code in (0x14, 0x34, 0x54, 0x74, 0xD4, 0xF4):
    _op(code, "NOP", "zpx")
_op(0x0C, "NOP", "abs")
for code in (0x1C, 0x3C, 0x5C, 0x7C, 0xDC, 0xFC):
    _op(code, "NOP", "absx")

Branches = {"BPL", "BMI", "BVC", "BVS", "BCC", "BCS", "BNE", "BEQ"}
Terminators = Branches | {"JMP", "JSR", "RTS", "RTI", "BRK"}


class Insn:
    __slots__ = ("bank", "addr", "code", "mnem", "mode", "size", "cycles",
                 "operand", "target", "next")

    def __init__(self, bank, addr, code, mnem, mode, size, cycles, operand):
        self.bank = bank
        self.addr = addr
        self.code = code
        self.mnem = mnem
        self.mode = mode
        self.size = size
        self.cycles = cycles
        self.operand = operand
        self.next = addr + size
        self.target = None
        if mode == "rel":
            off = operand - 256 if operand >= 128 else operand
            self.target = (addr + 2 + off) & 0xFFFF
        elif mode in ("abs", "ind"):
            self.target = operand

    def text(self):
        if self.mode == "imp":
            return self.mnem
        if self.mode == "acc":
            return self.mnem + " A"
        if self.mode == "imm":
            return "%s #$%02X" % (self.mnem, self.operand)
        if self.mode == "rel":
            return "%s $%04X" % (self.mnem, self.target)
        if self.mode == "zp":
            return "%s $%02X" % (self.mnem, self.operand)
        if self.mode == "zpx":
            return "%s $%02X,X" % (self.mnem, self.operand)
        if self.mode == "zpy":
            return "%s $%02X,Y" % (self.mnem, self.operand)
        if self.mode == "abs":
            return "%s $%04X" % (self.mnem, self.operand)
        if self.mode == "absx":
            return "%s $%04X,X" % (self.mnem, self.operand)
        if self.mode == "absy":
            return "%s $%04X,Y" % (self.mnem, self.operand)
        if self.mode == "indx":
            return "%s ($%02X,X)" % (self.mnem, self.operand)
        if self.mode == "indy":
            return "%s ($%02X),Y" % (self.mnem, self.operand)
        if self.mode == "ind":
            return "%s ($%04X)" % (self.mnem, self.operand)
        return self.mnem


def decode(buf, off, bank, addr):
    """Decode one instruction from buf at off; addr is its CPU address."""
    if off + 3 > len(buf):
        return Insn(bank, addr, 0xEA, "NOP", "imp", 1, 2, 0)
    code = buf[off]
    if code not in OPS:
        return Insn(bank, addr, code, "???", "imp", 1, 2, 0)
    mnem, mode, cycles = OPS[code]
    size = MODES[mode][0]
    operand = buf[off + 1] if size > 1 else 0
    if size > 2:
        operand |= buf[off + 2] << 8
    return Insn(bank, addr, code, mnem, mode, size, cycles, operand)
