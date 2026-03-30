/**
 * PocketDAW Synth Plugin API v2
 * 
 * Build synthesizers and samplers for PocketDAW.
 * Compile as a shared library (.so) for aarch64 Linux.
 * 
 * v2 additions: Sample loading API, host callbacks.
 * v1 plugins still load fine — new features are optional.
 * 
 * Example build:
 *   aarch64-none-linux-gnu-gcc -shared -fPIC -O2 -o my-synth.so my-synth.c -lm
 * 
 * Drop into: plugins/synths/my-synth/my-synth.so
 * Add a manifest.json alongside it for parameter metadata.
 */

#ifndef PDSYNTH_API_H
#define PDSYNTH_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PDSYNTH_API_VERSION 2
#define PDSYNTH_API_VERSION_MIN 1  /* Minimum compatible version */
#define PDSYNTH_MAX_PARAMS  32
#define PDSYNTH_MAX_NAME    64
#define PDSYNTH_MAX_SAMPLES 128    /* Max samples a plugin can load */

/* ── Types ─────────────────────────────────────────────── */

/** Audio context — plugin fills outputL/outputR */
typedef struct {
    float   sampleRate;     /* 44100 */
    int     bufferSize;     /* Number of samples to generate */
    float*  outputL;        /* Left channel buffer (write here) */
    float*  outputR;        /* Right channel buffer (write here) */
} PdSynthAudio;

/** Note event */
typedef struct {
    uint8_t note;           /* MIDI note 0-127 */
    uint8_t velocity;       /* 0-127 (0 = note off) */
    uint8_t channel;        /* Reserved, always 0 */
    uint8_t type;           /* 0=noteOff, 1=noteOn */
} PdSynthNote;

/** Opaque plugin instance handle */
typedef void* PdSynthInstance;

/* ── Sample Data (v2) ──────────────────────────────────── */

/** Sample audio data returned by the host */
typedef struct {
    float*      dataL;          /* Left channel (mono: same as dataR) */
    float*      dataR;          /* Right channel */
    int         frames;         /* Total sample frames */
    int         sampleRate;     /* Original sample rate */
    int         channels;       /* 1=mono, 2=stereo */
    const char* name;           /* Sample filename (without path) */
} PdSynthSample;

/** Host callback table — provided to plugin via pdsynth_set_host()
 *  All callbacks are called from the main thread (NOT audio thread).
 *  Loaded samples persist until the instance is destroyed. */
typedef struct {
    void* hostData;  /* Opaque host pointer — pass back in all callbacks */
    
    /** Load a WAV file by path (relative to plugin directory).
     *  Returns a PdSynthSample with audio data, or NULL on failure.
     *  The host owns the memory — valid until pdsynth_destroy().
     *  Paths starting with "/" are absolute; others are relative
     *  to the plugin's directory. "samples/" prefix is conventional.
     *  
     *  Example: host_load_sample(hostData, "samples/kick.wav")
     */
    const PdSynthSample* (*load_sample)(void* hostData, const char* path);
    
    /** Load a WAV from the global sample library by name.
     *  Searches the DAW's samples/ directory.
     *  Example: host_load_library_sample(hostData, "reetro/kick_01.wav")
     */
    const PdSynthSample* (*load_library_sample)(void* hostData, const char* name);
    
    /** Get number of loaded samples */
    int (*sample_count)(void* hostData);
    
    /** Get loaded sample by index (0-based) */
    const PdSynthSample* (*get_sample)(void* hostData, int index);
    
    /** Unload a sample by index (frees memory) */
    void (*unload_sample)(void* hostData, int index);
    
    /** Log a message (appears in stderr / crash log) */
    void (*log)(void* hostData, const char* message);
    
    /** Request the host to open a file browser for sample loading.
     *  The host shows its native file picker UI. When the user selects
     *  a file, the host loads it (using its WAV/MP3/OGG/FLAC loaders)
     *  and it becomes available via get_sample().
     *  
     *  slotIndex: which sample slot to load into (0-based).
     *             Use -1 to append to the next available slot.
     *  Returns: 1 if the browser was opened, 0 on failure.
     *  
     *  NOTE: This is asynchronous — the sample isn't available immediately.
     *  The host will call back into the plugin's process() once loaded.
     *  Check sample_count() to detect when new samples appear.
     */
    int (*request_sample_browser)(void* hostData, int slotIndex);
    
    /** Get the file path of a loaded sample (for display/persistence).
     *  Returns NULL if index is invalid or sample has no path. */
    const char* (*get_sample_path)(void* hostData, int index);
} PdSynthHostCallbacks;

/* ── Required Exports ──────────────────────────────────── */

/** Return PDSYNTH_API_VERSION. Host checks this before loading. */
int pdsynth_api_version(void);

/** Human-readable plugin name (e.g., "FM Synth") */
const char* pdsynth_name(void);

/** Number of adjustable parameters (0 to PDSYNTH_MAX_PARAMS) */
int pdsynth_param_count(void);

/** Create a new synth instance. Called once per track. */
PdSynthInstance pdsynth_create(float sampleRate);

/** Destroy instance. Free all memory. */
void pdsynth_destroy(PdSynthInstance inst);

/** Generate audio. Fill audio->outputL and audio->outputR.
 *  Called from the audio thread — must be fast and lock-free. */
void pdsynth_process(PdSynthInstance inst, PdSynthAudio* audio);

/** Handle a note event (note on/off). */
void pdsynth_note(PdSynthInstance inst, PdSynthNote* event);

