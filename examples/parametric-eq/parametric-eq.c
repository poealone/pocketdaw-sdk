/**
 * Parametric EQ — PocketDAW Effect Plugin
 *
 * 4-band parametric equalizer with interactive frequency response display.
 * Band 1: Low Shelf  |  Band 2: Peaking  |  Band 3: Peaking  |  Band 4: High Shelf
 * Each band has Freq, Gain (+-12 dB), and Q controls (12 params total).
 *
 * DSP: Robert Bristow-Johnson (RBJ) Audio EQ Cookbook biquad filters.
 * Visual: Interactive frequency response curve with draggable band handles.
 *
 * Build:
 *   aarch64-none-linux-gnu-gcc -shared -fPIC -O2 -o parametric-eq.so parametric-eq.c -lm
 *   x86_64-w64-mingw32-gcc -shared -o parametric-eq.dll parametric-eq.c -lm
 */

#include "../../pocketdaw.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* ── Forward declarations for SDL2 + PdDrawContext ──────── */
/* These are resolved at runtime by the host (which links SDL2). */

typedef struct SDL_Renderer SDL_Renderer;
typedef struct { int x, y, w, h; } SDL_Rect;

extern int SDL_SetRenderDrawColor(SDL_Renderer*, uint8_t, uint8_t, uint8_t, uint8_t);
extern int SDL_RenderDrawLine(SDL_Renderer*, int, int, int, int);
extern int SDL_RenderDrawPoint(SDL_Renderer*, int, int);
extern int SDL_RenderFillRect(SDL_Renderer*, const SDL_Rect*);
extern int SDL_RenderDrawRect(SDL_Renderer*, const SDL_Rect*);

typedef struct {
    float scopeBufL[128];
    float scopeBufR[128];
    float peakL, peakR;
    int   noteOn, lastMidi, voiceCount;
    int   x, y, w, h;
    int   mouseX, mouseY, mouseDown;
} PdDrawContext;

/* ── Constants ─────────────────────────────────────────── */

#define NUM_BANDS   4
#define NUM_PARAMS  12
#define NUM_PRESETS 5

#define DB_RANGE    12.0f   /* +/- 12 dB */

enum { BAND_LOW_SHELF = 0, BAND_PEAKING = 1, BAND_HIGH_SHELF = 2 };

typedef struct {
    int   type;
    float freqMin;
    float freqMax;
} BandConfig;

static const BandConfig BAND_CFG[NUM_BANDS] = {
    { BAND_LOW_SHELF,    20.0f,  1000.0f },
    { BAND_PEAKING,     100.0f,  8000.0f },
    { BAND_PEAKING,     200.0f, 16000.0f },
    { BAND_HIGH_SHELF, 2000.0f, 20000.0f }
};

static const uint8_t BAND_COLORS[NUM_BANDS][3] = {
    { 255, 100,  80 },   /* Band 1 — warm red    */
    { 100, 220, 100 },   /* Band 2 — green       */
    {  80, 160, 255 },   /* Band 3 — blue        */
    { 255, 200,  60 },   /* Band 4 — yellow      */
};

/* ── Biquad types ──────────────────────────────────────── */

typedef struct {
    float b0, b1, b2;
    float a1, a2;
} BiquadCoeffs;

typedef struct {
    float z1, z2;
} BiquadState;

/* ── Plugin struct ─────────────────────────────────────── */

typedef struct {
    float sampleRate;
    float params[NUM_PARAMS];

    BiquadCoeffs coeffs[NUM_BANDS];
    BiquadState  stateL[NUM_BANDS];
    BiquadState  stateR[NUM_BANDS];
    int          coeffsDirty;

    /* Mouse interaction */
    int dragBand;
    int wasMouseDown;

    /* Format buffer (struct-owned per SDK rule) */
    char fmt[32];

    int currentPreset;
} ParamEQ;

/* ── Parameter mapping ─────────────────────────────────── */

static float mapFreq(float p, float minHz, float maxHz) {
    return minHz * powf(maxHz / minHz, p);
}

static float mapGain(float p) {
    return (p - 0.5f) * (DB_RANGE * 2.0f);
}

