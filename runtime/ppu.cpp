#include "nes.h"
#include "hd.h"
#include "state.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
extern int trace_nt;
extern uint16_t cur_pc;
extern uint8_t cur_bank;

uint8_t ppu_ctrl = 0, ppu_mask = 0, ppu_status = 0, oam_addr = 0;
static uint8_t read_buf = 0;
static uint16_t v = 0, t = 0;
static uint8_t fine_x = 0, w = 0;

static uint8_t vram[0x800];       // 2 KiB nametable RAM
static uint8_t pal[32];
static uint8_t oam[256];
static uint8_t chr_ram[0x2000];

uint32_t ppu_fb[256 * 240];

static int dot = 0, scanline = 0;

static uint8_t mirror_mode = 3;   // 0=one-screen lo, 1=hi, 2=vertical, 3=horizontal

void ppu_set_mirror(int mode) { mirror_mode = (uint8_t)(mode & 3); }

int trace_nt = 0;
int trace_chr = 0;

// NTSC palette, identical to the jsnes reference (PaletteTable.loadNTSCPalette)
// with its 0xBBGGRR words converted to the 0x00RRGGBB our framebuffer uses.
static uint32_t nes_pal[64] = {
    0x525252, 0x0000B4, 0x0000A0, 0x3D00B1, 0x690074, 0x5B0000, 0x5F0000, 0x401800,
    0x102F00, 0x084A08, 0x006700, 0x004212, 0x00286D, 0x000000, 0x000000, 0x000000,
    0xE7D5C4, 0x0040FF, 0x220EDC, 0x6B47FF, 0x9F00D7, 0xD70A68, 0xBC1900, 0xB15400,
    0x5B6A00, 0x038C00, 0x00AB00, 0x00882C, 0x0072A4, 0x000000, 0x000000, 0x000000,
    0xF8F8F8, 0x3CABFF, 0x8179FF, 0xC55BFF, 0xF248FF, 0xFF49DF, 0xFF6D47, 0xF7B400,
    0xFFE000, 0x75E300, 0x2BF403, 0x2EB878, 0x18E2E5, 0x787878, 0x000000, 0x000000,
    0xFFFFFF, 0xBEF2FF, 0xB8B8F8, 0xD8B8F8, 0xFFB6FF, 0xFFC3FF, 0xFFD1C7, 0xFFDA9A,
    0xF8ED88, 0xDDFF83, 0xB8F8B8, 0xACF8F5, 0xB0FFFF, 0xF8D8F8, 0x000000, 0x000000,
};

static uint16_t nt_phys(int nt) {
    switch (mirror_mode) {
        case 0: return 0;
        case 1: return 1;
        case 2: return (uint16_t)(nt & 1);
        default: return (uint16_t)((nt >> 1) & 1);
    }
}

static uint8_t vram_read(uint16_t a) {
    a &= 0x3FFF;
    if (a < 0x2000) return chr_ram[a];
    if (a < 0x3F00) {
        int nt = (a >> 10) & 3;
        return vram[nt_phys(nt) * 0x400 + (a & 0x3FF)];
    }
    a = (uint16_t)((a - 0x3F00) & 0x1F);
    if ((a & 0x13) == 0x10) a &= 0x0F;
    return pal[a];
}

static void vram_write(uint16_t a, uint8_t val) {
    a &= 0x3FFF;
    if (a < 0x2000) { chr_ram[a] = val; return; }
    if (a < 0x3F00) {
        int nt = (a >> 10) & 3;
        vram[nt_phys(nt) * 0x400 + (a & 0x3FF)] = val;
        return;
    }
    a = (uint16_t)((a - 0x3F00) & 0x1F);
    if ((a & 0x13) == 0x10) a &= 0x0F;
    pal[a] = val & 0x3F;
    // Backdrop entries of the sprite palettes mirror the background ones.
    if (a < 0x10 && (a & 3) == 0) pal[a + 0x10] = val & 0x3F;
}

void oam_write_dma(uint8_t val) {
    oam[oam_addr] = val;
    oam_addr++;
}

