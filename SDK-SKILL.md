# SKILL: Build a PocketDAW Plugin

You are building a plugin for **PocketDAW** — a handheld/desktop DAW that loads plugins as compiled `.so` (Linux/Anbernic) or `.dll` (Windows) files at runtime via `dlsym`.

Plugins are written in **C99**. No C++, no external dependencies beyond `libm`.

There are two plugin types:
- **Synth** — instrument that generates audio and goes on MIDI tracks
- **FX** — effect that processes audio and chains into mixer strips

Both support **custom visual pages** rendered with SDL2 and reacting to live audio data.

---

## PART 1 — SYNTH PLUGINS

### Required exports

```c
const char*     pdsynth_name(void);
PdSynthInstance pdsynth_create(float sampleRate, PdSynthHost* host);
void            pdsynth_destroy(PdSynthInstance inst);
void            pdsynth_process(PdSynthInstance inst, PdSynthAudio* audio);
```

`pdsynth_create` receives a `PdSynthHost*` — use it to call host DSP primitives (see Part 3).

`pdsynth_process` fills `audio->outL[i]` and `audio->outR[i]` for `i` in `0..audio->frames-1`.

### Optional exports

```c
/* Notes */
void pdsynth_note(PdSynthInstance inst, int midiNote, int velocity);
// velocity > 0 = note on, velocity == 0 = note off

/* Parameters — shown as knobs (≤12) or sliders (>12) in editor */
int         pdsynth_param_count(void);
const char* pdsynth_param_name(int index);
float       pdsynth_get_param(PdSynthInstance inst, int index);
void        pdsynth_set_param(PdSynthInstance inst, int index, float value);
// All values are 0.0–1.0 normalized

/* Presets */
int         pdsynth_preset_count(void);
const char* pdsynth_preset_name(int index);
void        pdsynth_load_preset(PdSynthInstance inst, int index);

/* Sample loading */
void pdsynth_sample_loaded(PdSynthInstance inst,
                            const float* data, int length, float sampleRate);
// Host calls this after user loads an audio file in the file browser

/* Custom graphics page (see Part 4) */
void pdsynth_draw(PdSynthInstance inst, SDL_Renderer* r, PdDrawContext* ctx);
// Return 1 from this function to own the full graphics page
// Return 0 to draw behind host knobs/sliders
```

### Complete synth example — subtractive synth

