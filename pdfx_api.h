/**
 * PocketDAW Effects Plugin API v1
 * 
 * Build audio effects (reverb, delay, distortion, etc.) for PocketDAW.
 * Compile as a shared library (.so) for aarch64 Linux.
 * 
 * Example build:
 *   aarch64-none-linux-gnu-gcc -shared -fPIC -O2 -o my-effect.so my-effect.c -lm
 * 
 * Drop into: plugins/fx/my-effect/my-effect.so
 * Add a manifest.json alongside it for parameter metadata.
 * 
 * Effects are loaded into mixer FX slots (2 per strip).
 * They process stereo audio in-place on the audio thread.
 */

#ifndef PDFX_API_H
#define PDFX_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PDFX_API_VERSION  1
#define PDFX_MAX_PARAMS   16
#define PDFX_MAX_NAME     64

/* ── Types ─────────────────────────────────────────────── */

/** Audio buffer — effect processes in-place (inputL/R == outputL/R)
 *  or copies to separate output buffers. */
typedef struct {
    float   sampleRate;     /* 44100 */
    int     bufferSize;     /* Number of sample frames to process */
    float*  inputL;         /* Left input (read) */
    float*  inputR;         /* Right input (read) */
    float*  outputL;        /* Left output (write — may alias inputL) */
    float*  outputR;        /* Right output (write — may alias inputR) */
} PdFxAudio;

/** Opaque effect instance handle */
typedef void* PdFxInstance;

/* ── Required Exports ──────────────────────────────────── */

/** Return PDFX_API_VERSION. Host checks this before loading. */
int pdfx_api_version(void);

/** Human-readable effect name (e.g., "Tape Delay") */
const char* pdfx_name(void);

/** Number of adjustable parameters (0 to PDFX_MAX_PARAMS) */
int pdfx_param_count(void);

/** Create a new effect instance. Called once per mixer strip slot. */
PdFxInstance pdfx_create(float sampleRate);

/** Destroy instance. Free all memory. */
void pdfx_destroy(PdFxInstance inst);

/** Process audio. Read from input buffers, write to output buffers.
 *  input and output may alias (in-place processing is fine).
 *  Called from the audio thread — must be fast and lock-free. */
void pdfx_process(PdFxInstance inst, PdFxAudio* audio);

/** Get parameter value by index (0-based). Returns 0.0-1.0 normalized. */
float pdfx_get_param(PdFxInstance inst, int index);

/** Set parameter value by index. Value is 0.0-1.0 normalized. */
void pdfx_set_param(PdFxInstance inst, int index, float value);

/* ── Optional Exports ──────────────────────────────────── */

/** Get parameter name by index. Return NULL if not implemented. */
const char* pdfx_param_name(int index);

/** Reset internal state (clear delay lines, etc.).
 *  Called on transport stop or when effect is bypassed. */
void pdfx_reset(PdFxInstance inst);

/* ── Optional: Presets ─────────────────────────────────── */

/** Number of built-in presets. Return 0 if none. */
int pdfx_preset_count(void);

/** Get preset name by index. */
const char* pdfx_preset_name(int index);

/** Load a preset by index. Updates all internal params. */
void pdfx_load_preset(PdFxInstance inst, int index);

/** Get current preset index. Return -1 if modified/custom. */
int pdfx_get_preset(PdFxInstance inst);

/* ── manifest.json ─────────────────────────────────────── 
 *
 * {
 *   "name": "Tape Delay",
 *   "author": "Your Name",
 *   "version": "1.0",
 *   "type": "effect",
 *   "library": "tape-delay.so",
 *   "params": [
 *     { "name": "Time",     "default": 0.4,  "min": 0.0, "max": 1.0 },
 *     { "name": "Feedback", "default": 0.35, "min": 0.0, "max": 1.0 },
 *     { "name": "Mix",      "default": 0.3,  "min": 0.0, "max": 1.0 },
 *     { "name": "Tone",     "default": 0.6,  "min": 0.0, "max": 1.0 }
 *   ],
 *   "ui": {
 *     "accent": [60, 180, 255]
 *   }
 * }
 *
 * The "type" field MUST be "effect" (not "synth").
 * Effects are scanned from plugins/fx/<name>/ directories.
 */

#ifdef __cplusplus
}
#endif

#endif /* PDFX_API_H */
