/**
 * PocketDAW Plugin: Drum Machine
 * 
 * Synthesized drum sounds mapped to MIDI notes:
 *   C2 (36) = Kick
 *   D2 (38) = Snare
 *   F#2 (42) = Closed Hi-Hat
 *   A#2 (46) = Open Hi-Hat
 *   C#2 (37) = Rimshot
 *   D#2 (39) = Clap
 *   G#2 (44) = Pedal Hi-Hat
 *   A2 (45) = Tom
 * 
 * Each drum is a synthesis recipe (sine + noise + pitch envelope).
 * 8 simultaneous sounds, no polyphony needed per drum.
 */
#include "../../pdsynth_api.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HITS 8
#define TWO_PI 6.28318530718f

enum {
    P_KICK_TUNE = 0,
    P_KICK_DECAY,
    P_SNARE_TONE,
    P_SNARE_SNAP,
    P_HAT_TONE,
    P_HAT_DECAY,
    P_MASTER_DRIVE,
    P_ROOM,
    PARAM_COUNT
};

typedef enum {
    DRUM_KICK = 0,
    DRUM_SNARE,
    DRUM_CLOSED_HAT,
    DRUM_OPEN_HAT,
    DRUM_RIM,
    DRUM_CLAP,
    DRUM_PEDAL_HAT,
    DRUM_TOM,
    DRUM_COUNT
} DrumType;

typedef struct {
    DrumType type;
    float phase;
    float time;      // Time since trigger (seconds)
    float velocity;
    int active;
    float pitchEnv;
    uint32_t seed;
} DrumHit;

typedef struct {
    float sampleRate;
    float params[PARAM_COUNT];
    DrumHit hits[MAX_HITS];
    uint32_t seed;
    float roomBuf[4410]; // ~100ms reverb buffer
    int roomPos;
} DrumMachine;

static float frand(uint32_t* s) {
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) / 16777216.0f;
}

static DrumType noteToType(int8_t note) {
    switch (note) {
        case 36: return DRUM_KICK;
        case 38: return DRUM_SNARE;
        case 42: return DRUM_CLOSED_HAT;
        case 46: return DRUM_OPEN_HAT;
        case 37: return DRUM_RIM;
        case 39: return DRUM_CLAP;
        case 44: return DRUM_PEDAL_HAT;
        case 45: return DRUM_TOM;
        default: // Map any other note to nearest drum
            if (note < 37) return DRUM_KICK;
            if (note < 39) return DRUM_SNARE;
            if (note < 43) return DRUM_CLOSED_HAT;
            if (note < 46) return DRUM_OPEN_HAT;
            return DRUM_TOM;
    }
}

static float synthDrum(DrumHit* h, float* params, float sr) {
    float t = h->time;
    float vel = h->velocity;
    float out = 0;
    
    switch (h->type) {
        case DRUM_KICK: {
            float tune = params[P_KICK_TUNE] * 60.0f + 40.0f; // 40-100 Hz
            float decay = params[P_KICK_DECAY] * 0.5f + 0.1f;
            float pitchDrop = tune * 4.0f * expf(-t * 30.0f) + tune;
            float body = sinf(h->phase * TWO_PI) * expf(-t / decay);
            float click = (frand(&h->seed) * 2 - 1) * expf(-t * 200.0f) * 0.3f;
            h->phase += pitchDrop / sr;
            out = (body + click) * vel;
            if (t > decay * 4) h->active = 0;
            break;
        }
        case DRUM_SNARE: {
            float tone = params[P_SNARE_TONE] * 150.0f + 150.0f; // 150-300 Hz
            float snap = params[P_SNARE_SNAP] * 0.8f + 0.2f;
            float body = sinf(h->phase * TWO_PI) * expf(-t * 15.0f);
            float noise = (frand(&h->seed) * 2 - 1) * expf(-t / (snap * 0.2f));
            h->phase += tone / sr;
            out = (body * 0.5f + noise * 0.6f) * vel;
            if (t > 0.5f) h->active = 0;
            break;
        }
        case DRUM_CLOSED_HAT: {
            float tone = params[P_HAT_TONE];
            float decay = params[P_HAT_DECAY] * 0.05f + 0.02f;
            float n1 = sinf(h->phase * TWO_PI * (4000 + tone * 4000));
            float n2 = sinf(h->phase * TWO_PI * (6000 + tone * 3000) * 1.414f);
            float env = expf(-t / decay);
            h->phase += 1.0f / sr;
            out = (n1 * 0.5f + n2 * 0.5f) * env * vel * 0.5f;
            if (t > decay * 5) h->active = 0;
            break;
        }
        case DRUM_OPEN_HAT: {
            float tone = params[P_HAT_TONE];
            float decay = params[P_HAT_DECAY] * 0.3f + 0.1f;
            float n1 = sinf(h->phase * TWO_PI * (4000 + tone * 4000));
            float n2 = sinf(h->phase * TWO_PI * (7500 + tone * 2000));
            float env = expf(-t / decay);
            h->phase += 1.0f / sr;
            out = (n1 * 0.4f + n2 * 0.4f + (frand(&h->seed) - 0.5f) * 0.2f) * env * vel * 0.5f;
            if (t > decay * 4) h->active = 0;
            break;
        }
        case DRUM_RIM: {
            float freq = 800.0f * expf(-t * 60.0f) + 400.0f;
            float body = sinf(h->phase * TWO_PI) * expf(-t * 40.0f);
            h->phase += freq / sr;
            out = body * vel * 0.7f;
            if (t > 0.1f) h->active = 0;
            break;
        }
        case DRUM_CLAP: {
            // Multiple noise bursts
            float env1 = expf(-fmodf(t, 0.02f) * 200.0f);
            float env2 = expf(-t / 0.15f);
            float burst = (t < 0.03f) ? env1 : 0;
            float tail = (frand(&h->seed) * 2 - 1) * env2;
            out = (burst * 0.5f + tail * 0.6f) * vel;
            if (t > 0.4f) h->active = 0;
            break;
        }
        case DRUM_PEDAL_HAT: {
            float env = expf(-t * 80.0f);
            float n = sinf(h->phase * TWO_PI * 5000);
            h->phase += 1.0f / sr;
            out = n * env * vel * 0.3f;
            if (t > 0.05f) h->active = 0;
            break;
        }
        case DRUM_TOM: {
            float freq = 120.0f + 200.0f * expf(-t * 20.0f);
            float body = sinf(h->phase * TWO_PI) * expf(-t / 0.3f);
            h->phase += freq / sr;
            out = body * vel * 0.8f;
            if (t > 0.8f) h->active = 0;
            break;
        }
        default: break;
    }
    return out;
}