```c
#include "pocketdaw.h"
#include <stdlib.h>
#include <math.h>

typedef struct {
    float sampleRate;
    PdSynthHost* host;

    /* Voice state */
    int   noteOn;
    float freq;
    float phase;
    float gate;

    /* DSP state — zero-initialized by calloc */
    float ampEnv[3];    /* [value, stage, time] — passed to adsr_tick */
    float filtState[2]; /* [z1, z2] — passed to filter_sample */

    /* Params (0.0–1.0) */
    float p[6]; /* tune, cutoff, resonance, attack, decay, release */
} SubSynth;

static float noteToHz(int n) { return 440.0f * powf(2.0f,(n-69)/12.0f); }

const char*     pdsynth_name(void) { return "Sub Synth"; }
int             pdsynth_param_count(void) { return 6; }

const char* pdsynth_param_name(int i) {
    const char* n[] = {"Tune","Cutoff","Resonance","Attack","Decay","Release"};
    return (i>=0&&i<6) ? n[i] : NULL;
}

PdSynthInstance pdsynth_create(float sr, PdSynthHost* host) {
    SubSynth* s = calloc(1, sizeof(SubSynth));
    s->sampleRate = sr;
    s->host = host;
    s->p[0]=0.5f; s->p[1]=0.6f; s->p[2]=0.2f;
    s->p[3]=0.05f; s->p[4]=0.3f; s->p[5]=0.4f;
    return s;
}

void pdsynth_destroy(PdSynthInstance inst) { free(inst); }

void pdsynth_note(PdSynthInstance inst, int note, int vel) {
    SubSynth* s = (SubSynth*)inst;
    if (vel > 0) {
        s->freq = noteToHz(note);
        s->gate = 1.0f;
        s->phase = 0.0f;
        s->ampEnv[0] = s->ampEnv[1] = s->ampEnv[2] = 0.0f;
    } else {
        s->gate = 0.0f;
    }
}

void pdsynth_process(PdSynthInstance inst, PdSynthAudio* audio) {
    SubSynth* s = (SubSynth*)inst;
    float sr = s->sampleRate;

    float tune    = (s->p[0]-0.5f)*24.0f;             /* ±12 semitones */
    float cutoff  = 200.0f + s->p[1]*8000.0f;
    float res     = s->p[2]*0.95f;
    float attack  = 0.001f + s->p[3]*2.0f;
    float decay   = 0.01f  + s->p[4]*3.0f;
    float release = 0.01f  + s->p[5]*4.0f;
    float freq    = s->freq * powf(2.0f, tune/12.0f);

    for (int i = 0; i < audio->frames; i++) {
        float osc      = s->host->osc_sample(OSC_SAW, s->phase, freq);
        s->phase += freq/sr;
        if (s->phase >= 1.0f) s->phase -= 1.0f;

        float filtered = s->host->filter_sample(s->filtState, osc, cutoff, res, FILT_LP);
        float amp      = s->host->adsr_tick(s->ampEnv, s->gate, attack, decay, 0.7f, release);

        float out = filtered * amp * 0.7f;
        audio->outL[i] = out;
        audio->outR[i] = out;
    }
}

float pdsynth_get_param(PdSynthInstance inst, int i) {
    return (i>=0&&i<6) ? ((SubSynth*)inst)->p[i] : 0.0f;
}
void  pdsynth_set_param(PdSynthInstance inst, int i, float v) {
    if (i>=0&&i<6) ((SubSynth*)inst)->p[i]=v;
}

int pdsynth_preset_count(void) { return 3; }
const char* pdsynth_preset_name(int i) {
    const char* n[]={"Init","Warm Bass","Bright Lead"};
    return (i>=0&&i<3)?n[i]:NULL;
}
void pdsynth_load_preset(PdSynthInstance inst, int i) {
    SubSynth* s = (SubSynth*)inst;
    float pre[3][6]={
        {0.5f,0.6f,0.2f,0.05f,0.3f,0.4f},
        {0.4f,0.3f,0.4f,0.02f,0.5f,0.6f},
        {0.6f,0.8f,0.1f,0.01f,0.1f,0.2f},
    };
    if (i>=0&&i<3) for(int j=0;j<6;j++) s->p[j]=pre[i][j];
}
```

---

## PART 2 — FX PLUGINS

### Required exports

```c
const char* pdfx_name(void);
PdFxInstance pdfx_create(float sampleRate);
void         pdfx_destroy(PdFxInstance inst);
void         pdfx_process(PdFxInstance inst, PdFxAudio* audio);
```

`PdFxAudio` has `inputL`, `inputR` (read), `outputL`, `outputR` (write), `bufferSize`. Read input[i] before writing output[i] — buffers may overlap.

### Optional FX exports

```c
int  pdfx_api_version(void); /* Return 1, 2, or 3 */

/* Parameters — same 0.0–1.0 system as synths */
int pdfx_param_count(void);
const char* pdfx_param_name(int i);
float pdfx_get_param(PdFxInstance inst, int i);
void  pdfx_set_param(PdFxInstance inst, int i, float v);
int   pdfx_preset_count(void);
const char* pdfx_preset_name(int i);
void  pdfx_load_preset(PdFxInstance inst, int i);

/* SDK v2 — sidechain input */
int  pdfx_needs_sidechain(void); /* return 1 to request sidechain */
void pdfx_set_sidechain(PdFxInstance inst,
                         const float* scL, const float* scR, int frames);
/* Host calls set_sidechain BEFORE each pdfx_process call */

/* SDK v2.1 — editor hooks */
float       pdfx_get_gain_reduction(PdFxInstance inst); /* 0.0–1.0 */
void        pdfx_get_accent_color(PdFxInstance inst, uint8_t* r, uint8_t* g, uint8_t* b);
const char* pdfx_format_param(PdFxInstance inst, int i); /* "5.0ms", "4:1" */
const char* pdfx_param_group(int i);                     /* "Detector", "Output" */
```

### Complete FX example — sidechain compressor

