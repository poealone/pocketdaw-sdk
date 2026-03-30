/**
 * Simple Sampler — PocketDAW Synth Plugin v2 Example
 * 
 * Demonstrates the sample loading API: loads WAV files through
 * host callbacks and plays them back mapped to MIDI notes.
 * 
 * Features:
 *   - Load up to 128 samples via host callbacks
 *   - MIDI note-to-sample mapping (chromatic or key-mapped)
 *   - Pitch shifting (based on root note vs played note)
 *   - Per-voice ADSR envelope
 *   - Loop mode (one-shot or looping)
 *   - 8-voice polyphony
 * 
 * Build:
 *   aarch64-none-linux-gnu-gcc -shared -fPIC -O2 -o simple-sampler.so simple-sampler.c -lm
 */

#include "../../pdsynth_api.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#define MAX_VOICES 8
#define NUM_PARAMS 6
#define NUM_PRESETS 3

/* Parameter indices */
enum {
    P_ATTACK = 0,
    P_DECAY,
    P_SUSTAIN,
    P_RELEASE,
    P_PITCH,        /* -24 to +24 semitones (0.5 = center) */
    P_LOOP          /* 0.0 = one-shot, 1.0 = loop */
};

static const char* param_names[NUM_PARAMS] = {
    "Attack", "Decay", "Sustain", "Release", "Pitch", "Loop"
};

typedef struct {
    int active;
    int note;
    float velocity;
    
    /* Playback position (fractional for pitch shifting) */
    double position;
    double increment;   /* Playback speed (1.0 = normal) */
    
    /* ADSR envelope */
    float envLevel;
    int envStage;       /* 0=A, 1=D, 2=S, 3=R, 4=off */
    
    /* Which sample to play */
    int sampleIndex;
} Voice;

typedef struct {
    float sampleRate;
    float params[NUM_PARAMS];
    
    const PdSynthHostCallbacks* host;
    char pluginDir[256];
    
    Voice voices[MAX_VOICES];
    int currentPreset;
    
    /* Sample-to-note mapping: sampleIndex for each MIDI note */
    int noteMap[128];   /* -1 = no sample mapped */
    int rootNote;       /* Root note for pitch calc (default: 60/C4) */
    
    int samplesLoaded;
} SimpleSampler;

static const float presets[NUM_PRESETS][NUM_PARAMS] = {
    /* One-Shot */  { 0.001f, 0.1f, 1.0f, 0.1f, 0.5f, 0.0f },
    /* Pad */       { 0.2f,   0.3f, 0.7f, 0.5f, 0.5f, 1.0f },
    /* Staccato */  { 0.001f, 0.05f, 0.0f, 0.05f, 0.5f, 0.0f }
};

static const char* preset_names[NUM_PRESETS] = {
    "One-Shot", "Pad", "Staccato"
};

/* ── Voice management ──────────────────────────────────── */

static int findFreeVoice(SimpleSampler* ss) {
    /* Find inactive voice */
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!ss->voices[i].active) return i;
    }
    /* Steal oldest voice (lowest envelope) */
    int steal = 0;
    float minEnv = 999.0f;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (ss->voices[i].envLevel < minEnv) {
            minEnv = ss->voices[i].envLevel;
            steal = i;
        }
    }
    return steal;
}

static float envProcess(SimpleSampler* ss, Voice* v) {
    float a = 0.001f + ss->params[P_ATTACK] * 2.0f;    /* 1ms - 2s */
    float d = 0.001f + ss->params[P_DECAY] * 2.0f;
    float s = ss->params[P_SUSTAIN];
    float r = 0.001f + ss->params[P_RELEASE] * 2.0f;
    
    float rate;
    switch (v->envStage) {
        case 0: /* Attack */
            rate = 1.0f / (a * ss->sampleRate);
            v->envLevel += rate;
            if (v->envLevel >= 1.0f) { v->envLevel = 1.0f; v->envStage = 1; }
            break;
        case 1: /* Decay */
            rate = 1.0f / (d * ss->sampleRate);
            v->envLevel -= rate * (1.0f - s);
            if (v->envLevel <= s) { v->envLevel = s; v->envStage = 2; }
            break;
        case 2: /* Sustain */
            v->envLevel = s;
            break;
        case 3: /* Release */
            rate = 1.0f / (r * ss->sampleRate);
            v->envLevel -= rate;
            if (v->envLevel <= 0.0f) { v->envLevel = 0.0f; v->envStage = 4; v->active = 0; }
            break;
        case 4: /* Off */
            v->envLevel = 0.0f;
            v->active = 0;
            break;
    }
    return v->envLevel;
}

