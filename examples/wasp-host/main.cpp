#include <algorithm>
#include <filesystem>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <wasmtime.h>
#include <zip.h>

void write_wav(const char* filename, float* samples, int num_samples, int sample_rate) {
    FILE* f = fopen(filename, "wb");
    int num_channels = 2;
    int bits_per_sample = 16;
    int byte_rate = sample_rate * num_channels * bits_per_sample / 8;
    int block_align = num_channels * bits_per_sample / 8;
    int data_size = num_samples * num_channels * bits_per_sample / 8;
    int chunk_size = 36 + data_size;
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

bool wasp_call(wasmtime_store_t* store, wasmtime_func_t fn,
               wasmtime_val_t* args, size_t nargs,
               wasmtime_val_t* results, size_t nresults,
               const char* label) {
    wasmtime_context_t* context = wasmtime_store_context(store);
    wasm_trap_t* trap = NULL;
    wasmtime_error_t* error = wasmtime_func_call(context, &fn, args, nargs, results, nresults, &trap);
    if (error) {
        wasm_byte_vec_t msg; wasmtime_error_message(error, &msg);
        printf("%s error: %.*s\n", label, (int)msg.size, msg.data);
        wasm_byte_vec_delete(&msg); wasmtime_error_delete(error);
        return false;
    }
    if (trap) {
        wasm_byte_vec_t msg; wasm_trap_message(trap, &msg);
        printf("%s trap: %.*s\n", label, (int)msg.size, msg.data);
        wasm_byte_vec_delete(&msg); wasm_trap_delete(trap);
        return false;
    }
    return true;
}

// Load dsp.wasm from a .wasp bundle
// Returns heap-allocated bytes and sets size. Caller must free().
uint8_t* load_wasm_from_bundle(const char* wasp_path, size_t* out_size) {
    int err = 0;
    zip_t* zip = zip_open(wasp_path, ZIP_RDONLY, &err);
    if (!zip) {
        printf("Failed to open .wasp bundle: %s (error %d)\n", wasp_path, err);
        return nullptr;
    }

    zip_file_t* file = zip_fopen(zip, "dsp.wasm", 0);
    if (!file) {
        printf("dsp.wasm not found in bundle\n");
        zip_close(zip);
        return nullptr;
    }

    zip_stat_t stat = {};
    zip_stat(zip, "dsp.wasm", 0, &stat);

    uint8_t* bytes = (uint8_t*)malloc(stat.size);
    zip_fread(file, bytes, stat.size);
    zip_fclose(file);
    zip_close(zip);

    *out_size = stat.size;
    printf("Loaded dsp.wasm from bundle: %zu bytes\n", *out_size);
    return bytes;
}

#pragma pack(push, 1)
struct WaspTransport {
    uint32_t playing;
    float    bpm;
    float    beat;
    uint32_t time_sig_num;
    uint32_t time_sig_denom;
};
struct WaspEvent {
    uint32_t type;
    uint32_t sample_offset;
    uint32_t param0;
    uint32_t param1;
    uint32_t param2;
    uint32_t param3;
};
struct WaspProcessContext {
    uint32_t      inputs_offset;
    uint32_t      outputs_offset;
    uint32_t      input_count;
    uint32_t      output_count;
    uint32_t      frames;
    uint32_t      sample_rate;
    uint32_t      events_offset;
    uint32_t      event_count;
    WaspTransport transport;
};
#pragma pack(pop)

#define WASP_EVENT_MIDI  0
#define WASP_EVENT_PARAM 1

struct WaspInstance {
    wasmtime_instance_t instance;
    wasmtime_func_t     init_fn;
    wasmtime_func_t     process_fn;
    wasmtime_func_t     get_buf_fn;
    wasmtime_memory_t   memory;
    uint32_t            ctx_offset;
    uint32_t            ch0_offset;
    uint32_t            ch1_offset;
    uint32_t            ch_table_offset;
    uint32_t            events_offset;
};

WaspInstance create_instance(wasmtime_store_t* store, wasmtime_module_t* module,
                              int sample_rate, int block_size, int num_channels) {
    WaspInstance inst = {};
    wasmtime_context_t* ctx = wasmtime_store_context(store);
    wasm_trap_t* trap = NULL;
    wasmtime_error_t* error = wasmtime_instance_new(ctx, module, NULL, 0, &inst.instance, &trap);
    if (error || trap) { printf("Failed to instantiate\n"); exit(1); }

    wasmtime_extern_t init_e, process_e, get_buf_e, memory_e;
    ctx = wasmtime_store_context(store);
    wasmtime_instance_export_get(ctx, &inst.instance, "wasp_initialize",         15, &init_e);
    wasmtime_instance_export_get(ctx, &inst.instance, "wasp_process",            12, &process_e);
    wasmtime_instance_export_get(ctx, &inst.instance, "wasp_get_process_buffer", 23, &get_buf_e);
    wasmtime_instance_export_get(ctx, &inst.instance, "memory",                   6, &memory_e);

    inst.init_fn    = init_e.of.func;
    inst.process_fn = process_e.of.func;
    inst.get_buf_fn = get_buf_e.of.func;
    inst.memory     = memory_e.of.memory;

    wasmtime_val_t init_args[2];
    init_args[0].kind = WASMTIME_I32; init_args[0].of.i32 = sample_rate;
    init_args[1].kind = WASMTIME_I32; init_args[1].of.i32 = block_size;
    wasp_call(store, inst.init_fn, init_args, 2, NULL, 0, "wasp_initialize");

    wasmtime_val_t result; result.kind = WASMTIME_I32;
    wasp_call(store, inst.get_buf_fn, NULL, 0, &result, 1, "wasp_get_process_buffer");
    inst.ctx_offset = (uint32_t)result.of.i32;
    printf("instance ctx_offset: %u\n", inst.ctx_offset);

    inst.ch0_offset      = 1024;
    inst.ch1_offset      = inst.ch0_offset + block_size * sizeof(float);
    inst.ch_table_offset = inst.ch1_offset + block_size * sizeof(float);
    inst.events_offset   = inst.ch_table_offset + num_channels * sizeof(uint32_t);

    ctx = wasmtime_store_context(store);
    uint8_t* wasm_mem = wasmtime_memory_data(ctx, &inst.memory);
    uint32_t* ch_table = (uint32_t*)(wasm_mem + inst.ch_table_offset);
    ch_table[0] = inst.ch0_offset;
    ch_table[1] = inst.ch1_offset;

    return inst;
}

void process_instance(wasmtime_store_t* store, WaspInstance& inst,
                      WaspEvent* events, uint32_t event_count,
                      int frames_this_block, int sample_rate,
                      int num_channels, int frames_written,
                      float* mix_buffer) {
    wasmtime_context_t* ctx = wasmtime_store_context(store);
    uint8_t* wasm_mem = wasmtime_memory_data(ctx, &inst.memory);

    WaspEvent* wasm_events = (WaspEvent*)(wasm_mem + inst.events_offset);
    memcpy(wasm_events, events, event_count * sizeof(WaspEvent));

    WaspProcessContext* context_struct = (WaspProcessContext*)(wasm_mem + inst.ctx_offset);
    context_struct->inputs_offset  = 0;
    context_struct->outputs_offset = inst.ch_table_offset;
    context_struct->input_count    = 0;
    context_struct->output_count   = num_channels;
    context_struct->frames         = frames_this_block;
    context_struct->sample_rate    = sample_rate;
    context_struct->events_offset  = inst.events_offset;
    context_struct->event_count    = event_count;
    context_struct->transport      = { 1, 120.0f, (float)frames_written / sample_rate * 2.0f, 4, 4 };

    wasmtime_val_t args[1];
    args[0].kind = WASMTIME_I32; args[0].of.i32 = (int32_t)inst.ctx_offset;
    wasp_call(store, inst.process_fn, args, 1, NULL, 0, "wasp_process");

    ctx = wasmtime_store_context(store);
    wasm_mem = wasmtime_memory_data(ctx, &inst.memory);
    float* ch0 = (float*)(wasm_mem + inst.ch0_offset);
    float* ch1 = (float*)(wasm_mem + inst.ch1_offset);
    for (int i = 0; i < frames_this_block; i++) {
        mix_buffer[(frames_written + i) * 2 + 0] += ch0[i];
        mix_buffer[(frames_written + i) * 2 + 1] += ch1[i];
    }
}

int main() {
    wasm_engine_t* engine = wasm_engine_new();
    wasmtime_store_t* store = wasmtime_store_new(engine, NULL, NULL);

    std::cout << "Working dir: " << std::filesystem::current_path() << std::endl;

    // Load from .wasp bundle
    size_t wasm_size = 0;
    uint8_t* wasm_bytes = load_wasm_from_bundle(
        "../../wasp-sdk/target/wasm32-unknown-unknown/debug/Sting.wasp",
        &wasm_size);
    if (!wasm_bytes) return 1;

    wasmtime_module_t* module = NULL;
    wasmtime_error_t* error = wasmtime_module_new(engine, wasm_bytes, wasm_size, &module);
    free(wasm_bytes);
    if (error) {
        wasm_byte_vec_t msg; wasmtime_error_message(error, &msg);
        printf("compile error: %.*s\n", (int)msg.size, msg.data);
        wasm_byte_vec_delete(&msg); wasmtime_error_delete(error);
        return 1;
    }

    int sample_rate      = 44100;
    int num_channels     = 2;
    int block_size       = 512;
    int duration_seconds = 6;
    int total_frames     = sample_rate * duration_seconds;

    WaspInstance inst_a = create_instance(store, module, sample_rate, block_size, num_channels);
    WaspInstance inst_b = create_instance(store, module, sample_rate, block_size, num_channels);

    float* mix_buffer = (float*)calloc(total_frames * num_channels, sizeof(float));

    struct Note { int start_frame; int end_frame; uint8_t midi_note; };

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

    const int MAX_EVENTS = 64;

    int frames_written = 0;
    while (frames_written < total_frames) {
        int frames_this_block = block_size;
        if (frames_written + frames_this_block > total_frames)
            frames_this_block = total_frames - frames_written;

        WaspEvent events_a[MAX_EVENTS]; uint32_t count_a = 0;
        if (frames_written == 0) {
            float w = 0.0f; uint32_t bits; memcpy(&bits, &w, 4);
            events_a[count_a++] = { WASP_EVENT_PARAM, 0, 0, bits, 0, 0 };
        }
        for (int i = 0; i < num_notes_a; i++) {
            if (frames_written <= notes_a[i].start_frame &&
                frames_written + frames_this_block > notes_a[i].start_frame)
                events_a[count_a++] = { WASP_EVENT_MIDI,
                    (uint32_t)(notes_a[i].start_frame - frames_written),
                    0x90, notes_a[i].midi_note, 100, 0 };
            if (frames_written <= notes_a[i].end_frame &&
                frames_written + frames_this_block > notes_a[i].end_frame)
                events_a[count_a++] = { WASP_EVENT_MIDI,
                    (uint32_t)(notes_a[i].end_frame - frames_written),
                    0x80, notes_a[i].midi_note, 0, 0 };
        }
        std::sort(events_a, events_a + count_a, [](const WaspEvent& a, const WaspEvent& b) {
            return a.sample_offset < b.sample_offset; });

        WaspEvent events_b[MAX_EVENTS]; uint32_t count_b = 0;
        if (frames_written == 0) {
            float w = 1.0f; uint32_t bits; memcpy(&bits, &w, 4);
            events_b[count_b++] = { WASP_EVENT_PARAM, 0, 0, bits, 0, 0 };
        }
        for (int i = 0; i < num_notes_b; i++) {
            if (frames_written <= notes_b[i].start_frame &&
                frames_written + frames_this_block > notes_b[i].start_frame)
                events_b[count_b++] = { WASP_EVENT_MIDI,
                    (uint32_t)(notes_b[i].start_frame - frames_written),
                    0x90, notes_b[i].midi_note, 100, 0 };
            if (frames_written <= notes_b[i].end_frame &&
                frames_written + frames_this_block > notes_b[i].end_frame)
                events_b[count_b++] = { WASP_EVENT_MIDI,
                    (uint32_t)(notes_b[i].end_frame - frames_written),
                    0x80, notes_b[i].midi_note, 0, 0 };
        }
        std::sort(events_b, events_b + count_b, [](const WaspEvent& a, const WaspEvent& b) {
            return a.sample_offset < b.sample_offset; });

        process_instance(store, inst_a, events_a, count_a,
                         frames_this_block, sample_rate, num_channels,
                         frames_written, mix_buffer);
        process_instance(store, inst_b, events_b, count_b,
                         frames_this_block, sample_rate, num_channels,
                         frames_written, mix_buffer);

        frames_written += frames_this_block;
    }

    float peak = 0.0f;
    for (int i = 0; i < total_frames * num_channels; i++)
        peak = std::max(peak, std::abs(mix_buffer[i]));
    if (peak > 0.0f)
        for (int i = 0; i < total_frames * num_channels; i++)
            mix_buffer[i] /= peak * 1.1f;

    write_wav("output.wav", mix_buffer, total_frames, sample_rate);
    printf("Written output.wav — %d seconds at %d Hz\n", duration_seconds, sample_rate);

    free(mix_buffer);
    wasmtime_module_delete(module);
    wasmtime_store_delete(store);
    wasm_engine_delete(engine);

    return 0;
}