```c
#include "pocketdaw.h"
#include <stdlib.h>
#include <math.h>

typedef struct {
    float sr;
    float p[5];       /* threshold, ratio, attack, release, mix */
    const float* scL;
    const float* scR;
    int scFrames;
    float env;
    float gr;         /* current gain reduction for GR meter */
    char  fmt[32];
} SCComp;

int   pdfx_api_version(void)     { return 3; }
const char* pdfx_name(void)      { return "SC Comp"; }
int   pdfx_needs_sidechain(void) { return 1; }

PdFxInstance pdfx_create(float sr) {
    SCComp* c = calloc(1, sizeof(SCComp));
    c->sr = sr;
    c->p[0]=0.5f; c->p[1]=0.6f; c->p[2]=0.1f; c->p[3]=0.3f; c->p[4]=1.0f;
    return c;
}
void pdfx_destroy(PdFxInstance inst) { free(inst); }

void pdfx_set_sidechain(PdFxInstance inst,
                         const float* l, const float* r, int n) {
    SCComp* c=(SCComp*)inst; c->scL=l; c->scR=r; c->scFrames=n;
}

void pdfx_process(PdFxInstance inst, PdFxAudio* a) {
    SCComp* c = (SCComp*)inst;
    float thresh  = 0.01f + c->p[0]*0.99f;
    float ratio   = 1.0f  + c->p[1]*19.0f;
    float atk     = expf(-1.0f/(c->sr*(0.001f+c->p[2]*0.099f)));
    float rel     = expf(-1.0f/(c->sr*(0.01f +c->p[3]*0.49f)));
    float wet     = c->p[4];

    for (int i = 0; i < a->bufferSize; i++) {
        float sc = 0.0f;
        if (c->scL && i < c->scFrames) {
            float l=fabsf(c->scL[i]), r=c->scR?fabsf(c->scR[i]):l;
            sc = l>r?l:r;
        } else {
            float l=fabsf(a->inputL[i]), r=fabsf(a->inputR[i]);
            sc = l>r?l:r;
        }
        float coeff = (sc>c->env)?atk:rel;
        c->env = coeff*c->env + (1.0f-coeff)*sc;

        float gain = 1.0f;
        if (c->env > thresh)
            gain = thresh * powf(c->env/thresh, 1.0f/ratio-1.0f) / c->env;
        c->gr = 1.0f - gain;

        a->outputL[i] = a->inputL[i]*(1.0f-wet) + a->inputL[i]*gain*wet;
        a->outputR[i] = a->inputR[i]*(1.0f-wet) + a->inputR[i]*gain*wet;
    }
}

int   pdfx_param_count(void) { return 5; }
const char* pdfx_param_name(int i) {
    const char* n[]={"Threshold","Ratio","Attack","Release","Mix"};
    return (i>=0&&i<5)?n[i]:NULL;
}
float pdfx_get_param(PdFxInstance inst,int i){ return ((SCComp*)inst)->p[i]; }
void  pdfx_set_param(PdFxInstance inst,int i,float v){ ((SCComp*)inst)->p[i]=v; }
const char* pdfx_param_group(int i){
    if(i<=1) return "Detector"; if(i<=3) return "Envelope"; return "Output";
}
float pdfx_get_gain_reduction(PdFxInstance inst){ return ((SCComp*)inst)->gr; }
const char* pdfx_format_param(PdFxInstance inst, int i){
    SCComp* c=(SCComp*)inst;
    if(i==0){float db=20.0f*log10f(0.01f+c->p[0]*0.99f);snprintf(c->fmt,32,"%.1f dB",db);return c->fmt;}
    if(i==1){snprintf(c->fmt,32,"%.1f:1",1.0f+c->p[1]*19.0f);return c->fmt;}
    if(i==2||i==3){float ms=(i==2?(0.001f+c->p[2]*0.099f):(0.01f+c->p[3]*0.49f))*1000.0f;snprintf(c->fmt,32,"%.1f ms",ms);return c->fmt;}
    return NULL;
}
void pdfx_get_accent_color(PdFxInstance inst,uint8_t* r,uint8_t* g,uint8_t* b){
    *r=0;*g=255;*b=65;
}
```

---

## PART 3 — HOST PRIMITIVES (synths only)

