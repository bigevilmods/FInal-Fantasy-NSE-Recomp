// NES APU (2A03): pulse x2, triangle, noise, frame sequencer and mixer.
// Samples go to a Win32 waveOut ring buffer (interactive build) and can also
// be dumped to a RIFF/WAVE file for headless verification.
#include "nes.h"
#include "state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif

static const int CPU_HZ = 1789773;
static const int SAMPLE_HZ = 44100;
// The recompiler counts one PPU dot per "cycle" (3 dots = 1 CPU cycle), and
// cpu_run_frame feeds those units to apu_step().  The APU is clocked by the
// CPU, so every timer below is scaled to dots.
static const int DOTS_PER_CPU = 3;
static const int DOT_HZ = CPU_HZ * DOTS_PER_CPU;

// ---- static tables -------------------------------------------------------
static const uint8_t kLenTable[32] = {
    10, 254, 20,  2, 40,  4, 80,  6, 160,  8, 60, 10, 14, 12, 26, 14,
    12,  16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};
static const uint8_t kDuty[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0},
    {1, 0, 0, 1, 1, 1, 1, 1},
};
static const int kNoisePeriod[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068,
};
static uint8_t kTriSeq[32];

// ---- channel state -------------------------------------------------------
struct Pulse {
    int enabled, duty, step;
    int period, timer;
    int length, halt;
    int const_vol, env_period, env_div, env_decay, env_start;
    int sw_enable, sw_period, sw_shift, sw_negate, sw_div, sw_reload;
};

struct Triangle {
    int enabled, control, linear_period, linear, linear_reload;
    int period, timer, length, step;
};

struct Noise {
    int enabled, mode, period_idx;
    int timer, length, halt;
    int const_vol, env_period, env_div, env_decay, env_start;
    unsigned lfsr;
};

static Pulse p1, p2;
static Triangle tri;
static Noise noi;

// frame sequencer
static int frame_mode, frame_cycle, frame_idx;
static int cpu_phase, pulse_phase;

// sample generation
static double sample_acc;

// ---- envelope ------------------------------------------------------------
static void env_clock(int *div, int *decay, int *start, int period, int loop) {
    if (*start) {
        *start = 0;
        *decay = 15;
        *div = period;
    } else if (*div == 0) {
        *div = period;
        if (*decay > 0) (*decay)--;
        else if (loop) *decay = 15;
    } else {
        (*div)--;
    }
}

static void pulse_env_clock(Pulse *p) {
    env_clock(&p->env_div, &p->env_decay, &p->env_start, p->env_period, p->halt);
}

static void noise_env_clock(Noise *n) {
    env_clock(&n->env_div, &n->env_decay, &n->env_start, n->env_period, n->halt);
}

// ---- sweep ---------------------------------------------------------------
static int pulse_target(const Pulse *p, int ones_complement) {
    int delta = p->period >> p->sw_shift;
    if (p->sw_negate) delta = ones_complement ? -delta - 1 : -delta;
    int t = p->period + delta;
    return t < 0 ? 0 : t;
}

static void sweep_clock(Pulse *p, int ones_complement) {
    int target = pulse_target(p, ones_complement);
    if (p->sw_div == 0 && p->sw_enable && p->sw_shift > 0 &&
        p->period >= 8 && target <= 0x7FF) {
        p->period = target;
    }
    if (p->sw_div == 0 || p->sw_reload) {
        p->sw_div = p->sw_period;
        p->sw_reload = 0;
    } else {
        p->sw_div--;
    }
}

// ---- length counters -----------------------------------------------------
static void len_clock(int *len, int halt) {
    if (!halt && *len > 0) (*len)--;
}

// ---- frame sequencer -----------------------------------------------------
static void quarter_clock(void) {
    pulse_env_clock(&p1);
    pulse_env_clock(&p2);
    noise_env_clock(&noi);
    if (tri.linear_reload) tri.linear = tri.linear_period;
    else if (tri.linear > 0) tri.linear--;
    if (!tri.control) tri.linear_reload = 0;
}

static void half_clock(void) {
    len_clock(&p1.length, p1.halt);
    len_clock(&p2.length, p2.halt);
    len_clock(&tri.length, 0);
    len_clock(&noi.length, noi.halt);
    sweep_clock(&p1, 1);
    sweep_clock(&p2, 0);
}

