/**
 * JT Synth v3 — Johnytiger's Wavetable Synthesizer
 * 
 * Dual-oscillator wavetable synth with UNISON voicing:
 * - 8 morphable wavetable positions per oscillator
 * - Independent Osc1 + Osc2 with mix control
 * - 3 unison sub-voices per oscillator (stereo spread + detune)
 * - Osc2 detune for additional thickness
 * - Moog-style ladder filter (4-pole LPF) with drive
 * - ADSR envelope with velocity sensitivity
 * - 8-voice polyphony × 6 sub-voices = 48 oscillators
 *
 * Build:
 *   aarch64-none-linux-gnu-gcc -shared -fPIC -O2 -o jt-synth.so jt-synth.c -lm
 *
 * Accent: Red [255, 30, 60]
 */

#include "../../pdsynth_api.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_VOICES 8
#define WAVETABLE_SIZE 2048
#define NUM_WAVE_POSITIONS 8
#define UNISON_VOICES 3   /* Sub-voices per oscillator for thickness */

/* ── Wavetable Generation ── */

static void generateWavetable(float table[NUM_WAVE_POSITIONS][WAVETABLE_SIZE]) {
    for (int i = 0; i < WAVETABLE_SIZE; i++) {
        float phase = (float)i / WAVETABLE_SIZE;
        float t = phase * 2.0f * (float)M_PI;
        
        /* Position 0: Pure sine */
        table[0][i] = sinf(t);
        
        /* Position 1: Warm sine + harmonics (analog character) */
        table[1][i] = sinf(t) * 0.6f + sinf(t * 2) * 0.25f + sinf(t * 3) * 0.10f + sinf(t * 4) * 0.05f;
        
        /* Position 2: Triangle (bandlimited) */
        table[2][i] = 0;
        for (int h = 0; h < 20; h++) {
            int n = 2 * h + 1;
            float sign = (h % 2 == 0) ? 1.0f : -1.0f;
            table[2][i] += sign * sinf(t * n) / (float)(n * n);
        }
        table[2][i] *= (8.0f / ((float)M_PI * (float)M_PI));
        
        /* Position 3: Soft saw (fewer harmonics, warm) */
        table[3][i] = 0;
        for (int h = 1; h <= 24; h++) {
            float rolloff = 1.0f / (1.0f + 0.05f * h * h); /* Gentle rolloff */
            table[3][i] += sinf(t * h) * rolloff / (float)h;
        }
        table[3][i] *= -2.0f / (float)M_PI;
        
        /* Position 4: Full saw (Serum-style, maximum harmonics) */
        table[4][i] = 0;
        for (int h = 1; h <= 64; h++) {
            table[4][i] += sinf(t * h) / (float)h;
        }
        table[4][i] *= -2.0f / (float)M_PI;
        
        /* Position 5: Square (thick, hollow) */
        table[5][i] = 0;
        for (int h = 0; h < 32; h++) {
            int n = 2 * h + 1;
            table[5][i] += sinf(t * n) / (float)n;
        }
        table[5][i] *= 4.0f / (float)M_PI;
        
        /* Position 6: Pulse (nasal, biting) */
        table[6][i] = 0;
        for (int h = 1; h <= 48; h++) {
            table[6][i] += sinf(t * h) * cosf((float)M_PI * h * 0.12f) / (float)h;
        }
        table[6][i] *= 2.2f / (float)M_PI;
        
        /* Position 7: Metallic / Formant (complex, aggressive) */
        table[7][i] = sinf(t) * 0.35f + sinf(t * 2.76f) * 0.25f +
                       sinf(t * 4.07f) * 0.20f + sinf(t * 5.54f) * 0.12f +
                       sinf(t * 6.98f) * 0.08f;
    }
    
    /* Normalize all positions */
    for (int p = 0; p < NUM_WAVE_POSITIONS; p++) {
        float maxAbs = 0;
        for (int i = 0; i < WAVETABLE_SIZE; i++) {
            float a = table[p][i] < 0 ? -table[p][i] : table[p][i];
            if (a > maxAbs) maxAbs = a;
        }
        if (maxAbs > 0.001f) {
            float norm = 0.95f / maxAbs;
            for (int i = 0; i < WAVETABLE_SIZE; i++) table[p][i] *= norm;
        }
    }
}

