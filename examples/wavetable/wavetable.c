/**
 * PocketDAW Plugin: Wavetable Synth
 * 
 * Morphable wavetable synthesis with 8 waveforms.
 * Crossfade between adjacent waveforms via Position parameter.
 * 4-voice polyphony with ADSR envelope.
 */
#include "../../pocketdaw.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WT_SIZE 2048
#define NUM_WAVES 8
#define MAX_VOICES 4
#define TWO_PI 6.28318530718f

// Parameters
enum {
    P_POSITION = 0,  // Wavetable position (0-1 morphs across 8 waves)
    P_ATTACK,
    P_DECAY,
    P_SUSTAIN,
    P_RELEASE,
    P_DETUNE,        // Stereo detune spread
    P_BRIGHTNESS,    // Low-pass filter cutoff
    P_CHORUS,        // Chorus depth
    PARAM_COUNT
};

typedef struct {
    float phase;
    float phaseR;     // Detuned phase for stereo
    int8_t note;
    uint8_t velocity;
    float env;
    float envTarget;
    int stage;        // 0=off, 1=attack, 2=decay, 3=sustain, 4=release
    float releaseStart;
} WTVoice;

typedef struct {
    float sampleRate;
    float params[PARAM_COUNT];
    float wavetable[NUM_WAVES][WT_SIZE];
    WTVoice voices[MAX_VOICES];
    float lpState[2]; // Simple LP filter state (L/R)
} WTSynth;

static void generateWavetables(WTSynth* s) {
    for (int i = 0; i < WT_SIZE; i++) {
        float t = (float)i / WT_SIZE;
        // Wave 0: Sine
        s->wavetable[0][i] = sinf(TWO_PI * t);
        // Wave 1: Triangle
        s->wavetable[1][i] = (t < 0.5f) ? (4.0f * t - 1.0f) : (3.0f - 4.0f * t);
        // Wave 2: Saw
        s->wavetable[2][i] = 2.0f * t - 1.0f;
        // Wave 3: Square
        s->wavetable[3][i] = (t < 0.5f) ? 1.0f : -1.0f;
        // Wave 4: PWM 25%
        s->wavetable[4][i] = (t < 0.25f) ? 1.0f : -1.0f;
        // Wave 5: Half-rectified sine
        s->wavetable[5][i] = sinf(TWO_PI * t);
        if (s->wavetable[5][i] < 0) s->wavetable[5][i] = 0;
        s->wavetable[5][i] = s->wavetable[5][i] * 2.0f - 1.0f;
        // Wave 6: Organ (sine + 3rd + 5th harmonic)
        s->wavetable[6][i] = sinf(TWO_PI * t) * 0.6f + sinf(TWO_PI * t * 3) * 0.25f + sinf(TWO_PI * t * 5) * 0.15f;
        // Wave 7: Noise-ish (ring mod of harmonics)
        s->wavetable[7][i] = sinf(TWO_PI * t) * sinf(TWO_PI * t * 7) * 0.8f + sinf(TWO_PI * t * 13) * 0.2f;
    }
}

static float readWavetable(WTSynth* s, float pos, float phase) {
    // Interpolate between two adjacent waveforms
    float wf = pos * (NUM_WAVES - 1);
    int w0 = (int)wf;
    int w1 = w0 + 1;
    if (w0 < 0) w0 = 0;
    if (w1 >= NUM_WAVES) w1 = NUM_WAVES - 1;
    if (w0 >= NUM_WAVES) w0 = NUM_WAVES - 1;
    float mix = wf - (float)w0;
    
    // Table lookup with linear interpolation
    float idx = phase * WT_SIZE;
    int i0 = (int)idx;
    int i1 = (i0 + 1) % WT_SIZE;
    float frac = idx - (float)i0;
    i0 = i0 % WT_SIZE;
    
    float s0 = s->wavetable[w0][i0] * (1.0f - frac) + s->wavetable[w0][i1] * frac;
    float s1 = s->wavetable[w1][i0] * (1.0f - frac) + s->wavetable[w1][i1] * frac;
    
    return s0 * (1.0f - mix) + s1 * mix;
}

// ── API Implementation ──

int pdsynth_api_version(void) { return PDSYNTH_API_VERSION; }
const char* pdsynth_name(void) { return "Wavetable"; }
int pdsynth_param_count(void) { return PARAM_COUNT; }

const char* pdsynth_param_name(int idx) {
    static const char* names[] = {
        "Position", "Attack", "Decay", "Sustain", "Release", "Detune", "Brightness", "Chorus"
    };
    if (idx >= 0 && idx < PARAM_COUNT) return names[idx];
    return "?";
}

PdSynthInstance pdsynth_create(float sampleRate) {
    WTSynth* s = (WTSynth*)calloc(1, sizeof(WTSynth));
    s->sampleRate = sampleRate;
    s->params[P_POSITION] = 0.0f;
    s->params[P_ATTACK] = 0.05f;
    s->params[P_DECAY] = 0.3f;
    s->params[P_SUSTAIN] = 0.7f;
    s->params[P_RELEASE] = 0.3f;
    s->params[P_DETUNE] = 0.1f;
    s->params[P_BRIGHTNESS] = 0.8f;
    s->params[P_CHORUS] = 0.2f;
    generateWavetables(s);
    return (PdSynthInstance)s;
}

