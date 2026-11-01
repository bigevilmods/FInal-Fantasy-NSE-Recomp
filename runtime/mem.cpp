#include "nes.h"
#include "state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t ram[0x800];
uint8_t sram[0x2000];
uint8_t prg[16][0x4000];
uint8_t prg_map[2] = {0, 15};
uint8_t open_bus = 0;

uint8_t A, X, Y, S, P;
uint16_t PC;
int cycles = 0;

int frame_done = 0;
int nmi_pending = 0;
int nes_frame = 0;

uint8_t controller_state = 0;
static uint8_t ctrl_strobe = 0;
static uint16_t ctrl_shift = 0xFF00;

static uint8_t mmc_shift = 0x10;
static uint8_t mmc_ctrl = 0x0C;
static uint8_t mmc_chr0 = 0, mmc_chr1 = 0, mmc_prg = 0;

static void mmc1_update(void) {
    int mode = (mmc_ctrl >> 2) & 3;
    ppu_set_mirror(mmc_ctrl & 3);
    unsigned p = mmc_prg & 0x0F;
    if (mode == 0 || mode == 1) {          // 32 KiB mode
        prg_map[0] = (uint8_t)(p & ~1u);
        prg_map[1] = (uint8_t)((p & ~1u) + 1);
    } else if (mode == 2) {                // first bank fixed at $8000
        prg_map[0] = 0;
        prg_map[1] = (uint8_t)p;
    } else {                               // last bank fixed at $C000
        prg_map[0] = (uint8_t)p;
        prg_map[1] = 15;
    }
}

void mmc1_write(uint16_t addr, uint8_t v) {
    if (v & 0x80) {
        mmc_shift = 0x10;
        mmc_ctrl |= 0x0C;
        mmc1_update();
        return;
    }
    int fifth = mmc_shift & 1;
    mmc_shift = (uint8_t)((mmc_shift >> 1) | ((v & 1) << 4));
    if (!fifth) return;
    uint8_t val = mmc_shift & 0x1F;
    mmc_shift = 0x10;
    switch ((addr >> 13) & 3) {
        case 0: mmc_ctrl = val; break;
        case 1: mmc_chr0 = val; break;
        case 2: mmc_chr1 = val; break;
        case 3: mmc_prg = val; break;
    }
    mmc1_update();
}

static void oam_dma(uint8_t page) {
    uint16_t base = (uint16_t)(page << 8);
    for (int i = 0; i < 256; i++)
        oam_write_dma(rd8((uint16_t)(base + i)));
    cycles += 513;
}

uint8_t rd8(uint16_t a) {
    if (a < 0x2000) return ram[a & 0x7FF];
    if (a < 0x4000) { open_bus = ppu_read(a & 7); return open_bus; }
    if (a == 0x4015) { open_bus = apu_read(0x15); return open_bus; }
    if (a == 0x4016) {
        if (trace_chr)
            fprintf(stderr, "PADRD %d:%04X A=%02X X=%02X Y=%02X\n", cur_bank, cur_pc, A, X, Y);
        uint8_t v;
        if (ctrl_strobe) {
            v = (uint8_t)(controller_state & 1);
        } else {
            v = (uint8_t)(ctrl_shift & 1);
            ctrl_shift = (uint16_t)((ctrl_shift >> 1) | 0x8000);
        }
        open_bus = v;
        return v;
    }
    if (a == 0x4017) { open_bus = (uint8_t)(0x40); return open_bus; }
    if (a < 0x4020) return open_bus;
    if (a < 0x6000) return open_bus;
    if (a < 0x8000) return sram[a - 0x6000];
    open_bus = prg_read(a);
    return open_bus;
}

void wr8(uint16_t a, uint8_t v) {
    if (a < 0x2000) { ram[a & 0x7FF] = v; return; }
    if (a < 0x4000) { ppu_write(a & 7, v); return; }
    if (a == 0x4014) { oam_dma(v); return; }
    if (a == 0x4016) {
        ctrl_strobe = (uint8_t)(v & 1);
        if (ctrl_strobe) ctrl_shift = (uint16_t)(0xFF00 | controller_state);
        return;
    }
    if (a <= 0x4017) { apu_write(a, v); return; }
    if (a < 0x4020) return;
    if (a < 0x6000) return;
    if (a < 0x8000) { sram[a - 0x6000] = v; return; }
    mmc1_write(a, v);
}

void do_rti(void) {
    P = (uint8_t)((pop8() & ~0x30) | 0x20);
    uint16_t lo = pop8();
    uint16_t hi = pop8();
    PC = (uint16_t)(lo | (hi << 8));
}

void do_brk(uint16_t next) {
    fprintf(stderr, "BRK at next=%04X\n", next);
    dump_trace();
    push16(next);
    push8((uint8_t)(P | 0x30));
    P |= FI;
    PC = kIrqVector;
}

void illegal_op(uint16_t addr) {
    fprintf(stderr, "illegal opcode reached at $%04X (bank %d)\n", addr,
            prg_map[addr >= 0xC000 ? 1 : 0]);
    fprintf(stderr, "A=%02X X=%02X Y=%02X S=%02X P=%02X\n", A, X, Y, S, P);
    exit(2);
}

