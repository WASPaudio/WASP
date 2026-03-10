/*
 * wasp_host.h — WASP Host Library
 *
 * A C library for loading and running WASP audio plugins.
 * Part of the WASP (WebAssembly Sandboxed Audio Plugin) project.
 *
 * Usage:
 *   #include "wasp_host.h"
 *   Link against: wasp_host.c, wasmtime, zip
 */

#ifndef WASP_HOST_H
#define WASP_HOST_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Version ─────────────────────────────────────────────────────────────── */

#define WASP_HOST_VERSION_MAJOR 0
#define WASP_HOST_VERSION_MINOR 4
#define WASP_HOST_VERSION_PATCH 0

/* ── Event Types (incoming, host → plugin) ───────────────────────────────── */

#define WASP_EVENT_MIDI             0
#define WASP_EVENT_PARAM            1
#define WASP_EVENT_STORAGE_RESULT   2
#define WASP_EVENT_FILE_RESULT      3
#define WASP_EVENT_NETWORK_RESULT   4

/* ── Request Types (outgoing, plugin → host) ─────────────────────────────── */

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

/* ── Core Extensions ─────────────────────────────────────────────────────── */

#define WASP_EXT_MIDI        "wasp.midi"
#define WASP_EXT_GUI         "wasp.gui"
#define WASP_EXT_STATE       "wasp.state"
#define WASP_EXT_LATENCY     "wasp.latency"
#define WASP_EXT_TRANSPORT   "wasp.transport"
#define WASP_EXT_TAIL        "wasp.tail"
#define WASP_EXT_REQUESTS    "wasp.requests"

/* ── Structs ─────────────────────────────────────────────────────────────── */

#pragma pack(push, 1)

typedef struct {
    uint32_t playing;
    float    bpm;
    float    beat;
    uint32_t time_sig_num;
    uint32_t time_sig_denom;
} WaspTransport;  /* 20 bytes */

typedef struct {
    uint32_t type;
    uint32_t sample_offset;
    uint32_t param0;
    uint32_t param1;
    uint32_t param2;
    uint32_t param3;
} WaspEvent;  /* 24 bytes */

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
} WaspProcessContext;  /* 52 bytes */

#pragma pack(pop)

/* ── Parameter descriptor (from manifest) ───────────────────────────────── */

typedef enum {
    WASP_PARAM_FLOAT = 0,
    WASP_PARAM_ENUM  = 1,
    WASP_PARAM_BOOL  = 2,
} WaspParamType;

typedef struct {
    uint32_t     id;
    char*        name;
    char*        description;
    WaspParamType type;
    float        min;
    float        max;
    float        default_value;
    bool         visible;
    /* enum only */
    char**       enum_values;
    uint32_t     enum_count;
} WaspParamDescriptor;

/* ── Opaque types ────────────────────────────────────────────────────────── */

typedef struct WaspEngine        WaspEngine;
typedef struct WaspManifest      WaspManifest;
typedef struct WaspInstance      WaspInstance;
typedef struct WaspProcessBuffer WaspProcessBuffer;

/* ── Error codes ─────────────────────────────────────────────────────────── */

typedef enum {
    WASP_OK                    = 0,
    WASP_ERROR_BUNDLE_NOT_FOUND  = 1,
    WASP_ERROR_INVALID_BUNDLE    = 2,
    WASP_ERROR_WASM_NOT_FOUND    = 3,
    WASP_ERROR_COMPILE_FAILED    = 4,
    WASP_ERROR_INSTANTIATE_FAILED = 5,
    WASP_ERROR_MISSING_EXPORT    = 6,
    WASP_ERROR_PROCESS_FAILED    = 7,
    WASP_ERROR_OUT_OF_MEMORY     = 8,
    WASP_ERROR_INVALID_ARGUMENT  = 9,
} WaspError;

/* Returns a human-readable string for a WaspError code. */
const char* wasp_error_string(WaspError error);

/* ── Engine ──────────────────────────────────────────────────────────────── */

/*
 * Creates a new WASP engine. One engine per host application is typical.
 * The engine owns the Wasmtime engine and store.
 * Returns NULL on failure.
 */
