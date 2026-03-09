# WASP — WebAssembly Audio Plugin
## Specification v0.1.0 (Draft)

---

## 1. Overview

WASP (WebAssembly Audio Plugin) is an open, cross-platform audio plugin format
built on WebAssembly and web technologies. Plugins consist of a WASM DSP module
and an optional HTML/CSS/JS GUI, packaged together in a `.wasp` bundle.

Goals:
- True cross-platform support (Linux, Windows, macOS) from a single binary
- Sandboxed execution — plugins cannot crash the host
- Simple, approachable developer experience
- Host-neutral design, not tied to any specific DAW

---

## 2. Plugin Bundle Format

A `.wasp` file is a ZIP archive with the following structure:
```
myplugin.wasp
├── manifest.json
├── icon.png          (optional, recommended 256x256)
├── dsp.wasm
├── ui/               (optional)
│   ├── index.html
│   ├── app.js
│   ├── style.css
│   └── assets/
└── data/             (optional, for samples/wavetables/IRs)
```

The `ui/` folder is optional. Hosts must function correctly with DSP-only plugins.
The `data/` folder is optional.
The icon may also be an SVG file.

---

## 3. Manifest

The manifest is a JSON file describing the plugin to the host.
```json
{
  "name": "MyPlugin",
  "uniqueId": "com.MyCompany.MyPlugin",
  "description": "An awesome plugin",
  "icon": "icon.png",
  "version": "0.1.0",
  "author": "",
  "type": "instrument",
  "category": "synthesizer",
  "inputs": 0,
  "outputs": 2,
  "midi": true,
  "ui": "ui/index.html",
  "parameters": [
    {
      "id": 0,
      "name": "Cutoff",
      "description": "Filter cutoff frequency",
      "type": "float",
      "min": 20.0,
      "max": 20000.0,
      "default": 1000.0,
      "visible": true
    },
    {
	  "id": 2,
	  "name": "Waveform",
	  "description": "Oscillator waveform shape",
	  "type": "enum",
	  "values": ["Sine", "Square", "Sawtooth", "Triangle"],
	  "default": 0,
	  "visible": true
	}
  ]
}
```

### Plugin Types
- `"instrument"` — generates audio from MIDI. Typically has 0 inputs.
- `"effect"` — processes incoming audio. Has both inputs and outputs.

### Categories
Suggested values for `category`:
`"synthesizer"`, `"sampler"`, `"drum"`, `"filter"`, `"equalizer"`,
`"reverb"`, `"delay"`, `"distortion"`, `"compressor"`, `"utility"`, `"other"`

Hosts may use this field to organise plugins in their browser UI.

### Parameter Fields
- `id` — unique integer identifier used in the DSP ABI and messaging
- `name` — human-readable label for display in the host
- `description` — optional, longer description of the parameter
- `type` — currently only `"float"` is supported
- `min`, `max`, `default` — value range and default value
- `visible` — boolean, whether the host should display this parameter
  as automatable. Defaults to `true` if omitted.

### Parameter Types

- `"float"` — a continuous value between `min` and `max`.
  Hosts should render this as a knob or slider.

- `"enum"` — a discrete value represented as an integer index into `values`.
  Hosts should render this as a dropdown or segmented control.
  `min`, `max` are not required for enum parameters.
  `default` is an integer index, e.g. `0` for the first value.

The DSP always receives parameter values as floats via `wasp_set_parameter`.
For `enum` parameters the host passes the index as a float, e.g. `1.0` for
`"Square"`. Plugins should cast this to an integer:
```rust
2 => plugin.synth_type = match id as i32 {
    0 => SynthType::Sine,
    1 => SynthType::Square,
    2 => SynthType::Sawtooth,
    3 => SynthType::Triangle,
    _ => SynthType::Sine,
},
```

### Manifest Fields
- `uniqueId` — reverse-domain unique identifier, e.g. `com.MyCompany.MyPlugin`.
  Used by hosts to match presets to plugins.
- `category` — optional hint for host plugin browsers
- `icon` — optional path to icon file within the bundle
- `ui` — optional path to GUI entry point within the bundle

---

## 4. DSP ABI

