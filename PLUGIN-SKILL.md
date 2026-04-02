# SKILL: Build a PocketDAW FX Plugin

You are building an **FX (effects) plugin** for **PocketDAW** — a handheld/desktop DAW that loads plugins as compiled `.so` (Linux/Anbernic) or `.dll` (Windows) at runtime.

FX plugins chain into mixer strips. The host passes stereo audio in, you process it, pass it out.

Plugins are written in **C99**. No C++, no frameworks, no dependencies beyond `libm`.

---

## Step 1 — Understand the FX API

### Required exports (plugin won't load without these)

```c
const char* pdfx_name(void);
// Return your effect's display name. E.g. "My Reverb"

PdFxInstance pdfx_create(float sampleRate);
// Allocate and return your effect state.
// Use calloc(1, sizeof(MyFx)) — zero-initialized.

void pdfx_destroy(PdFxInstance inst);
// Free your state. free(inst);

void pdfx_process(PdFxInstance inst, PdFxAudio* audio);
// Process audio->inputL[i] → audio->outputL[i] (and R)
// audio->bufferSize = number of frames this call
```

### Optional exports — parameters

```c
int pdfx_api_version(void);
// Return 1 (v1), 2 (v2 with sidechain), or 3 (v2.1 with editor hooks)
// If not exported, host assumes v1

int pdfx_param_count(void);
const char* pdfx_param_name(int index);
float pdfx_get_param(PdFxInstance inst, int index);
void  pdfx_set_param(PdFxInstance inst, int index, float value);
// Values are 0.0–1.0 normalized

int pdfx_preset_count(void);
const char* pdfx_preset_name(int index);
void pdfx_load_preset(PdFxInstance inst, int index);
```

### Optional exports — SDK v2 (sidechain)

```c
int  pdfx_needs_sidechain(void);
// Return 1 if your effect needs a sidechain input track
// Host will show track picker in editor when user presses SELECT

void pdfx_set_sidechain(PdFxInstance inst,
                         const float* scL, const float* scR, int frames);
// Host calls this BEFORE pdfx_process each audio callback
// scL/scR are pre-FX audio from the user-selected source track
```

### Optional exports — SDK v2.1 (editor hooks)

```c
float pdfx_get_gain_reduction(PdFxInstance inst);
// Return current gain reduction 0.0 (none) → 1.0 (full)
// Host displays this as a GR meter in the full-screen editor

void pdfx_get_accent_color(PdFxInstance inst, uint8_t* r, uint8_t* g, uint8_t* b);
// Return your plugin's accent color (RGB 0–255)
// Used for meter bars and highlights in the editor UI

const char* pdfx_format_param(PdFxInstance inst, int index);
// Return a human-readable value string, e.g. "5.0ms", "4.2:1", "-12dB"
// Return NULL to use the default percentage display

const char* pdfx_param_group(int index);
// Return a group label for parameter sections, e.g. "Detector", "Output"
// Return NULL if you don't want groups
```

---

## Step 2 — PdFxAudio struct

```c
typedef struct {
    const float* inputL;   /* Input left channel  (read-only) */
    const float* inputR;   /* Input right channel (read-only) */
    float*       outputL;  /* Output left channel (write)     */
    float*       outputR;  /* Output right channel (write)    */
    int          bufferSize; /* Number of frames this callback */
} PdFxAudio;
```

You can write directly to outputL/outputR. They may point to the same memory as inputL/inputR (in-place), so read input[i] before writing output[i].

---

## Step 3 — Complete Example: Lo-Fi Crusher

A bitcrusher + sample-rate reducer. No sidechain, just pure audio mangling.

