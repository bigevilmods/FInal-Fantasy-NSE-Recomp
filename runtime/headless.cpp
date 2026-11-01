// Headless runner: executes N frames, dumps screenshots, reports faults.
// Usage: ff1_headless.exe [frames] [shot_interval]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nes.h"
#include "hd.h"
#include <windows.h>

int cpu_fault = 0;

static void load_rom(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "ROM not found: %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = (unsigned char *)malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) exit(1);
    fclose(f);
    if (n < 16 || memcmp(d, "NES\x1a", 4) != 0) { fprintf(stderr, "bad file\n"); exit(1); }
    long off = 16 + ((d[6] & 4) ? 512 : 0);
    memcpy(prg, d + off, 16 * 0x4000);
    free(d);
}

static void write_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int w = hd_active() ? HD_WIDTH : 256, h = hd_active() ? HD_HEIGHT : 240;
    int hneg = -h;
    const uint32_t *frame = hd_active() ? hd_framebuffer() : ppu_fb;
    unsigned char hdr[54] = {0};
    int filesize = 54 + w * h * 4;
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesize, 4);
    int off = 54;
    memcpy(hdr + 10, &off, 4);
    int ihs = 40;
    memcpy(hdr + 14, &ihs, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &hneg, 4);
    short planes = 1, bpp = 32;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    fwrite(hdr, 1, 54, f);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t c = frame[y * w + x];
            unsigned char px[4] = { (unsigned char)(c & 0xFF), (unsigned char)((c >> 8) & 0xFF),
                                    (unsigned char)((c >> 16) & 0xFF), 0 };
            fwrite(px, 1, 4, f);
        }
    }
    fclose(f);
}

struct InputEvent { int frame; int bit; int down; };

static InputEvent events[4096];
static int event_count = 0;

static void load_input_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int frame, down;
        char btn[32];
        if (sscanf(line, "%d %31s %d", &frame, btn, &down) != 3) continue;
        int bit = -1;
        if (!strcmp(btn, "a")) bit = 0;
        else if (!strcmp(btn, "b")) bit = 1;
        else if (!strcmp(btn, "select")) bit = 2;
        else if (!strcmp(btn, "start")) bit = 3;
        else if (!strcmp(btn, "up")) bit = 4;
        else if (!strcmp(btn, "down")) bit = 5;
        else if (!strcmp(btn, "left")) bit = 6;
        else if (!strcmp(btn, "right")) bit = 7;
        if (bit < 0 || event_count >= 4096) continue;
        events[event_count].frame = frame;
        events[event_count].bit = bit;
        events[event_count].down = down;
        event_count++;
    }
    fclose(f);
}

int main(int argc, char **argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 600;
    int interval = argc > 2 ? atoi(argv[2]) : 60;
    load_rom("Final Fantasy (USA).nes");
    nes_init();
    if (getenv("FF_HD")) hd_init("assets/handdrawn/pack.ffhd");
    if (argc > 3 && strcmp(argv[3], "test") == 0) {
        ppu_selftest();
        return 0;
    }

    const char *input_path = getenv("FF_INPUT");
    load_input_script(input_path && *input_path ? input_path : "input.txt");
    trace_nt = getenv("FF_TRACE_NT") != 0;
    trace_chr = getenv("FF_TRACE_CHR") != 0;
    const char *wavp = getenv("FF_APU_WAV");
    if (wavp && *wavp) apu_wav_open(wavp);
    const char *sf = getenv("FF_SHOT_FIRST");
    const char *sl = getenv("FF_SHOT_LAST");
    int shot_first = sf ? atoi(sf) : 0;
    int shot_last = sl ? atoi(sl) : 0x7FFFFFFF;

    for (int i = 0; i < frames; i++) {
        for (int e = 0; e < event_count; e++) {
            if (events[e].frame == i) {
                if (events[e].down)
                    controller_state |= (uint8_t)(1 << events[e].bit);
                else
                    controller_state &= (uint8_t)~(1 << events[e].bit);
            }
        }
        cpu_run_frame();
        if (cpu_fault) {
            fprintf(stderr, "FAULT at frame %d PC=%04X bank=%d\n", i, PC,
                    prg_map[PC >= 0xC000 ? 1 : 0]);
            return 2;
        }
        if (i == 1 || i == 60) dump_trace();
        if ((i % 60) == 0 || i + 1 == frames) {
            int ctrl, mask, status, chr_nz, vram_nz, pal_nz, oam_nz, scan, dt;
            ppu_snapshot(&ctrl, &mask, &status, &chr_nz, &vram_nz, &pal_nz,
                         &oam_nz, &scan, &dt);
            printf("f=%4d PC=%04X bk=%02d A=%02X X=%02X Y=%02X S=%02X P=%02X "
                   "ctl=%02X msk=%02X st=%02X chr=%5d vram=%4d pal=%2d oam=%3d "
                   "sl=%3d dot=%3d blocks=%llu nmi=%llu vb=%llu\n",
                   i, PC, prg_map[PC >= 0xC000 ? 1 : 0], A, X, Y, S, P,
                   ctrl, mask, status, chr_nz, vram_nz, pal_nz, oam_nz,
                   scan, dt, cpu_blocks, nmi_taken, vblank_events);
        }
        if (interval && (i % interval) == 0 && i >= shot_first && i <= shot_last) {
            char path[256];
            snprintf(path, sizeof(path), "shots/frame_%04d.bmp", i);
            write_bmp(path);
        }
    }
    write_bmp("shots/final.bmp");
    ppu_dump_mem();
    apu_wav_close();
    printf("ran %d frames, ok\n", frames);
    return 0;
}
