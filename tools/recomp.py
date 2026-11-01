"""Static 6502 -> C++ recompiler for NES Final Fantasy (USA), MMC1.

Pipeline:
  1. parse iNES / NES 2.0 header
  2. recursive-descent disassembly per 16 KiB PRG bank
  3. direct-threading code generation (one C++ function per basic block)
"""
import os
import sys
import collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nes6502 import decode, Branches, Terminators  # noqa: E402

BANK_SIZE = 0x4000
BASE = 0x8000


class Rom:
    def __init__(self, path):
        d = open(path, "rb").read()
        assert d[:4] == b"NES\x1a", "not an iNES image"
        self.header = d[:16]
        self.prg_banks = d[4]
        self.chr_banks = d[5]
        self.flags6 = d[6]
        self.flags7 = d[7]
        self.mapper = (d[7] & 0xF0) | (d[6] >> 4)
        self.nes2 = (d[7] & 0x0C) == 0x08
        self.prg_ram_shift = d[10] & 0x0F
        self.prg_ram_size = 64 << self.prg_ram_shift if self.prg_ram_shift else 0
        off = 16
        if d[6] & 0x04:
            off += 512
        self.prg = d[off:off + self.prg_banks * BANK_SIZE]
        self.chr = d[off + self.prg_banks * BANK_SIZE:]
        self.path = path

    def vectors(self):
        last = self.prg_banks - 1
        b = self.prg[last * BANK_SIZE:(last + 1) * BANK_SIZE]
        return (b[0x3FFA] | b[0x3FFB] << 8,
                b[0x3FFC] | b[0x3FFD] << 8,
                b[0x3FFE] | b[0x3FFF] << 8)


class Analysis:
    def __init__(self, rom, bank, base):
        self.rom = rom
        self.bank = bank
        self.base = base
        self.img = rom.prg[bank * BANK_SIZE:(bank + 1) * BANK_SIZE]
        self.starts = {}
        self.entries = set()
        self.blocks = {}
        self.conflicts = []
        self.unresolved = []

    def byte(self, addr):
        return self.img[addr - self.base]

    def word(self, addr):
        a = addr - self.base
        if a < 0 or a + 1 >= len(self.img):
            return 0
        return self.img[a] | self.img[a + 1] << 8

    def seeds_from_linear_sweep(self):
        """Harvest every plausible code pointer in the bank (over-approximate)."""
        seeds = set()
        off = 0
        while off < BANK_SIZE:
            ins = decode(self.img, off, self.bank, self.base + off)
            if ins.mnem in Branches:
                seeds.add(ins.target)
            elif ins.mnem in ("JSR", "JMP") and ins.mode in ("abs", "ind"):
                t = ins.operand
                if self.base <= t < 0x10000:
                    seeds.add(t)
            off += ins.size
        return seeds

    def indirect_targets(self, ins):
        """Resolve JMP (abs) tables that live in this bank's ROM window."""
        out = []
        t = ins.operand
        if not (self.base <= t < 0x10000):
            return out
        for i in range(64):
            a = t + i * 2
            if not (self.base <= a < self.base + 0x3FFD):
                break
            v = self.word(a)
            if v == 0:
                break
            if not (self.base <= v < 0x10000):
                break
            if v not in self.starts:
                out.append(v)
        return out

    def zp_indirect_targets(self, hist, zp):
        """Resolve JMP (zp) where zp/zp+1 were loaded from a ROM jump table."""
        lo = hi = None
        for i, h in enumerate(hist):
            if h.mnem != "STA" or h.mode != "zp":
                continue
            if h.operand not in (zp, (zp + 1) & 0xFF):
                continue
            src = None
            for p in reversed(hist[:i]):
                if p.mnem == "LDA" and p.mode in ("absx", "absy", "abs"):
                    src = p.operand
                    break
            if src is None or not (self.base <= src < self.base + 0x4000):
                continue
            if h.operand == zp:
                lo = src
            else:
                hi = src
        tbl = lo if lo is not None else hi
        if hi is not None and (tbl is None or hi < tbl):
            tbl = hi
        if tbl is None:
            return []
        # Bound the table using the index mask seen just before the jump
        # (e.g. "AND #$1E" before the LDA tbl,X / JMP (zp) pair).
        count = 8
        for h in hist[-16:]:
            if h.mode == "imm" and h.mnem in ("AND", "CMP", "CPX", "CPY"):
                count = (h.operand >> 1) + 1
        count = max(1, min(count, 32))
        out = []
        seen = set()
        for i in range(0, count * 2, 2):
            a = tbl + i
            if not (self.base <= a < self.base + 0x3FFE):
                break
            v = self.word(a)
            if not (self.base <= v < 0x10000):
                break
            if v in seen:
                continue
            seen.add(v)
            out.append(v)
        return out

    def run(self, seeds):
        queue = collections.deque(sorted(seeds))
        self.entries.update(seeds)
        visited = self.starts
        while queue:
            a = queue.popleft()
            hist = []
            while True:
                if a in visited:
                    # already decoded: still a valid block entry
                    self.entries.add(a)
                    break
                if not (self.base <= a < 0x10000):
                    break
                ins = decode(self.img, a - self.base, self.bank, a)
                if ins.mnem == "???":
                    self.unresolved.append((self.bank, a, "illegal-opcode"))
                    visited[a] = ins
                    break
                visited[a] = ins
                hist.append(ins)

                if ins.mnem in Branches:
                    self.entries.add(ins.target)
                    self.entries.add(ins.next)
                    queue.append(ins.target)
                    queue.append(ins.next)
                    break
                if ins.mnem == "JMP":
                    if ins.mode == "abs":
                        self.entries.add(ins.target)
                        queue.append(ins.target)
                    else:
                        tbl = self.indirect_targets(ins)
                        if not tbl and ins.mode == "ind" and ins.operand < 0x0100:
                            tbl = self.zp_indirect_targets(hist, ins.operand)
                        if tbl:
                            self.entries.update(tbl)
                            queue.extend(tbl)
                        else:
                            self.unresolved.append((self.bank, a, "indirect-jmp"))
                    break
                if ins.mnem == "JSR":
                    self.entries.add(ins.target)
                    self.entries.add(ins.next)
                    queue.append(ins.target)
                    queue.append(ins.next)
                    break
                if ins.mnem in ("RTS", "RTI", "BRK"):
                    break
                a = ins.next

    def build_blocks(self):
        for start in sorted(self.entries):
            if start not in self.starts:
                continue
            seq = []
            a = start
            while True:
                ins = self.starts.get(a)
                if ins is None:
                    break
                seq.append(ins)
                if ins.mnem in Terminators or ins.mnem == "???":
                    break
                if ins.next in self.entries:
                    break
                if len(seq) > 512:
                    break
                a = ins.next
            if seq:
                self.blocks[start] = seq