/* ── Required API ──────────────────────────────────────── */

int pdsynth_api_version(void) { return PDSYNTH_API_VERSION; }
const char* pdsynth_name(void) { return "Simple Sampler"; }
int pdsynth_param_count(void) { return NUM_PARAMS; }

PdSynthInstance pdsynth_create(float sampleRate) {
    SimpleSampler* ss = (SimpleSampler*)calloc(1, sizeof(SimpleSampler));
    if (!ss) return NULL;
    ss->sampleRate = sampleRate;
    ss->rootNote = 60; /* C4 */
    memset(ss->noteMap, -1, sizeof(ss->noteMap));
    /* Load default preset */
    for (int i = 0; i < NUM_PARAMS; i++)
        ss->params[i] = presets[0][i];
    return ss;
}

void pdsynth_destroy(PdSynthInstance inst) {
    free(inst);
}

void pdsynth_process(PdSynthInstance inst, PdSynthAudio* audio) {
    SimpleSampler* ss = (SimpleSampler*)inst;
    if (!ss || !ss->host) {
        /* No host = no samples, output silence */
        memset(audio->outputL, 0, audio->bufferSize * sizeof(float));
        memset(audio->outputR, 0, audio->bufferSize * sizeof(float));
        return;
    }
    
    int loopMode = ss->params[P_LOOP] > 0.5f;
    
    for (int i = 0; i < audio->bufferSize; i++) {
        float outL = 0.0f, outR = 0.0f;
        
        for (int v = 0; v < MAX_VOICES; v++) {
            Voice* voice = &ss->voices[v];
            if (!voice->active) continue;
            
            /* Get sample data */
            const PdSynthSample* smp = ss->host->get_sample(ss->host->hostData, voice->sampleIndex);
            if (!smp || !smp->dataL) { voice->active = 0; continue; }
            
            /* Check bounds */
            int pos = (int)voice->position;
            if (pos >= smp->frames) {
                if (loopMode) {
                    voice->position = fmod(voice->position, (double)smp->frames);
                    pos = (int)voice->position;
                } else {
                    voice->envStage = 3; /* trigger release */
                    if (voice->envLevel <= 0.0f) { voice->active = 0; continue; }
                }
            }
            
            if (pos < smp->frames) {
                /* Linear interpolation */
                float frac = (float)(voice->position - pos);
                int next = pos + 1;
                if (next >= smp->frames) next = loopMode ? 0 : pos;
                
                float sL = smp->dataL[pos] * (1.0f - frac) + smp->dataL[next] * frac;
                float sR = smp->dataR[pos] * (1.0f - frac) + smp->dataR[next] * frac;
                
                float env = envProcess(ss, voice);
                outL += sL * env * voice->velocity;
                outR += sR * env * voice->velocity;
            }
            
            voice->position += voice->increment;
        }
        
        audio->outputL[i] = outL;
        audio->outputR[i] = outR;
    }
}