static void frame_tick(void) {
    // Quarter/half frame boundaries are 7457/14913/22371/29829/37281 CPU
    // cycles; expressed in dots.
    static const int seq[6] = {
        7457 * DOTS_PER_CPU, 14913 * DOTS_PER_CPU, 22371 * DOTS_PER_CPU,
        29829 * DOTS_PER_CPU, 37281 * DOTS_PER_CPU, 37282 * DOTS_PER_CPU,
    };
    int n = frame_mode ? 6 : 5;
    frame_cycle++;
    if (frame_idx >= n || frame_cycle < seq[frame_idx]) return;
    int i = frame_idx++;
    if (!frame_mode) {
        if (i == 4) { frame_cycle = 0; frame_idx = 0; }
        else if (i == 0 || i == 2) quarter_clock();
        else half_clock();
    } else {
        if (i == 5) { frame_cycle = 0; frame_idx = 0; }
        else if (i == 0 || i == 2) quarter_clock();
        else if (i == 1 || i == 4) half_clock();
    }
}

// ---- per-cycle timers ----------------------------------------------------
static void pulse_timer_clock(Pulse *p) {
    if (p->timer == 0) {
        p->timer = p->period;
        p->step = (p->step + 1) & 7;
    } else {
        p->timer--;
    }
}

static void tri_timer_clock(void) {
    if (tri.timer == 0) {
        tri.timer = tri.period;
        tri.step = (tri.step + 1) & 31;
    } else {
        tri.timer--;
    }
}

static void noise_timer_clock(void) {
    if (noi.timer == 0) {
        noi.timer = kNoisePeriod[noi.period_idx] - 1;
        unsigned fb;
        if (noi.mode) fb = (noi.lfsr & 1) ^ ((noi.lfsr >> 6) & 1);
        else fb = (noi.lfsr & 1) ^ ((noi.lfsr >> 1) & 1);
        noi.lfsr >>= 1;
        if (fb) noi.lfsr |= 0x4000;
    } else {
        noi.timer--;
    }
}

// ---- channel outputs -----------------------------------------------------
static int pulse_volume(const Pulse *p) {
    return p->const_vol ? p->env_period : p->env_decay;
}

static int pulse_output(const Pulse *p, int ones_complement) {
    if (!p->enabled || p->length == 0) return 0;
    if (p->period < 8) return 0;
    if (p->sw_enable && p->sw_shift > 0 && pulse_target(p, ones_complement) > 0x7FF)
        return 0;
    if (!kDuty[p->duty][p->step]) return 0;
    return pulse_volume(p);
}

static int tri_output(void) {
    if (!tri.enabled || tri.length == 0 || tri.linear == 0) return 0;
    if (tri.period < 2) return 0;
    return kTriSeq[tri.step];
}

static int noise_output(void) {
    if (!noi.enabled || noi.length == 0) return 0;
    if (noi.lfsr & 1) return 0;
    return noi.const_vol ? noi.env_period : noi.env_decay;
}

static double mix_value(void) {
    int p = pulse_output(&p1, 1) + pulse_output(&p2, 0);
    int t = tri_output();
    int n = noise_output();
    double v = 0.0;
    if (p > 0) v += 95.88 / (8128.0 / (double)p + 100.0);
    if (t + n > 0)
        v += 159.79 / (1.0 / ((double)t / 8227.0 + (double)n / 12241.0) + 100.0);
    return v * 2.0 * 32767.0;
}

// AC coupling: the NES output stage is capacitor-coupled, so remove the DC
// component introduced by the (unipolar) channel mixer.
static double hp_in, hp_out;

static int16_t next_sample(void) {
    double y = mix_value();
    double hp = y - hp_in + 0.995 * hp_out;
    hp_in = y;
    hp_out = hp;
    if (hp > 32767.0) hp = 32767.0;
    if (hp < -32768.0) hp = -32768.0;
    return (int16_t)(hp < 0 ? hp - 0.5 : hp + 0.5);
}

// ---- sample sink: waveOut ring + optional WAV dump -----------------------
static FILE *g_wav = NULL;
static long g_wav_bytes = 0;