/** Get parameter value by index (0-based). Returns 0.0-1.0 normalized. */
float pdsynth_get_param(PdSynthInstance inst, int index);

/** Set parameter value by index. Value is 0.0-1.0 normalized. */
void pdsynth_set_param(PdSynthInstance inst, int index, float value);

/* ── Optional Exports ──────────────────────────────────── */

/** Get parameter name by index. Return NULL if not implemented. */
const char* pdsynth_param_name(int index);

/** Reset all voices / clear state. Called on transport stop. */
void pdsynth_reset(PdSynthInstance inst);

/** Pitch bend (-1.0 to 1.0). */
void pdsynth_pitch_bend(PdSynthInstance inst, float bend);

/** Mod wheel (0.0 to 1.0). */
void pdsynth_mod_wheel(PdSynthInstance inst, float value);

/* ── Optional: Presets ─────────────────────────────────── */

/** Number of built-in presets. Return 0 if none. */
int pdsynth_preset_count(void);

/** Get preset name by index. */
const char* pdsynth_preset_name(int index);

/** Load a preset by index. Updates all internal params. */
void pdsynth_load_preset(PdSynthInstance inst, int index);

/** Get current preset index. Return -1 if modified/custom. */
int pdsynth_get_preset(PdSynthInstance inst);

/* ── Optional: Host Callbacks (v2) ─────────────────────── */

/** Receive the host callback table. Called once after pdsynth_create().
 *  Store the callbacks pointer — it remains valid for the instance lifetime.
 *  If not exported, the plugin cannot load samples (pure synthesis only). */
void pdsynth_set_host(PdSynthInstance inst, const PdSynthHostCallbacks* host);

/** Called after set_host with the plugin's directory path.
 *  Use this to auto-load samples on initialization.
 *  Example: pluginDir = "/mnt/mmc/MUOS/application/PocketDAW/plugins/synths/my-sampler/"
 */
void pdsynth_set_plugin_dir(PdSynthInstance inst, const char* dir);

/* ── Optional: Visualization ───────────────────────────── */

/** Fill buf with waveform data (-1.0 to 1.0) for display.
 *  Return actual number of samples written (0 if unsupported).
 *  Called each frame when synth editor is open. */
int pdsynth_get_waveform(PdSynthInstance inst, float* buf, int maxSamples);

/* ── manifest.json UI Section ──────────────────────────── 
 *
 * The manifest.json file alongside your .so defines plugin metadata
 * and optional UI customization:
 *
 * {
 *   "name": "My Synth",
 *   "author": "Your Name",
 *   "version": "1.0",
 *   "params": [...],
 *   "ui": {
 *     "accent": [255, 30, 60],           // RGB accent color
 *     "skin": "skin.bmp",                // 320x240 background image
 *     "knobStrip": "knobs.bmp",          // Filmstrip sprite sheet
 *     "knobFrames": 64,                  // Number of frames in strip
 *     "knobSize": 48,                    // Frame width/height in px
 *     "viz": {
 *       "type": "waveform",              // "waveform"|"spectrum"|"custom"
 *       "x": 6, "y": 14,                // Position on screen
 *       "w": 300, "h": 32,              // Size in pixels
 *       "color": [255, 30, 60],          // Viz color (or uses accent)
 *       "reactive": true,                // React to THIS synth's audio
 *       "bars": 16                       // Number of spectrum bars (8-32)
 *     }
 *   }
 * }
 *
 * FILES (place alongside .so):
 *   skin.bmp     - 320x240 background (BMP format)
 *   knobs.bmp    - Vertical filmstrip, frames stacked top-to-bottom
 *                  Each frame is knobSize x knobSize pixels
 *                  Frame 0 = min value, last frame = max value
 *   samples/     - WAV files for sampler plugins (loaded via host callbacks)
 *
 * If no knobStrip is specified, the default SDL2 drawn knobs are used.
 * If no skin is specified, the default dark background is shown.
 * The waveform visualizer only shows if pdsynth_get_waveform is exported.
 *
 * SAMPLE LOADING (v2):
 *   Plugins can request WAV files through the host callback table.
 *   Two loading modes:
 *     1. Plugin-local: "samples/kick.wav" — relative to plugin directory
 *     2. Library: "reetro/kick_01.wav" — from the global samples/ folder
 *   
 *   The host decodes WAV to float [-1.0, 1.0], resamples if needed,
 *   and provides both mono and stereo access. Memory is managed by the
 *   host — plugins just use the pointers.
 *
 *   manifest.json can declare required samples:
 *   {
 *     "samples": [
 *       { "path": "samples/kick.wav",   "note": 36 },
 *       { "path": "samples/snare.wav",  "note": 38 },
 *       { "path": "samples/hihat.wav",  "note": 42 }
 *     ]
 *   }
 *   The host will pre-load these before calling pdsynth_set_host().
 *   The "note" field maps the sample to a MIDI note (for key-mapped samplers).
 *
 * INTERACTIVE SAMPLE LOADING (v2):
 *   Plugins can request the host to open a file browser at runtime:
 *     host->request_sample_browser(hostData, slotIndex)
 *   
 *   The host shows its native file picker. When the user selects a file,
 *   the host loads it and makes it available via get_sample(). Plugins
 *   with "samples" in their manifest and "pages": true in the UI section
 *   get a dedicated sample picker on page 0 of the synth editor.
 *   
 *   get_sample_path() returns the file path of a loaded sample for
 *   display purposes (e.g. showing the filename in the plugin UI).
 */

#ifdef __cplusplus
}
#endif

#endif /* PDSYNTH_API_H */
