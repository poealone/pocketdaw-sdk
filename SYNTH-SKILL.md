# SKILL: Build a PocketDAW Synth Plugin

You are building a synth plugin for **PocketDAW** — a handheld/desktop DAW that loads plugins as compiled `.so` (Linux/Anbernic) or `.dll` (Windows) files at runtime.

Plugins are written in **C99**. No C++, no frameworks, no dependencies beyond `libm`.

---

## Step 1 — Understand the API

Every synth plugin implements a set of exported C functions. The host loads them via `dlsym` — any you don't export are simply skipped.

### Required exports (plugin won't load without these)

```c
const char* pdsynth_name(void);
// Return your plugin's display name. E.g. "My Synth"

PdSynthInstance pdsynth_create(float sampleRate, PdSynthHost* host);
// Allocate and return your synth state. host gives access to primitives.
// Use calloc(1, sizeof(MySynth)) — zero-initialized.

void pdsynth_destroy(PdSynthInstance inst);
// Free your state. free(inst);

void pdsynth_process(PdSynthInstance inst, PdSynthAudio* audio);
// Fill audio->outL[i] and audio->outR[i] for i in 0..audio->frames-1
```

### Common optional exports

```c
void pdsynth_note(PdSynthInstance inst, int midiNote, int velocity);
// velocity > 0 = note on, velocity == 0 = note off

int pdsynth_param_count(void);
const char* pdsynth_param_name(int index);
float pdsynth_get_param(PdSynthInstance inst, int index);
void pdsynth_set_param(PdSynthInstance inst, int index, float value);
// Parameters shown as knobs (≤12) or sliders (>12) in the editor
// Values are 0.0–1.0 normalized

int pdsynth_preset_count(void);
const char* pdsynth_preset_name(int index);
void pdsynth_load_preset(PdSynthInstance inst, int index);

void pdsynth_draw(PdSynthInstance inst, SDL_Renderer* renderer, PdDrawContext* ctx);
// Return 1 from your draw function to own the full graphics page
// ctx->scopeBufL/R, ctx->peakL/R, ctx->noteOn, ctx->voiceCount available
```

---

## Step 2 — Host Primitives

The `PdSynthHost*` pointer from `pdsynth_create` gives you DSP building blocks. Use them instead of coding everything from scratch.

```c
// Oscillator — type: OSC_SINE, OSC_SAW, OSC_SQUARE, OSC_TRI, OSC_NOISE, OSC_PULSE
float host->osc_sample(int type, float phase, float freq);

// State-variable filter — mode: FILT_LP, FILT_HP, FILT_BP, FILT_NOTCH
// state is a float[2] you keep in your struct
float host->filter_sample(float* state, float input, float cutoff, float resonance, int mode);

// ADSR envelope — gate: 1.0 = key held, 0.0 = released
// env is a float[3] you keep in your struct (value, stage, time)
float host->adsr_tick(float* env, float gate, float attack, float decay, float sustain, float release);

// Morphing wavetable — type: WT_ANALOG, WT_DIGITAL, WT_VOCAL
float host->wavetable_sample(int type, float phase, float morph);

// Host FX bus — type: FX_REVERB, FX_DELAY, FX_CHORUS, FX_DISTORT, FX_BITCRUSH
// buf is float* (in-place, stereo interleaved), param 0.0–1.0
void host->fx_process(int type, float* buf, int frames, float param);
```

---

## Step 3 — Plugin File Structure

Write a **single `.c` file**. Here is a complete minimal template:

