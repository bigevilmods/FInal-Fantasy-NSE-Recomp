// Core NES runtime shared by the statically recompiled code.
#pragma once
#include <stdint.h>

// ---- CPU registers / state ------------------------------------------------
extern uint8_t A, X, Y, S, P;
extern uint16_t PC;
extern int cycles;

// ---- memories -------------------------------------------------------------
extern uint8_t ram[0x800];
extern uint8_t sram[0x2000];
extern uint8_t prg[16][0x4000];
extern uint8_t prg_map[2];   // bank mapped at $8000 and $C000
extern uint8_t open_bus;

// ---- PPU / APU / input ----------------------------------------------------
extern int frame_done;
extern int nmi_pending;
extern int nes_frame;

uint8_t ppu_read(uint16_t reg);
void ppu_write(uint16_t reg, uint8_t v);
void ppu_step(int cpu_cycles);
void ppu_reset(void);
void ppu_set_mirror(int mode);
void oam_write_dma(uint8_t v);
extern uint32_t ppu_fb[256 * 240];
extern uint8_t ppu_ctrl, ppu_mask, ppu_status;
void ppu_dump_mem(void);
void ppu_selftest(void);
void ppu_snapshot(int *ctrl, int *mask, int *status, int *chr_nz,
                  int *vram_nz, int *pal_nz, int *oam_nz, int *scan, int *dot);
extern unsigned long long cpu_blocks;
extern uint16_t cur_pc;
extern uint8_t cur_bank;
extern int trace_nt;
extern int trace_chr;
extern unsigned long long nmi_taken;
extern unsigned long long vblank_events;
extern int cpu_fault;

void apu_write(uint16_t reg, uint8_t v);
uint8_t apu_read(uint16_t reg);
void apu_step(int cpu_cycles);
void apu_reset(void);
void apu_audio_start(void);
void apu_audio_stop(void);
void apu_wav_open(const char *path);
void apu_wav_close(void);

extern uint8_t controller_state;   // bit0 A, 1 B, 2 Select, 3 Start, 4 Up, 5 Down, 6 Left, 7 Right

// ---- memory access --------------------------------------------------------
static inline uint8_t prg_read(uint16_t a) {
    if (a < 0xC000) return prg[prg_map[0]][a - 0x8000];
    return prg[prg_map[1]][a - 0xC000];
}

uint8_t rd8(uint16_t a);
void wr8(uint16_t a, uint8_t v);

// ---- flag helpers ---------------------------------------------------------
#define FN 0x80
#define FV 0x40
#define FU 0x20
#define FB 0x10
#define FD 0x08
#define FI 0x04
#define FZ 0x02
#define FC 0x01

static inline void SETNZ(uint8_t v) {
    P = (uint8_t)((P & 0x7D) | (v ? 0 : FZ) | (v & FN));
}

static inline void adc8(uint8_t v) {
    unsigned s = (unsigned)A + v + (P & 1);
    unsigned ov = (~(A ^ v) & (A ^ s) & 0x80) ? FV : 0;
    P = (uint8_t)((P & ~(FC | FV)) | (s > 0xFF ? FC : 0) | ov);
    A = (uint8_t)s;
    SETNZ(A);
}

static inline void sbc8(uint8_t v) { adc8((uint8_t)(v ^ 0xFF)); }

static inline void cmp8(uint8_t r, uint8_t v) {
    unsigned d = (unsigned)r - v;
    P = (uint8_t)((P & ~FC) | (r >= v ? FC : 0));
    SETNZ((uint8_t)d);
}

static inline uint8_t asl8(uint8_t v) {
    P = (uint8_t)((P & ~FC) | (v >> 7));
    v = (uint8_t)(v << 1);
    SETNZ(v);
    return v;
}
static inline uint8_t lsr8(uint8_t v) {
    P = (uint8_t)((P & ~FC) | (v & 1));
    v = (uint8_t)(v >> 1);
    SETNZ(v);
    return v;
}
static inline uint8_t rol8(uint8_t v) {
    uint8_t c = (uint8_t)(P & FC);
    P = (uint8_t)((P & ~FC) | (v >> 7));
    v = (uint8_t)((v << 1) | c);
    SETNZ(v);
    return v;
}
static inline uint8_t ror8(uint8_t v) {
    uint8_t c = (uint8_t)(P & FC);
    P = (uint8_t)((P & ~FC) | (v & 1));
    v = (uint8_t)((v >> 1) | (c << 7));
    SETNZ(v);
    return v;
}

static inline void push8(uint8_t v) { ram[0x100 + S] = v; S--; }
static inline uint8_t pop8(void) { S++; return ram[0x100 + S]; }
static inline void push16(uint16_t v) { push8((uint8_t)(v >> 8)); push8((uint8_t)v); }
static inline uint16_t pop16(void) { uint16_t lo = pop8(); return (uint16_t)(lo | (pop8() << 8)); }

static inline uint16_t zp_ind_x(uint8_t z) {
    uint8_t p = (uint8_t)(z + X);
    return (uint16_t)(ram[p] | (ram[(uint8_t)(p + 1)] << 8));
}
static inline uint16_t zp_ind_y(uint8_t z) {
    uint16_t b = (uint16_t)(ram[z] | (ram[(uint8_t)(z + 1)] << 8));
    return (uint16_t)(b + Y);
}
static inline uint16_t jmp_ind(uint16_t a) {
    uint16_t lo = rd8(a);
    uint16_t hi = rd8((uint16_t)((a & 0xFF00) | ((a + 1) & 0xFF)));
    return (uint16_t)(lo | (hi << 8));
}

void do_rti(void);
void do_brk(uint16_t next);
void illegal_op(uint16_t addr);
void dump_trace(void);
void mmc1_write(uint16_t addr, uint8_t v);

// ---- generated program ----------------------------------------------------
struct BlockEntry { unsigned char bank; uint16_t addr; void (*fn)(void); };
extern const BlockEntry kEntries[];
extern const unsigned kEntryCount;
extern const uint16_t kResetVector, kNmiVector, kIrqVector;

void cpu_run_frame(void);
void nes_init(void);