/* Read wavetable with cubic interpolation for smoother sound */
static float readWavetable(float table[NUM_WAVE_POSITIONS][WAVETABLE_SIZE], 
                           float position, float phase) {
    float posF = position * (NUM_WAVE_POSITIONS - 1);
    int pos0 = (int)posF;
    if (pos0 < 0) pos0 = 0;
    if (pos0 >= NUM_WAVE_POSITIONS - 1) pos0 = NUM_WAVE_POSITIONS - 2;
    int pos1 = pos0 + 1;
    float posFrac = posF - pos0;
    
    float idx = phase * WAVETABLE_SIZE;
    int i0 = (int)idx % WAVETABLE_SIZE;
    int i1 = (i0 + 1) % WAVETABLE_SIZE;
    float frac = idx - (int)idx;
    
    float s0 = table[pos0][i0] + frac * (table[pos0][i1] - table[pos0][i0]);
    float s1 = table[pos1][i0] + frac * (table[pos1][i1] - table[pos1][i0]);
    
    return s0 + posFrac * (s1 - s0);
}

/* ── Moog-style Ladder Filter with drive ── */

typedef struct {
    float stage[4];
    float delay[4];
} LadderFilter;

static float processLadder(LadderFilter* f, float input, float cutoff, float resonance) {
    /* Nonlinear cutoff mapping for more musical range */
    float fc = cutoff * cutoff * cutoff;
    if (fc > 0.99f) fc = 0.99f;
    if (fc < 0.001f) fc = 0.001f;
    
    /* Resonance feedback with self-oscillation potential */
    float fb = resonance * 4.0f;
    float comp = 1.0f + fb * 0.25f; /* Input compensation */
    
    /* Soft-clip the feedback for warmth */
    float fbSig = f->stage[3];
    fbSig = fbSig * (1.0f + 0.3f * fbSig * fbSig); /* Subtle saturation */
    
    float inp = input * comp - fb * fbSig;
    
    /* 4-pole cascade with tanh saturation per stage */
    for (int s = 0; s < 4; s++) {
        float prev = (s == 0) ? inp : f->stage[s - 1];
        f->stage[s] += fc * (tanhf(prev) - tanhf(f->stage[s]));
    }
    
    return f->stage[3];
}

/* ── Unison sub-voice state ── */
typedef struct {
    float phase;
    float detuneRatio;  /* Frequency multiplier for this sub-voice */
    float pan;          /* -1 to 1 stereo position */
} UnisonVoice;

/* ── Voice (polyphonic) ── */

typedef struct {
    int active;
    uint8_t note;
    uint8_t velocity;
    
    /* Unison sub-voices for each oscillator */
    UnisonVoice osc1[UNISON_VOICES];
    UnisonVoice osc2[UNISON_VOICES];
    float freq;
    
    /* ADSR */
    float envLevel;
    int envStage;       /* 0=off, 1=attack, 2=decay, 3=sustain, 4=release */
    float envTime;
    
    /* Per-voice stereo filters */
    LadderFilter filterL;
    LadderFilter filterR;
} Voice;

/* ── Preset Definition ── */

typedef struct {
    const char* name;
    float osc1Wave;
    float osc2Wave;
    float osc2Vol;
    float detune;
    float attack;
    float decay;
    float sustain;
    float release;
    float cutoff;
    float resonance;
} JTPreset;

/* Unison detune spreads (in cents deviation from center) */
static const float UNISON_SPREAD[UNISON_VOICES] = { 0.0f, -12.0f, 12.0f };
/* Unison stereo pan positions */
static const float UNISON_PAN[UNISON_VOICES] = { 0.0f, -0.6f, 0.6f };