Access via the `PdSynthHost*` pointer from `pdsynth_create`.

```c
/* Oscillator — type constants: OSC_SINE, OSC_SAW, OSC_SQUARE, OSC_TRI, OSC_NOISE, OSC_PULSE */
float host->osc_sample(int type, float phase, float freq);
// phase: 0.0–1.0 (you increment it each sample: phase += freq/sampleRate)

/* State-variable filter — mode: FILT_LP, FILT_HP, FILT_BP, FILT_NOTCH */
// state is float[2] in your struct, zero-initialized
float host->filter_sample(float* state, float input, float cutoff_hz, float resonance, int mode);

/* ADSR envelope */
// env is float[3] in your struct, zero-initialized
// gate: 1.0 = key held, 0.0 = released
float host->adsr_tick(float* env, float gate, float attack_s, float decay_s, float sustain, float release_s);

/* Morphing wavetable — type: WT_ANALOG, WT_DIGITAL, WT_VOCAL */
float host->wavetable_sample(int type, float phase, float morph); /* morph 0.0–1.0 */

/* Host FX bus — type: FX_REVERB, FX_DELAY, FX_CHORUS, FX_DISTORT, FX_BITCRUSH */
// buf: stereo interleaved float*, in-place
void host->fx_process(int type, float* buf, int frames, float param); /* param 0.0–1.0 */
```

---

## PART 4 — CUSTOM VISUALS

Add `pdsynth_draw` to a synth or `pdfx_draw` to an FX plugin to own the graphics page.

```c
/* Synth visual callback */
void pdsynth_draw(PdSynthInstance inst, SDL_Renderer* renderer, PdDrawContext* ctx);

/* FX visual callback (optional — FX editor has built-in scope/meters) */
void pdfx_draw(PdFxInstance inst, SDL_Renderer* renderer, PdDrawContext* ctx);
```

Return `1` from the function to own the full graphics area. Return `0` to draw behind host UI.

### PdDrawContext fields

```c
typedef struct {
    /* Live audio data — updated every frame */
    float scopeBufL[128]; /* last 128 samples of output, left  */
    float scopeBufR[128]; /* last 128 samples of output, right */
    float peakL;          /* 0.0–1.0 peak level, left          */
    float peakR;          /* 0.0–1.0 peak level, right         */

    /* Playback state */
    int noteOn;           /* 1 if any note is held             */
    int lastMidi;         /* last MIDI note number (0–127)     */
    int voiceCount;       /* number of active voices           */

    /* Render bounds */
    int x, y, w, h;      /* your drawable area in pixels       */

    /* Desktop mouse (SDK v3.1) */
    int mouseX, mouseY;   /* cursor position relative to x,y   */
    int mouseDown;        /* 1 if left button held              */
} PdDrawContext;
```

### SDL2 drawing — available functions

```c
SDL_SetRenderDrawColor(renderer, r, g, b, a);
SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
SDL_RenderDrawPoint(renderer, x, y);
SDL_RenderFillRect(renderer, &rect);   /* SDL_Rect rect = {x,y,w,h}; */
SDL_RenderDrawRect(renderer, &rect);
```

### Complete visual example — reactive waveform scope

```c
/* Add this to any synth plugin */
/* Return 1 means we own the full page */
int pdsynth_draw(PdSynthInstance inst, SDL_Renderer* r, PdDrawContext* ctx) {
    /* Background */
    SDL_SetRenderDrawColor(r, 5, 5, 5, 255);
    SDL_Rect bg = {ctx->x, ctx->y, ctx->w, ctx->h};
    SDL_RenderFillRect(r, &bg);

    /* Center line */
    int cy = ctx->y + ctx->h / 2;
    SDL_SetRenderDrawColor(r, 30, 30, 30, 255);
    SDL_RenderDrawLine(r, ctx->x, cy, ctx->x + ctx->w, cy);

    /* Color shifts green → red with peak level */
    uint8_t red   = (uint8_t)(ctx->peakL * 255.0f);
    uint8_t green = (uint8_t)((1.0f - ctx->peakL) * 200.0f + 55.0f);

    /* Draw waveform from scope buffer */
    SDL_SetRenderDrawColor(r, red, green, 65, 255);
    for (int i = 1; i < 128; i++) {
        int x1 = ctx->x + (i-1) * ctx->w / 128;
        int x2 = ctx->x + i     * ctx->w / 128;
        int y1 = cy - (int)(ctx->scopeBufL[i-1] * ctx->h * 0.45f);
        int y2 = cy - (int)(ctx->scopeBufL[i]   * ctx->h * 0.45f);
        SDL_RenderDrawLine(r, x1, y1, x2, y2);
    }

    /* Flash border on note-on */
    if (ctx->noteOn) {
        SDL_SetRenderDrawColor(r, 0, 255, 65, 80);
        SDL_RenderDrawRect(r, &bg);
    }

    return 1;
}
```