void pdsynth_note(PdSynthInstance inst, PdSynthNote* event) {
    SimpleSampler* ss = (SimpleSampler*)inst;
    if (!ss) return;
    
    if (event->type == 1 && event->velocity > 0) {
        /* Note on */
        int smpIdx = ss->noteMap[event->note];
        if (smpIdx < 0) {
            /* No mapping — use first sample if available */
            if (ss->samplesLoaded > 0) smpIdx = 0;
            else return;
        }
        
        int vi = findFreeVoice(ss);
        Voice* v = &ss->voices[vi];
        v->active = 1;
        v->note = event->note;
        v->velocity = event->velocity / 127.0f;
        v->position = 0.0;
        v->envLevel = 0.0f;
        v->envStage = 0;
        v->sampleIndex = smpIdx;
        
        /* Calculate pitch shift */
        float pitchParam = ss->params[P_PITCH];
        float semitones = (pitchParam - 0.5f) * 48.0f; /* -24 to +24 */
        float noteShift = (float)(event->note - ss->rootNote);
        v->increment = pow(2.0, (noteShift + semitones) / 12.0);
    } else {
        /* Note off — trigger release on matching voices */
        for (int i = 0; i < MAX_VOICES; i++) {
            if (ss->voices[i].active && ss->voices[i].note == event->note) {
                ss->voices[i].envStage = 3;
            }
        }
    }
}

float pdsynth_get_param(PdSynthInstance inst, int index) {
    SimpleSampler* ss = (SimpleSampler*)inst;
    if (!ss || index < 0 || index >= NUM_PARAMS) return 0.0f;
    return ss->params[index];
}

void pdsynth_set_param(PdSynthInstance inst, int index, float value) {
    SimpleSampler* ss = (SimpleSampler*)inst;
    if (!ss || index < 0 || index >= NUM_PARAMS) return;
    ss->params[index] = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    ss->currentPreset = -1;
}

/* ── v2: Host Callbacks ────────────────────────────────── */

void pdsynth_set_host(PdSynthInstance inst, const PdSynthHostCallbacks* host) {
    SimpleSampler* ss = (SimpleSampler*)inst;
    if (!ss) return;
    ss->host = host;
    if (host && host->log) {
        host->log(host->hostData, "Simple Sampler: host callbacks received");
    }
}

void pdsynth_set_plugin_dir(PdSynthInstance inst, const char* dir) {
    SimpleSampler* ss = (SimpleSampler*)inst;
    if (!ss || !dir) return;
    strncpy(ss->pluginDir, dir, 255);
    
    /* Auto-load samples from our samples/ directory */
    if (ss->host && ss->host->load_sample) {
        /* Try loading samples/sample_01.wav through samples/sample_16.wav */
        char path[128];
        for (int i = 1; i <= 16; i++) {
            snprintf(path, sizeof(path), "samples/sample_%02d.wav", i);
            const PdSynthSample* smp = ss->host->load_sample(ss->host->hostData, path);
            if (smp) {
                /* Map to MIDI notes starting at C3 (48) */
                int note = 48 + (ss->samplesLoaded);
                if (note < 128) ss->noteMap[note] = ss->samplesLoaded;
                ss->samplesLoaded++;
            }
        }
        if (ss->host->log) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Simple Sampler: loaded %d samples", ss->samplesLoaded);
            ss->host->log(ss->host->hostData, msg);
        }
    }
}

/* ── Optional API ──────────────────────────────────────── */

const char* pdsynth_param_name(int index) {
    if (index < 0 || index >= NUM_PARAMS) return NULL;
    return param_names[index];
}

void pdsynth_reset(PdSynthInstance inst) {
    SimpleSampler* ss = (SimpleSampler*)inst;
    if (!ss) return;
    for (int i = 0; i < MAX_VOICES; i++) ss->voices[i].active = 0;
}

int pdsynth_preset_count(void) { return NUM_PRESETS; }

const char* pdsynth_preset_name(int index) {
    if (index < 0 || index >= NUM_PRESETS) return NULL;
    return preset_names[index];
}

void pdsynth_load_preset(PdSynthInstance inst, int index) {
    SimpleSampler* ss = (SimpleSampler*)inst;
    if (!ss || index < 0 || index >= NUM_PRESETS) return;
    for (int i = 0; i < NUM_PARAMS; i++)
        ss->params[i] = presets[index][i];
    ss->currentPreset = index;
}

int pdsynth_get_preset(PdSynthInstance inst) {
    SimpleSampler* ss = (SimpleSampler*)inst;
    return ss ? ss->currentPreset : -1;
}