static void put_u32le(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

void apu_wav_open(const char *path) {
    apu_wav_close();
    g_wav = fopen(path, "wb");
    if (!g_wav) return;
    unsigned char h[44];
    memset(h, 0, sizeof(h));
    memcpy(h, "RIFF", 4);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put_u32le(h + 16, 16);
    h[20] = 1;              // PCM
    h[22] = 1;              // mono
    put_u32le(h + 24, SAMPLE_HZ);
    put_u32le(h + 28, SAMPLE_HZ * 2);
    h[32] = 2;              // block align
    h[34] = 16;             // bits
    memcpy(h + 36, "data", 4);
    fwrite(h, 1, sizeof(h), g_wav);
    g_wav_bytes = 0;
}

void apu_wav_close(void) {
    if (!g_wav) return;
    unsigned char b[4];
    long data = g_wav_bytes;
    long riff = 36 + data;
    put_u32le(b, (unsigned)riff);
    fseek(g_wav, 4, SEEK_SET);
    fwrite(b, 1, 4, g_wav);
    put_u32le(b, (unsigned)data);
    fseek(g_wav, 40, SEEK_SET);
    fwrite(b, 1, 4, g_wav);
    fclose(g_wav);
    g_wav = NULL;
}

#ifdef _WIN32
#define AUDIO_HEADERS 6
#define AUDIO_FRAMES 1024

static HWAVEOUT g_hwo = NULL;
static WAVEHDR g_hdr[AUDIO_HEADERS];
static int16_t g_pcm[AUDIO_HEADERS][AUDIO_FRAMES];
static int g_cur = 0, g_fill = 0;
static int g_audio_on = 0;

static void audio_shutdown(void) {
    if (!g_audio_on) return;
    g_audio_on = 0;
    waveOutReset(g_hwo);
    for (int i = 0; i < AUDIO_HEADERS; i++)
        waveOutUnprepareHeader(g_hwo, &g_hdr[i], sizeof(WAVEHDR));
    waveOutClose(g_hwo);
    g_hwo = NULL;
}

void apu_audio_start(void) {
    if (g_audio_on) return;
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = SAMPLE_HZ;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = SAMPLE_HZ * 2;
    if (waveOutOpen(&g_hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "audio: waveOutOpen failed, running silent\n");
        return;
    }
    for (int i = 0; i < AUDIO_HEADERS; i++) {
        memset(&g_hdr[i], 0, sizeof(g_hdr[i]));
        g_hdr[i].lpData = (LPSTR)g_pcm[i];
        g_hdr[i].dwBufferLength = AUDIO_FRAMES * (DWORD)sizeof(int16_t);
        waveOutPrepareHeader(g_hwo, &g_hdr[i], sizeof(WAVEHDR));
    }
    g_cur = g_fill = 0;
    g_audio_on = 1;
    atexit(audio_shutdown);
}

void apu_audio_stop(void) { audio_shutdown(); }
#else
void apu_audio_start(void) {}
void apu_audio_stop(void) {}
#endif

static int sink_active(void) {
    if (g_wav) return 1;
#ifdef _WIN32
    if (g_audio_on) return 1;
#endif
    return 0;
}

static void push_sample(int16_t s) {
    if (g_wav) {
        fwrite(&s, sizeof(s), 1, g_wav);
        g_wav_bytes += (long)sizeof(s);
    }
#ifdef _WIN32
    if (!g_audio_on) return;
    WAVEHDR *h = &g_hdr[g_cur];
    if (h->dwFlags & WHDR_INQUEUE) return;   // no free buffer slot yet: drop
    g_pcm[g_cur][g_fill++] = s;
    if (g_fill >= AUDIO_FRAMES) {
        h->dwBufferLength = AUDIO_FRAMES * (DWORD)sizeof(int16_t);
        waveOutWrite(g_hwo, h, sizeof(WAVEHDR));
        g_cur = (g_cur + 1) % AUDIO_HEADERS;
        g_fill = 0;
    }
#endif
}

// ---- public API ----------------------------------------------------------
void apu_reset(void) {
    memset(&p1, 0, sizeof(p1));
    memset(&p2, 0, sizeof(p2));
    memset(&tri, 0, sizeof(tri));
    memset(&noi, 0, sizeof(noi));
    noi.lfsr = 1;
    frame_mode = 0;
    frame_cycle = 0;
    frame_idx = 0;
    cpu_phase = 0;
    pulse_phase = 0;
    sample_acc = 0.0;
    hp_in = hp_out = 0.0;
    for (int i = 0; i < 16; i++) kTriSeq[i] = (uint8_t)(15 - i);
    for (int i = 0; i < 16; i++) kTriSeq[16 + i] = (uint8_t)i;
}

static void write_pulse(Pulse *p, uint16_t reg, uint8_t v, int ones_complement) {
    (void)ones_complement;
    switch (reg & 3) {
    case 0:
        p->duty = (v >> 6) & 3;
        p->halt = (v >> 5) & 1;
        p->const_vol = (v >> 4) & 1;
        p->env_period = v & 0x0F;
        break;
    case 1:
        p->sw_enable = (v >> 7) & 1;
        p->sw_period = (v >> 4) & 7;
        p->sw_negate = (v >> 3) & 1;
        p->sw_shift = v & 7;
        p->sw_reload = 1;
        break;
    case 2:
        p->period = (p->period & 0x700) | v;
        break;
    case 3:
        p->period = (p->period & 0x0FF) | ((v & 7) << 8);
        if (p->enabled) p->length = kLenTable[v >> 3];
        p->env_start = 1;
        p->step = 0;
        p->timer = p->period;
        break;
    }
}

void apu_write(uint16_t a, uint8_t v) {
    uint16_t reg = (uint16_t)(a & 0x1F);
    static int trace = -1;
    if (trace < 0) trace = getenv("FF_APU_TRACE") ? 1 : 0;
    if (trace) fprintf(stderr, "APUW %d %04X %02X\n", nes_frame, a, v);
    switch (reg) {
    case 0x00: case 0x01: case 0x02: case 0x03:
        write_pulse(&p1, reg, v, 1);
        break;
    case 0x04: case 0x05: case 0x06: case 0x07:
        write_pulse(&p2, reg, v, 0);
        break;
    case 0x08:
        tri.control = (v >> 7) & 1;
        tri.linear_period = v & 0x7F;
        break;
    case 0x09:
        break;                       // unused
    case 0x0A:
        tri.period = (tri.period & 0x700) | v;
        break;
    case 0x0B:
        tri.period = (tri.period & 0x0FF) | ((v & 7) << 8);
        if (tri.enabled) tri.length = kLenTable[v >> 3];
        tri.linear_reload = 1;
        tri.timer = tri.period;
        break;
    case 0x0C:
        noi.halt = (v >> 5) & 1;
        noi.const_vol = (v >> 4) & 1;
        noi.env_period = v & 0x0F;
        break;
    case 0x0D:
        break;                       // unused
    case 0x0E:
        noi.mode = (v >> 7) & 1;
        noi.period_idx = v & 0x0F;
        break;
    case 0x0F:
        if (noi.enabled) noi.length = kLenTable[v >> 3];
        noi.env_start = 1;
        break;
    case 0x10:                       // DMC IRQ / loop (DMC output not emulated)
    case 0x11:
    case 0x12:
    case 0x13:
        break;
    case 0x15:
        p1.enabled = (v >> 0) & 1;
        p2.enabled = (v >> 1) & 1;
        tri.enabled = (v >> 2) & 1;
        noi.enabled = (v >> 3) & 1;
        if (!p1.enabled) p1.length = 0;
        if (!p2.enabled) p2.length = 0;
        if (!tri.enabled) tri.length = 0;
        if (!noi.enabled) noi.length = 0;
        break;
    case 0x17:
        frame_mode = (v >> 7) & 1;
        frame_cycle = 0;
        frame_idx = 0;
        cpu_phase = 0;
        pulse_phase = 0;
        if (frame_mode) {            // immediate clock in 5-step mode
            quarter_clock();
            half_clock();
        }
        break;
    default:
        break;
    }
}

uint8_t apu_read(uint16_t a) {
    uint16_t reg = (uint16_t)(a & 0x1F);
    if (reg != 0x15) return 0;
    uint8_t v = 0;
    if (p1.length > 0) v |= 0x01;
    if (p2.length > 0) v |= 0x02;
    if (tri.length > 0) v |= 0x04;
    if (noi.length > 0) v |= 0x08;
    return v;
}

void apu_step(int cpu_cycles) {
    // cpu_cycles is really a PPU dot count (see DOTS_PER_CPU above).
    for (int i = 0; i < cpu_cycles; i++) {
        frame_tick();                       // CPU rate, thresholds in dots
        if (++cpu_phase >= DOTS_PER_CPU) {
            cpu_phase = 0;
            tri_timer_clock();              // CPU rate
            noise_timer_clock();
        }
        if (++pulse_phase >= 2 * DOTS_PER_CPU) {
            pulse_phase = 0;
            pulse_timer_clock(&p1);         // CPU/2 rate
            pulse_timer_clock(&p2);
        }
        sample_acc += (double)SAMPLE_HZ;
        if (sample_acc >= (double)DOT_HZ) {
            sample_acc -= (double)DOT_HZ;
            if (sink_active()) push_sample(next_sample());
        }
    }
}

void apu_state(StateArchive &a) {
    a.value(p1); a.value(p2); a.value(tri); a.value(noi);
    a.value(frame_mode); a.value(frame_cycle); a.value(frame_idx);
    a.value(cpu_phase); a.value(pulse_phase); a.value(sample_acc);
    a.value(hp_in); a.value(hp_out); a.bytes(kTriSeq,sizeof(kTriSeq));
    // Device handles, queued Windows audio and output FILEs are host resources,
    // not machine state. The frontend flushes the device around a restore.
}