uint8_t ppu_read(uint16_t reg) {
    switch (reg & 7) {
        case 2: {
            uint8_t r = ppu_status;
            ppu_status &= (uint8_t)~0x80;
            w = 0;
            return r;
        }
        case 4:
            return oam[oam_addr];
        case 7: {
            uint8_t r;
            if ((v & 0x3FFF) >= 0x3F00) {
                r = vram_read(v);
                read_buf = vram_read((uint16_t)(v - 0x1000));
            } else {
                r = read_buf;
                read_buf = vram_read(v);
            }
            v = (uint16_t)(v + ((ppu_ctrl & 0x04) ? 32 : 1));
            return r;
        }
        default:
            return 0;
    }
}

void ppu_write(uint16_t reg, uint8_t val) {
    static int trace = -1;
    if (trace < 0) trace = getenv("FF_PPU_TRACE") ? 1 : 0;
    if (trace) {
        int r = reg & 7;
        if (r == 0 || r == 1 || r == 5 || r == 6)
            fprintf(stderr, "PPUW %d sl=%3d dot=%3d r=%d v=%02X m=%02X\n",
                    nes_frame, scanline, dot, r, val, ppu_mask);
    }
    switch (reg & 7) {
        case 0:
            ppu_ctrl = val;
            t = (uint16_t)((t & 0xF3FF) | ((val & 3) << 10));
            break;
        case 1:
            ppu_mask = val;
            break;
        case 3:
            oam_addr = val;
            break;
        case 4:
            oam[oam_addr] = val;
            oam_addr++;
            break;
        case 5:
            if (!w) {
                t = (uint16_t)((t & 0xFFE0) | (val >> 3));
                fine_x = val & 7;
                w = 1;
            } else {
                t = (uint16_t)((t & 0x8FFF) | ((val & 7) << 12));
                t = (uint16_t)((t & 0xFC1F) | ((val & 0xF8) << 2));
                v = t;
                w = 0;
            }
            break;
        case 6:
            if (!w) {
                t = (uint16_t)((t & 0x00FF) | ((val & 0x3F) << 8));
                w = 1;
            } else {
                t = (uint16_t)((t & 0xFF00) | val);
                v = t;
                w = 0;
            }
            break;
        case 7:
            if (trace_chr) {
                uint16_t ca = v & 0x3FFF;
                if (ca < 0x800)
                    fprintf(stderr, "CHRW %d:%04X v=%04X val=%02X A=%02X X=%02X Y=%02X ptr=%04X\n",
                            cur_bank, cur_pc, ca, val, A, X, Y,
                            ram[0x10] | (ram[0x11] << 8));
            }
            if (trace_nt) {
                uint16_t a = v & 0x3FFF;
                if (a >= 0x2000 && a < 0x2400)
                    fprintf(stderr, "NTW %d:%04X v=%04X val=%02X A=%02X X=%02X Y=%02X\n",
                            cur_bank, cur_pc, a, val, A, X, Y);
            }
            vram_write(v, val);
            v = (uint16_t)(v + ((ppu_ctrl & 0x04) ? 32 : 1));
            break;
    }
}

static void inc_vertical(void) {
    if ((v & 0x7000) != 0x7000) {
        v = (uint16_t)(v + 0x1000);
    } else {
        v &= (uint16_t)~0x7000;
        int cy = (v >> 5) & 0x1F;
        if (cy == 29) { cy = 0; v ^= 0x0800; }
        else if (cy == 31) { cy = 0; }
        else cy++;
        v = (uint16_t)((v & ~0x03E0) | (cy << 5));
    }
}