void pdsynth_destroy(PdSynthInstance inst) { free(inst); }

void pdsynth_note(PdSynthInstance inst, PdSynthNote* note) {
    WTSynth* s = (WTSynth*)inst;
    if (note->type == 1) { // Note on
        // Find free or steal oldest
        int slot = -1;
        for (int i = 0; i < MAX_VOICES; i++) {
            if (s->voices[i].stage == 0) { slot = i; break; }
        }
        if (slot < 0) slot = 0; // Steal voice 0
        
        WTVoice* v = &s->voices[slot];
        v->note = note->note;
        v->velocity = note->velocity;
        v->phase = 0;
        v->phaseR = 0;
        v->env = 0;
        v->stage = 1;
    } else { // Note off
        for (int i = 0; i < MAX_VOICES; i++) {
            if (s->voices[i].note == note->note && s->voices[i].stage > 0 && s->voices[i].stage < 4) {
                s->voices[i].stage = 4;
                s->voices[i].releaseStart = s->voices[i].env;
            }
        }
    }
}

void pdsynth_process(PdSynthInstance inst, PdSynthAudio* audio) {
    WTSynth* s = (WTSynth*)inst;
    float pos = s->params[P_POSITION];
    float atk = s->params[P_ATTACK] * 2.0f + 0.001f;
    float dec = s->params[P_DECAY] * 2.0f + 0.001f;
    float sus = s->params[P_SUSTAIN];
    float rel = s->params[P_RELEASE] * 3.0f + 0.001f;
    float detAmt = s->params[P_DETUNE] * 0.02f;
    float bright = s->params[P_BRIGHTNESS];
    float chorusD = s->params[P_CHORUS] * 0.005f;
    
    float lpCoeff = bright * 0.9f + 0.1f; // 0.1 to 1.0
    
    for (int i = 0; i < audio->bufferSize; i++) {
        float mixL = 0, mixR = 0;
        
        for (int v = 0; v < MAX_VOICES; v++) {
            WTVoice* vc = &s->voices[v];
            if (vc->stage == 0) continue;
            
            // ADSR
            float atkRate = 1.0f / (atk * s->sampleRate);
            float decRate = 1.0f / (dec * s->sampleRate);
            float relRate = 1.0f / (rel * s->sampleRate);
            
            switch (vc->stage) {
                case 1: // Attack
                    vc->env += atkRate;
                    if (vc->env >= 1.0f) { vc->env = 1.0f; vc->stage = 2; }
                    break;
                case 2: // Decay
                    vc->env -= (1.0f - sus) * decRate;
                    if (vc->env <= sus) { vc->env = sus; vc->stage = 3; }
                    break;
                case 3: // Sustain
                    break;
                case 4: // Release
                    vc->env -= vc->releaseStart * relRate;
                    if (vc->env <= 0.0f) { vc->env = 0; vc->stage = 0; continue; }
                    break;
            }
            
            // Frequency
            float freq = 440.0f * powf(2.0f, ((int)vc->note - 69) / 12.0f);
            float inc = freq / s->sampleRate;
            float incR = inc * (1.0f + detAmt);
            
            // Chorus modulation
            float chorusMod = 1.0f + sinf(vc->phase * TWO_PI * 0.1f) * chorusD;
            
            // Read wavetable
            float sL = readWavetable(s, pos, vc->phase);
            float sR = readWavetable(s, pos, vc->phaseR);
            
            float vel = vc->velocity / 127.0f;
            float amp = vc->env * vel * 0.25f;
            
            mixL += sL * amp * chorusMod;
            mixR += sR * amp;
            
            vc->phase += inc;
            if (vc->phase >= 1.0f) vc->phase -= 1.0f;
            vc->phaseR += incR;
            if (vc->phaseR >= 1.0f) vc->phaseR -= 1.0f;
        }
        
        // Simple one-pole LP filter
        s->lpState[0] += lpCoeff * (mixL - s->lpState[0]);
        s->lpState[1] += lpCoeff * (mixR - s->lpState[1]);
        
        audio->outputL[i] += s->lpState[0];
        audio->outputR[i] += s->lpState[1];
    }
}

float pdsynth_get_param(PdSynthInstance inst, int idx) {
    WTSynth* s = (WTSynth*)inst;
    if (idx >= 0 && idx < PARAM_COUNT) return s->params[idx];
    return 0;
}

void pdsynth_set_param(PdSynthInstance inst, int idx, float val) {
    WTSynth* s = (WTSynth*)inst;
    if (idx >= 0 && idx < PARAM_COUNT) s->params[idx] = val;
}

void pdsynth_reset(PdSynthInstance inst) {
    WTSynth* s = (WTSynth*)inst;
    for (int i = 0; i < MAX_VOICES; i++) s->voices[i].stage = 0;
    s->lpState[0] = s->lpState[1] = 0;
}
