/**
 * Tape Delay — PocketDAW Effect Plugin Example
 * 
 * Warm stereo delay with low-pass filtering on the feedback path,
 * simulating analog tape echo characteristics.
 * 
 * Build:
 *   aarch64-none-linux-gnu-gcc -shared -fPIC -O2 -o tape-delay.so tape-delay.c -lm
 */

#include "../../pdfx_api.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_DELAY_SAMPLES (44100 * 2)  /* 2 seconds max */
#define NUM_PARAMS 5
#define NUM_PRESETS 4

typedef struct {
    float sampleRate;
    
    /* Parameters (0.0-1.0 normalized) */
    float params[NUM_PARAMS];
    
    /* Delay buffers */
    float delayL[MAX_DELAY_SAMPLES];
    float delayR[MAX_DELAY_SAMPLES];
    int writePos;
    
    /* Low-pass filter state (feedback path) */
    float lpStateL;
    float lpStateR;
    
    int currentPreset;
} TapeDelay;

/* Parameter indices */
enum {
    P_TIME = 0,       /* 0.0=50ms, 1.0=2000ms */
    P_FEEDBACK,        /* 0.0=0%, 1.0=90% */
    P_MIX,             /* 0.0=dry, 1.0=wet */
    P_TONE,            /* 0.0=dark, 1.0=bright */
    P_STEREO_SPREAD    /* 0.0=mono, 1.0=wide */
};

static const char* param_names[NUM_PARAMS] = {
    "Time", "Feedback", "Mix", "Tone", "Spread"
};

static const float presets[NUM_PRESETS][NUM_PARAMS] = {
    /* Slapback */     { 0.08f, 0.15f, 0.35f, 0.7f, 0.3f },
    /* Tape Echo */    { 0.35f, 0.45f, 0.30f, 0.4f, 0.5f },
    /* Dub Delay */    { 0.50f, 0.65f, 0.40f, 0.25f, 0.7f },
    /* Ambient Wash */ { 0.75f, 0.55f, 0.50f, 0.3f, 0.9f }
};

static const char* preset_names[NUM_PRESETS] = {
    "Slapback", "Tape Echo", "Dub Delay", "Ambient Wash"
};

/* ── Required API ──────────────────────────────────────── */

int pdfx_api_version(void) { return PDFX_API_VERSION; }
const char* pdfx_name(void) { return "Tape Delay"; }
int pdfx_param_count(void) { return NUM_PARAMS; }

PdFxInstance pdfx_create(float sampleRate) {
    TapeDelay* td = (TapeDelay*)calloc(1, sizeof(TapeDelay));
    if (!td) return NULL;
    td->sampleRate = sampleRate;
    td->currentPreset = 0;
    /* Load default preset */
    for (int i = 0; i < NUM_PARAMS; i++)
        td->params[i] = presets[0][i];
    return td;
}

void pdfx_destroy(PdFxInstance inst) {
    free(inst);
}

void pdfx_process(PdFxInstance inst, PdFxAudio* audio) {
    TapeDelay* td = (TapeDelay*)inst;
    if (!td) return;
    
    /* Map parameters */
    float timeMs = 50.0f + td->params[P_TIME] * 1950.0f;      /* 50-2000ms */
    float feedback = td->params[P_FEEDBACK] * 0.9f;             /* 0-90% */
    float mix = td->params[P_MIX];
    float tone = td->params[P_TONE];
    float spread = td->params[P_STEREO_SPREAD];
    
    int delaySamples = (int)(timeMs * td->sampleRate / 1000.0f);
    if (delaySamples >= MAX_DELAY_SAMPLES) delaySamples = MAX_DELAY_SAMPLES - 1;
    if (delaySamples < 1) delaySamples = 1;
    
    /* Stereo spread: offset R channel delay */
    int delaySamplesR = delaySamples + (int)(spread * delaySamples * 0.3f);
    if (delaySamplesR >= MAX_DELAY_SAMPLES) delaySamplesR = MAX_DELAY_SAMPLES - 1;
    
    /* Low-pass coefficient (tone control on feedback) */
    float lpCoeff = 0.05f + tone * 0.9f;  /* dark → bright */
    
    float dry = 1.0f - mix * 0.5f;  /* Keep some dry signal */
    float wet = mix;
    
    for (int i = 0; i < audio->bufferSize; i++) {
        float inL = audio->inputL[i];
        float inR = audio->inputR[i];
        
        /* Read from delay lines */
        int readL = td->writePos - delaySamples;
        if (readL < 0) readL += MAX_DELAY_SAMPLES;
        int readR = td->writePos - delaySamplesR;
        if (readR < 0) readR += MAX_DELAY_SAMPLES;
        
        float delL = td->delayL[readL];
        float delR = td->delayR[readR];
        
        /* Low-pass filter on feedback (tape warmth) */
        td->lpStateL += lpCoeff * (delL - td->lpStateL);
        td->lpStateR += lpCoeff * (delR - td->lpStateR);
        
        /* Write to delay lines (input + filtered feedback) */
        td->delayL[td->writePos] = inL + td->lpStateL * feedback;
        td->delayR[td->writePos] = inR + td->lpStateR * feedback;
        
        /* Soft-clip the delay buffer to prevent runaway */
        float dL = td->delayL[td->writePos];
        float dR = td->delayR[td->writePos];
        td->delayL[td->writePos] = tanhf(dL);
        td->delayR[td->writePos] = tanhf(dR);
        
        /* Mix output */
        audio->outputL[i] = inL * dry + delL * wet;
        audio->outputR[i] = inR * dry + delR * wet;
        
        td->writePos = (td->writePos + 1) % MAX_DELAY_SAMPLES;
    }
}

float pdfx_get_param(PdFxInstance inst, int index) {
    TapeDelay* td = (TapeDelay*)inst;
    if (!td || index < 0 || index >= NUM_PARAMS) return 0.0f;
    return td->params[index];
}

void pdfx_set_param(PdFxInstance inst, int index, float value) {
    TapeDelay* td = (TapeDelay*)inst;
    if (!td || index < 0 || index >= NUM_PARAMS) return;
    td->params[index] = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    td->currentPreset = -1;
}

/* ── Optional API ──────────────────────────────────────── */

const char* pdfx_param_name(int index) {
    if (index < 0 || index >= NUM_PARAMS) return NULL;
    return param_names[index];
}

void pdfx_reset(PdFxInstance inst) {
    TapeDelay* td = (TapeDelay*)inst;
    if (!td) return;
    memset(td->delayL, 0, sizeof(td->delayL));
    memset(td->delayR, 0, sizeof(td->delayR));
    td->lpStateL = 0.0f;
    td->lpStateR = 0.0f;
    td->writePos = 0;
}

int pdfx_preset_count(void) { return NUM_PRESETS; }

const char* pdfx_preset_name(int index) {
    if (index < 0 || index >= NUM_PRESETS) return NULL;
    return preset_names[index];
}

void pdfx_load_preset(PdFxInstance inst, int index) {
    TapeDelay* td = (TapeDelay*)inst;
    if (!td || index < 0 || index >= NUM_PRESETS) return;
    for (int i = 0; i < NUM_PARAMS; i++)
        td->params[i] = presets[index][i];
    td->currentPreset = index;
}

int pdfx_get_preset(PdFxInstance inst) {
    TapeDelay* td = (TapeDelay*)inst;
    return td ? td->currentPreset : -1;
}
