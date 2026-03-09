# WASP - WebAssembly Sandboxed Audio Plugin
## Specification v0.3.0 (Draft)

---

## 1. Overview

WASP (WebAssembly Sandboxed Audio Plugin) is an open, cross-platform audio plugin format
built on WebAssembly and web technologies. Plugins consist of a WASM DSP module
and an optional HTML/CSS/JS GUI, packaged together in a `.wasp` bundle.

Goals:
- True cross-platform support (Linux, Windows, macOS, web, mobile) from a single binary
- Sandboxed execution - plugins cannot crash the host
- Simple, approachable developer experience
- Host-neutral design, not tied to any specific DAW
- Sample-accurate automation and MIDI via event queues
- Fine-grained permission system for host resource access

---

## 2. Plugin Bundle Format

A `.wasp` file is a ZIP archive with the following structure:
```
myplugin.wasp
├── manifest.json
├── icon.png          (optional, recommended 256x256, SVG also accepted)
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
  "permissions": [
    "storage.own",
    "storage.domain",
    "network.internet",
    "files.read"
  ],
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
      "id": 1,
      "name": "Waveform",
      "description": "Oscillator waveform shape",
      "type": "enum",
      "values": ["Sine", "Square", "Sawtooth", "Triangle"],
      "default": 0,
      "visible": true
    },
    {
      "id": 2,
      "name": "Bypass",
      "description": "Bypass the plugin",
      "type": "bool",
      "default": 0,
      "visible": true
    }
  ]
}
```

### Plugin Types
- `"instrument"` - generates audio from MIDI. Typically has 0 inputs.
- `"effect"` - processes incoming audio. Has both inputs and outputs.

### Categories
Suggested values for `category`:
`"synthesizer"`, `"sampler"`, `"drum"`, `"filter"`, `"equalizer"`,
`"reverb"`, `"delay"`, `"distortion"`, `"compressor"`, `"utility"`, `"other"`

Hosts may use this field to organise plugins in their browser UI.