static void render_scanline(int y) {
    uint32_t backdrop = nes_pal[pal[0] & 0x3F];
    uint8_t line[256];
    uint8_t bgidx[256];
    HdPixel hd_bg[256];
    HdPixel hd_sprite_art[256];
    uint32_t hd_original_bg[256];
    uint8_t hd_sprites[256] = {};
    memset(bgidx, 0, sizeof(bgidx));

    if (!(ppu_mask & 0x18)) {
        for (int x = 0; x < 256; x++) line[x] = 255;
    } else {
        for (int x = 0; x < 256; x++) line[x] = 255;

        if (ppu_mask & 0x08) {
            int base_nt = (v >> 10) & 3;
            int coarse_x = v & 0x1F;
            int coarse_y = (v >> 5) & 0x1F;
            int fine_y = (v >> 12) & 7;
            int pat = (ppu_ctrl & 0x10) ? 0x1000 : 0x0000;
            int fx = fine_x;

            for (int tx = 0; tx < 33; tx++) {
                int cx = coarse_x + tx;
                int nt = base_nt;
                if (cx >= 32) { cx -= 32; nt ^= 1; }
                int addr = 0x2000 + nt * 0x400 + coarse_y * 32 + cx;
                uint8_t tile = vram_read((uint16_t)addr);
                uint8_t attr = vram_read((uint16_t)(0x2000 + nt * 0x400 + 0x3C0 +
                                                    (coarse_y >> 2) * 8 + (cx >> 2)));
                int sh = ((coarse_y & 2) ? 4 : 0) + ((cx & 2) ? 2 : 0);
                int p = (attr >> sh) & 3;
                uint8_t hd_pal[4] = {pal[0], pal[p*4+1], pal[p*4+2], pal[p*4+3]};
                const uint32_t *hd_tile = hd_find(chr_ram + pat + tile * 16, hd_pal);
                int lo = chr_ram[pat + tile * 16 + fine_y];
                int hi = chr_ram[pat + tile * 16 + fine_y + 8];
                int x0 = tx * 8 - fx;
                for (int b = 0; b < 8; b++) {
                    int x = x0 + b;
                    if (x < 0 || x > 255) continue;
                    int bit = 7 - b;
                    hd_bg[x] = {hd_tile, (uint8_t)b, (uint8_t)fine_y,false,false,true};
                    int pix = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                    if (pix) {
                        line[x] = (uint8_t)(p * 4 + pix);
                        bgidx[x] = (uint8_t)pix;
                    }
                }
            }
        }

        for (int x = 0; x < 256; ++x)
            hd_original_bg[x] = line[x] == 255 ? backdrop : nes_pal[pal[line[x]] & 63];
        if (ppu_mask & 0x10) {
            int count = 0;
            for (int i = 0; i < 64; i++) {
                int sy = oam[i * 4];
                int row = y - sy - 1;
                if (row < 0 || row >= 8) continue;
                count++;
                if (count > 8) { ppu_status |= 0x20; break; }

                int tile = oam[i * 4 + 1];
                int attr = oam[i * 4 + 2];
                int sx = oam[i * 4 + 3];
                if (attr & 0x80) row = 7 - row;

                int pat = (ppu_ctrl & 0x08) ? 0x1000 : 0x0000;
                if (ppu_ctrl & 0x20) {
                    pat = (tile & 1) ? 0x1000 : 0x0000;
                    tile &= 0xFE;
                    if (row >= 8) { tile |= 1; row -= 8; }
                    if (row >= 8) continue;
                }

                int lo = chr_ram[pat + tile * 16 + row];
                int hi = chr_ram[pat + tile * 16 + row + 8];
                int sp = 16 + (attr & 3)*4;
                uint8_t sprite_palette[4] = {pal[0], pal[sp+1], pal[sp+2], pal[sp+3]};
                const uint32_t *sprite_tile = hd_find_sprite(chr_ram+pat+tile*16, sprite_palette);
                for (int b = 0; b < 8; b++) {
                    int x = sx + b;
                    if (x > 255) break;
                    int bit = (attr & 0x40) ? b : 7 - b;
                    int pix = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                    if (!pix) continue;
                    if (i == 0 && bgidx[x]) ppu_status |= 0x40;
                    if ((attr & 0x20) && line[x] != 255) continue;
                    line[x] = (uint8_t)(16 + (attr & 3) * 4 + pix);
                    hd_sprites[x] = 1;
                    hd_sprite_art[x] = {sprite_tile, (uint8_t)((attr&0x40) ? 7-b : b),
                        (uint8_t)row, (attr&0x40)!=0, (attr&0x80)!=0};
                }
            }
        }
    }

    for (int x = 0; x < 256; x++) {
        uint8_t li = line[x];
        ppu_fb[y * 256 + x] = (li == 255) ? backdrop : nes_pal[pal[li] & 0x3F];
    }
    if (!(ppu_mask & 0x18)) for (int x = 0; x < 256; ++x) hd_original_bg[x] = backdrop;
    hd_scanline(y, ppu_fb + y * 256, hd_bg, hd_sprites, hd_sprite_art, hd_original_bg);
}

