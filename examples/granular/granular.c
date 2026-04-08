/**
 * PocketDAW Plugin: Granular Synth
 * 
 * Generates sound from overlapping micro-grains of basic waveforms.
 * Creates evolving textures, pads, and atmospheric sounds.
 * 4 grains running simultaneously, each with randomized parameters.
 */
#include "../../pocketdaw.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_GRAINS 8
#define MAX_VOICES 4
#define TWO_PI 6.28318530718f
#define GRAIN_BUF_SIZE 4410  // Max grain = 100ms at 44100

enum {
    P_DENSITY = 0,   // Grain rate (grains per second)
    P_SIZE,          // Grain size (duration)
    P_PITCH_SPREAD,  // Random pitch variation
    P_POSITION,      // Scan position through waveform
    P_ATTACK,        // Note attack
    P_RELEASE,       // Note release
    P_STEREO,        // Stereo spread
    P_TEXTURE,       // Waveform blend (sine→noise)
    PARAM_COUNT
};

typedef struct {
    float phase;
    float freq;
    float pan;       // -1 to 1
    float env;       // Grain envelope position 0-1
    float envRate;
    int active;
    float waveform;  // 0=sine, 1=noise-ish
} Grain;

typedef struct {
    int8_t note;
    uint8_t velocity;
    float env;
    int stage;       // 0=off, 1=atk, 2=sus, 3=rel
    float relStart;
    Grain grains[MAX_GRAINS];
    float spawnTimer;
    uint32_t seed;   // Per-voice PRNG
} GranVoice;

typedef struct {
    float sampleRate;
    float params[PARAM_COUNT];
    GranVoice voices[MAX_VOICES];
    uint32_t globalSeed;
} GranSynth;

// Simple fast PRNG
static float frandf(uint32_t* seed) {
    *seed = *seed * 1664525u + 1013904223u;
    return (float)(*seed >> 8) / 16777216.0f;
}

static float grainWaveform(float phase, float texture, uint32_t* seed) {
    float sine = sinf(TWO_PI * phase);
    float noise = frandf(seed) * 2.0f - 1.0f;
    return sine * (1.0f - texture) + noise * texture;
}

// Hann window for grain envelope
static float hannWindow(float t) {
    return 0.5f * (1.0f - cosf(TWO_PI * t));
}

// ── API ──

int pdsynth_api_version(void) { return PDSYNTH_API_VERSION; }
const char* pdsynth_name(void) { return "Granular"; }
int pdsynth_param_count(void) { return PARAM_COUNT; }

const char* pdsynth_param_name(int idx) {
    static const char* names[] = {
        "Density", "Grain Size", "Pitch Spread", "Position", 
        "Attack", "Release", "Stereo", "Texture"
    };
    if (idx >= 0 && idx < PARAM_COUNT) return names[idx];
    return "?";
}

PdSynthInstance pdsynth_create(float sampleRate) {
    GranSynth* s = (GranSynth*)calloc(1, sizeof(GranSynth));
    s->sampleRate = sampleRate;
    s->params[P_DENSITY] = 0.5f;
    s->params[P_SIZE] = 0.3f;
    s->params[P_PITCH_SPREAD] = 0.1f;
    s->params[P_POSITION] = 0.5f;
    s->params[P_ATTACK] = 0.1f;
    s->params[P_RELEASE] = 0.4f;
    s->params[P_STEREO] = 0.5f;
    s->params[P_TEXTURE] = 0.2f;
    s->globalSeed = 42;
    return (PdSynthInstance)s;
}

void pdsynth_destroy(PdSynthInstance inst) { free(inst); }

void pdsynth_note(PdSynthInstance inst, PdSynthNote* note) {
    GranSynth* s = (GranSynth*)inst;
    if (note->type == 1) {
        int slot = -1;
        for (int i = 0; i < MAX_VOICES; i++) {
            if (s->voices[i].stage == 0) { slot = i; break; }
        }
        if (slot < 0) slot = 0;
        
        GranVoice* v = &s->voices[slot];
        memset(v, 0, sizeof(GranVoice));
        v->note = note->note;
        v->velocity = note->velocity;
        v->stage = 1;
        v->seed = s->globalSeed++;
    } else {
        for (int i = 0; i < MAX_VOICES; i++) {
            if (s->voices[i].note == note->note && s->voices[i].stage > 0 && s->voices[i].stage < 3) {
                s->voices[i].stage = 3;
                s->voices[i].relStart = s->voices[i].env;
            }
        }
    }
}

