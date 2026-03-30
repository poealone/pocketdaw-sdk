/**
 * Example PocketDAW Synth Plugin: Simple 2-operator FM Synth
 * 
 * Build:
 *   aarch64-none-linux-gnu-gcc -shared -fPIC -O2 -o fm-synth.so fm-synth.c -lm
 */

#include "../../pdsynth_api.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_VOICES 8

typedef struct {
    uint8_t note;
    uint8_t velocity;
    float   phase1;     // Carrier phase
    float   phase2;     // Modulator phase
    float   envLevel;   // Simple envelope
    float   envStage;   // 0=off, 1=attack, 2=sustain, 3=release
    int     active;
} Voice;

typedef struct {
    float sampleRate;
    Voice voices[MAX_VOICES];
    
    // Parameters (normalized 0-1)
    float ratio;        // Modulator ratio (mapped to 0.5-8.0)
    float modDepth;     // Modulation depth
    float attack;       // Attack time
    float release;      // Release time
    float brightness;   // High-frequency content
    float detune;       // Slight detune for thickness
} FMSynth;

/* ── Required exports ─────────────────────────────────── */

int pdsynth_api_version(void) { return PDSYNTH_API_VERSION; }
const char* pdsynth_name(void) { return "FM Synth"; }
int pdsynth_param_count(void) { return 6; }

PdSynthInstance pdsynth_create(float sampleRate) {
    FMSynth* s = (FMSynth*)calloc(1, sizeof(FMSynth));
    s->sampleRate = sampleRate;
    s->ratio = 0.25f;       // Default: ratio 2.0
    s->modDepth = 0.3f;
    s->attack = 0.01f;
    s->release = 0.3f;
    s->brightness = 0.5f;
    s->detune = 0.0f;
    return (PdSynthInstance)s;
}

void pdsynth_destroy(PdSynthInstance inst) {
    free(inst);
}

static float noteToFreq(uint8_t note) {
    return 440.0f * powf(2.0f, ((int)note - 69) / 12.0f);
}

void pdsynth_process(PdSynthInstance inst, PdSynthAudio* audio) {
    FMSynth* s = (FMSynth*)inst;
    float sr = audio->sampleRate;
    
    // Map parameters
    float modRatio = 0.5f + s->ratio * 7.5f;   // 0.5 - 8.0
    float modIdx = s->modDepth * 8.0f;          // 0 - 8 
    float atkRate = 1.0f / (sr * (0.001f + s->attack * 0.5f));
    float relRate = 1.0f / (sr * (0.01f + s->release * 2.0f));
    
    memset(audio->outputL, 0, audio->bufferSize * sizeof(float));
    memset(audio->outputR, 0, audio->bufferSize * sizeof(float));
    
    for (int v = 0; v < MAX_VOICES; v++) {
        Voice* vc = &s->voices[v];
        if (!vc->active && vc->envStage == 0) continue;
        
        float freq = noteToFreq(vc->note);
        float vel = vc->velocity / 127.0f;
        float phase1Inc = freq / sr;
        float phase2Inc = freq * modRatio / sr;
        
        for (int i = 0; i < audio->bufferSize; i++) {
            // Envelope
            if (vc->envStage == 1) {
                vc->envLevel += atkRate;
                if (vc->envLevel >= 1.0f) { vc->envLevel = 1.0f; vc->envStage = 2; }
            } else if (vc->envStage == 3) {
                vc->envLevel -= relRate;
                if (vc->envLevel <= 0.0f) { vc->envLevel = 0.0f; vc->envStage = 0; vc->active = 0; }
            }
            
            // FM synthesis: modulator → carrier
            float mod = sinf(vc->phase2 * 2.0f * M_PI) * modIdx;
            float sample = sinf((vc->phase1 + mod) * 2.0f * M_PI);
            
            sample *= vc->envLevel * vel * 0.3f;
            
            audio->outputL[i] += sample;
            audio->outputR[i] += sample;
            
            vc->phase1 += phase1Inc;
            vc->phase2 += phase2Inc;
            if (vc->phase1 > 1.0f) vc->phase1 -= 1.0f;
            if (vc->phase2 > 1.0f) vc->phase2 -= 1.0f;
        }
    }
}

void pdsynth_note(PdSynthInstance inst, PdSynthNote* event) {
    FMSynth* s = (FMSynth*)inst;
    
    if (event->type == 1) {
        // Note on — find free voice
        int slot = -1;
        for (int i = 0; i < MAX_VOICES; i++) {
            if (!s->voices[i].active && s->voices[i].envStage == 0) { slot = i; break; }
        }
        if (slot < 0) slot = 0; // Voice steal
        
        s->voices[slot].note = event->note;
        s->voices[slot].velocity = event->velocity;
        s->voices[slot].phase1 = 0;
        s->voices[slot].phase2 = 0;
        s->voices[slot].envLevel = 0;
        s->voices[slot].envStage = 1; // Attack
        s->voices[slot].active = 1;
    } else {
        // Note off — release matching voice
        for (int i = 0; i < MAX_VOICES; i++) {
            if (s->voices[i].active && s->voices[i].note == event->note) {
                s->voices[i].envStage = 3; // Release
                s->voices[i].active = 0;
            }
        }
    }
}

float pdsynth_get_param(PdSynthInstance inst, int index) {
    FMSynth* s = (FMSynth*)inst;
    switch (index) {
        case 0: return s->ratio;
        case 1: return s->modDepth;
        case 2: return s->attack;
        case 3: return s->release;
        case 4: return s->brightness;
        case 5: return s->detune;
        default: return 0;
    }
}

void pdsynth_set_param(PdSynthInstance inst, int index, float value) {
    FMSynth* s = (FMSynth*)inst;
    if (value < 0) value = 0; if (value > 1) value = 1;
    switch (index) {
        case 0: s->ratio = value; break;
        case 1: s->modDepth = value; break;
        case 2: s->attack = value; break;
        case 3: s->release = value; break;
        case 4: s->brightness = value; break;
        case 5: s->detune = value; break;
    }
}

/* ── Optional exports ─────────────────────────────────── */

const char* pdsynth_param_name(int index) {
    static const char* names[] = {
        "Mod Ratio", "Mod Depth", "Attack", "Release", "Brightness", "Detune"
    };
    return (index >= 0 && index < 6) ? names[index] : NULL;
}

void pdsynth_reset(PdSynthInstance inst) {
    FMSynth* s = (FMSynth*)inst;
    for (int i = 0; i < MAX_VOICES; i++) {
        s->voices[i].active = 0;
        s->voices[i].envStage = 0;
        s->voices[i].envLevel = 0;
    }
}
