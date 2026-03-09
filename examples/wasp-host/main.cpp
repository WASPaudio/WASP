#include <filesystem>
#include <iostream>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wasmtime.h>

// Minimal WAV writer so we can hear the output
void write_wav(const char* filename, float* samples, int num_samples, int sample_rate) {
    FILE* f = fopen(filename, "wb");

    int num_channels = 2;
    int bits_per_sample = 16;
    int byte_rate = sample_rate * num_channels * bits_per_sample / 8;
    int block_align = num_channels * bits_per_sample / 8;
    int data_size = num_samples * num_channels * bits_per_sample / 8;
    int chunk_size = 36 + data_size;

    // RIFF header
    fwrite("RIFF", 1, 4, f);
    fwrite(&chunk_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    // fmt chunk
    fwrite("fmt ", 1, 4, f);
    int fmt_size = 16;
    short audio_format = 1; // PCM
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&audio_format, 2, 1, f);
    fwrite(&num_channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);

    // data chunk
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);

    // Convert float samples to int16
    for (int i = 0; i < num_samples * num_channels; i++) {
        short s = (short)(samples[i] * 32767.0f);
        fwrite(&s, 2, 1, f);
    }

    fclose(f);
}

int main() {
    // Load the WASM file
    wasm_engine_t* engine = wasm_engine_new();
    wasmtime_store_t* store = wasmtime_store_new(engine, NULL, NULL);
    wasmtime_context_t* context = wasmtime_store_context(store);

    // Read the .wasm file

    std::cout << std::filesystem::current_path() << std::endl;

    FILE* f = fopen("../../wasp-sdk/target/wasm32-unknown-unknown/debug/wasp_sine.wasm", "rb");
    if (!f) {
        printf("Failed to open .wasm file\n");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long wasm_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* wasm_bytes = (uint8_t*)malloc(wasm_size);
    fread(wasm_bytes, 1, wasm_size, f);
    fclose(f);

    // Compile and instantiate
    wasmtime_module_t* module = NULL;
    wasmtime_error_t* error = wasmtime_module_new(engine, wasm_bytes, wasm_size, &module);
    free(wasm_bytes);
    if (error) {
        printf("Failed to compile module\n");
        return 1;
    }

    wasmtime_instance_t instance;
    wasm_trap_t* trap = NULL;
    error = wasmtime_instance_new(context, module, NULL, 0, &instance, &trap);
    if (error || trap) {
        printf("Failed to instantiate module\n");
        return 1;
    }

    // Get exported functions
    wasmtime_extern_t init_fn, process_fn, param_fn, midi_fn, memory_extern;

    wasmtime_instance_export_get(context, &instance, "wasp_initialize", 15, &init_fn);
    wasmtime_instance_export_get(context, &instance, "wasp_process", 12, &process_fn);
    wasmtime_instance_export_get(context, &instance, "wasp_set_parameter", 18, &param_fn);
    wasmtime_instance_export_get(context, &instance, "wasp_midi_event", 15, &midi_fn);
    wasmtime_instance_export_get(context, &instance, "memory", 6, &memory_extern);

    wasmtime_memory_t memory = memory_extern.of.memory;

    // Call wasp_initialize(44100, 512)
    wasmtime_val_t init_args[2];
    init_args[0].kind = WASMTIME_I32; init_args[0].of.i32 = 44100;
    init_args[1].kind = WASMTIME_I32; init_args[1].of.i32 = 512;
    wasmtime_func_call(context, &init_fn.of.func, init_args, 2, NULL, 0, &trap);

    auto send_midi = [&](uint8_t status, uint8_t data1, uint8_t data2) {
        wasm_trap_t* midi_trap = NULL;  // separate trap variable
        wasmtime_val_t midi_args[3];
        midi_args[0].kind = WASMTIME_I32; midi_args[0].of.i32 = status;
        midi_args[1].kind = WASMTIME_I32; midi_args[1].of.i32 = data1;
        midi_args[2].kind = WASMTIME_I32; midi_args[2].of.i32 = data2;
        wasmtime_func_call(context, &midi_fn.of.func, midi_args, 3, NULL, 0, &midi_trap);
        if (midi_trap) {
            printf("MIDI call trapped!\n");
            wasm_trap_delete(midi_trap);
        }
    };

    // Simple melody - each note lasts 0.5 seconds = 22050 frames
    struct Note {
        int start_frame;
        int end_frame;
        uint8_t midi_note;
    };

    Note melody[] = {
        { 0,      22050, 60 }, // C4
        { 22050,  44100, 62 }, // D4
        { 44100,  66150, 64 }, // E4
        { 66150,  88200, 65 }, // F4
        { 88200,  110250, 67 }, // G4
        { 110250, 132300, 69 }, // A4
        { 132300, 154350, 71 }, // B4
        { 154350, 176400, 72 }, // C5
    };
    int num_notes = 8;

    // Set waveform to square before loop
    wasmtime_val_t param_args[2];
    param_args[0].kind = WASMTIME_I32; param_args[0].of.i32 = 2;
    param_args[1].kind = WASMTIME_F32; param_args[1].of.f32 = 0.0f;
    wasmtime_func_call(context, &param_fn.of.func, param_args, 2, NULL, 0, &trap);

    // Set up audio buffers in WASM memory
    int sample_rate = 44100;
    int num_channels = 2;
    int block_size = 512;
    int duration_seconds = 10;
    int total_frames = sample_rate * duration_seconds;

    // Allocate output buffer in host memory for WAV writing
    float* wav_buffer = (float*)calloc(total_frames * num_channels, sizeof(float));

    // We'll put input and output buffers at known offsets in WASM memory
    uint8_t* wasm_mem = wasmtime_memory_data(context, &memory);
    int input_offset  = 0;
    int output_offset = block_size * num_channels * sizeof(float);

    int frames_written = 0;
    while (frames_written < total_frames) {
        int frames_this_block = block_size;
        if (frames_written + frames_this_block > total_frames)
            frames_this_block = total_frames - frames_written;

        // Call wasp_process(input_ptr, output_ptr, num_frames, num_channels)
        wasmtime_val_t process_args[4];
        process_args[0].kind = WASMTIME_I32; process_args[0].of.i32 = input_offset;
        process_args[1].kind = WASMTIME_I32; process_args[1].of.i32 = output_offset;
        process_args[2].kind = WASMTIME_I32; process_args[2].of.i32 = frames_this_block;
        process_args[3].kind = WASMTIME_I32; process_args[3].of.i32 = num_channels;


        // Send note on/off events based on position
        for (int i = 0; i < num_notes; i++) {
            // Note on at start of note
            if (frames_written <= melody[i].start_frame &&
                frames_written + frames_this_block > melody[i].start_frame) {
                send_midi(0x90, melody[i].midi_note, 100); // note on, velocity 100
                }
            // Note off at end of note
            if (frames_written <= melody[i].end_frame &&
                frames_written + frames_this_block > melody[i].end_frame) {
                send_midi(0x80, melody[i].midi_note, 0); // note off
                }
        }

        wasmtime_func_call(context, &process_fn.of.func, process_args, 4, NULL, 0, &trap);

        // Copy output from WASM memory to our WAV buffer
        float* output_ptr = (float*)(wasm_mem + output_offset);
        printf("first sample: %.6f\n", output_ptr[0]);
        memcpy(wav_buffer + frames_written * num_channels,
               output_ptr,
               frames_this_block * num_channels * sizeof(float));

        frames_written += frames_this_block;
    }

    // Write WAV file
    write_wav("output.wav", wav_buffer, total_frames, sample_rate);
    printf("Written output.wav — %d seconds at %d Hz\n", duration_seconds, sample_rate);

    // Cleanup
    free(wav_buffer);
    wasmtime_module_delete(module);
    wasmtime_store_delete(store);
    wasm_engine_delete(engine);

    return 0;
}