WaspEngine* wasp_engine_create();

/*
 * Destroys the engine and frees all associated resources.
 * All instances created from this engine must be destroyed first.
 */
void wasp_engine_destroy(WaspEngine* engine);

/* ── Manifest ────────────────────────────────────────────────────────────── */

/*
 * Loads and parses the manifest from a .wasp bundle.
 * Returns NULL on failure.
 */
WaspManifest* wasp_manifest_load(const char* bundle_path);

/* Destroys the manifest and frees all associated resources. */
void wasp_manifest_destroy(WaspManifest* manifest);

/* Returns true if the plugin declares the given extension. */
bool wasp_manifest_has_extension(const WaspManifest* manifest, const char* extension);

/* Returns true if the plugin declares the given permission. */
bool wasp_manifest_has_permission(const WaspManifest* manifest, const char* permission);

/* Accessors */
const char* wasp_manifest_name(const WaspManifest* manifest);
const char* wasp_manifest_unique_id(const WaspManifest* manifest);
const char* wasp_manifest_version(const WaspManifest* manifest);
const char* wasp_manifest_author(const WaspManifest* manifest);
const char* wasp_manifest_type(const WaspManifest* manifest);
const char* wasp_manifest_category(const WaspManifest* manifest);
const char* wasp_manifest_ui_path(const WaspManifest* manifest);
uint32_t    wasp_manifest_input_count(const WaspManifest* manifest);
uint32_t    wasp_manifest_output_count(const WaspManifest* manifest);
uint32_t    wasp_manifest_param_count(const WaspManifest* manifest);

/* Returns a pointer to the parameter descriptor at the given index.
 * Owned by the manifest — do not free. */
const WaspParamDescriptor* wasp_manifest_get_param(const WaspManifest* manifest, uint32_t index);

/* ── Instance ────────────────────────────────────────────────────────────── */

/*
 * Creates a new plugin instance from a .wasp bundle.
 * Loads dsp.wasm from the bundle, compiles it, and instantiates it.
 * Does not call wasp_initialize — call wasp_instance_initialize separately.
 * Returns NULL on failure. Check wasp_instance_last_error for details.
 */
WaspInstance* wasp_instance_create(WaspEngine* engine, const char* bundle_path);

/* Destroys the instance and frees all associated resources. */
void wasp_instance_destroy(WaspInstance* instance);

/*
 * Returns the last error that occurred on this instance.
 * Cleared on each successful call.
 */
WaspError wasp_instance_last_error(const WaspInstance* instance);

/*
 * Returns the manifest for this instance.
 * Owned by the instance — do not free.
 */
const WaspManifest* wasp_instance_manifest(const WaspInstance* instance);

/*
 * Calls wasp_initialize on the plugin.
 * Must be called before wasp_instance_process.
 * Returns WASP_OK on success.
 */
WaspError wasp_instance_initialize(WaspInstance* instance,
                                    uint32_t sample_rate,
                                    uint32_t max_block_size);

/*
 * Calls wasp_process on the plugin using the given process buffer.
 * The buffer must have been committed via wasp_process_buffer_commit
 * before calling this.
 * Returns WASP_OK on success.
 */
WaspError wasp_instance_process(WaspInstance* instance, WaspProcessBuffer* buffer);

/*
 * Calls wasp_terminate on the plugin.
 */
void wasp_instance_terminate(WaspInstance* instance);

/*
 * Returns the plugin's current latency in samples.
 * Requires wasp.latency extension. Returns 0 if not supported.
 */
uint32_t wasp_instance_get_latency(WaspInstance* instance);

/*
 * Returns the plugin's current tail length in samples.
 * Requires wasp.tail extension. Returns 0 if not supported.
 */
uint32_t wasp_instance_get_tail(WaspInstance* instance);

/*
 * Calls wasp_save_state and returns the state data.
 * Requires wasp.state extension.
 * Caller must free the returned buffer.
 * Returns NULL if not supported or on failure.
 */