void pdsynth_process(PdSynthInstance inst, PdSynthAudio* audio) {
    GranSynth* s = (GranSynth*)inst;
    float density = s->params[P_DENSITY] * 40.0f + 2.0f;  // 2-42 grains/sec
    float grainSize = s->params[P_SIZE] * 0.09f + 0.01f;   // 10-100ms
    float pitchSpread = s->params[P_PITCH_SPREAD];
    float atk = s->params[P_ATTACK] * 2.0f + 0.001f;
    float rel = s->params[P_RELEASE] * 3.0f + 0.001f;
    float stereoW = s->params[P_STEREO];
    float texture = s->params[P_TEXTURE];
    
    float spawnInterval = s->sampleRate / density;
    
    for (int i = 0; i < audio->bufferSize; i++) {
        float outL = 0, outR = 0;
        
        for (int vi = 0; vi < MAX_VOICES; vi++) {
            GranVoice* v = &s->voices[vi];
            if (v->stage == 0) continue;
            
            // Voice envelope
            float atkRate = 1.0f / (atk * s->sampleRate);
            float relRate = 1.0f / (rel * s->sampleRate);
            switch (v->stage) {
                case 1:
                    v->env += atkRate;
                    if (v->env >= 1.0f) { v->env = 1.0f; v->stage = 2; }
                    break;
                case 3:
                    v->env -= v->relStart * relRate;
                    if (v->env <= 0.0f) { v->env = 0; v->stage = 0; continue; }
                    break;
            }
            
            // Spawn new grains
            v->spawnTimer -= 1.0f;
            if (v->spawnTimer <= 0) {
                v->spawnTimer += spawnInterval;
                // Find free grain slot
                for (int g = 0; g < MAX_GRAINS; g++) {
                    if (!v->grains[g].active) {
                        Grain* gr = &v->grains[g];
                        float baseFreq = 440.0f * powf(2.0f, ((int)v->note - 69) / 12.0f);
                        float pitchVar = 1.0f + (frandf(&v->seed) - 0.5f) * pitchSpread;
                        gr->freq = baseFreq * pitchVar;
                        gr->phase = frandf(&v->seed);
                        gr->pan = (frandf(&v->seed) - 0.5f) * 2.0f * stereoW;
                        gr->env = 0;
                        gr->envRate = 1.0f / (grainSize * s->sampleRate);
                        gr->active = 1;
                        gr->waveform = texture;
                        break;
                    }
                }
            }
            
            // Process grains
            float vel = v->velocity / 127.0f;
            for (int g = 0; g < MAX_GRAINS; g++) {
                Grain* gr = &v->grains[g];
                if (!gr->active) continue;
                
                float sample = grainWaveform(gr->phase, gr->waveform, &v->seed);
                float window = hannWindow(gr->env);
                float amp = sample * window * v->env * vel * 0.15f;
                
                float panL = 1.0f - gr->pan * 0.5f;
                float panR = 1.0f + gr->pan * 0.5f;
                outL += amp * panL;
                outR += amp * panR;
                
                gr->phase += gr->freq / s->sampleRate;
                if (gr->phase >= 1.0f) gr->phase -= 1.0f;
                gr->env += gr->envRate;
                if (gr->env >= 1.0f) gr->active = 0;
            }
        }
        
        audio->outputL[i] += outL;
        audio->outputR[i] += outR;
    }
}

float pdsynth_get_param(PdSynthInstance inst, int idx) {
    GranSynth* s = (GranSynth*)inst;
    if (idx >= 0 && idx < PARAM_COUNT) return s->params[idx];
    return 0;
}

void pdsynth_set_param(PdSynthInstance inst, int idx, float val) {
    GranSynth* s = (GranSynth*)inst;
    if (idx >= 0 && idx < PARAM_COUNT) s->params[idx] = val;
}

void pdsynth_reset(PdSynthInstance inst) {
    GranSynth* s = (GranSynth*)inst;
    for (int i = 0; i < MAX_VOICES; i++) {
        s->voices[i].stage = 0;
        for (int g = 0; g < MAX_GRAINS; g++) s->voices[i].grains[g].active = 0;
    }
}