static const JTPreset FACTORY_PRESETS[] = {
    /* === BASS — Massive Low End (0-3) === */
    /*                          o1w    o2w    o2v   det    atk    dec    sus    rel    cut    res */
    {"Nexus 808",       0.00f, 0.12f, 0.65f, 0.06f, 0.00f, 0.40f, 0.95f, 0.45f, 0.30f, 0.28f},
    {"Serum Reese",     0.50f, 0.52f, 0.92f, 0.28f, 0.01f, 0.50f, 0.88f, 0.35f, 0.36f, 0.45f},
    {"Supersaw Bass",   0.55f, 0.58f, 0.90f, 0.35f, 0.00f, 0.08f, 0.92f, 0.18f, 0.42f, 0.55f},
    {"Growl Machine",   0.68f, 0.72f, 0.85f, 0.22f, 0.00f, 0.06f, 0.90f, 0.22f, 0.35f, 0.65f},
    
    /* === PADS — Lush & Cinematic (4-7) === */
    {"Analog Heaven",   0.18f, 0.24f, 0.88f, 0.30f, 0.58f, 0.55f, 0.78f, 0.95f, 0.60f, 0.18f},
    {"Serum Atmosphere",0.30f, 0.35f, 0.92f, 0.38f, 0.70f, 0.50f, 0.72f, 1.00f, 0.68f, 0.12f},
    {"Nexus Dreamscape",0.38f, 0.45f, 0.95f, 0.42f, 0.65f, 0.42f, 0.82f, 0.92f, 0.65f, 0.22f},
    {"Cinematic Swell", 0.45f, 0.50f, 0.88f, 0.48f, 0.75f, 0.52f, 0.68f, 0.98f, 0.75f, 0.15f},
    
    /* === KEYS & PLUCKS (8-11) === */
    {"Vintage Keys",    0.32f, 0.38f, 0.82f, 0.20f, 0.02f, 0.25f, 0.65f, 0.40f, 0.58f, 0.20f},
    {"Synth Choir",     0.42f, 0.48f, 0.90f, 0.32f, 0.42f, 0.32f, 0.80f, 0.62f, 0.62f, 0.16f},
    {"Digital Bell",    0.22f, 0.60f, 0.70f, 0.15f, 0.00f, 0.15f, 0.45f, 0.55f, 0.72f, 0.08f},
    {"Pluck Stab",      0.55f, 0.58f, 0.85f, 0.18f, 0.00f, 0.04f, 0.30f, 0.15f, 0.68f, 0.35f},
    
    /* === LEADS — Cutting & Modern (12-15) === */
    {"Serum Supersaw",  0.55f, 0.58f, 0.95f, 0.35f, 0.01f, 0.15f, 0.85f, 0.18f, 0.72f, 0.30f},
    {"Nexus Lead",      0.62f, 0.65f, 0.90f, 0.28f, 0.00f, 0.12f, 0.88f, 0.15f, 0.70f, 0.38f},
    {"Acid Screamer",   0.65f, 0.70f, 0.88f, 0.10f, 0.00f, 0.10f, 0.78f, 0.12f, 0.45f, 0.78f},
    {"Laser Cannon",    0.78f, 0.82f, 0.92f, 0.25f, 0.00f, 0.06f, 0.90f, 0.10f, 0.78f, 0.52f},
};

#define NUM_PRESETS (sizeof(FACTORY_PRESETS) / sizeof(FACTORY_PRESETS[0]))

/* ── Plugin State ── */

typedef struct {
    Voice voices[MAX_VOICES];
    float wavetable[NUM_WAVE_POSITIONS][WAVETABLE_SIZE];
    
    /* Parameters (10 total) */
    float osc1Wave;     /* 0 */
    float osc2Wave;     /* 1 */
    float osc2Vol;      /* 2 */
    float detune;       /* 3 */
    float attack;       /* 4 */
    float decay;        /* 5 */
    float sustain;      /* 6 */
    float release;      /* 7 */
    float cutoff;       /* 8 */
    float resonance;    /* 9 */
    
    float sampleRate;
    int currentPreset;
} JTSynth;

/* ── Param Helpers ── */

static float paramToTime(float p, float minMs, float maxMs) {
    return (minMs + (maxMs - minMs) * p * p) / 1000.0f;
}

static float centsToRatio(float cents) {
    return powf(2.0f, cents / 1200.0f);
}

/* ── API ── */

int pdsynth_api_version(void) { return PDSYNTH_API_VERSION; }
const char* pdsynth_name(void) { return "JT Synth"; }
int pdsynth_param_count(void) { return 10; }