// --------------------------------------------------------------------------
static void (*block_tbl[16][0x8000])(void);
unsigned long long cpu_blocks = 0;
uint16_t cur_pc = 0;
uint8_t cur_bank = 0;
unsigned long long nmi_taken = 0;
unsigned long long vblank_events = 0;

#define TRACE_N 64
static uint16_t trace_pc[TRACE_N];
static uint8_t trace_bank[TRACE_N];
static unsigned trace_i = 0;

static void trace_push(uint16_t pc, uint8_t bank) {
    extern int trace_chr;
    if (trace_chr && pc >= 0xEA28 && pc <= 0xEA80)
        fprintf(stderr, "EA pc=%04X A=%02X X=%02X Y=%02X z14=%02X%02X z6A=%02X bnk=%02X\n",
                pc, A, X, Y, ram[0x15], ram[0x14], ram[0x6A], prg_map[0]);
    if (trace_chr && pc == 0xE95A)
        fprintf(stderr, "BLIT dest=%04X pages=%d src=%04X bank=%02X\n",
                A << 8, X, ram[0x10] | (ram[0x11] << 8), prg_map[0]);
    trace_pc[trace_i] = pc;
    trace_bank[trace_i] = bank;
    trace_i = (trace_i + 1) % TRACE_N;
}

void dump_trace(void) {
    fprintf(stderr, "--- last PCs ---\n");
    for (int k = 0; k < TRACE_N; k++) {
        unsigned i = (trace_i + k) % TRACE_N;
        fprintf(stderr, "%02d:%02X:%04X  ", k, trace_bank[i], trace_pc[i]);
        if ((k % 4) == 3) fprintf(stderr, "\n");
    }
    fprintf(stderr, "\nA=%02X X=%02X Y=%02X S=%02X P=%02X prg_map=%02X/%02X\n",
            A, X, Y, S, P, prg_map[0], prg_map[1]);
}
static int missing_logged = 0;

void cpu_run_frame(void) {
    frame_done = 0;
    while (!frame_done) {
        if (cycles) { ppu_step(cycles); apu_step(cycles); cycles = 0; }
        if (frame_done) break;

        if (nmi_pending) {
            nmi_pending = 0;
            nmi_taken++;
            push16(PC);
            push8((uint8_t)((P & ~0x10) | 0x20));
            P |= FI;
            PC = kNmiVector;
        }

        int bank = (PC >= 0xC000) ? prg_map[1] : prg_map[0];
        void (*fn)(void) = block_tbl[bank][PC & 0x7FFF];
        if (!fn) {
            if (!missing_logged) {
                fprintf(stderr, "MISSING block bank=%d addr=%04X\n", bank, PC);
                dump_trace();
            }
            missing_logged = 1;
            cpu_fault = 1;
            return;
        }
        {
            static int trapped = 0;
            if (PC == 0xFEBB && !trapped) {
                trapped = 1;
                fprintf(stderr, "TRAP FEBB\n");
                dump_trace();
            }
        }
        cur_pc = PC;
        cur_bank = (uint8_t)bank;
        trace_push(PC, (uint8_t)bank);
        fn();
        cpu_blocks++;
    }
    nes_frame++;
}

void nes_init(void) {
    for (unsigned i = 0; i < kEntryCount; i++)
        block_tbl[kEntries[i].bank][kEntries[i].addr & 0x7FFF] = kEntries[i].fn;

    // NES power-on RAM is undefined; the reference (jsnes) fills it with $FF
    // and the game reads uninitialised bytes (e.g. $6A during class select).
    memset(ram, 0xFF, sizeof(ram));
    A = X = Y = 0;
    S = 0xFD;
    P = 0x24;
    PC = kResetVector;
    cycles = 0;
    mmc_ctrl = 0x0C;
    mmc_prg = 0;
    mmc_shift = 0x10;
    mmc1_update();
    ppu_reset();
    apu_reset();
}

void mem_state(StateArchive &a) {
    a.value(A); a.value(X); a.value(Y); a.value(S); a.value(P); a.value(PC); a.value(cycles);
    a.bytes(ram,sizeof(ram)); a.bytes(sram,sizeof(sram));
    a.bytes(prg_map,sizeof(prg_map)); a.value(open_bus);
    a.value(frame_done); a.value(nmi_pending); a.value(nes_frame);
    a.value(controller_state); a.value(ctrl_strobe); a.value(ctrl_shift);
    a.value(mmc_shift); a.value(mmc_ctrl); a.value(mmc_chr0); a.value(mmc_chr1); a.value(mmc_prg);
    a.value(cpu_blocks); a.value(cur_pc); a.value(cur_bank); a.value(nmi_taken); a.value(vblank_events);
    a.bytes(trace_pc,sizeof(trace_pc)); a.bytes(trace_bank,sizeof(trace_bank)); a.value(trace_i);
    a.value(missing_logged); a.value(cpu_fault);
}
