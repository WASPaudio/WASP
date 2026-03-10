#include <algorithm>
#include <filesystem>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "wasp_host.h"
}

static void write_wav(const char* filename, float* samples,
                      int num_samples, int sample_rate) {
    FILE* f = fopen(filename, "wb");
    int num_channels    = 2;
    int bits_per_sample = 16;
    int byte_rate       = sample_rate * num_channels * bits_per_sample / 8;
    int block_align     = num_channels * bits_per_sample / 8;
    int data_size       = num_samples * num_channels * bits_per_sample / 8;
    int chunk_size      = 36 + data_size;
    fwrite("RIFF", 1, 4, f); fwrite(&chunk_size, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    int fmt_size = 16; short audio_format = 1;
    fwrite(&fmt_size, 4, 1, f); fwrite(&audio_format, 2, 1, f);
    fwrite(&num_channels, 2, 1, f); fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f); fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_size, 4, 1, f);
    for (int i = 0; i < num_samples * num_channels; i++) {
        short s = (short)(samples[i] * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

struct Note { int start_frame; int end_frame; uint8_t midi_note; };

static void build_events(WaspProcessBuffer* buf, uint8_t waveform,
                          Note* notes, int note_count,
                          int frames_written, int frames_this_block,
                          bool first_block) {
    if (first_block)
        wasp_process_buffer_add_event(buf, wasp_event_param(0, 0, (float)waveform));

    for (int i = 0; i < note_count; i++) {
        if (frames_written <= notes[i].start_frame &&
            frames_written + frames_this_block > notes[i].start_frame)
            wasp_process_buffer_add_event(buf, wasp_event_midi(
                (uint32_t)(notes[i].start_frame - frames_written),
                0x90, notes[i].midi_note, 100));

        if (frames_written <= notes[i].end_frame &&
            frames_written + frames_this_block > notes[i].end_frame)
            wasp_process_buffer_add_event(buf, wasp_event_midi(
                (uint32_t)(notes[i].end_frame - frames_written),
                0x80, notes[i].midi_note, 0));
    }
}

int main() {
    std::cout << "Working dir: " << std::filesystem::current_path() << std::endl;

    const char* bundle = "../../wasp-sdk/target/wasm32-unknown-unknown/debug/Sting.wasp";

    int sample_rate      = 44100;
    int num_channels     = 2;
    int block_size       = 512;
    int duration_seconds = 6;
    int total_frames     = sample_rate * duration_seconds;
    int max_events       = 64;

    WaspEngine* engine = wasp_engine_create();
    if (!engine) { fprintf(stderr, "Failed to create engine\n"); return 1; }

    // Create two instances from the same bundle
    WaspInstance* inst_a = wasp_instance_create(engine, bundle);
    WaspInstance* inst_b = wasp_instance_create(engine, bundle);
    if (!inst_a || !inst_b) {
        fprintf(stderr, "Failed to create instances: %s\n",
                wasp_error_string(inst_a
                    ? wasp_instance_last_error(inst_b)
                    : wasp_instance_last_error(inst_a)));
        return 1;
    }

    wasp_instance_initialize(inst_a, sample_rate, block_size);
    wasp_instance_initialize(inst_b, sample_rate, block_size);

    WaspProcessBuffer* buf_a = wasp_process_buffer_create(inst_a, block_size, max_events, num_channels);
    WaspProcessBuffer* buf_b = wasp_process_buffer_create(inst_b, block_size, max_events, num_channels);

    // Instance A — sine arpeggio
    Note notes_a[] = {
        { 0,      20000, 60 },
        { 22050,  42050, 64 },
        { 44100,  64100, 67 },
        { 66150,  86150, 72 },
        { 88200, 108200, 67 },
        { 110250,130250, 64 },
        { 132300,152300, 60 },
    };
    int num_notes_a = 7;

    // Instance B — square sustained chords
    Note notes_b[] = {
        { 0,      83790, 48 },
        { 0,      83790, 55 },
        { 88200, 171990, 53 },
        { 88200, 171990, 60 },
    };
    int num_notes_b = 4;

    float* mix_buffer = (float*)calloc(total_frames * num_channels, sizeof(float));

    int frames_written = 0;
    while (frames_written < total_frames) {
        int frames_this_block = std::min(block_size, total_frames - frames_written);
        bool first_block      = frames_written == 0;

        WaspTransport transport = wasp_transport_playing(
            120.0f,
            (float)frames_written / sample_rate * 2.0f,
            4, 4);

        // Instance A — sine (waveform 0)
        wasp_process_buffer_begin(buf_a, frames_this_block, transport);
        build_events(buf_a, 0, notes_a, num_notes_a,
                     frames_written, frames_this_block, first_block);
        wasp_process_buffer_commit(buf_a);
        wasp_instance_process(inst_a, buf_a);

        // Instance B — square (waveform 1)
        wasp_process_buffer_begin(buf_b, frames_this_block, transport);
        build_events(buf_b, 1, notes_b, num_notes_b,
                     frames_written, frames_this_block, first_block);
        wasp_process_buffer_commit(buf_b);
        wasp_instance_process(inst_b, buf_b);

        // Mix both instances into mix_buffer
        float* a_ch0 = wasp_process_buffer_get_channel(buf_a, 0);
        float* a_ch1 = wasp_process_buffer_get_channel(buf_a, 1);
        float* b_ch0 = wasp_process_buffer_get_channel(buf_b, 0);
        float* b_ch1 = wasp_process_buffer_get_channel(buf_b, 1);

        for (int i = 0; i < frames_this_block; i++) {
            mix_buffer[(frames_written + i) * 2 + 0] += a_ch0[i] + b_ch0[i];
            mix_buffer[(frames_written + i) * 2 + 1] += a_ch1[i] + b_ch1[i];
        }

        frames_written += frames_this_block;
    }

    // Normalise
    float peak = 0.0f;
    for (int i = 0; i < total_frames * num_channels; i++)
        peak = std::max(peak, std::abs(mix_buffer[i]));
    if (peak > 0.0f)
        for (int i = 0; i < total_frames * num_channels; i++)
            mix_buffer[i] /= peak * 1.1f;

    write_wav("output.wav", mix_buffer, total_frames, sample_rate);
    printf("Written output.wav — %d seconds at %d Hz\n", duration_seconds, sample_rate);

    // Cleanup
    free(mix_buffer);
    wasp_process_buffer_destroy(buf_a);
    wasp_process_buffer_destroy(buf_b);
    wasp_instance_terminate(inst_a);
    wasp_instance_terminate(inst_b);
    wasp_instance_destroy(inst_a);
    wasp_instance_destroy(inst_b);
    wasp_engine_destroy(engine);

    return 0;
}