# --------------------------------------------------------------------------
# code generation
# --------------------------------------------------------------------------

def addr_expr(ins):
    m = ins.mode
    op = ins.operand
    if m == "zp":
        return "0x%02X" % op
    if m == "zpx":
        return "(uint8_t)(0x%02X+X)" % op
    if m == "zpy":
        return "(uint8_t)(0x%02X+Y)" % op
    if m == "abs":
        return "0x%04X" % op
    if m == "absx":
        return "(uint16_t)(0x%04X+X)" % op
    if m == "absy":
        return "(uint16_t)(0x%04X+Y)" % op
    if m == "indx":
        return "zp_ind_x(0x%02X)" % op
    if m == "indy":
        return "zp_ind_y(0x%02X)" % op
    raise ValueError("no address for mode " + m)


def rd_expr(ins):
    if ins.mode == "imm":
        return "0x%02X" % ins.operand
    return "rd8(%s)" % addr_expr(ins)


BRANCH_FLAG = {
    "BPL": "!(P&0x80)", "BMI": "(P&0x80)",
    "BVC": "!(P&0x40)", "BVS": "(P&0x40)",
    "BCC": "!(P&0x01)", "BCS": "(P&0x01)",
    "BNE": "!(P&0x02)", "BEQ": "(P&0x02)",
}