PdSynthInstance pdsynth_create(float sampleRate) {
    JTSynth* s = (JTSynth*)calloc(1, sizeof(JTSynth));
    if (!s) return NULL;
    s->sampleRate = sampleRate;
    
    generateWavetable(s->wavetable);
    
    /* Default preset: Serum Supersaw (index 12) */
    s->currentPreset = 12;
    const JTPreset* p = &FACTORY_PRESETS[s->currentPreset];
    s->osc1Wave   = p->osc1Wave;
    s->osc2Wave   = p->osc2Wave;
    s->osc2Vol    = p->osc2Vol;
    s->detune     = p->detune;
    s->attack     = p->attack;
    s->decay      = p->decay;
    s->sustain    = p->sustain;
    s->release    = p->release;
    s->cutoff     = p->cutoff;
    s->resonance  = p->resonance;
    
    fprintf(stderr, "[jt-synth] Created v3 (unison), preset: %s\n", p->name);
    return (PdSynthInstance)s;
}

void pdsynth_destroy(PdSynthInstance inst) {
    if (inst) free(inst);
}

static void initUnisonVoices(UnisonVoice uv[], float baseDetuneCents) {
    for (int u = 0; u < UNISON_VOICES; u++) {
        uv[u].phase = (float)u * 0.33f; /* Stagger initial phases to avoid constructive peak */
        uv[u].detuneRatio = centsToRatio(UNISON_SPREAD[u] + baseDetuneCents);
        uv[u].pan = UNISON_PAN[u];
    }
}

void pdsynth_note(PdSynthInstance inst, PdSynthNote* event) {
    JTSynth* s = (JTSynth*)inst;
    
    if (event->type == 1) {
        /* Note on — find free voice */
        int slot = -1;
        for (int i = 0; i < MAX_VOICES; i++) {
            if (!s->voices[i].active) { slot = i; break; }
        }
        if (slot < 0) {
            float maxTime = -1;
            for (int i = 0; i < MAX_VOICES; i++) {
                if (s->voices[i].envTime > maxTime) { maxTime = s->voices[i].envTime; slot = i; }
            }
        }
        if (slot >= 0) {
            Voice* v = &s->voices[slot];
            v->active = 1;
            v->note = event->note;
            v->velocity = event->velocity;
            v->freq = 440.0f * powf(2.0f, (event->note - 69) / 12.0f);
            v->envLevel = 0.0f;
            v->envStage = 1;
            v->envTime = 0.0f;
            memset(&v->filterL, 0, sizeof(LadderFilter));
            memset(&v->filterR, 0, sizeof(LadderFilter));
            
            /* Initialize unison sub-voices */
            /* Osc2 gets extra detune spread from the Detune param */
            float osc2ExtraCents = s->detune * 25.0f; /* 0-25 cents extra */
            initUnisonVoices(v->osc1, 0.0f);
            initUnisonVoices(v->osc2, osc2ExtraCents);
        }
    } else {
        for (int i = 0; i < MAX_VOICES; i++) {
            if (s->voices[i].active && s->voices[i].note == event->note && s->voices[i].envStage != 4) {
                s->voices[i].envStage = 4;
                s->voices[i].envTime = 0.0f;
            }
        }
    }
}

