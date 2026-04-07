# PocketDAW Plugin SDK

Build custom synths, effects, and visualizers for [PocketDAW](https://pocketdaw.net) — the handheld DAW for Anbernic devices.

## 📖 Documentation

**Full docs → [docs.pocketdaw.net](http://docs.pocketdaw.net)**

- [SDK Overview](http://docs.pocketdaw.net/#/sdk/overview)
- [Synth API Reference](http://docs.pocketdaw.net/#/sdk/synth-api)
- [Effects API Reference](http://docs.pocketdaw.net/#/sdk/fx-api)
- [Manifest Format](http://docs.pocketdaw.net/#/sdk/manifest)
- [Sample Loading (v2)](http://docs.pocketdaw.net/#/sdk/samples)
- [Building Plugins](http://docs.pocketdaw.net/#/sdk/building)
- [Themes & Skins](http://docs.pocketdaw.net/#/sdk/themes)
- [Visualizer Shaders](http://docs.pocketdaw.net/#/sdk/visualizers)


## SDK v3.1 — Desktop Mouse & Touch

> Released with PocketDAW v0.4.4 · No breaking changes — v3.0 plugins work without recompilation.

### What's New in v3.1

**`PD_SDK_VERSION_MINOR`** bumped to `1`

#### How Plugin Editors Work on Desktop

**Knob layout (≤12 params)**
- Click knob → focus/select
- Vertical drag → adjust value (`0.005` per pixel)
- Scroll wheel → fine adjust

**Slider layout (>12 params)**
- Click row → select param
- Drag value bar → set directly

**Custom draw page (`pdsynth_draw` returning `1`)**
- Host does NOT intercept mouse on your custom area
- Plugin owns the full page — handle input in your draw callback
- `PdDrawContext.x/y/w/h` gives your render bounds

**Touch**
- Single tap = left click
- Long press 500ms = right click / context menu

No new exports required — all mouse/touch routing is handled by the host.

## Quick Start

```bash
git clone https://github.com/poealone/pocketdaw-sdk
cd pocketdaw-sdk

# Copy an example as your starting point
cp -r examples/simple-sampler my-plugin
cd my-plugin

# Edit source and manifest
vim simple-sampler.c
vim manifest.json

# Cross-compile for ARM64
aarch64-linux-gnu-gcc -shared -fPIC -O2 -o my-plugin.so simple-sampler.c -lm

# Deploy: copy to device SD card
# → MUOS/application/PocketDAW/plugins/synths/my-plugin/
```

## API Headers

| File | Version | Description |
|------|---------|-------------|
| `pocketdaw.h` | SDK v3 — unified header (synths + FX + visuals) |
| `pdsynth_api.h` | v2 | Synth/sampler plugin API — MIDI input, audio output, sample loading |
| `pdfx_api.h` | v1 | Effects plugin API — stereo audio processing in the mixer FX chain |

## Examples

| Plugin | Type | Description |
|--------|------|-------------|
| `jt-synth` | Synth | Dual oscillator with morphable waveforms, ladder filter, custom skin |
| `fm-synth` | Synth | 4-operator FM synthesis with 8 algorithms |
| `wavetable` | Synth | 64-frame wavetable with LFO position scanning |
| `granular` | Synth | Granular texture engine with v2 sample loading |
| `drum-machine` | Sampler | Multi-sample drum kit with MIDI note mapping |
| `simple-sampler` | Sampler | Basic chromatic sampler |
| `tape-delay` | Effect | Warm tape-style delay with filtered feedback |
| `jt-sidechain` | Effect | Sidechain compressor with 6 presets (pump, duck, gate) |
| `pulse-ring` | Visualizer | Audio-reactive GLSL shader (bass rings + beat flash) |

Each example includes source code, a pre-compiled ARM64 `.so`, and a `manifest.json`.

## Plugin Types

### Synths (`pdsynth_api.h`)
Generate audio from MIDI notes. Placed on MIDI tracks.
- Required: `pdsynth_create`, `pdsynth_destroy`, `pdsynth_process`, `pdsynth_note`
- Optional: presets, waveform visualization, host sample loading (v2)

### Effects (`pdfx_api.h`)
Process stereo audio in the mixer FX chain. 2 slots per channel strip.
- Required: `pdfx_create`, `pdfx_destroy`, `pdfx_process`
- Optional: presets, parameter names

### Visualizers (GLSL)
Audio-reactive fragment shaders rendered via OpenGL ES 2.0.
- Uniforms: `u_bass`, `u_mid`, `u_high`, `u_beat`, `u_volume`, `u_time`
- Up to 2 user-adjustable parameters

## Directory Structure

```
plugins/
├── synths/my-synth/
│   ├── my-synth.so        # ARM64 shared library
│   ├── manifest.json      # Metadata + params
│   ├── skin.bmp           # Optional: 320×240 background
│   └── samples/           # Optional: WAV files
├── fx/my-effect/
│   ├── my-effect.so
│   └── manifest.json
└── visualizers/my-viz/
    ├── shader.frag
    └── manifest.json
```

## Community

- **Website:** [pocketdaw.net](https://pocketdaw.net)
- **Docs:** [docs.pocketdaw.net](http://docs.pocketdaw.net)
- **Forum:** [community.pocketdaw.net](https://community.pocketdaw.net)
- **Downloads:** [pocketdaw.net/download](https://pocketdaw.net/download)

Share your plugins on the [community forum](https://community.pocketdaw.net) under Plugins & Presets!

## License

SDK headers and examples are **MIT licensed** — free to use, modify, and distribute.

PocketDAW itself is proprietary — free to use.
