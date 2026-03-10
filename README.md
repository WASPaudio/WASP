# WASP - WebAssembly Sandboxed Audio Plugin
## Specification v0.5.0 (Draft)

---

## 1. Overview

WASP (WebAssembly Sandboxed Audio Plugin) is an open, cross-platform audio plugin format
built on WebAssembly and web technologies. Plugins consist of a WASM DSP module
and an optional HTML/CSS/JS GUI, packaged together in a `.wasp` bundle.

Goals:
- True cross-platform support (Windows, Linux, macOS, web, mobile) from a single binary
- Sandboxed execution - plugins cannot crash the host
- Simple, approachable developer experience
- Host-neutral design, not tied to any specific DAW
- Sample-accurate automation and MIDI via event queues
- Fine-grained permission system for host resource access
- Extensible capability system for optional features

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
  "ui": "ui/index.html",
  "extensions": [
    "wasp.midi",
    "wasp.gui",
    "wasp.state",
    "wasp.latency",
    "wasp.transport"
  ],
  "permissions": [
    "storage.own",
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

### Parameter Types

- `"float"` - a continuous value between `min` and `max`.
- `"enum"` - a discrete integer index into `values`.
- `"bool"` - `0.0` or `1.0`, rendered as a toggle.

The DSP always receives parameter values as floats via the event system.
For `enum` parameters the host passes the index as a float. Plugins should
cast to integer internally.

### Parameter Fields
- `id` - unique integer identifier
- `name` - human-readable label
- `description` - optional longer description
- `type` - `"float"`, `"enum"`, or `"bool"`
- `min`, `max`, `default` - value range and default
- `visible` - whether the host should expose this parameter for automation.
  Defaults to `true` if omitted.

### Manifest Fields
- `uniqueId` - reverse-domain unique identifier e.g. `com.MyCompany.MyPlugin`
- `category` - optional hint for host plugin browsers
- `icon` - optional path to icon file within the bundle
- `ui` - optional path to GUI entry point. Required if `wasp.gui` is declared.
- `extensions` - list of capability extensions. See section 4.
- `permissions` - list of resource access permissions. See section 8.

---

## 4. Extensions

Extensions are optional capabilities a plugin declares in its manifest.
The host checks the `extensions` list before calling any optional exports
or sending any optional events. Hosts must silently ignore optional exports
from plugins that have not declared the corresponding extension.

### Core Extensions

**wasp.midi**
Plugin accepts MIDI events. The host may send `WASP_EVENT_MIDI` events.
No additional exports required.

**wasp.gui**
Plugin has a GUI. The host should load `ui/index.html` from the bundle
into a WebView when the user opens the plugin window.
The manifest `ui` field must be present if this extension is declared.

**wasp.state**
Plugin manages binary state beyond parameters. Requires exports:
```c
uint32_t wasp_save_state(uint32_t* size_out);
void     wasp_load_state(uint32_t data_offset, uint32_t size);
```

**wasp.latency**
Plugin reports its processing latency. Requires export:
```c
uint32_t wasp_get_latency(); // returns latency in samples
```
The host calls this after `wasp_initialize` and after receiving
`WASP_REQUEST_LATENCY_CHANGED`. See latency section below.

**wasp.transport**
Plugin uses transport information from `WaspProcessContext`. The host
should populate the `transport` field accurately when this extension
is declared. Hosts may skip populating transport for plugins that do
not declare this extension.

**wasp.tail**
Plugin produces audio after its input stops - e.g. reverb, delay.
Requires export:
```c
// Returns the tail length in samples.
// Called by the host after the last note off or input signal ends.
// Host continues calling wasp_process until tail expires or returns 0.
uint32_t wasp_get_tail();
```

**wasp.requests**
Plugin uses the outgoing request buffer to communicate with the host.
Requires export:
```c
uint32_t wasp_get_request_buffer();
```
Required if the plugin uses any permission-gated host services
(storage, network, files, notifications, clipboard, MIDI hardware, IPC).

### Third-Party Extensions

Third-party extensions follow reverse-domain naming:
```json
"extensions": ["com.MyCompany.my_extension"]
```

Hosts must ignore unrecognised extension identifiers. Third-party
extensions may define their own exports, events, and request types
using the same memory model as core extensions.

---

## 5. DSP ABI

### Overview

The WASP DSP ABI is designed around a single process call. All audio, MIDI,
parameter changes, and transport information are delivered to the plugin inside
a single `WaspProcessContext` per audio block. This ensures:

- Sample-accurate event timing
- Single-threaded audio processing
- No race conditions between parameter, MIDI, and audio callbacks
- Clean host implementation - collect everything, sort, call once

### Memory Model

WASP plugins run inside a WASM linear memory space. The host can read and
write into this memory directly. Rather than passing pointers, all references
inside structs are expressed as `uint32_t` offsets into WASM linear memory.

The plugin exports `wasp_get_process_buffer` which returns the offset of a
`WaspProcessContext` in its own linear memory. The host writes into this
buffer before each call to `wasp_process`.

### Structs

All structs use little-endian byte order and no implicit padding.
Fields are listed in memory order.

**WaspTransport**
```c
typedef struct {
    uint32_t playing;        // 1 if playing, 0 if stopped
    float    bpm;            // beats per minute
    float    beat;           // current position in beats
    uint32_t time_sig_num;   // time signature numerator
    uint32_t time_sig_denom; // time signature denominator
} WaspTransport;             // 20 bytes
```

**WaspEvent**
```c
typedef struct {
    uint32_t type;          // event type
    uint32_t sample_offset; // sample offset within the current block
    uint32_t param0;
    uint32_t param1;
    uint32_t param2;
    uint32_t param3;
} WaspEvent;                // 24 bytes
```

**WaspProcessContext**
```c
typedef struct {
    uint32_t      inputs_offset;
    uint32_t      outputs_offset;
    uint32_t      input_count;
    uint32_t      output_count;
    uint32_t      frames;
    uint32_t      sample_rate;
    uint32_t      events_offset;
    uint32_t      event_count;
    WaspTransport transport;
} WaspProcessContext;        // 52 bytes
```

**Channel offset tables**

`inputs_offset` and `outputs_offset` point to arrays of `uint32_t`, one per
channel, each being the offset of that channel's float buffer.
```
outputs_offset → [uint32_t ch0_offset, uint32_t ch1_offset]
ch0_offset     → [float, float, ... (frames floats)]
ch1_offset     → [float, float, ... (frames floats)]
```

### Incoming Event Types
```c
#define WASP_EVENT_MIDI           0
#define WASP_EVENT_PARAM          1
#define WASP_EVENT_STORAGE_RESULT 2
#define WASP_EVENT_FILE_RESULT    3
#define WASP_EVENT_NETWORK_RESULT 4
```

**WASP_EVENT_MIDI** - requires `wasp.midi`
```
param0 = status byte
param1 = data1
param2 = data2
param3 = unused
```

**WASP_EVENT_PARAM**
```
param0 = parameter id
param1 = value as float bits (use memcpy to convert)
param2 = unused
param3 = unused
```

**WASP_EVENT_STORAGE_RESULT**
```
param0 = request id
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

### Outgoing Request Types

Requires `wasp.requests`. Plugin writes these into the outgoing request
buffer returned by `wasp_get_request_buffer`. The host reads this buffer
after each `wasp_process` call.
```c
#define WASP_REQUEST_STORAGE_GET        0
#define WASP_REQUEST_STORAGE_SET        1
#define WASP_REQUEST_FILE_OPEN          2
#define WASP_REQUEST_FILE_SAVE          3
#define WASP_REQUEST_NETWORK_GET        4
#define WASP_REQUEST_NETWORK_POST       5
#define WASP_REQUEST_NOTIFY             6
#define WASP_REQUEST_CLIPBOARD_READ     7
#define WASP_REQUEST_CLIPBOARD_WRITE    8
#define WASP_REQUEST_LATENCY_CHANGED    9
#define WASP_REQUEST_TAIL_CHANGED       10
```

**WASP_REQUEST_LATENCY_CHANGED** - requires `wasp.latency`
```
param0 = new latency in samples (informational)
param1 = unused
param2 = unused
param3 = unused
```
Host must call `wasp_get_latency` to confirm the new value.

**WASP_REQUEST_TAIL_CHANGED** - requires `wasp.tail`
```
param0 = new tail length in samples (informational)
param1 = unused
param2 = unused
param3 = unused
```
Host must call `wasp_get_tail` to confirm the new value.

### Required Exported Functions
```c
// Called once when the plugin is loaded. Returns 1 on success, 0 on failure.
int32_t wasp_initialize(uint32_t sample_rate, uint32_t max_block_size);

// Returns offset of WaspProcessContext in WASM linear memory.
uint32_t wasp_get_process_buffer();

// Called once per audio block.
void wasp_process(uint32_t ctx_offset);

// Called when the plugin is unloaded.
void wasp_terminate();
```

### Optional Exported Functions
```c
// wasp.requests
uint32_t wasp_get_request_buffer();

// wasp.state
uint32_t wasp_save_state(uint32_t* size_out);
void     wasp_load_state(uint32_t data_offset, uint32_t size);

// wasp.latency
uint32_t wasp_get_latency();

// wasp.tail
uint32_t wasp_get_tail();
```

### Processing Rules
- The host must not call `wasp_process` from multiple threads simultaneously.
- The host must sort incoming events by `sample_offset` before writing them.
- Block size may vary between calls.
- The host must only call optional exports if the corresponding extension
  is declared in the manifest.
- The host must ignore outgoing request events for undeclared extensions.

### Latency

Plugins with zero latency must not declare `wasp.latency`. The host assumes
zero latency for plugins that do not declare this extension.

Plugins must not change latency arbitrarily during playback. Latency changes
should only occur in response to parameter changes that affect processing
algorithm (e.g. FFT size, lookahead duration).

Latency is always reported in whole samples. Fractional sample latency is
not supported.

The host must compensate for reported latency by delaying parallel signal
paths to keep the mix time-aligned.

---

## 6. GUI

The GUI is an HTML page rendered inside a WebView provided by the host.
Requires `wasp.gui` extension. The GUI is optional - hosts must work
correctly without it.

### Host JavaScript API
```javascript
wasp.send({ type: "parameter", id: 0, value: 1200.0 });

wasp.onmessage = (msg) => {
    if (msg.type === "parameter") { ... }
};
```

### Message Types

**parameter**
```json
{ "type": "parameter", "id": 0, "value": 1200.0 }
```

**transport** - only sent if `wasp.transport` is declared
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

**midi** - only sent if `wasp.midi` is declared
```json
{ "type": "midi", "status": 144, "data1": 60, "data2": 100 }
```

### Notes
- The GUI must not assume it is always visible.
- Parameter changes from automation will arrive via `wasp.onmessage`.
- Heavy visual processing should be driven by periodic polling.

---

## 7. State, Session & Presets

### State Model

**Parameter state** - always managed by the host. The basis for automation
and presets.

**Binary state** - requires `wasp.state`. Arbitrary plugin-managed data
returned by `wasp_save_state`. Opaque to the host.

On restore, the host must:
1. Inject all parameter values as `WASP_EVENT_PARAM` events at the start
   of the first block after `wasp_initialize`.
2. Call `wasp_load_state` with the binary blob if `wasp.state` is declared.

Hosts that do not support binary state fall back to parameter-only restore.
Plugins must remain functional under parameter-only restore.

### Session State

Session state format is host-defined. The session binary blob may differ
from the preset binary blob - plugins may include session-only data
(UI state, undo history) not appropriate for a shareable preset.

### Preset Format
```
mypreset.wpreset  (ZIP)
├── preset.json
└── blob.bin      (optional, requires wasp.state)
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

Hosts should warn if `pluginVersion` does not match the installed version.
Hosts must not refuse to load a preset on this basis.

---

## 8. Permissions

Plugins declare required permissions in their manifest. The host presents
these to the user when the plugin is first loaded. Hosts must not fulfil
requests for undeclared permissions and must silently ignore such requests.

All permission-gated features require `wasp.requests`.

### Permission Types

**Storage**

- `storage.own` - implicit, always granted. Plugin can read and write its
  own storage at:
  - Linux: `~/WASP/data/com.MyCompany.MyPlugin/`
  - Windows: `C:\Users\[user]\WASP\data\com.MyCompany.MyPlugin\`
  - macOS: `~/WASP/data/com.MyCompany.MyPlugin\`
  - iOS: `[Host]/Application Support/wasp/com.MyCompany.MyPlugin/`
  - Android: `[Host]/files/wasp/com.MyCompany.MyPlugin/`
  - Web: host-defined

- `storage.domain` - requires user approval. Access to all storage under
  the plugin's domain prefix e.g. `com.MyCompany.*`.

**Network**

- `network.internet` - requires user approval. Plugin may request HTTP/HTTPS
  calls via the host.

**Files**

- `files.read` - requires user approval. Plugin may request a host file
  picker and receive file contents.

- `files.write` - requires user approval. Plugin may request a host save
  dialog and write data to a user-chosen location.

- `audio.decode` - requires user approval. Plugin may request the host
  decode an audio file into raw PCM samples.

**MIDI Hardware**

- `midi.hardware.input` - requires user approval. Plugin may receive from
  external MIDI devices.

- `midi.hardware.output` - requires user approval. Plugin may send to
  external MIDI devices.

**Inter-plugin Communication**

- `ipc.domain` - requires user approval. Plugin may communicate with other
  loaded plugins sharing its domain prefix.

**UI**

- `notifications` - requires user approval. Plugin may ask the host to
  display a notification.

- `clipboard.read` - requires user approval.

- `clipboard.write` - requires user approval.

### Permission Guidelines for Hosts

- `storage.own` must always be granted without user interaction.
- Permissions must be presented clearly before being granted.
- Users must be able to review and revoke permissions at any time.
- Hosts must silently ignore requests for undeclared permissions.
- For `network.internet`, hosts should log domains contacted.
- Hosts must ignore all permission-gated requests if `wasp.requests`
  is not declared.

---

## 9. Installation & Storage Locations

### Plugin Installation

WASP plugins are installed into `~/WASP/plugins/` on all desktop platforms:

- Linux: `~/WASP/`
- Windows: `C:\Users\[username]\WASP\`
- macOS: `~/WASP/`
```
~/WASP/
├── plugins/      ← installed .wasp bundles
├── presets/      ← user .wpreset files
│   └── com.MyCompany.MyPlugin/
├── data/         ← global plugin storage (storage.own)
│   └── com.MyCompany.MyPlugin/
└── cache/        ← host-managed cache, safe to delete
```

Hosts must scan `~/WASP/plugins/` on startup.
Hosts may support additional scan paths configured by the user.

### Mobile Platforms

On mobile, global storage is scoped to the host app:

- iOS: `[Host]/Application Support/wasp/[uniqueId]/`
- Android: `[Host]/files/wasp/[uniqueId]/`

`storage.own` and `storage.domain` on mobile mean "shared within the same
host app" rather than across all DAWs. Plugins relying on cross-host
persistent storage must handle this gracefully.

### Web Platforms

Web hosts have no local filesystem access. Storage is host-defined:

- Recommended: `localStorage` key prefix `wasp.[uniqueId].`
- Or server-side store keyed by `uniqueId`

Plugin installation on web is host-defined.

---

## 10. Sandbox Rules

WASP plugins run inside a WASM sandbox. Plugins:
- Must not require direct filesystem access
- Must not require direct network access
- Must not rely on OS-specific behaviour
- Must not access other plugins' storage without explicit user permission
- May request host-mediated access to resources via the permission system

---

## 11. Open Questions

- [ ] Threading model - can plugins spawn their own threads?
- [ ] Plugin versioning and compatibility guarantees
- [ ] Parameter modulation and sample-accurate automation curves
- [ ] In-process vs sandboxed process execution model
- [ ] MPE support
- [ ] Note expression events
- [ ] Global storage on web platforms
- [ ] Network permission: enforce domain allowlist or warn on undeclared domains?
- [ ] IPC event format - how are inter-plugin messages structured?
- [ ] Storage size limits per plugin
- [ ] Permission revocation behaviour mid-session
- [ ] Should hosts be required to support latency compensation or is it optional?
- [ ] Third-party extension registry - should there be a central registry
  to avoid extension ID collisions?