void pdsynth_process(PdSynthInstance inst, PdSynthAudio* audio) {
    JTSynth* s = (JTSynth*)inst;
    float dt = 1.0f / s->sampleRate;
    
    float attackTime  = paramToTime(s->attack,  1.0f, 3000.0f);
    float decayTime   = paramToTime(s->decay,   1.0f, 2000.0f);
    float sustainLvl  = s->sustain;
    float releaseTime = paramToTime(s->release, 1.0f, 5000.0f);
    
    /* Osc2 master detune in Hz (0-1 → 0-15Hz) */
    float detuneHz = s->detune * 15.0f;
    
    /* Unison gain compensation: 1/sqrt(N) per oscillator */
    float uniGain = 1.0f / sqrtf((float)UNISON_VOICES);
    
    for (int i = 0; i < audio->bufferSize; i++) {
        float mixL = 0.0f, mixR = 0.0f;
        
        for (int v = 0; v < MAX_VOICES; v++) {
            Voice* vc = &s->voices[v];
            if (!vc->active) continue;
            
            /* ADSR Envelope */
            switch (vc->envStage) {
                case 1:
                    vc->envLevel += dt / (attackTime + 0.0001f);
                    if (vc->envLevel >= 1.0f) { vc->envLevel = 1.0f; vc->envStage = 2; vc->envTime = 0; }
                    break;
                case 2:
                    vc->envLevel -= dt / (decayTime + 0.0001f) * (1.0f - sustainLvl);
                    if (vc->envLevel <= sustainLvl) { vc->envLevel = sustainLvl; vc->envStage = 3; }
                    break;
                case 3:
                    break;
                case 4:
                    vc->envLevel -= dt / (releaseTime + 0.0001f) * vc->envLevel;
                    if (vc->envLevel <= 0.001f) { vc->envLevel = 0; vc->active = 0; continue; }
                    break;
                default:
                    vc->active = 0; continue;
            }
            vc->envTime += dt;
            
            /* === Oscillator 1 — Unison rendering === */
            float osc1L = 0.0f, osc1R = 0.0f;
            for (int u = 0; u < UNISON_VOICES; u++) {
                float sample = readWavetable(s->wavetable, s->osc1Wave, vc->osc1[u].phase);
                float pan = vc->osc1[u].pan;
                osc1L += sample * (0.5f - pan * 0.5f);
                osc1R += sample * (0.5f + pan * 0.5f);
                /* Advance phase with per-sub-voice detuning */
                vc->osc1[u].phase += (vc->freq * vc->osc1[u].detuneRatio) / s->sampleRate;
                if (vc->osc1[u].phase >= 1.0f) vc->osc1[u].phase -= 1.0f;
            }
            osc1L *= uniGain;
            osc1R *= uniGain;
            
            /* === Oscillator 2 — Unison rendering (with master detune) === */
            float osc2L = 0.0f, osc2R = 0.0f;
            float osc2vol = s->osc2Vol;
            if (osc2vol > 0.01f) {
                for (int u = 0; u < UNISON_VOICES; u++) {
                    float sample = readWavetable(s->wavetable, s->osc2Wave, vc->osc2[u].phase);
                    float pan = vc->osc2[u].pan;
                    osc2L += sample * (0.5f - pan * 0.5f);
                    osc2R += sample * (0.5f + pan * 0.5f);
                    vc->osc2[u].phase += ((vc->freq + detuneHz) * vc->osc2[u].detuneRatio) / s->sampleRate;
                    if (vc->osc2[u].phase >= 1.0f) vc->osc2[u].phase -= 1.0f;
                }
                osc2L *= uniGain;
                osc2R *= uniGain;
            }
            
            /* Mix oscillators */
            float sampleL = osc1L * (1.0f - osc2vol * 0.4f) + osc2L * osc2vol;
            float sampleR = osc1R * (1.0f - osc2vol * 0.4f) + osc2R * osc2vol;
            
            /* Apply envelope + velocity */
            float vel = 0.3f + 0.7f * (vc->velocity / 127.0f); /* Velocity sensitivity */
            float envGain = vc->envLevel * vel;
            sampleL *= envGain;
            sampleR *= envGain;
            
            /* Independent stereo filters — preserves width cleanly */
            sampleL = processLadder(&vc->filterL, sampleL, s->cutoff, s->resonance);
            sampleR = processLadder(&vc->filterR, sampleR, s->cutoff, s->resonance);
            
            /* Soft-clip output to prevent distortion */
            mixL += tanhf(sampleL);
            mixR += tanhf(sampleR);
        }
        
        audio->outputL[i] += mixL;
        audio->outputR[i] += mixR;
    }
}

/* ── Param get/set ── */

float pdsynth_get_param(PdSynthInstance inst, int index) {
    JTSynth* s = (JTSynth*)inst;
    switch (index) {
        case 0: return s->osc1Wave;
        case 1: return s->osc2Wave;
        case 2: return s->osc2Vol;
        case 3: return s->detune;
        case 4: return s->attack;
        case 5: return s->decay;
        case 6: return s->sustain;
        case 7: return s->release;
        case 8: return s->cutoff;
        case 9: return s->resonance;
        default: return 0;
    }
}