```c
#include "pocketdaw.h"
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    float sampleRate;

    /* Params (0.0–1.0 normalized) */
    float params[4]; /* bits, rate, drive, wet */

    /* Sample-rate reduction state */
    float holdL, holdR;
    float holdTimer;

    /* Param format buffer */
    char formatBuf[32];
} LofiFx;

int   pdfx_api_version(void)  { return 3; } /* v2.1 for format_param */
const char* pdfx_name(void)   { return "Lo-Fi Crusher"; }

PdFxInstance pdfx_create(float sr) {
    LofiFx* fx = calloc(1, sizeof(LofiFx));
    fx->sampleRate = sr;
    fx->params[0] = 0.8f; /* Bits     (high = less crush) */
    fx->params[1] = 0.9f; /* Rate     (high = less downsampling) */
    fx->params[2] = 0.3f; /* Drive    */
    fx->params[3] = 1.0f; /* Wet      */
    return fx;
}

void pdfx_destroy(PdFxInstance inst) { free(inst); }

void pdfx_process(PdFxInstance inst, PdFxAudio* audio) {
    LofiFx* fx = (LofiFx*)inst;

    /* Map params to real values */
    int   bits     = 2 + (int)(fx->params[0] * 14.0f); /* 2–16 bits */
    float rateDiv  = 1.0f + (1.0f - fx->params[1]) * 31.0f; /* 1–32x downsampling */
    float drive    = 1.0f + fx->params[2] * 15.0f;           /* 1–16x */
    float wet      = fx->params[3];
    float dry      = 1.0f - wet;

    float stepSize = 2.0f / (float)(1 << bits); /* quantization step */
    float holdStep = rateDiv;                    /* samples to hold   */

    for (int i = 0; i < audio->bufferSize; i++) {
        /* Drive */
        float inL = tanhf(audio->inputL[i] * drive) / tanhf(drive);
        float inR = tanhf(audio->inputR[i] * drive) / tanhf(drive);

        /* Sample-rate reduction — hold sample for holdStep frames */
        fx->holdTimer += 1.0f;
        if (fx->holdTimer >= holdStep) {
            fx->holdTimer -= holdStep;
            fx->holdL = inL;
            fx->holdR = inR;
        }

        /* Bit crush — quantize to `bits` steps */
        float crushedL = floorf(fx->holdL / stepSize + 0.5f) * stepSize;
        float crushedR = floorf(fx->holdR / stepSize + 0.5f) * stepSize;

        /* Wet/dry mix */
        audio->outputL[i] = audio->inputL[i] * dry + crushedL * wet;
        audio->outputR[i] = audio->inputR[i] * dry + crushedR * wet;
    }
}

/* ── Parameters ── */

int pdfx_param_count(void) { return 4; }

const char* pdfx_param_name(int i) {
    const char* n[] = {"Bits","Sample Rate","Drive","Wet"};
    return (i >= 0 && i < 4) ? n[i] : NULL;
}

float pdfx_get_param(PdFxInstance inst, int i) {
    return (i >= 0 && i < 4) ? ((LofiFx*)inst)->params[i] : 0.0f;
}

void pdfx_set_param(PdFxInstance inst, int i, float v) {
    if (i >= 0 && i < 4) ((LofiFx*)inst)->params[i] = v;
}

/* ── Presets ── */

int pdfx_preset_count(void) { return 3; }

const char* pdfx_preset_name(int i) {
    const char* n[] = {"Subtle","Classic Lofi","Destroy"};
    return (i >= 0 && i < 3) ? n[i] : NULL;
}

void pdfx_load_preset(PdFxInstance inst, int i) {
    LofiFx* fx = (LofiFx*)inst;
    float p[3][4] = {
        {0.95f, 0.95f, 0.1f, 0.4f}, /* Subtle      */
        {0.75f, 0.70f, 0.3f, 0.8f}, /* Classic Lofi*/
        {0.30f, 0.30f, 0.8f, 1.0f}, /* Destroy     */
    };
    if (i >= 0 && i < 3)
        for (int j = 0; j < 4; j++)
            fx->params[j] = p[i][j];
}

/* ── v2.1: Formatted params ── */

const char* pdfx_format_param(PdFxInstance inst, int i) {
    LofiFx* fx = (LofiFx*)inst;
    if (i == 0) {
        int bits = 2 + (int)(fx->params[0] * 14.0f);
        snprintf(fx->formatBuf, sizeof(fx->formatBuf), "%d bit", bits);
        return fx->formatBuf;
    }
    if (i == 1) {
        float div = 1.0f + (1.0f - fx->params[1]) * 31.0f;
        snprintf(fx->formatBuf, sizeof(fx->formatBuf), "1/%dx", (int)div);
        return fx->formatBuf;
    }
    return NULL; /* default % display for drive and wet */
}

const char* pdfx_param_group(int i) {
    if (i <= 1) return "Crush";
    return "Output";
}

void pdfx_get_accent_color(PdFxInstance inst, uint8_t* r, uint8_t* g, uint8_t* b) {
    *r = 255; *g = 160; *b = 0; /* amber */
}
```

---

## Step 4 — Complete Example: Sidechain Compressor (SDK v2)

