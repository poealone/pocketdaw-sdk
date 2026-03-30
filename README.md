# PocketDAW Plugin SDK

Build custom synths and effects for [PocketDAW](https://pocketdaw.net) — the handheld DAW for Anbernic devices.

## Overview

PocketDAW uses a C plugin API. Write a synth or effect, compile to `.so` (ARM64), and drop it into the plugins folder on your device.

**SDK Version:** 2.1  
**Minimum Compatible:** 1.0  
**Target:** aarch64-linux (ARM64)

## Getting Started

### Requirements
- ARM64 cross-compiler (`aarch64-none-linux-gnu-gcc` or similar)
- Basic C knowledge
- PocketDAW installed on your device

### Quick Start

```c
#include "pdsynth_api.h"

const char* pdsynth_name(void) { return "My Synth"; }
int pdsynth_api_version(void) { return 2; }

void* pdsynth_create(float sampleRate) {
    // Allocate your state here
    return calloc(1, sizeof(MyState));
}

void pdsynth_destroy(void* instance) {
    free(instance);
}

void pdsynth_process(void* instance, PdSynthAudio* audio) {
    // Generate audio into audio->outputL and audio->outputR
}

void pdsynth_note_on(void* instance, uint8_t note, uint8_t vel) {
    // Handle MIDI note
}

void pdsynth_note_off(void* instance, uint8_t note) {
    // Handle note release
}
```

### Build

```bash
aarch64-none-linux-gnu-gcc -shared -fPIC -O2 -o my-synth.so my-synth.c -lm
```

### Install

1. Copy `my-synth.so` + `manifest.json` to `PocketDAW/plugins/synths/my-synth/` on your SD card
2. Restart PocketDAW
3. Select your synth from the MIDI track synth list

## API Headers

| File | Description |
|------|-------------|
| `pdsynth_api.h` | Synth/sampler plugin API (instruments) |
| `pdfx_api.h` | Effects plugin API (FX chain processors) |

## Examples

| Plugin | Type | Description |
|--------|------|-------------|
| `jt-synth` | Subtractive | Dual oscillator with unison voicing |
| `fm-synth` | FM | 4-operator FM synthesis |
| `wavetable` | Wavetable | Wavetable scanning synth |
| `granular` | Granular | Granular texture engine |
| `drum-machine` | Sampler | Multi-sample drum kit |
| `simple-sampler` | Sampler | Basic chromatic sampler |
| `tape-delay` | Effect | Tape-style delay effect |
| `pulse-ring` | Synth | Pulse wave ring modulator |

Each example includes source code, a compiled `.so`, and a `manifest.json`.

## manifest.json

Every plugin needs a manifest file:

```json
{
    "name": "My Synth",
    "author": "Your Name",
    "version": "1.0.0",
    "description": "What it does",
    "type": "synth",
    "library": "my-synth.so",
    "params": [
        { "name": "Attack", "default": 0.01 },
        { "name": "Decay", "default": 0.1 }
    ]
}
```

## v2 Features (New)

- **Sample Loading API** — load WAV files through host callbacks
- **Host Callbacks** — request samples, query host state
- **Up to 128 samples per plugin**

## Community

- **Website:** [pocketdaw.net](https://pocketdaw.net)
- **Forum:** [community.pocketdaw.net](https://community.pocketdaw.net)
- **Downloads:** [pocketdaw.net/download](https://pocketdaw.net/download)

Share your plugins on the [community forum](https://community.pocketdaw.net) in the Plugins & Presets category!

## License

MIT — free to use, modify, and distribute.
