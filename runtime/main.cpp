// Win32 front-end: window, input, frame pacing, ROM/battery loading.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nes.h"
#include "hd.h"
#include "video.h"
#include "session.h"
#include "frame_pacer.h"

int cpu_fault = 0;

static int win_w = 256 * 3, win_h = 240 * 3;
static int rom_loaded = 0;
static char sram_path[MAX_PATH] = "ff1.sav";

static void die(const char *msg) {
    MessageBoxA(NULL, msg, "Final Fantasy", MB_ICONERROR);
    exit(1);
}

static void load_rom(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) die("ROM not found: Final Fantasy (USA).nes");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = (unsigned char *)malloc(n);
    if (!d) die("out of memory");
    if (fread(d, 1, n, f) != (size_t)n) die("short read");
    fclose(f);

    if (n < 16 || memcmp(d, "NES\x1a", 4) != 0) die("not an iNES file");
    int prg_banks = d[4];
    long off = 16 + ((d[6] & 4) ? 512 : 0);
    if (prg_banks != 16) die("unexpected PRG size");
    if (off + 16 * 0x4000 > n) die("truncated PRG");
    memcpy(prg, d + off, 16 * 0x4000);
    free(d);
    rom_loaded = 1;
}

static void load_sram(void) {
    FILE *f = fopen(sram_path, "rb");
    if (!f) return;
    fread(sram, 1, sizeof(sram), f);
    fclose(f);
}

static void save_sram(void) {
    FILE *f = fopen(sram_path, "wb");
    if (!f) return;
    fwrite(sram, 1, sizeof(sram), f);
    fclose(f);
}

static void set_key(WPARAM vk, int down) {
    static const struct { int vk; int bit; } map[] = {
        {'Z', 0}, {'X', 1}, {VK_RSHIFT, 2}, {VK_RETURN, 3},
        {VK_UP, 4}, {VK_DOWN, 5}, {VK_LEFT, 6}, {VK_RIGHT, 7},
        {VK_SPACE, 0},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i].vk == (int)vk) {
            if (down) controller_state |= (uint8_t)(1 << map[i].bit);
            else controller_state &= (uint8_t)~(1 << map[i].bit);
        }
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m==WM_KEYDOWN && session_key(wp,lp)) return 0;
    if (video_message(h, m, wp, lp)) return 0;
    switch (m) {
        case WM_CLOSE:
            session_save_resume();
            save_sram();
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_F1 && !(lp & (1LL << 30))) { hd_toggle(); return 0; }
            set_key(wp, 1);
            return 0;
        case WM_KEYUP:
            set_key(wp, 0);
            return 0;
        case WM_KILLFOCUS:
            controller_state = 0;
            return 0;
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcA(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev; (void)cmd;
    SetProcessDPIAware();
    char executable[MAX_PATH];
    GetModuleFileNameA(nullptr, executable, MAX_PATH);
    char *directory_end = strrchr(executable, '\\');
    if (directory_end) { *directory_end = 0; SetCurrentDirectoryA(executable); }
    load_rom("Final Fantasy (USA).nes");
    load_sram();
    nes_init();
    hd_init("assets/handdrawn/pack.ffhd");

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "FF1Static";
    RegisterClassA(&wc);

    RECT rc = {0, 0, win_w, win_h};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("FF1Static", "Final Fantasy - Remaster RGB32 | F2 menu | F5 salvar | F9 carregar",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top,
                              NULL, NULL, inst, NULL);
    if (!hwnd) die("CreateWindow failed");
    video_init(hwnd);
    session_init();
    apu_audio_start();
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    double next_frame = 0.0;
    const double frame_sec = 1.0 / 60.0988;
    FramePacer pacer;

    long long frames = 0;
    int saved = 0;
    bool audio_paused = false;

    for (;;) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                save_sram();
                apu_audio_stop();
                video_shutdown();
                return 0;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        bool paused = video_menu_open();
        if (paused != audio_paused) {
            if (paused) apu_audio_stop(); else apu_audio_start();
            audio_paused = paused;
        }
        if (!paused) cpu_run_frame();
        if (cpu_fault) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "Recompiled code jumped to an un-translated address.\n"
                     "PC=%04X bank=%d", PC, prg_map[PC >= 0xC000 ? 1 : 0]);
            MessageBoxA(hwnd, buf, "Static recompiler fault", MB_ICONERROR);
            save_sram();
            return 2;
        }

        video_present(hwnd);
        if (!paused) frames++;
        if (!paused && (frames & 1023) == 0) { save_sram(); saved++; }

        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
        next_frame += frame_sec;
        double wait = next_frame - elapsed;
        if (wait > 0) {
            pacer.sleep_for(wait);
        } else if (wait < -0.25) {
            next_frame = elapsed;
        }
    }
}