```c
#include "pocketdaw.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* ── State ── */
typedef struct {
    float sampleRate;
    PdSynthHost* host;

    /* Voice */
    int   noteOn;
    float freq;
    float phase;
    float gate;

    /* ADSR state: [value, stage, time] */
    float ampEnv[3];

    /* Filter state: [z1, z2] */
    float filtState[2];

    /* Params (0.0–1.0) */
    float params[6];
} MySynth;

static float noteToFreq(int note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

/* ── Required ── */

const char* pdsynth_name(void) { return "My Synth"; }

PdSynthInstance pdsynth_create(float sr, PdSynthHost* host) {
    MySynth* s = calloc(1, sizeof(MySynth));
    s->sampleRate = sr;
    s->host = host;
    /* Default params */
    s->params[0] = 0.5f; /* Tune     */
    s->params[1] = 0.6f; /* Cutoff   */
    s->params[2] = 0.2f; /* Resonance*/
    s->params[3] = 0.05f;/* Attack   */
    s->params[4] = 0.3f; /* Decay    */
    s->params[5] = 0.4f; /* Release  */
    return s;
}

void pdsynth_destroy(PdSynthInstance inst) { free(inst); }

void pdsynth_process(PdSynthInstance inst, PdSynthAudio* audio) {
    MySynth* s = (MySynth*)inst;
    float sr = s->sampleRate;

    /* Map normalized params to real values */
    float tune    = (s->params[0] - 0.5f) * 24.0f;   /* ±12 semitones */
    float cutoff  = 200.0f + s->params[1] * 8000.0f;  /* 200–8200 Hz */
    float res     = s->params[2] * 0.95f;
    float attack  = 0.001f + s->params[3] * 2.0f;     /* 1ms – 2s */
    float decay   = 0.01f  + s->params[4] * 3.0f;
    float release = 0.01f  + s->params[5] * 4.0f;
    float sustain = 0.7f;

    float tuneFreq = s->freq * powf(2.0f, tune / 12.0f);

    for (int i = 0; i < audio->frames; i++) {
        /* Oscillator */
        float osc = s->host->osc_sample(OSC_SAW, s->phase, tuneFreq);
        s->phase += tuneFreq / sr;
        if (s->phase >= 1.0f) s->phase -= 1.0f;

        /* Filter */
        float filtered = s->host->filter_sample(s->filtState, osc, cutoff, res, FILT_LP);

        /* Amplitude envelope */
        float amp = s->host->adsr_tick(s->ampEnv, s->gate, attack, decay, sustain, release);

        float out = filtered * amp * 0.7f;
        audio->outL[i] = out;
        audio->outR[i] = out;
    }
}

/* ── Optional: Notes ── */

void pdsynth_note(PdSynthInstance inst, int note, int vel) {
    MySynth* s = (MySynth*)inst;
    if (vel > 0) {
        s->freq  = noteToFreq(note);
        s->gate  = 1.0f;
        s->phase = 0.0f;
        /* Reset envelope attack */
        s->ampEnv[0] = 0.0f;
        s->ampEnv[1] = 0.0f;
        s->ampEnv[2] = 0.0f;
    } else {
        s->gate = 0.0f;   /* Trigger release */
    }
}

/* ── Optional: Parameters ── */

int pdsynth_param_count(void) { return 6; }

const char* pdsynth_param_name(int i) {
    const char* names[] = {"Tune","Cutoff","Resonance","Attack","Decay","Release"};
    return (i >= 0 && i < 6) ? names[i] : NULL;
}

float pdsynth_get_param(PdSynthInstance inst, int i) {
    return (i >= 0 && i < 6) ? ((MySynth*)inst)->params[i] : 0.0f;
}

void pdsynth_set_param(PdSynthInstance inst, int i, float v) {
    if (i >= 0 && i < 6) ((MySynth*)inst)->params[i] = v;
}

/* ── Optional: Presets ── */

int pdsynth_preset_count(void) { return 3; }

const char* pdsynth_preset_name(int i) {
    const char* names[] = {"Init", "Warm Bass", "Bright Lead"};
    return (i >= 0 && i < 3) ? names[i] : NULL;
}

void pdsynth_load_preset(PdSynthInstance inst, int i) {
    MySynth* s = (MySynth*)inst;
    float presets[3][6] = {
        {0.5f, 0.6f, 0.2f, 0.05f, 0.3f, 0.4f},  /* Init       */
        {0.4f, 0.3f, 0.4f, 0.02f, 0.5f, 0.6f},  /* Warm Bass  */
        {0.6f, 0.8f, 0.1f, 0.01f, 0.1f, 0.2f},  /* Bright Lead*/
    };
    if (i >= 0 && i < 3)
        for (int p = 0; p < 6; p++)
            s->params[p] = presets[i][p];
}
```