The WASM module must export the following functions:
```c
// Called once when the plugin is loaded
void wasp_initialize(int sample_rate, int max_block_size);

// Called once per audio block
void wasp_process(
    float* inputs,      // interleaved input samples
    float* outputs,     // interleaved output samples
    int    num_frames,
    int    num_channels
);

// Called by the host when a parameter changes
// id corresponds to the integer parameter id in the manifest
void wasp_set_parameter(int id, float value);

// Called by the host when a MIDI event arrives
// Delivered before wasp_process for the same block
void wasp_midi_event(uint8_t status, uint8_t data1, uint8_t data2);

// Called by the host to save plugin state
// Plugin writes arbitrary state data into a buffer and returns its size
// Host owns the returned memory and will free it after reading
int wasp_save_state(uint8_t** data_out);

// Called by the host to restore plugin state
// data and size correspond to a buffer previously returned by wasp_save_state
void wasp_load_state(uint8_t* data, int size);

// Called when the plugin is unloaded
void wasp_terminate();
```

### Notes
- The host may call `wasp_set_parameter` from the audio thread.
  Plugins must handle this safely.
- Block size may vary between calls. Plugins must not assume a fixed size.
- MIDI events are delivered before the `wasp_process` call for the same block.
- Audio buffers are passed via WASM linear memory. The host writes input
  samples into the plugin's memory and reads output samples from it after
  `wasp_process` returns.
- `wasp_save_state` and `wasp_load_state` are optional exports. Hosts must
  function correctly if they are absent, falling back to parameter-only state.

---

## 5. GUI

The GUI is an HTML page rendered inside a WebView provided by the host.
It communicates with the DSP via the host messaging API. The GUI is optional —
hosts must work correctly without it.

### Host JavaScript API

The host injects a global `wasp` object into the WebView:
```javascript
// Send a parameter change to the DSP (via the host)
wasp.send({ type: "parameter", id: 0, value: 1200.0 });

// Receive messages from the host
wasp.onmessage = (msg) => {
    if (msg.type === "parameter") {
        // update UI to reflect parameter change
    }
};
```

### Message Types

**parameter** — a parameter value change
```json
{ "type": "parameter", "id": 0, "value": 1200.0 }
```

**transport** — host playback state
```json
{ "type": "transport", "playing": true, "bpm": 120.0, "position": 0 }
```

**midi** — a MIDI event
```json
{ "type": "midi", "status": 144, "data1": 60, "data2": 100 }
```

### Notes
- The GUI must not assume it is always visible. Hosts may close and reopen
  the GUI window at any time.
- Parameter changes from the host (e.g. automation) will arrive via
  `wasp.onmessage`. The GUI should reflect these changes.
- Heavy visual processing (spectrum analysers, oscilloscopes) should be
  driven by periodic polling rather than per-sample messages.

---

## 6. Session State & Presets

### Session State
The host saves session state in two stages:

1. **Parameter state** — the host saves and restores all parameter values
   by calling `wasp_set_parameter` for each parameter after `wasp_initialize`.
2. **Plugin state** — if the plugin exports `wasp_save_state` and
   `wasp_load_state`, the host will use these to save and restore any
   additional internal state beyond parameters.

Hosts should always restore parameter state first, then plugin state.

### Preset Format
Presets are stored as `.waspreset` files. This is a JSON file structured
as follows:
```json
{
  "pluginUniqueId": "com.MyCompany.MyPlugin",
  "pluginVersion": "0.1.0",
  "presetVersion": "0.1.0",
  "name": "My Preset",
  "author": "",
  "parameters": [
    {
      "id": 0,
      "value": 182.233
    }
  ]
}
```

### Preset Fields
- `pluginUniqueId` — must match the plugin's `uniqueId` in the manifest
- `pluginVersion` — the version of the plugin this preset was created with
- `presetVersion` — the version of the preset format
- `name` — human-readable preset name
- `author` — optional

Hosts should warn the user if `pluginVersion` does not match the installed
version of the plugin. Hosts must not refuse to load a preset on this basis.

---

## 7. Sandbox Rules

WASP plugins run inside a WASM sandbox. Plugins:
- Must not require direct filesystem access
- Must not require direct network access
- Must not rely on OS-specific behaviour
- May request sandboxed file access via host API (see Open Questions)

These restrictions are enforced by the WASM runtime. They are a feature,
not a limitation — they guarantee plugins cannot crash or compromise the host.

---

## 8. Open Questions

These are unresolved design decisions for future drafts:

- [ ] Threading model — can plugins spawn their own threads?
- [ ] Host-provided sandboxed file access API (for samples, IRs, presets)
- [ ] Plugin versioning and compatibility guarantees
- [ ] Parameter modulation and sample-accurate automation
- [ ] In-process vs sandboxed process execution model