static float mapQ(float p) {
    return 0.1f * powf(100.0f, p);
}

static float mapShelfQ(float p) {
    return 0.1f + p * 1.9f;
}

static float unmapFreq(float hz, float minHz, float maxHz) {
    if (hz <= minHz) return 0.0f;
    if (hz >= maxHz) return 1.0f;
    return logf(hz / minHz) / logf(maxHz / minHz);
}

static float unmapGain(float db) {
    float p = db / (DB_RANGE * 2.0f) + 0.5f;
    if (p < 0.0f) return 0.0f;
    if (p > 1.0f) return 1.0f;
    return p;
}

/* ── Biquad coefficient calculation (RBJ Cookbook) ──────── */

static void calcBiquad(BiquadCoeffs* c, int type, float freq, float gainDb, float Q, float sr) {
    /* Near 0 dB: bypass (unity passthrough) */
    if (fabsf(gainDb) < 0.05f) {
        c->b0 = 1.0f; c->b1 = 0.0f; c->b2 = 0.0f;
        c->a1 = 0.0f; c->a2 = 0.0f;
        return;
    }

    float A     = powf(10.0f, gainDb / 40.0f);
    float w0    = 2.0f * (float)M_PI * freq / sr;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);

    float b0, b1, b2, a0, a1, a2;

    if (type == BAND_PEAKING) {
        float alpha = sinw0 / (2.0f * Q);
        b0 =  1.0f + alpha * A;
        b1 = -2.0f * cosw0;
        b2 =  1.0f - alpha * A;
        a0 =  1.0f + alpha / A;
        a1 = -2.0f * cosw0;
        a2 =  1.0f - alpha / A;
    } else {
        float sqrtA = sqrtf(A);
        float alpha = sinw0 / 2.0f * sqrtf((A + 1.0f / A) * (1.0f / Q - 1.0f) + 2.0f);

        if (type == BAND_LOW_SHELF) {
            b0 =     A * ((A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha);
            b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0);
            b2 =     A * ((A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha);
            a0 =          (A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha;
            a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0);
            a2 =          (A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha;
        } else { /* BAND_HIGH_SHELF */
            b0 =     A * ((A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha);
            b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0);
            b2 =     A * ((A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha);
            a0 =          (A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha;
            a1 =  2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0);
            a2 =          (A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha;
        }
    }

    /* Normalize */
    float inv = 1.0f / a0;
    c->b0 = b0 * inv;
    c->b1 = b1 * inv;
    c->b2 = b2 * inv;
    c->a1 = a1 * inv;
    c->a2 = a2 * inv;
}

static void recalcCoeffs(ParamEQ* eq) {
    for (int b = 0; b < NUM_BANDS; b++) {
        int pi = b * 3;
        float freq = mapFreq(eq->params[pi], BAND_CFG[b].freqMin, BAND_CFG[b].freqMax);
        float gain = mapGain(eq->params[pi + 1]);
        float q    = (BAND_CFG[b].type == BAND_PEAKING)
                     ? mapQ(eq->params[pi + 2])
                     : mapShelfQ(eq->params[pi + 2]);

        /* Clamp to safe range */
        float nyq = eq->sampleRate * 0.499f;
        if (freq > nyq) freq = nyq;
        if (freq < 10.0f) freq = 10.0f;

        calcBiquad(&eq->coeffs[b], BAND_CFG[b].type, freq, gain, q, eq->sampleRate);
    }
    eq->coeffsDirty = 0;
}

/* ── Biquad processing (Direct Form II Transposed) ─────── */

static float processBiquad(const BiquadCoeffs* c, BiquadState* s, float in) {
    float out = c->b0 * in + s->z1;
    s->z1 = c->b1 * in - c->a1 * out + s->z2;
    s->z2 = c->b2 * in - c->a2 * out;
    /* Flush denormals (ARM performance) */
    if (fabsf(s->z1) < 1e-15f) s->z1 = 0.0f;
    if (fabsf(s->z2) < 1e-15f) s->z2 = 0.0f;
    return out;
}

/* ── Frequency response evaluation for visualization ───── */

static float biquadMagSq(const BiquadCoeffs* c, float cosw, float cos2w) {
    float num = c->b0 * c->b0 + c->b1 * c->b1 + c->b2 * c->b2
              + 2.0f * (c->b0 * c->b1 + c->b1 * c->b2) * cosw
              + 2.0f * c->b0 * c->b2 * cos2w;
    float den = 1.0f + c->a1 * c->a1 + c->a2 * c->a2
              + 2.0f * (c->a1 + c->a1 * c->a2) * cosw
              + 2.0f * c->a2 * cos2w;
    if (den < 1e-10f) den = 1e-10f;
    return num / den;
}

static float combinedResponseDb(const ParamEQ* eq, float freqHz) {
    float w    = 2.0f * (float)M_PI * freqHz / eq->sampleRate;
    float cosw  = cosf(w);
    float cos2w = cosf(2.0f * w);

    float totalMagSq = 1.0f;
    for (int b = 0; b < NUM_BANDS; b++)
        totalMagSq *= biquadMagSq(&eq->coeffs[b], cosw, cos2w);

    if (totalMagSq < 1e-10f) totalMagSq = 1e-10f;
    return 10.0f * log10f(totalMagSq);
}

/* ── Coordinate mapping for visualization ──────────────── */

static int freqToX(float hz, int x, int w) {
    float norm = logf(hz / 20.0f) / logf(20000.0f / 20.0f);
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    return x + (int)(norm * (float)(w - 1));
}

static float xToFreq(int px, int x, int w) {
    float norm = (float)(px - x) / (float)(w - 1);
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    return 20.0f * powf(20000.0f / 20.0f, norm);
}

static int dbToY(float db, int y, int h) {
    float norm = (db + DB_RANGE) / (DB_RANGE * 2.0f);
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    return y + h - 1 - (int)(norm * (float)(h - 1));
}

static float yToDb(int py, int y, int h) {
    float norm = 1.0f - (float)(py - y) / (float)(h - 1);
    return norm * (DB_RANGE * 2.0f) - DB_RANGE;
}

/* ── Drawing helpers ───────────────────────────────────── */

static void drawGrid(SDL_Renderer* r, int x, int y, int w, int h) {
    /* Background */
    SDL_SetRenderDrawColor(r, 15, 15, 20, 255);
    SDL_Rect bg = { x, y, w, h };
    SDL_RenderFillRect(r, &bg);

    /* Horizontal dB lines */
    float dbLines[] = { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f };
    int i;
    for (i = 0; i < 5; i++) {
        int ly = dbToY(dbLines[i], y, h);
        if (dbLines[i] == 0.0f)
            SDL_SetRenderDrawColor(r, 60, 60, 70, 255);
        else
            SDL_SetRenderDrawColor(r, 30, 30, 40, 255);
        SDL_RenderDrawLine(r, x, ly, x + w - 1, ly);
    }

    /* Vertical frequency lines */
    float freqLines[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
    SDL_SetRenderDrawColor(r, 30, 30, 40, 255);
    for (i = 0; i < 8; i++) {
        int lx = freqToX(freqLines[i], x, w);
        SDL_RenderDrawLine(r, lx, y, lx, y + h - 1);
    }
}

static void drawCurve(SDL_Renderer* r, const ParamEQ* eq, int x, int y, int w, int h) {
    SDL_SetRenderDrawColor(r, 220, 220, 240, 255);

    int prevY = -1;
    int px;
    for (px = 0; px < w; px++) {
        float freq = xToFreq(x + px, x, w);
        float db   = combinedResponseDb(eq, freq);
        int   py   = dbToY(db, y, h);

        if (py < y) py = y;
        if (py > y + h - 1) py = y + h - 1;

        if (prevY >= 0)
            SDL_RenderDrawLine(r, x + px - 1, prevY, x + px, py);
        prevY = py;
    }

    /* Fill under curve with subtle tint */
    int zeroY = dbToY(0.0f, y, h);
    prevY = -1;
    for (px = 0; px < w; px++) {
        float freq = xToFreq(x + px, x, w);
        float db   = combinedResponseDb(eq, freq);
        int   py   = dbToY(db, y, h);
        if (py < y) py = y;
        if (py > y + h - 1) py = y + h - 1;

        if (py < zeroY) {
            /* Boost region — subtle green fill */
            SDL_SetRenderDrawColor(r, 80, 200, 120, 25);
            SDL_Rect fill = { x + px, py, 1, zeroY - py };
            SDL_RenderFillRect(r, &fill);
        } else if (py > zeroY) {
            /* Cut region — subtle red fill */
            SDL_SetRenderDrawColor(r, 200, 80, 80, 25);
            SDL_Rect fill = { x + px, zeroY, 1, py - zeroY };
            SDL_RenderFillRect(r, &fill);
        }
    }
}

static void drawHandles(SDL_Renderer* r, const ParamEQ* eq, int x, int y, int w, int h) {
    int b;
    for (b = 0; b < NUM_BANDS; b++) {
        int pi = b * 3;
        float freq = mapFreq(eq->params[pi], BAND_CFG[b].freqMin, BAND_CFG[b].freqMax);
        float gain = mapGain(eq->params[pi + 1]);

        int hx = freqToX(freq, x, w);
        int hy = dbToY(gain, y, h);

        /* Filled circle (scanline approach) */
        int radius = 5;
        int dy;
        SDL_SetRenderDrawColor(r, BAND_COLORS[b][0], BAND_COLORS[b][1], BAND_COLORS[b][2], 255);
        for (dy = -radius; dy <= radius; dy++) {
            int halfW = (int)sqrtf((float)(radius * radius - dy * dy));
            SDL_Rect line = { hx - halfW, hy + dy, halfW * 2 + 1, 1 };
            SDL_RenderFillRect(r, &line);
        }

        /* Highlight ring when dragging */
        if (eq->dragBand == b) {
            int outerR = radius + 3;
            SDL_SetRenderDrawColor(r, 255, 255, 255, 200);
            for (dy = -outerR; dy <= outerR; dy++) {
                int halfW = (int)sqrtf((float)(outerR * outerR - dy * dy));
                SDL_RenderDrawPoint(r, hx - halfW, hy + dy);
                SDL_RenderDrawPoint(r, hx + halfW, hy + dy);
            }
        }
    }
}

static void handleMouse(ParamEQ* eq, PdDrawContext* ctx) {
    int absMx = ctx->x + ctx->mouseX;
    int absMy = ctx->y + ctx->mouseY;

    /* Detect mouse-down edge */
    if (ctx->mouseDown && !eq->wasMouseDown) {
        eq->dragBand = -1;
        float bestDist = 25.0f * 25.0f;
        int b;
        for (b = 0; b < NUM_BANDS; b++) {
            int pi = b * 3;
            float freq = mapFreq(eq->params[pi], BAND_CFG[b].freqMin, BAND_CFG[b].freqMax);
            float gain = mapGain(eq->params[pi + 1]);
            int hx = freqToX(freq, ctx->x, ctx->w);
            int hy = dbToY(gain, ctx->y, ctx->h);

            float dx = (float)(absMx - hx);
            float dy = (float)(absMy - hy);
            float dist = dx * dx + dy * dy;
            if (dist < bestDist) {
                bestDist = dist;
                eq->dragBand = b;
            }
        }
    }

    /* Dragging: update freq and gain from mouse position */
    if (ctx->mouseDown && eq->dragBand >= 0) {
        int b  = eq->dragBand;
        int pi = b * 3;

        float freq = xToFreq(absMx, ctx->x, ctx->w);
        float gain = yToDb(absMy, ctx->y, ctx->h);

        /* Clamp to band range */
        if (freq < BAND_CFG[b].freqMin) freq = BAND_CFG[b].freqMin;
        if (freq > BAND_CFG[b].freqMax) freq = BAND_CFG[b].freqMax;
        if (gain < -DB_RANGE) gain = -DB_RANGE;
        if (gain >  DB_RANGE) gain =  DB_RANGE;

        eq->params[pi]     = unmapFreq(freq, BAND_CFG[b].freqMin, BAND_CFG[b].freqMax);
        eq->params[pi + 1] = unmapGain(gain);
        eq->coeffsDirty    = 1;
        eq->currentPreset  = -1;
    }

    /* Mouse-up edge */
    if (!ctx->mouseDown && eq->wasMouseDown)
        eq->dragBand = -1;

    eq->wasMouseDown = ctx->mouseDown;
}

/* ── Presets ────────────────────────────────────────────── */

static const char* preset_names[NUM_PRESETS] = {
    "Flat", "Vocal Boost", "Bass Heavy", "Smiley", "Mid Scoop"
};

static const float presets[NUM_PRESETS][NUM_PARAMS] = {
    /* Flat: all gains 0 dB */
    { 0.40f, 0.50f, 0.32f,  0.35f, 0.50f, 0.50f,  0.45f, 0.50f, 0.50f,  0.30f, 0.50f, 0.32f },
    /* Vocal Boost: mids pushed, gentle high lift */
    { 0.40f, 0.44f, 0.32f,  0.55f, 0.62f, 0.50f,  0.50f, 0.60f, 0.45f,  0.35f, 0.56f, 0.32f },
    /* Bass Heavy: big low shelf, slight high cut */
    { 0.35f, 0.67f, 0.40f,  0.30f, 0.52f, 0.50f,  0.45f, 0.48f, 0.50f,  0.40f, 0.40f, 0.32f },
    /* Smiley: boost lows + highs, cut mids */
    { 0.35f, 0.63f, 0.32f,  0.45f, 0.38f, 0.40f,  0.55f, 0.40f, 0.40f,  0.30f, 0.62f, 0.32f },
    /* Mid Scoop: deep cut 500-1k */
    { 0.40f, 0.50f, 0.32f,  0.40f, 0.30f, 0.35f,  0.35f, 0.33f, 0.35f,  0.30f, 0.50f, 0.32f },
};

/* ── Required FX API ───────────────────────────────────── */

int         pdfx_api_version(void) { return PDFX_API_VERSION; }
const char* pdfx_name(void)        { return "Param EQ"; }
int         pdfx_param_count(void) { return NUM_PARAMS; }

PdFxInstance pdfx_create(float sampleRate) {
    ParamEQ* eq = (ParamEQ*)calloc(1, sizeof(ParamEQ));
    if (!eq) return NULL;
    eq->sampleRate = sampleRate;
    eq->dragBand = -1;
    eq->currentPreset = 0;

    /* Default: Flat preset */
    int i;
    for (i = 0; i < NUM_PARAMS; i++)
        eq->params[i] = presets[0][i];
    eq->coeffsDirty = 1;
    return eq;
}

void pdfx_destroy(PdFxInstance inst) { free(inst); }

void pdfx_process(PdFxInstance inst, PdFxAudio* audio) {
    ParamEQ* eq = (ParamEQ*)inst;
    if (!eq) return;

    if (eq->coeffsDirty)
        recalcCoeffs(eq);

    int i, b;
    for (i = 0; i < audio->bufferSize; i++) {
        float L = audio->inputL[i];
        float R = audio->inputR[i];

        for (b = 0; b < NUM_BANDS; b++) {
            L = processBiquad(&eq->coeffs[b], &eq->stateL[b], L);
            R = processBiquad(&eq->coeffs[b], &eq->stateR[b], R);
        }

        /* Hard clip to [-1, 1] */
        if (L >  1.0f) L =  1.0f;
        if (L < -1.0f) L = -1.0f;
        if (R >  1.0f) R =  1.0f;
        if (R < -1.0f) R = -1.0f;

        audio->outputL[i] = L;
        audio->outputR[i] = R;
    }
}

float pdfx_get_param(PdFxInstance inst, int i) {
    ParamEQ* eq = (ParamEQ*)inst;
    if (!eq || i < 0 || i >= NUM_PARAMS) return 0.0f;
    return eq->params[i];
}

void pdfx_set_param(PdFxInstance inst, int i, float v) {
    ParamEQ* eq = (ParamEQ*)inst;
    if (!eq || i < 0 || i >= NUM_PARAMS) return;
    eq->params[i] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    eq->coeffsDirty = 1;
    eq->currentPreset = -1;
}

/* ── Optional FX API ───────────────────────────────────── */

static const char* param_names[NUM_PARAMS] = {
    "LS Freq", "LS Gain", "LS Q",
    "P1 Freq", "P1 Gain", "P1 Q",
    "P2 Freq", "P2 Gain", "P2 Q",
    "HS Freq", "HS Gain", "HS Q"
};

const char* pdfx_param_name(int i) {
    if (i >= 0 && i < NUM_PARAMS) return param_names[i];
    return NULL;
}

const char* pdfx_param_group(int i) {
    if (i < 3)  return "Low Shelf";
    if (i < 6)  return "Peak 1";
    if (i < 9)  return "Peak 2";
    if (i < 12) return "High Shelf";
    return NULL;
}

const char* pdfx_format_param(PdFxInstance inst, int i) {
    ParamEQ* eq = (ParamEQ*)inst;
    if (!eq || i < 0 || i >= NUM_PARAMS) return NULL;

    int band = i / 3;
    int sub  = i % 3;

    if (sub == 0) {
        float hz = mapFreq(eq->params[i], BAND_CFG[band].freqMin, BAND_CFG[band].freqMax);
        if (hz >= 1000.0f)
            snprintf(eq->fmt, sizeof(eq->fmt), "%.1f kHz", hz / 1000.0f);
        else
            snprintf(eq->fmt, sizeof(eq->fmt), "%.0f Hz", hz);
    } else if (sub == 1) {
        float db = mapGain(eq->params[i]);
        snprintf(eq->fmt, sizeof(eq->fmt), "%+.1f dB", db);
    } else {
        float q = (BAND_CFG[band].type == BAND_PEAKING)
                  ? mapQ(eq->params[i])
                  : mapShelfQ(eq->params[i]);
        snprintf(eq->fmt, sizeof(eq->fmt), "%.2f", q);
    }
    return eq->fmt;
}

void pdfx_get_accent_color(PdFxInstance inst, uint8_t* r, uint8_t* g, uint8_t* b) {
    (void)inst;
    *r = 80; *g = 180; *b = 255;
}

void pdfx_reset(PdFxInstance inst) {
    ParamEQ* eq = (ParamEQ*)inst;
    if (!eq) return;
    int b;
    for (b = 0; b < NUM_BANDS; b++) {
        eq->stateL[b].z1 = eq->stateL[b].z2 = 0.0f;
        eq->stateR[b].z1 = eq->stateR[b].z2 = 0.0f;
    }
}

/* ── Presets API ───────────────────────────────────────── */

int         pdfx_preset_count(void) { return NUM_PRESETS; }
const char* pdfx_preset_name(int i) {
    if (i >= 0 && i < NUM_PRESETS) return preset_names[i];
    return NULL;
}

void pdfx_load_preset(PdFxInstance inst, int i) {
    ParamEQ* eq = (ParamEQ*)inst;
    if (!eq || i < 0 || i >= NUM_PRESETS) return;
    int j;
    for (j = 0; j < NUM_PARAMS; j++)
        eq->params[j] = presets[i][j];
    eq->coeffsDirty = 1;
    eq->currentPreset = i;
}

int pdfx_get_preset(PdFxInstance inst) {
    ParamEQ* eq = (ParamEQ*)inst;
    return eq ? eq->currentPreset : -1;
}

/* ── Custom visual — interactive EQ curve ──────────────── */

int pdfx_draw(PdFxInstance inst, SDL_Renderer* r, PdDrawContext* ctx) {
    ParamEQ* eq = (ParamEQ*)inst;
    if (!eq) return 0;

    /* Ensure coefficients are current for accurate curve */
    if (eq->coeffsDirty)
        recalcCoeffs(eq);

    handleMouse(eq, ctx);

    drawGrid(r, ctx->x, ctx->y, ctx->w, ctx->h);
    drawCurve(r, eq, ctx->x, ctx->y, ctx->w, ctx->h);
    drawHandles(r, eq, ctx->x, ctx->y, ctx->w, ctx->h);

    return 1;
}
