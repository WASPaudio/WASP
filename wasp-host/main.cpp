/*
 * main.cpp — WASP real-time host demo
 * Two synth instances → clipper, driven by MIDI keyboard + computer keyboard.
 */

#include <algorithm>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <csignal>

#include <portaudio.h>
#include <rtmidi/RtMidi.h>

// terminal raw mode
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

extern "C" {
#include "wasp_host.h"
#include "ring_buffer.h"
}

/* ── Global state ────────────────────────────────────────────────────────── */

static std::atomic<bool> g_running { true };

// shared ring buffer between input threads and audio thread
static RingBuffer g_midi_ring;

// plugin instances — set up on main thread, read on audio thread
static WaspEngine*       g_engine    = nullptr;
static WaspInstance*     g_synth_a   = nullptr;
static WaspInstance*     g_synth_b   = nullptr;
static WaspInstance*     g_clipper   = nullptr;
static WaspProcessBuffer* g_buf_a    = nullptr;
static WaspProcessBuffer* g_buf_b    = nullptr;
static WaspProcessBuffer* g_buf_clip = nullptr;

static int g_sample_rate  = 44100;
static int g_block_size   = 256;  // smaller block for lower latency
static int g_num_channels = 2;

// transport state
static std::atomic<uint64_t> g_frames_processed { 0 };

/* ── Computer keyboard note map ──────────────────────────────────────────── */

// a s d f g h j k = C3 D3 E3 F3 G3 A3 B3 C4
static const struct { char key; uint8_t note; } KEY_MAP[] = {
    { 'a', 48 }, // C3
    { 's', 50 }, // D3
    { 'd', 52 }, // E3
    { 'f', 53 }, // F3
    { 'g', 55 }, // G3
    { 'h', 57 }, // A3
    { 'j', 59 }, // B3
    { 'k', 60 }, // C4
    // upper octave: shift row
    { 'z', 60 }, // C4
    { 'x', 62 }, // D4
    { 'c', 64 }, // E4
    { 'v', 65 }, // F4
    { 'b', 67 }, // G4
    { 'n', 69 }, // A4
    { 'm', 71 }, // B4
};
static const int NUM_KEYS = sizeof(KEY_MAP) / sizeof(KEY_MAP[0]);

static uint8_t key_note_for(char c) {
    for (int i = 0; i < NUM_KEYS; i++)
        if (KEY_MAP[i].key == c) return KEY_MAP[i].note;
    return 0;
}

/* ── Terminal raw mode ───────────────────────────────────────────────────── */

static struct termios g_orig_termios;

static void terminal_raw_mode_enter() {
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);  // no echo, no line buffering
    raw.c_cc[VMIN]  = 0;              // non-blocking reads
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    // make stdin non-blocking
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}

static void terminal_raw_mode_exit() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
}

/* ── Signal handler ──────────────────────────────────────────────────────── */

static void on_signal(int) {
    g_running = false;
}

/* ── PortAudio callback ───────────────────────────────────────────────────── */

static int audio_callback(const void* /*input*/,
                           void* output,
                           unsigned long frames_per_buffer,
                           const PaStreamCallbackTimeInfo* /*time_info*/,
                           PaStreamCallbackFlags /*flags*/,
                           void* /*user_data*/) {
    float** out = (float**)output;
    uint64_t frames_so_far = g_frames_processed.load(std::memory_order_relaxed);

    WaspTransport transport = wasp_transport_playing(
        120.0f,
        (float)frames_so_far / g_sample_rate * 2.0f,
        4, 4);

    // drain ring buffer into events
    // collect all pending MIDI messages — will go to both synths
    MidiMessage msgs[64];
    int msg_count = 0;
    MidiMessage m;
    while (msg_count < 64 && ring_buffer_pop(&g_midi_ring, &m))
        msgs[msg_count++] = m;

    // ── Synth A (sine) ────────────────────────────────────────────────────
    wasp_process_buffer_begin(g_buf_a, (uint32_t)frames_per_buffer, transport);
    for (int i = 0; i < msg_count; i++)
        wasp_process_buffer_add_event(g_buf_a,
            wasp_event_midi(0, msgs[i].status, msgs[i].data1, msgs[i].data2));
    wasp_process_buffer_commit(g_buf_a);
    wasp_instance_process(g_synth_a, g_buf_a);

    // ── Synth B (square) ──────────────────────────────────────────────────
    wasp_process_buffer_begin(g_buf_b, (uint32_t)frames_per_buffer, transport);
    for (int i = 0; i < msg_count; i++)
        wasp_process_buffer_add_event(g_buf_b,
            wasp_event_midi(0, msgs[i].status, msgs[i].data1, msgs[i].data2));
    wasp_process_buffer_commit(g_buf_b);
    wasp_instance_process(g_synth_b, g_buf_b);

    // ── Mix A + B ─────────────────────────────────────────────────────────
    float* a0 = wasp_process_buffer_get_output_channel(g_buf_a, 0);
    float* a1 = wasp_process_buffer_get_output_channel(g_buf_a, 1);
    float* b0 = wasp_process_buffer_get_output_channel(g_buf_b, 0);
    float* b1 = wasp_process_buffer_get_output_channel(g_buf_b, 1);

    float* clip_in0 = wasp_process_buffer_get_input_channel(g_buf_clip, 0);
    float* clip_in1 = wasp_process_buffer_get_input_channel(g_buf_clip, 1);

    for (unsigned i = 0; i < frames_per_buffer; i++) {
        clip_in0[i] = a0[i] + b0[i];
        clip_in1[i] = a1[i] + b1[i];
    }

    // ── Clipper ───────────────────────────────────────────────────────────
    wasp_process_buffer_begin(g_buf_clip, (uint32_t)frames_per_buffer, transport);
    wasp_process_buffer_commit(g_buf_clip);
    wasp_instance_process(g_clipper, g_buf_clip);

    // ── Write to PortAudio output (interleaved stereo) ────────────────────
    float* clip_out0 = wasp_process_buffer_get_output_channel(g_buf_clip, 0);
    float* clip_out1 = wasp_process_buffer_get_output_channel(g_buf_clip, 1);

    float* interleaved = (float*)output;
    for (unsigned i = 0; i < frames_per_buffer; i++) {
        interleaved[i * 2 + 0] = clip_out0[i];
        interleaved[i * 2 + 1] = clip_out1[i];
    }

    g_frames_processed.fetch_add(frames_per_buffer, std::memory_order_relaxed);
    return paContinue;
}