def emit_insn(ins):
    """Return list of C++ lines (no cycle accounting, no return)."""
    mnem, mode, op = ins.mnem, ins.mode, ins.operand
    out = []

    def load(reg):
        out.append("%s = %s; SETNZ(%s);" % (reg, rd_expr(ins), reg))

    def store(reg):
        out.append("wr8(%s, %s);" % (addr_expr(ins), reg))

    if mnem == "???":
        out.append("illegal_op(0x%04X);" % ins.addr)
    elif mnem in ("LDA", "LDX", "LDY"):
        load({"LDA": "A", "LDX": "X", "LDY": "Y"}[mnem])
    elif mnem in ("STA", "STX", "STY"):
        store({"STA": "A", "STX": "X", "STY": "Y"}[mnem])
    elif mnem in ("AND", "ORA", "EOR"):
        sym = {"AND": "&", "ORA": "|", "EOR": "^"}[mnem]
        out.append("A %s= %s; SETNZ(A);" % (sym, rd_expr(ins)))
    elif mnem == "ADC":
        out.append("adc8(%s);" % rd_expr(ins))
    elif mnem == "SBC":
        out.append("sbc8(%s);" % rd_expr(ins))
    elif mnem == "CMP":
        out.append("cmp8(A, %s);" % rd_expr(ins))
    elif mnem == "CPX":
        out.append("cmp8(X, %s);" % rd_expr(ins))
    elif mnem == "CPY":
        out.append("cmp8(Y, %s);" % rd_expr(ins))
    elif mnem == "BIT":
        a = addr_expr(ins)
        out.append("{ uint8_t t = rd8(%s); P = (P & 0x3D) | (t & 0xC0) | "
                   "((A & t) ? 0 : 0x02); }" % a)
    elif mnem in ("INX", "DEX", "INY", "DEY"):
        reg = {"INX": "X", "DEX": "X", "INY": "Y", "DEY": "Y"}[mnem]
        sym = "+" if mnem.startswith("IN") else "-"
        out.append("%s %s= 1; SETNZ(%s);" % (reg, sym, reg))
    elif mnem in ("INC", "DEC"):
        a = addr_expr(ins)
        sym = "+" if mnem == "INC" else "-"
        out.append("{ uint8_t t = (uint8_t)(rd8(%s) %s 1); wr8(%s, t); "
                   "SETNZ(t); }" % (a, sym, a))
    elif mnem in ("ASL", "LSR", "ROL", "ROR"):
        fn = {"ASL": "asl8", "LSR": "lsr8", "ROL": "rol8", "ROR": "ror8"}[mnem]
        if mode == "acc" or mnem == "ASL" and mode == "acc":
            out.append("A = %s(A);" % fn)
        else:
            a = addr_expr(ins)
            out.append("{ uint8_t t = %s(rd8(%s)); wr8(%s, t); }"
                       % (fn, a, a))
    elif mnem == "TAX":
        out.append("X = A; SETNZ(X);")
    elif mnem == "TAY":
        out.append("Y = A; SETNZ(Y);")
    elif mnem == "TXA":
        out.append("A = X; SETNZ(A);")
    elif mnem == "TYA":
        out.append("A = Y; SETNZ(A);")
    elif mnem == "TSX":
        out.append("X = S; SETNZ(X);")
    elif mnem == "TXS":
        out.append("S = X;")
    elif mnem == "PHA":
        out.append("push8(A);")
    elif mnem == "PHP":
        out.append("push8(P | 0x30);")
    elif mnem == "PLA":
        out.append("A = pop8(); SETNZ(A);")
    elif mnem == "PLP":
        out.append("P = pop8() | 0x20;")
    elif mnem == "CLC":
        out.append("P &= ~0x01;")
    elif mnem == "SEC":
        out.append("P |= 0x01;")
    elif mnem == "CLI":
        out.append("P &= ~0x04;")
    elif mnem == "SEI":
        out.append("P |= 0x04;")
    elif mnem == "CLV":
        out.append("P &= ~0x40;")
    elif mnem == "CLD":
        out.append("P &= ~0x08;")
    elif mnem == "SED":
        out.append("P |= 0x08;")
    elif mnem == "NOP":
        pass
    else:
        raise ValueError("unhandled mnemonic " + mnem)
    return out


def emit_block(fname, insns):
    out = ["static void %s(void) {" % fname]
    for k, ins in enumerate(insns):
        last = k == len(insns) - 1
        cyc = ins.cycles

        if ins.mnem not in Terminators:
            for line in emit_insn(ins):
                out.append("  " + line)
            out.append("  cycles += %d;" % cyc)
            if last:
                out.append("  PC = 0x%04X; return;" % ins.next)
            continue

        if ins.mnem in Branches:
            cond = BRANCH_FLAG[ins.mnem]
            out.append("  cycles += %d;" % cyc)
            out.append("  if (%s) { PC = 0x%04X; cycles++; return; }"
                       % (cond, ins.target))
            if last:
                out.append("  PC = 0x%04X; return;" % ins.next)
            continue
        if ins.mnem == "JMP":
            if ins.mode == "abs":
                out.append("  cycles += %d; PC = 0x%04X; return;"
                           % (cyc, ins.target))
            else:
                out.append("  cycles += %d; PC = jmp_ind(0x%04X); return;"
                           % (cyc, ins.operand))
            continue
        if ins.mnem == "JSR":
            out.append("  push16(0x%04X);" % (ins.next - 1))
            out.append("  cycles += %d; PC = 0x%04X; return;"
                       % (cyc, ins.target))
            continue
        if ins.mnem == "RTS":
            out.append("  cycles += %d; PC = (uint16_t)(pop16() + 1); return;"
                       % cyc)
            continue
        if ins.mnem == "RTI":
            out.append("  cycles += %d; do_rti(); return;" % cyc)
            continue
        if ins.mnem == "BRK":
            out.append("  cycles += %d; do_brk(0x%04X); return;"
                       % (cyc, ins.next))
            continue

    out.append("}")
    return out