### Complete visual example — reactive particles

```c
#define MAX_PARTICLES 64

typedef struct { float x, y, vx, vy, life; } Particle;

typedef struct {
    /* ... your synth fields ... */
    Particle particles[MAX_PARTICLES];
    int      pCount;
} MySynth;

static void spawnParticles(MySynth* s, int cx, int cy) {
    for (int i = 0; i < 8 && s->pCount < MAX_PARTICLES; i++) {
        Particle* p = &s->particles[s->pCount++];
        p->x = (float)cx; p->y = (float)cy;
        /* Random-ish velocity using particle index */
        p->vx = ((i * 37 % 11) - 5) * 0.8f;
        p->vy = -((i * 13 % 8) + 2) * 0.6f;
        p->life = 1.0f;
    }
}

int pdsynth_draw(PdSynthInstance inst, SDL_Renderer* r, PdDrawContext* ctx) {
    MySynth* s = (MySynth*)inst;

    /* Background */
    SDL_SetRenderDrawColor(r, 5, 5, 5, 255);
    SDL_Rect bg = {ctx->x, ctx->y, ctx->w, ctx->h};
    SDL_RenderFillRect(r, &bg);

    /* Spawn on note-on */
    if (ctx->noteOn && s->pCount < MAX_PARTICLES / 2)
        spawnParticles(s, ctx->x + ctx->w/2, ctx->y + ctx->h/2);

    /* Update + draw particles */
    int alive = 0;
    for (int i = 0; i < s->pCount; i++) {
        Particle* p = &s->particles[i];
        p->x += p->vx; p->y += p->vy;
        p->vy += 0.1f; /* gravity */
        p->life -= 0.02f;
        if (p->life <= 0.0f) continue;

        uint8_t a = (uint8_t)(p->life * 255.0f);
        SDL_SetRenderDrawColor(r, 0, 255, 65, a);
        SDL_RenderDrawPoint(r, (int)p->x, (int)p->y);
        s->particles[alive++] = *p;
    }
    s->pCount = alive;

    /* Frequency bars from scope buffer */
    SDL_SetRenderDrawColor(r, 0, 180, 40, 120);
    int bars = 32;
    int barW = ctx->w / bars;
    for (int b = 0; b < bars; b++) {
        /* Approximate energy in this freq bin from scope */
        float energy = 0.0f;
        int start = b * (128 / bars);
        int end   = start + (128 / bars);
        for (int s2 = start; s2 < end; s2++)
            energy += fabsf(ctx->scopeBufL[s2]);
        energy /= (128 / bars);

        int barH = (int)(energy * ctx->h * 2.0f);
        SDL_Rect bar = {
            ctx->x + b * barW + 1,
            ctx->y + ctx->h - barH,
            barW - 2, barH
        };
        SDL_RenderFillRect(r, &bar);
    }

    return 1;
}
```

---

## PART 5 — MANIFEST.JSON

Every plugin folder needs a `manifest.json` alongside the `.c` file.

**Synth:**
```json
{
  "name": "My Synth",
  "version": "1.0.0",
  "author": "Your Name",
  "description": "What it does.",
  "type": "synth",
  "category": "subtractive",
  "ui": {
    "layout": "knobs",
    "pages": ["PARAMS", "WAVE"]
  }
}
```