```c
#include "pocketdaw.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

typedef struct {
    float sampleRate;
    float params[5]; /* threshold, ratio, attack, release, mix */

    /* Sidechain audio (set by host before each process call) */
    const float* scL;
    const float* scR;
    int          scFrames;

    /* Envelope follower state */
    float envelope;

    /* For GR meter */
    float currentGR;

    /* Format buffer */
    char fmtBuf[32];
} SCComp;

int   pdfx_api_version(void)  { return 3; } /* v2.1 */
const char* pdfx_name(void)   { return "SC Comp"; }
int   pdfx_needs_sidechain(void) { return 1; }

PdFxInstance pdfx_create(float sr) {
    SCComp* c = calloc(1, sizeof(SCComp));
    c->sampleRate = sr;
    c->params[0] = 0.5f; /* Threshold */
    c->params[1] = 0.6f; /* Ratio     */
    c->params[2] = 0.1f; /* Attack    */
    c->params[3] = 0.3f; /* Release   */
    c->params[4] = 1.0f; /* Mix       */
    return c;
}

void pdfx_destroy(PdFxInstance inst) { free(inst); }

void pdfx_set_sidechain(PdFxInstance inst,
                         const float* scL, const float* scR, int frames) {
    SCComp* c = (SCComp*)inst;
    c->scL = scL;
    c->scR = scR;
    c->scFrames = frames;
}

void pdfx_process(PdFxInstance inst, PdFxAudio* audio) {
    SCComp* c = (SCComp*)inst;

    /* Map params */
    float thresh  = 0.01f + c->params[0] * 0.99f;      /* 0.01–1.0 linear amp */
    float ratio   = 1.0f  + c->params[1] * 19.0f;      /* 1:1 – 20:1 */
    float attack  = expf(-1.0f / (c->sampleRate * (0.001f + c->params[2] * 0.099f)));
    float release = expf(-1.0f / (c->sampleRate * (0.01f  + c->params[3] * 0.49f)));
    float wet     = c->params[4];
    float dry     = 1.0f - wet;

    for (int i = 0; i < audio->bufferSize; i++) {
        /* Sidechain detector — use sidechain signal if available */
        float sc = 0.0f;
        if (c->scL && i < c->scFrames) {
            float l = fabsf(c->scL[i]);
            float r = (c->scR && i < c->scFrames) ? fabsf(c->scR[i]) : l;
            sc = (l > r) ? l : r;
        } else {
            /* Fall back to program material if no sidechain */
            float l = fabsf(audio->inputL[i]);
            float r = fabsf(audio->inputR[i]);
            sc = (l > r) ? l : r;
        }

        /* Envelope follower */
        float coeff = (sc > c->envelope) ? attack : release;
        c->envelope = coeff * c->envelope + (1.0f - coeff) * sc;

        /* Gain computer */
        float gain = 1.0f;
        if (c->envelope > thresh) {
            float over = c->envelope / thresh;
            /* gain reduction in linear domain */
            gain = thresh * powf(over, 1.0f / ratio - 1.0f) / c->envelope;
        }
        c->currentGR = 1.0f - gain; /* 0=no GR, 1=full GR */

        /* Apply with wet/dry */
        audio->outputL[i] = audio->inputL[i] * dry + audio->inputL[i] * gain * wet;
        audio->outputR[i] = audio->inputR[i] * dry + audio->inputR[i] * gain * wet;
    }
}

/* ── Params ── */

int pdfx_param_count(void) { return 5; }

const char* pdfx_param_name(int i) {
    const char* n[] = {"Threshold","Ratio","Attack","Release","Mix"};
    return (i >= 0 && i < 5) ? n[i] : NULL;
}

float pdfx_get_param(PdFxInstance inst, int i) {
    return (i >= 0 && i < 5) ? ((SCComp*)inst)->params[i] : 0.0f;
}

void pdfx_set_param(PdFxInstance inst, int i, float v) {
    if (i >= 0 && i < 5) ((SCComp*)inst)->params[i] = v;
}

const char* pdfx_param_group(int i) {
    if (i <= 1) return "Detector";
    if (i <= 3) return "Envelope";
    return "Output";
}

/* ── Presets ── */

int pdfx_preset_count(void) { return 4; }

const char* pdfx_preset_name(int i) {
    const char* n[] = {"Classic Pump","Gentle Duck","Hard Gate","EDM Pump"};
    return (i >= 0 && i < 4) ? n[i] : NULL;
}

void pdfx_load_preset(PdFxInstance inst, int i) {
    SCComp* c = (SCComp*)inst;
    float p[4][5] = {
        {0.5f, 0.6f, 0.05f, 0.3f, 1.0f}, /* Classic Pump */
        {0.6f, 0.3f, 0.10f, 0.5f, 0.7f}, /* Gentle Duck  */
        {0.4f, 0.9f, 0.02f, 0.1f, 1.0f}, /* Hard Gate    */
        {0.45f,0.75f,0.01f, 0.2f, 1.0f}, /* EDM Pump     */
    };
    if (i >= 0 && i < 4)
        for (int j = 0; j < 5; j++)
            c->params[j] = p[i][j];
}

/* ── v2.1 editor hooks ── */

float pdfx_get_gain_reduction(PdFxInstance inst) {
    return ((SCComp*)inst)->currentGR;
}

const char* pdfx_format_param(PdFxInstance inst, int i) {
    SCComp* c = (SCComp*)inst;
    if (i == 0) { /* Threshold in dB */
        float thresh = 0.01f + c->params[0] * 0.99f;
        float db = 20.0f * log10f(thresh < 0.00001f ? 0.00001f : thresh);
        snprintf(c->fmtBuf, sizeof(c->fmtBuf), "%.1f dB", db);
        return c->fmtBuf;
    }
    if (i == 1) { /* Ratio as X:1 */
        float ratio = 1.0f + c->params[1] * 19.0f;
        snprintf(c->fmtBuf, sizeof(c->fmtBuf), "%.1f:1", ratio);
        return c->fmtBuf;
    }
    if (i == 2 || i == 3) { /* Attack/Release in ms */
        float base = (i == 2) ? 0.001f : 0.01f;
        float range = (i == 2) ? 0.099f : 0.49f;
        float ms = (base + c->params[i] * range) * 1000.0f;
        snprintf(c->fmtBuf, sizeof(c->fmtBuf), "%.1f ms", ms);
        return c->fmtBuf;
    }
    return NULL;
}

void pdfx_get_accent_color(PdFxInstance inst, uint8_t* r, uint8_t* g, uint8_t* b) {
    *r = 0; *g = 255; *b = 65; /* PocketDAW green */
}
```