/* ── RtMidi callback ─────────────────────────────────────────────────────── */

static void midi_callback(double /*timestamp*/,
                           std::vector<unsigned char>* message,
                           void* /*user_data*/) {
    if (!message || message->size() < 2) return;
    MidiMessage m;
    m.status = (*message)[0];
    m.data1  = message->size() > 1 ? (*message)[1] : 0;
    m.data2  = message->size() > 2 ? (*message)[2] : 0;
    ring_buffer_push(&g_midi_ring, m);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main() {
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    const char* synth_bundle   = "../../wasp-sdk/target/wasm32-unknown-unknown/debug/synth.wasp";
    const char* clipper_bundle = "../../wasp-sdk/target/wasm32-unknown-unknown/debug/clipper.wasp";

    ring_buffer_init(&g_midi_ring);

    // ── Load plugins ──────────────────────────────────────────────────────
    g_engine  = wasp_engine_create();
    g_synth_a = wasp_instance_create(g_engine, synth_bundle);
    g_synth_b = wasp_instance_create(g_engine, synth_bundle);
    g_clipper = wasp_instance_create(g_engine, clipper_bundle);

    if (!g_engine || !g_synth_a || !g_synth_b || !g_clipper) {
        fprintf(stderr, "Failed to create plugin instances\n");
        return 1;
    }

    wasp_instance_initialize(g_synth_a, g_sample_rate, g_block_size);
    wasp_instance_initialize(g_synth_b, g_sample_rate, g_block_size);
    wasp_instance_initialize(g_clipper, g_sample_rate, g_block_size);

    g_buf_a    = wasp_process_buffer_create(g_synth_a, g_block_size, 64, g_num_channels);
    g_buf_b    = wasp_process_buffer_create(g_synth_b, g_block_size, 64, g_num_channels);
    g_buf_clip = wasp_process_buffer_create(g_clipper, g_block_size, 64, g_num_channels);

    // set initial parameters
    // synth A — sine (waveform 0), synth B — square (waveform 1)
    // send as first-block events by processing a silent block
    {
        WaspTransport t = wasp_transport_default();

        wasp_process_buffer_begin(g_buf_a, g_block_size, t);
        wasp_process_buffer_add_event(g_buf_a, wasp_event_param(0, 0, 0.0f));
        wasp_process_buffer_commit(g_buf_a);
        wasp_instance_process(g_synth_a, g_buf_a);

        wasp_process_buffer_begin(g_buf_b, g_block_size, t);
        wasp_process_buffer_add_event(g_buf_b, wasp_event_param(0, 0, 1.0f));
        wasp_process_buffer_commit(g_buf_b);
        wasp_instance_process(g_synth_b, g_buf_b);

        wasp_process_buffer_begin(g_buf_clip, g_block_size, t);
        wasp_process_buffer_add_event(g_buf_clip, wasp_event_param(0, 0, 1.0f));  // pregain
        wasp_process_buffer_add_event(g_buf_clip, wasp_event_param(0, 1, 0.9f));  // threshold
        wasp_process_buffer_add_event(g_buf_clip, wasp_event_param(0, 2, 1.0f));  // outgain
        wasp_process_buffer_commit(g_buf_clip);
        wasp_instance_process(g_clipper, g_buf_clip);
    }

    // ── PortAudio setup ───────────────────────────────────────────────────
    Pa_Initialize();

    PaStreamParameters out_params;
    out_params.device                    = Pa_GetDefaultOutputDevice();
    out_params.channelCount              = g_num_channels;
    out_params.sampleFormat              = paFloat32; //| paNonInterleaved;
    out_params.suggestedLatency          = Pa_GetDeviceInfo(out_params.device)->defaultLowOutputLatency;
    out_params.hostApiSpecificStreamInfo = nullptr;

    int num_devices = Pa_GetDeviceCount();
    for (int i = 0; i < num_devices; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info->maxOutputChannels >= 2 &&
            std::string(info->name) == "pulse") {
            out_params.device = i;
            break;
            }
    }

    printf("PortAudio version: %s\n", Pa_GetVersionText());
    printf("Default output device: %d\n", Pa_GetDefaultOutputDevice());
    printf("Audio devices:\n");
    for (int i = 0; i < num_devices; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info->maxOutputChannels > 0)
            printf("  %d: %s (outputs: %d)\n", i, info->name, info->maxOutputChannels);
    }
    printf("Using audio device %d: %s\n", out_params.device,
           Pa_GetDeviceInfo(out_params.device)->name);

    PaStream* stream = nullptr;
    PaError err = Pa_OpenStream(&stream, nullptr, &out_params,
                    g_sample_rate, g_block_size,
                    paClipOff, audio_callback, nullptr);
    if (err != paNoError) {
        fprintf(stderr, "Pa_OpenStream failed: %s\n", Pa_GetErrorText(err));
        // try with default latency as fallback
        out_params.suggestedLatency = Pa_GetDeviceInfo(out_params.device)->defaultHighOutputLatency;
        err = Pa_OpenStream(&stream, nullptr, &out_params,
                            g_sample_rate, g_block_size,
                            paClipOff, audio_callback, nullptr);
        if (err != paNoError) {
            fprintf(stderr, "Pa_OpenStream fallback failed: %s\n", Pa_GetErrorText(err));
            return 1;
        }
    }

    Pa_StartStream(stream);

    // ── RtMidi setup ──────────────────────────────────────────────────────
    RtMidiIn* midi_in = nullptr;
    try {
        midi_in = new RtMidiIn();
        unsigned port_count = midi_in->getPortCount();
        if (port_count == 0) {
            printf("No MIDI input ports found — keyboard only\n");
        } else {
            printf("MIDI ports available:\n");
            for (unsigned i = 0; i < port_count; i++)
                printf("  %u: %s\n", i, midi_in->getPortName(i).c_str());
            midi_in->openPort(1);
            midi_in->setCallback(midi_callback);
            midi_in->ignoreTypes(false, true, true); // receive note on/off, ignore sysex/timing/sense
            printf("Opened MIDI port: %s\n", midi_in->getPortName(1).c_str());
        }
    } catch (RtMidiError& e) {
        fprintf(stderr, "RtMidi error: %s\n", e.getMessage().c_str());
    }

    // ── Keyboard input ────────────────────────────────────────────────────
    terminal_raw_mode_enter();

    printf("\nWASP real-time demo\n");
    printf("Keys: a s d f g h j k (C3-C4)  |  z x c v b n m (C4-B4)\n");
    printf("Press Ctrl+C or q to quit\n\n");

    bool key_held[256] = {};

    while (g_running) {
        char c = 0;
        ssize_t n = read(STDIN_FILENO, &c, 1);

        if (n > 0) {
            if (c == 'q' || c == 3 /* Ctrl+C */) {
                g_running = false;
                break;
            }

            uint8_t note = key_note_for(c);
            if (note > 0 && !key_held[(uint8_t)c]) {
                key_held[(uint8_t)c] = true;
                MidiMessage m { 0x90, note, 100 };
                ring_buffer_push(&g_midi_ring, m);
            }
        } else {
            // check for key releases — any held key not seen this cycle
            for (int i = 0; i < NUM_KEYS; i++) {
                uint8_t k = (uint8_t)KEY_MAP[i].key;
                if (key_held[k]) {
                    // re-check if key is still held by attempting a read
                    // terminal raw mode gives us keypresses but not releases
                    // so we release after a short timeout instead
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    terminal_raw_mode_exit();

    // ── Cleanup ───────────────────────────────────────────────────────────
    // send all notes off before stopping
    for (int i = 0; i < NUM_KEYS; i++) {
        if (key_held[(uint8_t)KEY_MAP[i].key]) {
            MidiMessage m { 0x80, KEY_MAP[i].note, 0 };
            ring_buffer_push(&g_midi_ring, m);
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();

    if (midi_in) {
        midi_in->closePort();
        delete midi_in;
    }

    wasp_process_buffer_destroy(g_buf_a);
    wasp_process_buffer_destroy(g_buf_b);
    wasp_process_buffer_destroy(g_buf_clip);
    wasp_instance_terminate(g_synth_a);
    wasp_instance_terminate(g_synth_b);
    wasp_instance_terminate(g_clipper);
    wasp_instance_destroy(g_synth_a);
    wasp_instance_destroy(g_synth_b);
    wasp_instance_destroy(g_clipper);
    wasp_engine_destroy(g_engine);

    return 0;
}