---

## Step 4 — manifest.json

Create a `manifest.json` in the same directory as your `.c` file:

```json
{
  "name": "My Synth",
  "version": "1.0.0",
  "author": "Your Name",
  "description": "A simple subtractive synth.",
  "type": "synth",
  "category": "subtractive",
  "ui": {
    "layout": "knobs",
    "pages": ["PARAMS", "WAVE"]
  }
}
```

**`ui.layout`**: `"knobs"` (≤12 params), `"sliders"` (>12), `"custom"` (full `pdsynth_draw` control)

---

## Step 5 — Compile

```bash
# Linux / Anbernic desktop (x86_64)
gcc -shared -fPIC -O2 -o my-synth.so my-synth.c -lm

# Anbernic RG35XX (aarch64 cross-compile)
aarch64-none-linux-gnu-gcc -shared -fPIC -O2 -o my-synth.so my-synth.c -lm

# Windows x64 (llvm-mingw)
x86_64-w64-mingw32-gcc -shared -o my-synth.dll my-synth.c -lm
```

---

## Step 6 — Install

```bash
# Desktop (Linux / Windows)
cp my-synth.so plugins/synths/

# Anbernic device (via SCP)
scp my-synth.so root@192.168.1.99:/mnt/mmc/MUOS/application/PocketDAW/plugins/synths/
```

PocketDAW scans `plugins/synths/` at launch. Your synth appears in the MIDI track instrument list.

---

## Rules for code generation

1. **Single `.c` file** — all logic in one file
2. **C99 only** — no C++, no `//` comments if targeting C89 cross-compilers
3. **No dynamic allocations in process()** — allocate everything in `pdsynth_create`
4. **Parameters are always 0.0–1.0** — scale to real values inside `pdsynth_process`
5. **Thread safety**: `pdsynth_process` runs on the audio thread. `pdsynth_note` and `pdsynth_set_param` may be called from the UI thread — use `_Atomic` or keep it simple (small float writes are safe on ARM/x86)
6. **Phase wrapping**: always keep phase in `[0.0, 1.0)` — wrap after incrementing
7. **No global state** — everything lives in your struct
8. **Output range**: keep `outL[i]` and `outR[i]` in `[-1.0, 1.0]` — the host clips to prevent distortion

---

## Checklist before handing off

- [ ] `pdsynth_name` returns a unique string
- [ ] `pdsynth_create` uses `calloc` and stores `sampleRate` and `host`
- [ ] `pdsynth_process` fills `outL` and `outR` for all `audio->frames` samples
- [ ] `pdsynth_destroy` calls `free(inst)`
- [ ] `pdsynth_note` handles both note-on (`vel > 0`) and note-off (`vel == 0`)
- [ ] All param indices checked against bounds
- [ ] `manifest.json` present with correct `"type": "synth"`
- [ ] Compiles with: `gcc -shared -fPIC -O2 -o plugin.so plugin.c -lm`

---

## Resources

- **Full API reference**: http://docs.pocketdaw.net/#/sdk/synth-api
- **FX plugins**: http://docs.pocketdaw.net/#/sdk/fx-api
- **Custom visuals**: http://docs.pocketdaw.net/#/sdk/visualizers
- **Build guide**: http://docs.pocketdaw.net/#/sdk/building
- **SDK repo + examples**: https://github.com/poealone/pocketdaw-sdk
- **Community**: https://community.pocketdaw.net (share your plugin in Plugins & Presets)