def generate(rom, out_cpp, out_h):
    nmi, reset, irq = rom.vectors()
    # Global pointer harvest: MMC1 banks are selected at runtime, so a JSR
    # target found in the fixed bank may belong to any switchable bank.
    low_targets, high_targets = set(), set()
    for b in range(rom.prg_banks):
        base = 0xC000 if b == rom.prg_banks - 1 else 0x8000
        tmp = Analysis(rom, b, base)
        for t in tmp.seeds_from_linear_sweep():
            (high_targets if t >= 0xC000 else low_targets).add(t)
    low_targets.update(range(0x8000, 0xC000))
    low_targets = {t for t in low_targets if 0x8000 <= t < 0xC000}
    high_targets.update((nmi, reset, irq))

    analyses = []
    for b in range(rom.prg_banks):
        base = 0xC000 if b == rom.prg_banks - 1 else 0x8000
        an = Analysis(rom, b, base)
        seeds = set(high_targets if b == rom.prg_banks - 1 else low_targets)
        seeds.add(an.base)
        an.run(seeds)
        an.build_blocks()
        analyses.append(an)

    total_ins = 0
    lines = []
    lines.append("// generated by tools/recomp.py - static 6502 translation")
    lines.append('#include "nes.h"')
    lines.append("")
    for an in analyses:
        for start in sorted(an.blocks):
            insns = an.blocks[start]
            total_ins += len(insns)
            lines.extend(emit_block("B%d_%04X" % (an.bank, start), insns))
            lines.append("")

    lines.append("const BlockEntry kEntries[] = {")
    for an in analyses:
        for start in sorted(an.blocks):
            lines.append("  {%d, 0x%04X, B%d_%04X}," %
                         (an.bank, start, an.bank, start))
    lines.append("};")
    lines.append("const unsigned kEntryCount = sizeof(kEntries)/sizeof(kEntries[0]);")
    lines.append("const uint16_t kResetVector = 0x%04X;" % reset)
    lines.append("const uint16_t kNmiVector = 0x%04X;" % nmi)
    lines.append("const uint16_t kIrqVector = 0x%04X;" % irq)
    lines.append("")

    os.makedirs(os.path.dirname(out_cpp), exist_ok=True)
    with open(out_cpp, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    # report
    total_blocks = sum(len(a.blocks) for a in analyses)
    print("banks=%d blocks=%d instructions=%d" %
          (len(analyses), total_blocks, total_ins))
    for an in analyses:
        n_ins = sum(len(v) for v in an.blocks.values())
        print("  bank %2d: %3d bytes decoded, %4d blocks, %5d insns, %d unresolved"
              % (an.bank, len(an.starts), len(an.blocks), n_ins,
                 len(an.unresolved)))

    with open(out_h, "w", encoding="utf-8") as f:
        f.write("#pragma once\n#include <stdint.h>\n")
        f.write("struct BlockEntry { unsigned char bank; uint16_t addr; void (*fn)(void); };\n")
        f.write("extern const BlockEntry kEntries[];\n")
        f.write("extern const unsigned kEntryCount;\n")
        f.write("extern const uint16_t kResetVector, kNmiVector, kIrqVector;\n")
    return analyses


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    rom = Rom(os.path.join(root, "Final Fantasy (USA).nes"))
    print("mapper=%d prg=%dKB chr=%dKB ram=%d" %
          (rom.mapper, rom.prg_banks * 16, len(rom.chr) // 1024,
           rom.prg_ram_size))
    print("vectors nmi=%04X reset=%04X irq=%04X" % rom.vectors())
    generate(rom,
             os.path.join(root, "out", "gen.cpp"),
             os.path.join(root, "out", "gen.h"))