void ppu_step(int cpu_cycles) {
    for (int i = 0; i < cpu_cycles; i++) {
        dot++;
        if (dot >= 341) {
            dot = 0;
            int finished = scanline;
            scanline++;
            if (finished < 240) {
                render_scanline(finished);
                if (ppu_mask & 0x18) inc_vertical();
            }
            if (scanline == 241) {
                ppu_status |= 0x80;
                vblank_events++;
                if (ppu_ctrl & 0x80) nmi_pending = 1;
                // End the host frame exactly at vblank start, like the
                // reference emulator: the NMI is then taken at the top of the
                // next frame, so input -> pad poll alignment matches.
                frame_done = 1;
            }
            if (scanline == 261) {
                ppu_status &= (uint8_t)~0xE0;
                // Vertical scroll reload happens only while rendering is on.
                if (ppu_mask & 0x18) v = t;
            }
            if (scanline >= 262) { scanline = 0; }
        }
    }
}

void ppu_reset(void) {
    hd_set_colors(nes_pal);
    memset(vram, 0, sizeof(vram));
    memset(pal, 0, sizeof(pal));
    memset(oam, 0, sizeof(oam));
    memset(chr_ram, 0, sizeof(chr_ram));
    ppu_ctrl = ppu_mask = ppu_status = 0;
    oam_addr = read_buf = 0;
    v = t = 0;
    fine_x = w = 0;
    dot = scanline = 0;
    for (int i = 0; i < 256 * 240; i++) ppu_fb[i] = 0;
}

void ppu_snapshot(int *ctrl, int *mask, int *status, int *chr_nz,
                  int *vram_nz, int *pal_nz, int *oam_nz, int *scan, int *dt) {
    int c = 0, v = 0, p = 0, o = 0;
    for (int i = 0; i < 0x2000; i++) if (chr_ram[i]) c++;
    for (int i = 0; i < 0x800; i++) if (vram[i]) v++;
    for (int i = 0; i < 32; i++) if (pal[i]) p++;
    for (int i = 0; i < 256; i++) if (oam[i]) o++;
    *ctrl = ppu_ctrl; *mask = ppu_mask; *status = ppu_status;
    *chr_nz = c; *vram_nz = v; *pal_nz = p; *oam_nz = o;
    *scan = scanline; *dt = dot;
}

void ppu_dump_mem(void) {
    FILE *f = fopen("shots/chr.bin", "wb");
    if (f) { fwrite(chr_ram, 1, 0x2000, f); fclose(f); }
    f = fopen("shots/vram.bin", "wb");
    if (f) { fwrite(vram, 1, 0x800, f); fclose(f); }
    f = fopen("shots/pal.bin", "wb");
    if (f) { fwrite(pal, 1, 32, f); fclose(f); }
    f = fopen("shots/ram.bin", "wb");
    if (f) { fwrite(ram, 1, 0x800, f); fclose(f); }
    f = fopen("shots/oam.bin", "wb");
    if (f) { fwrite(oam, 1, 256, f); fclose(f); }
    f = fopen("shots/fb.bin", "wb");
    if (f) { fwrite(ppu_fb, 4, 256 * 240, f); fclose(f); }
}

void ppu_selftest(void) {
    memset(chr_ram, 0, sizeof(chr_ram));
    memset(vram, 0, sizeof(vram));
    memset(oam, 0xFF, sizeof(oam));
    memset(pal, 0, sizeof(pal));
    chr_ram[0] = 0x80;   // leftmost pixel of the top row
    chr_ram[8] = 0x00;
    pal[0] = 0x0F;
    pal[1] = 0x30;
    v = t = 0;
    fine_x = 0;
    w = 0;
    ppu_ctrl = 0;
    ppu_mask = 0x08;
    render_scanline(0);
    printf("selftest: fb(0,0)=%06X fb(7,0)=%06X (expect 00303030 / 000F0F0F)\n",
           ppu_fb[0] & 0xFFFFFF, ppu_fb[7] & 0xFFFFFF);
}

void ppu_state(StateArchive &a) {
    a.value(ppu_ctrl); a.value(ppu_mask); a.value(ppu_status); a.value(oam_addr);
    a.value(read_buf); a.value(v); a.value(t); a.value(fine_x); a.value(w);
    a.bytes(vram,sizeof(vram)); a.bytes(pal,sizeof(pal)); a.bytes(oam,sizeof(oam));
    a.bytes(chr_ram,sizeof(chr_ram)); a.bytes(ppu_fb,sizeof(ppu_fb));
    a.value(dot); a.value(scanline); a.value(mirror_mode);
}