[//]: # (TODO: These categories need work)

### Parameter Types

- `"float"` - a continuous value between `min` and `max`.
  Hosts should render this as a knob or slider.

- `"enum"` - a discrete value represented as an integer index into `values`.
  Hosts should render this as a dropdown or segmented control.
  `min` and `max` are not required for enum parameters.
  `default` is an integer index, e.g. `0` for the first value.

- `"bool"` - a value of either `0.0` or `1.0`.
  Hosts should render this as a toggle or on/off switch.
  `min` and `max` are not required for bool parameters.

The DSP always receives parameter values as floats via the event system.
For `enum` parameters the host passes the index as a float, e.g. `1.0` for
the second value. Plugins should cast this to an integer internally.

### Parameter Fields
- `id` - unique integer identifier used in the DSP ABI and messaging
- `name` - human-readable label for display in the host
- `description` - optional longer description of the parameter
- `type` - `"float"`, `"enum"`, or `"bool"`
- `min`, `max`, `default` - value range and default value
- `visible` - boolean, whether the host should display this parameter
  as automatable. Defaults to `true` if omitted.

### Manifest Fields
- `uniqueId` - reverse-domain unique identifier, e.g. `com.MyCompany.MyPlugin`.
  Used by hosts to match presets to plugins and to scope permissions.
- `category` - optional hint for host plugin browsers
- `icon` - optional path to icon file within the bundle
- `ui` - optional path to GUI entry point within the bundle
- `permissions` - list of permissions the plugin requests. See section 8.

---

## 4. DSP ABI

### Overview

The WASP DSP ABI is designed around a single process call. All audio, MIDI,
parameter changes, and transport information are delivered to the plugin inside
a single `WaspProcessContext` per audio block. This ensures:

- Sample-accurate event timing
- Single-threaded audio processing
- No race conditions between parameter, MIDI, and audio callbacks
- Clean host implementation - collect everything, sort, call once

### Memory Model

WASP plugins run inside a WASM linear memory space. The host can read and write
into this memory directly. Rather than passing pointers (which are meaningless
across the host/plugin boundary), all references inside structs are expressed as
`uint32_t` offsets into WASM linear memory.

The plugin exports a function `wasp_get_process_buffer` which allocates and
returns the offset of a `WaspProcessContext` in its own linear memory. The host
writes into this buffer before each call to `wasp_process`. This ensures the
plugin controls its own memory layout.

### Structs

All structs use little-endian byte order and no implicit padding.
Fields are listed in memory order.

**WaspTransport**
```c
typedef struct {
    uint32_t playing;        // 1 if playing, 0 if stopped
    float    bpm;            // beats per minute
    float    beat;           // current position in beats
    uint32_t time_sig_num;   // time signature numerator e.g. 4
    uint32_t time_sig_denom; // time signature denominator e.g. 4
} WaspTransport;             // 20 bytes
```

**WaspEvent**
```c
typedef struct {
    uint32_t type;          // event type (see Event Types below)
    uint32_t sample_offset; // sample offset within the current block
    uint32_t param0;        // reinterpreted based on type
    uint32_t param1;
    uint32_t param2;
    uint32_t param3;
} WaspEvent;                // 24 bytes
```

**WaspProcessContext**
```c
typedef struct {
    uint32_t      inputs_offset;  // offset to input channel offset table
    uint32_t      outputs_offset; // offset to output channel offset table
    uint32_t      input_count;    // number of input channels
    uint32_t      output_count;   // number of output channels
    uint32_t      frames;         // number of frames in this block
    uint32_t      sample_rate;    // current sample rate
    uint32_t      events_offset;  // offset to WaspEvent array
    uint32_t      event_count;    // number of events in this block
    WaspTransport transport;      // current transport state (inline)
} WaspProcessContext;             // 52 bytes
```

**Channel offset tables**

`inputs_offset` and `outputs_offset` point to arrays of `uint32_t`, one per
channel, each being the offset into WASM memory of that channel's float buffer.

Example for stereo output:
```
outputs_offset → [uint32_t ch0_offset, uint32_t ch1_offset]
ch0_offset     → [float, float, float, ... (frames floats)]
ch1_offset     → [float, float, float, ... (frames floats)]
```

### Event Types
```c
#define WASP_EVENT_MIDI           0
#define WASP_EVENT_PARAM          1
#define WASP_EVENT_STORAGE_RESULT 2
#define WASP_EVENT_FILE_RESULT    3
#define WASP_EVENT_NETWORK_RESULT 4
```

**WASP_EVENT_MIDI**
```
param0 = status byte (e.g. 0x90 for note on)
param1 = data1      (e.g. MIDI note number)
param2 = data2      (e.g. velocity)
param3 = unused
```

**WASP_EVENT_PARAM**
```
param0 = parameter id
param1 = value reinterpreted as float bits (use memcpy to convert safely)
param2 = unused
param3 = unused
```

**WASP_EVENT_STORAGE_RESULT**
```
param0 = request id (matched to original request)
param1 = offset of result string in WASM memory (UTF-8)
param2 = length of result string in bytes
param3 = 1 if found, 0 if key did not exist
```

**WASP_EVENT_FILE_RESULT**
```
param0 = request id
param1 = offset of file data in WASM memory
param2 = length of file data in bytes
param3 = 1 on success, 0 on failure or user cancelled
```

**WASP_EVENT_NETWORK_RESULT**
```
param0 = request id
param1 = HTTP status code
param2 = offset of response body in WASM memory
param3 = length of response body in bytes
```

### Host Request Events

Plugins send requests to the host by writing request events into a separate
outgoing event buffer. The host reads this buffer after each `wasp_process`
call and fulfils requests asynchronously, returning results as events in a
future block.
```c
#define WASP_REQUEST_STORAGE_GET  0
#define WASP_REQUEST_STORAGE_SET  1
#define WASP_REQUEST_FILE_OPEN    2
#define WASP_REQUEST_FILE_SAVE    3
#define WASP_REQUEST_NETWORK_GET  4
#define WASP_REQUEST_NETWORK_POST 5
#define WASP_REQUEST_NOTIFY       6
#define WASP_REQUEST_CLIPBOARD_READ  7
#define WASP_REQUEST_CLIPBOARD_WRITE 8
```

The outgoing event buffer offset is returned by `wasp_get_request_buffer`.
Its layout mirrors the incoming event buffer - a count followed by an array
of `WaspEvent` structs, reinterpreted for request types.

### Exported Functions

The WASM module must export the following functions:
```c
// Called once when the plugin is loaded.
// Returns 1 on success, 0 on failure.
int32_t wasp_initialize(uint32_t sample_rate, uint32_t max_block_size);

// Returns the offset in WASM linear memory of the plugin's WaspProcessContext.
// The host writes into this buffer before each call to wasp_process.
uint32_t wasp_get_process_buffer();

// Returns the offset of the plugin's outgoing request buffer.
// The host reads this after each wasp_process call.
uint32_t wasp_get_request_buffer();

// Called once per audio block.
// ctx_offset is the value returned by wasp_get_process_buffer.
void wasp_process(uint32_t ctx_offset);

// Called by the host to save plugin state.
// Returns offset of state data; writes size in bytes to size_out.
uint32_t wasp_save_state(uint32_t* size_out);

// Called by the host to restore plugin state.
void wasp_load_state(uint32_t data_offset, uint32_t size);

// Called when the plugin is unloaded.
void wasp_terminate();
```

### Notes
- `wasp_save_state`, `wasp_load_state`, and `wasp_get_request_buffer` are
  optional exports. Hosts must function correctly if they are absent.
- The host must not call `wasp_process` from multiple threads simultaneously.
- The host must sort incoming events by `sample_offset` before writing them
  into the context buffer.
- Block size may vary between calls. Plugins must not assume a fixed size.
- Hosts must ignore request events for permissions the plugin has not declared
  in its manifest.

---

## 5. GUI

The GUI is an HTML page rendered inside a WebView provided by the host.
It communicates with the DSP via the host messaging API. The GUI is optional -
hosts must work correctly without it.

### Host JavaScript API

The host injects a global `wasp` object into the WebView:
```javascript
// Send a message to the host
wasp.send({ type: "parameter", id: 0, value: 1200.0 });

// Receive messages from the host
wasp.onmessage = (msg) => {
    if (msg.type === "parameter") {
        // update UI to reflect parameter change
    }
};
```

### Message Types

**parameter** - a parameter value change
```json
{ "type": "parameter", "id": 0, "value": 1200.0 }
```

**transport** - host playback state
```json
{
    "type": "transport",
    "playing": true,
    "bpm": 120.0,
    "beat": 4.0,
    "timeSigNum": 4,
    "timeSigDenom": 4
}
```

**midi** - a MIDI event
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

## 6. State, Session & Presets

### State Model

WASP separates plugin state into two layers:

**Parameter state** - the set of all parameter values as defined in the
manifest. This is always managed by the host, is human-readable, and is
the basis for automation and presets.

**Binary state** - arbitrary plugin-managed data returned by
`wasp_save_state`. This may include internal state that has no corresponding
parameter - for example, a sequencer's step data, a sampler's loaded file
path, or an arpeggiator pattern. This is opaque to the host.

Hosts that support binary state must:
1. Call `wasp_save_state` and store the returned blob alongside parameter state.
2. On restore, first inject all parameter values as `WASP_EVENT_PARAM` events
   at the start of the first block, then call `wasp_load_state` with the blob.

Hosts that do not support binary state must fall back to parameter-only
restore. Plugins must remain functional under parameter-only restore, even
if some internal state is lost.

### Session State

When saving a project the host saves:
- All parameter values from its own parameter model
- The binary blob from `wasp_save_state` if exported

Session state format is host-defined. Hosts are not required to use
`.wpreset` for internal project saves. The session binary blob may differ
from the preset binary blob - plugins may choose to include additional
session-only data (e.g. UI state, undo history) that is not appropriate
for a shareable preset.

### Preset Format

Presets are stored as `.wpreset` files. A `.wpreset` is a ZIP archive:
```
mypreset.wpreset
├── preset.json
└── blob.bin        (optional)
```

**preset.json**
```json
{
  "pluginUniqueId": "com.MyCompany.MyPlugin",
  "pluginVersion": "0.1.0",
  "presetVersion": "0.1.0",
  "name": "My Preset",
  "author": "",
  "parameters": [
    { "id": 0, "value": 182.233 },
    { "id": 1, "value": 1.0 }
  ]
}
```

**blob.bin**

Optional. Raw binary data returned by `wasp_save_state` at the time the
preset was saved. The host passes this to `wasp_load_state` after restoring
parameters. If present but the plugin does not export `wasp_load_state`,
the host must ignore it silently.

### Preset Fields
- `pluginUniqueId` - must match the plugin's `uniqueId` in the manifest
- `pluginVersion` - the version of the plugin this preset was saved with
- `presetVersion` - the version of the preset format, currently `"0.1.0"`
- `name` - human-readable preset name
- `author` - optional

Hosts should warn the user if `pluginVersion` does not match the installed
version of the plugin. Hosts must not refuse to load a preset on this basis.

---

## 7. Sandbox Rules

WASP plugins run inside a WASM sandbox. Plugins:
- Must not require direct filesystem access
- Must not require direct network access
- Must not rely on OS-specific behaviour
- Must not access other plugins' storage without explicit user permission
- May request host-mediated access to resources via the permission system

These restrictions are enforced by the WASM runtime. They are a feature,
not a limitation - they guarantee plugins cannot crash or compromise the host.

---

## 8. Permissions

Plugins declare required permissions in their manifest. The host presents
these to the user when the plugin is first loaded. Users may grant or deny
permissions individually. Hosts must not fulfil requests for permissions
the plugin has not declared, and must silently ignore such requests.

### Permission Types

**Storage**

- `storage.own` - implicit, always granted. Plugin can read and write its
  own storage. Data is stored by the host at a conventional location:
  - Linux: `~/.config/wasp/com.MyCompany.MyPlugin/`
  - Windows: `%APPDATA%\wasp\com.MyCompany.MyPlugin\`
  - macOS: `~/Library/Application Support/wasp/com.MyCompany.MyPlugin\`
  - Web: host-defined (e.g. localStorage or server-side)
  - mobile: host-defined

- `storage.domain` - requires user approval. Plugin can read and write
  storage for all plugins sharing its domain prefix.
  e.g. `com.MyCompany.MyPlugin` may access `com.MyCompany.*/`.
  Host prompt: *"[Plugin] is requesting access to shared storage for all
  [com.MyCompany] plugins."*

**Network**

- `network.internet` - requires user approval. Plugin may request the host
  make HTTP/HTTPS calls on its behalf. Hosts should display the domains
  a plugin intends to contact at permission grant time if declared in the
  manifest. Hosts may show a warning if a plugin attempts to contact an
  undeclared domain.

**Files**

- `files.read` - requires user approval. Plugin may request the host open
  a file picker and return file contents. The host always presents a native
  file dialog - plugins cannot request arbitrary paths directly.

- `files.write` - requires user approval. Plugin may request the host open
  a save dialog and write data to a user-chosen location.

- `audio.decode` - requires user approval. Plugin may request the host
  decode an audio file into raw PCM samples. The host handles format
  support - the plugin receives raw floats regardless of source format.

**MIDI Hardware**

- `midi.input` - requires user approval. Plugin may receive events from
  external MIDI devices connected to the host.

- `midi.output` - requires user approval. Plugin may send MIDI events to
  external MIDI devices connected to the host.

**Inter-plugin Communication**

- `ipc.domain` - requires user approval. Plugin may send and receive
  messages from other loaded plugins sharing its domain prefix.
  Plugins outside the domain cannot be contacted.

**UI**

- `notifications` - requires user approval. Plugin may ask the host to
  display a notification to the user. Hosts may rate-limit or suppress
  notifications at their discretion.

- `clipboard.read` - requires user approval. Plugin may request the
  current clipboard contents from the host.

- `clipboard.write` - requires user approval. Plugin may ask the host
  to write data to the clipboard.

### Permission Guidelines for Hosts

- Permissions must be presented clearly to the user before being granted.
- Hosts should allow users to review and revoke permissions at any time.
- `storage.own` must always be granted and must not require user interaction.
- Hosts must silently ignore requests from plugins for undeclared permissions.
- Hosts should log permission requests and fulfilments for auditability.
- For `network.internet`, hosts should consider showing the user which
  domains are being contacted to protect against data exfiltration.

---
## 9. Installation & Storage Locations

### Plugin Installation

WASP plugins are installed as `.wasp` files into the user's WASP directory.
The WASP directory is located at `~/WASP/` on all desktop platforms:

- Linux: `~/WASP/`
- Windows: `C:\Users\[username]\WASP\`
- macOS: `~/WASP/`

The WASP directory has the following structure:
```
~/WASP/
├── plugins/      ← installed .wasp bundles
├── presets/      ← user .wpreset files
│   └── com.MyCompany.MyPlugin/
├── data/         ← global plugin storage (storage.own)
│   └── com.MyCompany.MyPlugin/
└── cache/        ← host-managed cache, safe to delete
```

Hosts must scan `~/WASP/plugins/` for installed plugins on startup.
Hosts may also support additional scan paths configured by the user.

### Global Plugin Storage

Plugin storage via `storage.own` is written to `~/WASP/data/[uniqueId]/`
on all desktop platforms. Using a consistent path across platforms and hosts
ensures that data such as login tokens and user preferences persists
correctly when a user switches between DAWs.

| Platform | Path                                                    |
| -------- | ------------------------------------------------------- |
| Linux    | `~/WASP/data/com.MyCompany.MyPlugin/`                   |
| Windows  | `C:\Users\[username]\WASP\data\com.MyCompany.MyPlugin\` |
| macOS    | `~/WASP/data/com.MyCompany.MyPlugin\`                   |

`storage.domain` access extends this to `~/WASP/data/com.MyCompany.*/`
with user approval.

### Mobile Platforms

Mobile operating systems do not permit shared storage between apps.
On mobile platforms, global plugin storage is scoped to the host app:

- iOS: `[Host App]/Application Support/wasp/[uniqueId]/`
- Android: `[Host App]/files/wasp/[uniqueId]/`

As a result, `storage.own` and `storage.domain` on mobile mean "shared
within the same host app" rather than "shared across all DAWs on the
device". Plugins that rely on cross-host persistent storage (e.g. for
licence verification) should be aware of this limitation and handle it
gracefully.

### Web Platforms

Web hosts have no access to the local filesystem. Global plugin storage
on web platforms is host-defined. Hosts should use a consistent key
namespace based on the plugin's `uniqueId` to avoid collisions:

- Recommended: `localStorage` key prefix `wasp.[uniqueId].`
- Or a server-side store keyed by `uniqueId` if the host has a backend

Plugin installation on web platforms is also host-defined. Hosts may
load `.wasp` bundles from a URL, a server-side registry, or a local
file picker.

---
## 10. Open Questions

- [ ] Threading model - can plugins spawn their own threads?
- [ ] Plugin versioning and compatibility guarantees
- [ ] Parameter modulation and sample-accurate automation curves
- [ ] In-process vs sandboxed process execution model
- [ ] MPE support
- [ ] Note expression events
- [ ] Global storage on web platforms - localStorage vs server-side
- [ ] Network permission: should hosts enforce a domain allowlist declared
      in the manifest, or just warn on undeclared domains?
- [ ] IPC event format - how are inter-plugin messages structured?
- [ ] Storage size limits per plugin
- [ ] Permission revocation behaviour - what happens to a running plugin
      if the user revokes a permission mid-session?