---

## Step 5 — manifest.json

```json
{
  "name": "My FX",
  "version": "1.0.0",
  "author": "Your Name",
  "description": "What your effect does.",
  "type": "fx",
  "category": "dynamics",
  "ui": {
    "layout": "sliders",
    "editor": {
      "scope": true,
      "meters": true,
      "grMeter": true,
      "particles": false
    }
  }
}
```

**`category`**: `"dynamics"` | `"filter"` | `"delay"` | `"reverb"` | `"distortion"` | `"modulation"` | `"utility"`

**`ui.editor` flags** (all optional):
- `scope` — show waveform oscilloscope in full-screen editor
- `meters` — show L/R level meters
- `grMeter` — show gain reduction graph (requires `pdfx_get_gain_reduction`)
- `particles` — audio-reactive particle system

---

## Step 6 — Compile

```bash
# Linux / Anbernic desktop (x86_64)
gcc -shared -fPIC -O2 -o my-fx.so my-fx.c -lm

# Anbernic RG35XX (aarch64 cross-compile)
aarch64-none-linux-gnu-gcc -shared -fPIC -O2 -o my-fx.so my-fx.c -lm

# Windows x64 (llvm-mingw)
x86_64-w64-mingw32-gcc -shared -o my-fx.dll my-fx.c -lm
```

---

## Step 7 — Install

```bash
# Desktop
cp my-fx.so plugins/fx/

# Anbernic device
scp my-fx.so root@192.168.1.99:/mnt/mmc/MUOS/application/PocketDAW/plugins/fx/
```

FX plugins appear in the mixer strip FX chain. Users can chain multiple FX plugins per strip.

---

## Rules for code generation

1. **Single `.c` file** — all logic in one file
2. **C99 only** — no C++
3. **Read `inputL[i]` before writing `outputL[i]`** — buffers may overlap (in-place processing)
4. **No allocations in `pdfx_process`** — allocate everything in `pdfx_create`
5. **`pdfx_set_sidechain` stores pointers only** — don't copy the buffer, just keep the pointer for use in `pdfx_process`
6. **`pdfx_format_param` must use a persistent buffer** — host may keep the pointer between calls. Store it in your struct.
7. **Parameters are 0.0–1.0** — scale to real values inside `pdfx_process`
8. **Output range**: keep output in `[-1.0, 1.0]`
9. **`pdfx_get_gain_reduction`** is called from the UI thread — use a float that's written atomically in `pdfx_process` (single float writes are safe on ARM/x86)

---

## Checklist before compiling

- [ ] `pdfx_name` returns a unique string
- [ ] `pdfx_create` uses `calloc` and stores `sampleRate`
- [ ] `pdfx_process` writes to `outputL` and `outputR` for all `bufferSize` frames
- [ ] `pdfx_destroy` calls `free(inst)`
- [ ] If sidechain: `pdfx_needs_sidechain` returns `1` and `pdfx_set_sidechain` stores pointer
- [ ] `pdfx_format_param` returns pointer to a struct-owned buffer (not a stack string)
- [ ] `manifest.json` present with `"type": "fx"`
- [ ] Compiles: `gcc -shared -fPIC -O2 -o plugin.so plugin.c -lm`

---

## Resources

- **Full FX API reference**: http://docs.pocketdaw.net/#/sdk/fx-api
- **Synth plugin skill**: http://docs.pocketdaw.net/#/sdk/synth-skill
- **Custom visuals**: http://docs.pocketdaw.net/#/sdk/visualizers
- **Build guide**: http://docs.pocketdaw.net/#/sdk/building
- **SDK repo + examples**: https://github.com/poealone/pocketdaw-sdk
- **Community**: https://community.pocketdaw.net (share in Plugins & Presets)