**FX:**
```json
{
  "name": "My FX",
  "version": "1.0.0",
  "author": "Your Name",
  "description": "What it does.",
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

**`ui.layout`**: `"knobs"` (≤12 params), `"sliders"` (>12), `"custom"` (full draw control)

**`ui.pages`** (synths): array of page names shown in tab bar, e.g. `["PARAMS", "WAVE", "ENV"]`

**FX `ui.editor` flags**: `scope`, `meters`, `grMeter` (requires `pdfx_get_gain_reduction`), `particles`

**`category`**: synths: `"subtractive"` `"fm"` `"wavetable"` `"granular"` `"sampler"` `"drum"`  
FX: `"dynamics"` `"filter"` `"delay"` `"reverb"` `"distortion"` `"modulation"` `"utility"`

---

## PART 6 — BUILD & INSTALL

```bash
# Linux x64 (synth)
gcc -shared -fPIC -O2 -o my-synth.so my-synth.c -lm

# Linux x64 (FX)
gcc -shared -fPIC -O2 -o my-fx.so my-fx.c -lm

# Anbernic RG35XX (aarch64 cross-compile)
aarch64-none-linux-gnu-gcc -shared -fPIC -O2 -o my-plugin.so my-plugin.c -lm

# Windows x64
x86_64-w64-mingw32-gcc -shared -o my-plugin.dll my-plugin.c -lm
```

```bash
# Install — desktop
cp my-synth.so plugins/synths/
cp my-fx.so    plugins/fx/

# Install — Anbernic device
scp my-synth.so root@<device-ip>:/mnt/mmc/MUOS/application/PocketDAW/plugins/synths/
scp my-fx.so    root@<device-ip>:/mnt/mmc/MUOS/application/PocketDAW/plugins/fx/
```

PocketDAW scans plugin folders at launch. No restart needed on desktop — reload project.

---

## PART 7 — CODE RULES

1. **Single `.c` file** — all logic in one file
2. **C99 only** — no C++, no `bool` (use `int`), no `//` if targeting strict C89 compilers
3. **`calloc` not `malloc`** — zero-initializes your struct
4. **No allocations in `process()`** — allocate everything in `create()`
5. **Parameters are always 0.0–1.0** — scale to real values inside `process()`
6. **Phase wrapping**: keep phase in `[0.0, 1.0)` — `if (phase >= 1.0f) phase -= 1.0f;`
7. **No global state** — everything lives in your struct
8. **Output range**: keep output in `[-1.0, 1.0]`
9. **FX in-place safety**: read `input[i]` before writing `output[i]`
10. **`format_param` buffer**: store it in your struct — host may hold the pointer
11. **Sidechain pointer**: `pdfx_set_sidechain` stores the pointer, valid only until next `pdfx_process`
12. **Thread safety**: `process()` runs on audio thread; `set_param()` on UI thread. Single float writes are safe on ARM/x86 — no mutex needed for params

---

## PART 8 — CHECKLIST

**Synth:**
- [ ] `pdsynth_name` returns a unique string
- [ ] `pdsynth_create` uses `calloc`, stores `sampleRate` and `host`
- [ ] `pdsynth_process` fills `outL` and `outR` for all `audio->frames` samples
- [ ] `pdsynth_destroy` calls `free(inst)`
- [ ] `pdsynth_note` handles both on (`vel>0`) and off (`vel==0`)
- [ ] `manifest.json` with `"type": "synth"`
- [ ] Compiles: `gcc -shared -fPIC -O2 -o plugin.so plugin.c -lm`

**FX:**
- [ ] `pdfx_name` returns a unique string
- [ ] `pdfx_create` uses `calloc`, stores `sampleRate`
- [ ] `pdfx_process` writes `outputL` and `outputR` for all `bufferSize` frames
- [ ] `pdfx_destroy` calls `free(inst)`
- [ ] If sidechain: `pdfx_needs_sidechain` returns `1`, `pdfx_set_sidechain` stores pointer
- [ ] `pdfx_format_param` returns a struct-owned buffer, not a stack string
- [ ] `manifest.json` with `"type": "fx"`
- [ ] Compiles: `gcc -shared -fPIC -O2 -o plugin.so plugin.c -lm`

---

## Resources

- **Docs**: http://docs.pocketdaw.net
- **SDK repo + examples**: https://github.com/poealone/pocketdaw-sdk
- **Community (share your plugin)**: https://community.pocketdaw.net — Plugins & Presets
