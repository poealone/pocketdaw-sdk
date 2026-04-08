# Changelog — PocketDAW SDK

## v4.5 — Sample Source APIs

> Released with PocketDAW v4.6

### New APIs
- **`PdSampleSlotInfo`** struct — sample slot metadata (loaded, frames, sampleRate, channels, name)
- **`get_sample_slot_info(slot)`** — retrieve metadata for any of the 16 host sample slots
- **`get_sample_slot_name(slot)`** — get the display name of a sample slot (e.g. "ReCorder Take 3")
- **`pdsynth_host_sample_changed(ctx, slot)`** — callback notifying synth plugins when a host sample source changes

### Purpose
Enables project-aware synth plugins (e.g. Re-Pitcher) to browse, bind to, and play samples already loaded in the PocketDAW project — including ReCorder takes and Sample Edit imports — without requiring manual file loading.

---

## v4.3 — Unified Header

> Released with PocketDAW v0.4.4

- Consolidated all plugin APIs into single `pocketdaw.h` header
- Legacy `pdsynth_api.h` and `pdfx_api.h` become redirect stubs
- All examples updated to use unified header

---

## v4.0–4.2 — Transport, MIDI, Audio, VizEngine, Plugin Scope

- Transport API (playback position, BPM, time signature, play/stop requests)
- MIDI event API (note on/off, CC, pitch bend)
- Audio host API (FFT spectrum, sample data, device enumeration)
- Visualizer engine with GLSL shader support
- Plugin scope system (MIDI vs audio tracks)

---

## v3.x — FX Pipeline + Recording

- FX plugin API with sidechain input support
- ReCorder-style recording plugin interface
- Plugin capabilities and finalize callbacks
- Track-level input routing and capture