// ── API ──

int pdsynth_api_version(void) { return PDSYNTH_API_VERSION; }
const char* pdsynth_name(void) { return "Drum Machine"; }
int pdsynth_param_count(void) { return PARAM_COUNT; }

const char* pdsynth_param_name(int idx) {
    static const char* names[] = {
        "Kick Tune", "Kick Decay", "Snare Tone", "Snare Snap",
        "Hat Tone", "Hat Decay", "Drive", "Room"
    };
    if (idx >= 0 && idx < PARAM_COUNT) return names[idx];
    return "?";
}

PdSynthInstance pdsynth_create(float sampleRate) {
    DrumMachine* s = (DrumMachine*)calloc(1, sizeof(DrumMachine));
    s->sampleRate = sampleRate;
    s->params[P_KICK_TUNE] = 0.4f;
    s->params[P_KICK_DECAY] = 0.5f;
    s->params[P_SNARE_TONE] = 0.5f;
    s->params[P_SNARE_SNAP] = 0.5f;
    s->params[P_HAT_TONE] = 0.5f;
    s->params[P_HAT_DECAY] = 0.3f;
    s->params[P_MASTER_DRIVE] = 0.3f;
    s->params[P_ROOM] = 0.2f;
    s->seed = 12345;
    return (PdSynthInstance)s;
}

void pdsynth_destroy(PdSynthInstance inst) { free(inst); }

void pdsynth_note(PdSynthInstance inst, PdSynthNote* note) {
    DrumMachine* s = (DrumMachine*)inst;
    if (note->type != 1) return; // Only note-on (drums don't need note-off)
    
    DrumType dt = noteToType(note->note);
    
    // Choke: close hat kills open hat
    if (dt == DRUM_CLOSED_HAT || dt == DRUM_PEDAL_HAT) {
        for (int i = 0; i < MAX_HITS; i++) {
            if (s->hits[i].active && s->hits[i].type == DRUM_OPEN_HAT) {
                s->hits[i].active = 0;
            }
        }
    }
    
    // Find free slot or steal same type
    int slot = -1;
    for (int i = 0; i < MAX_HITS; i++) {
        if (!s->hits[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        // Steal same type, or oldest
        for (int i = 0; i < MAX_HITS; i++) {
            if (s->hits[i].type == dt) { slot = i; break; }
        }
        if (slot < 0) slot = 0;
    }
    
    DrumHit* h = &s->hits[slot];
    memset(h, 0, sizeof(DrumHit));
    h->type = dt;
    h->velocity = note->velocity / 127.0f;
    h->active = 1;
    h->seed = s->seed++;
}

void pdsynth_process(PdSynthInstance inst, PdSynthAudio* audio) {
    DrumMachine* s = (DrumMachine*)inst;
    float drive = 1.0f + s->params[P_MASTER_DRIVE] * 3.0f;
    float room = s->params[P_ROOM] * 0.3f;
    float dt = 1.0f / s->sampleRate;
    
    for (int i = 0; i < audio->bufferSize; i++) {
        float mix = 0;
        
        for (int h = 0; h < MAX_HITS; h++) {
            if (!s->hits[h].active) continue;
            mix += synthDrum(&s->hits[h], s->params, s->sampleRate);
            s->hits[h].time += dt;
        }
        
        // Soft clip (drive)
        mix *= drive;
        if (mix > 1.0f) mix = 1.0f - 1.0f / (mix + 1.0f);
        else if (mix < -1.0f) mix = -1.0f + 1.0f / (-mix + 1.0f);
        
        // Simple room (comb filter)
        int roomDelay = (int)(s->sampleRate * 0.023f); // ~23ms
        float roomSample = s->roomBuf[s->roomPos];
        s->roomBuf[s->roomPos] = mix + roomSample * room * 0.6f;
        s->roomPos = (s->roomPos + 1) % roomDelay;
        
        float wet = mix + roomSample * room;
        
        audio->outputL[i] += wet;
        audio->outputR[i] += wet;
    }
}

float pdsynth_get_param(PdSynthInstance inst, int idx) {
    DrumMachine* s = (DrumMachine*)inst;
    if (idx >= 0 && idx < PARAM_COUNT) return s->params[idx];
    return 0;
}

void pdsynth_set_param(PdSynthInstance inst, int idx, float val) {
    DrumMachine* s = (DrumMachine*)inst;
    if (idx >= 0 && idx < PARAM_COUNT) s->params[idx] = val;
}

void pdsynth_reset(PdSynthInstance inst) {
    DrumMachine* s = (DrumMachine*)inst;
    for (int i = 0; i < MAX_HITS; i++) s->hits[i].active = 0;
    memset(s->roomBuf, 0, sizeof(s->roomBuf));
    s->roomPos = 0;
}