uint8_t* wasp_instance_save_state(WaspInstance* instance, uint32_t* size_out);

/*
 * Calls wasp_load_state with the given data.
 * Requires wasp.state extension.
 * Returns WASP_OK on success.
 */
WaspError wasp_instance_load_state(WaspInstance* instance,
                                    const uint8_t* data,
                                    uint32_t size);

/* ── Process Buffer ──────────────────────────────────────────────────────── */

/*
 * Creates a process buffer for the given instance.
 * Allocates audio and event buffers in WASM memory.
 * max_events: maximum number of events per block.
 * num_channels: number of output channels.
 * Returns NULL on failure.
 */
WaspProcessBuffer* wasp_process_buffer_create(WaspInstance* instance,
                                               uint32_t max_block_size,
                                               uint32_t max_events,
                                               uint32_t num_channels);

/* Destroys the process buffer. */
void wasp_process_buffer_destroy(WaspProcessBuffer* buffer);

/*
 * Begins a new block. Must be called before adding events.
 * Clears the event list and sets the frame count and transport.
 */
void wasp_process_buffer_begin(WaspProcessBuffer* buffer,
                                uint32_t frames,
                                WaspTransport transport);

/*
 * Adds an event to the current block.
 * Events are automatically sorted by sample_offset on commit.
 * Returns false if the event buffer is full.
 */
bool wasp_process_buffer_add_event(WaspProcessBuffer* buffer, WaspEvent event);

/*
 * Commits the buffer — sorts events and writes the context struct
 * into WASM memory ready for wasp_instance_process.
 * Must be called after wasp_process_buffer_begin and before
 * wasp_instance_process.
 */
void wasp_process_buffer_commit(WaspProcessBuffer* buffer);

/*
 * Returns a pointer to the output samples for the given channel
 * after wasp_instance_process has been called.
 * The pointer is valid until the next call to wasp_process_buffer_begin.
 */
float* wasp_process_buffer_get_channel(WaspProcessBuffer* buffer, uint32_t channel);

/*
 * Returns the number of channels this buffer was created with.
 */
uint32_t wasp_process_buffer_channel_count(const WaspProcessBuffer* buffer);

/* ── Event Builders ──────────────────────────────────────────────────────── */

/* Creates a MIDI event. */
static inline WaspEvent wasp_event_midi(uint32_t sample_offset,
                                         uint8_t status,
                                         uint8_t data1,
                                         uint8_t data2) {
    WaspEvent e = {0};
    e.type          = WASP_EVENT_MIDI;
    e.sample_offset = sample_offset;
    e.param0        = status;
    e.param1        = data1;
    e.param2        = data2;
    e.param3        = 0;
    return e;
}

/* Creates a parameter change event. */
static inline WaspEvent wasp_event_param(uint32_t sample_offset,
                                          uint32_t param_id,
                                          float value) {
    WaspEvent e = {0};
    uint32_t bits;
    e.type          = WASP_EVENT_PARAM;
    e.sample_offset = sample_offset;
    e.param0        = param_id;
    /* safe float-to-bits conversion */
    __builtin_memcpy(&bits, &value, sizeof(float));
    e.param1        = bits;
    e.param2        = 0;
    e.param3        = 0;
    return e;
}

/* Creates a default transport struct (stopped, 120 BPM, 4/4). */
static inline WaspTransport wasp_transport_default() {
    WaspTransport t = {0};
    t.playing        = 0;
    t.bpm            = 120.0f;
    t.beat           = 0.0f;
    t.time_sig_num   = 4;
    t.time_sig_denom = 4;
    return t;
}

/* Creates a playing transport struct. */
static inline WaspTransport wasp_transport_playing(float bpm, float beat,
                                                    uint32_t time_sig_num,
                                                    uint32_t time_sig_denom) {
    WaspTransport t = {0};
    t.playing        = 1;
    t.bpm            = bpm;
    t.beat           = beat;
    t.time_sig_num   = time_sig_num;
    t.time_sig_denom = time_sig_denom;
    return t;
}

#ifdef __cplusplus
}
#endif

#endif /* WASP_HOST_H */