void pdsynth_set_param(PdSynthInstance inst, int index, float value) {
    JTSynth* s = (JTSynth*)inst;
    switch (index) {
        case 0: s->osc1Wave   = value; break;
        case 1: s->osc2Wave   = value; break;
        case 2: s->osc2Vol    = value; break;
        case 3: s->detune     = value; break;
        case 4: s->attack     = value; break;
        case 5: s->decay      = value; break;
        case 6: s->sustain    = value; break;
        case 7: s->release    = value; break;
        case 8: s->cutoff     = value; break;
        case 9: s->resonance  = value; break;
    }
}

const char* pdsynth_param_name(int index) {
    static const char* names[] = {
        "Osc1Wave", "Osc2Wave", "Osc2Vol", "Detune",
        "Attack", "Decay", "Sustain", "Release",
        "Cutoff", "Resonance"
    };
    if (index >= 0 && index < 10) return names[index];
    return NULL;
}

void pdsynth_reset(PdSynthInstance inst) {
    JTSynth* s = (JTSynth*)inst;
    for (int i = 0; i < MAX_VOICES; i++) {
        s->voices[i].active = 0;
        s->voices[i].envStage = 0;
        memset(&s->voices[i].filterL, 0, sizeof(LadderFilter));
        memset(&s->voices[i].filterR, 0, sizeof(LadderFilter));
    }
}

void pdsynth_pitch_bend(PdSynthInstance inst, float semitones) {
    (void)inst; (void)semitones;
}

void pdsynth_mod_wheel(PdSynthInstance inst, float value) {
    (void)inst; (void)value;
}

/* ── Presets ── */

int pdsynth_preset_count(void) { return NUM_PRESETS; }

const char* pdsynth_preset_name(int index) {
    if (index >= 0 && index < (int)NUM_PRESETS) return FACTORY_PRESETS[index].name;
    return NULL;
}

void pdsynth_load_preset(PdSynthInstance inst, int index) {
    JTSynth* s = (JTSynth*)inst;
    if (index < 0 || index >= (int)NUM_PRESETS) return;
    const JTPreset* p = &FACTORY_PRESETS[index];
    s->osc1Wave   = p->osc1Wave;
    s->osc2Wave   = p->osc2Wave;
    s->osc2Vol    = p->osc2Vol;
    s->detune     = p->detune;
    s->attack     = p->attack;
    s->decay      = p->decay;
    s->sustain    = p->sustain;
    s->release    = p->release;
    s->cutoff     = p->cutoff;
    s->resonance  = p->resonance;
    s->currentPreset = index;
}

int pdsynth_get_preset(PdSynthInstance inst) {
    JTSynth* s = (JTSynth*)inst;
    return s->currentPreset;
}

/* ── Waveform visualization ──
 * First half = Osc1, second half = Osc2
 */
int pdsynth_get_waveform(PdSynthInstance inst, float* buffer, int maxSamples) {
    JTSynth* s = (JTSynth*)inst;
    if (!buffer || maxSamples < 1) return 0;
    
    int halfLen = maxSamples / 2;
    if (halfLen < 1) halfLen = 1;
    
    /* Osc1 waveform (render all 3 unison voices summed) */
    for (int i = 0; i < halfLen && i < maxSamples; i++) {
        float phase = (float)i / halfLen;
        float sum = 0;
        for (int u = 0; u < UNISON_VOICES; u++) {
            float p = phase * centsToRatio(UNISON_SPREAD[u]);
            p -= (int)p;
            sum += readWavetable(s->wavetable, s->osc1Wave, p);
        }
        buffer[i] = sum / UNISON_VOICES;
    }
    
    /* Osc2 waveform */
    for (int i = 0; i < halfLen && (halfLen + i) < maxSamples; i++) {
        float phase = (float)i / halfLen;
        float sum = 0;
        for (int u = 0; u < UNISON_VOICES; u++) {
            float p = phase * centsToRatio(UNISON_SPREAD[u] + s->detune * 25.0f);
            p -= (int)p;
            sum += readWavetable(s->wavetable, s->osc2Wave, p);
        }
        buffer[halfLen + i] = sum / UNISON_VOICES;
    }
    
    return halfLen * 2;
}
