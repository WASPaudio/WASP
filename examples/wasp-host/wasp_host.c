/*
 * wasp_host.c — WASP Host Library Implementation
 *
 * Part of the WASP (WebAssembly Sandboxed Audio Plugin) project.
 * Requires: wasmtime C API, libzip
 */

#include "wasp_host.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <wasmtime.h>
#include <zip.h>

/* ── Internal helpers ────────────────────────────────────────────────────── */

#define WASP_MAX_EXTENSIONS  32
#define WASP_MAX_PERMISSIONS 32
#define WASP_MAX_PARAMS      256
#define WASP_WASM_BASE_OFFSET 1024  /* low memory reserved for host buffers */

static void* wasp_malloc(size_t size) {
    void* p = malloc(size);
    if (!p) fprintf(stderr, "wasp_host: out of memory\n");
    return p;
}

static char* wasp_strdup(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = (char*)wasp_malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

/* ── Minimal JSON helpers ────────────────────────────────────────────────── */
/*
 * We avoid a full JSON library dependency by implementing just enough
 * parsing for the manifest format — string values, number values,
 * and simple arrays of strings.
 */

/* Returns a pointer just past the opening quote of the value for key,
 * or NULL if not found. The value itself may be a string, number, or bool. */
static const char* json_find_key(const char* json, const char* key) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return NULL;
    pos += strlen(search);
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') pos++;
    if (*pos != ':') return NULL;
    pos++;
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r') pos++;
    return pos;
}

/* Extracts a string value for a key into buf. Returns true on success. */
static bool json_get_string(const char* json, const char* key,
                             char* buf, size_t buf_size) {
    const char* pos = json_find_key(json, key);
    if (!pos || *pos != '"') return false;
    pos++; /* skip opening quote */
    size_t i = 0;
    while (*pos && *pos != '"' && i < buf_size - 1) {
        if (*pos == '\\') pos++; /* skip escape char */
        buf[i++] = *pos++;
    }
    buf[i] = '\0';
    return true;
}

/* Extracts a float value for a key. Returns true on success. */
static bool json_get_float(const char* json, const char* key, float* out) {
    const char* pos = json_find_key(json, key);
    if (!pos) return false;
    char* end;
    *out = (float)strtod(pos, &end);
    return end != pos;
}

/* Extracts an integer value for a key. Returns true on success. */
static bool json_get_int(const char* json, const char* key, int* out) {
    const char* pos = json_find_key(json, key);
    if (!pos) return false;
    char* end;
    *out = (int)strtol(pos, &end, 10);
    return end != pos;
}

/* Extracts a bool value for a key. Returns true on success. */
static bool json_get_bool(const char* json, const char* key, bool* out) {
    const char* pos = json_find_key(json, key);
    if (!pos) return false;
    if (strncmp(pos, "true", 4) == 0)  { *out = true;  return true; }
    if (strncmp(pos, "false", 5) == 0) { *out = false; return true; }
    /* also accept 0/1 */
    if (*pos == '1') { *out = true;  return true; }
    if (*pos == '0') { *out = false; return true; }
    return false;
}

/*
 * Iterates over strings in a JSON array value.
 * pos should point to the '['.
 * Calls callback(value, user_data) for each string element.
 * Returns the number of elements found.
 */
typedef void (*JsonStringCallback)(const char* value, void* user_data);

static int json_iter_string_array(const char* pos, JsonStringCallback cb,
                                   void* user_data) {
    if (!pos || *pos != '[') return 0;
    pos++;
    int count = 0;
    char buf[512];
    while (*pos) {
        while (*pos == ' ' || *pos == '\t' || *pos == '\n' ||
               *pos == '\r' || *pos == ',') pos++;
        if (*pos == ']') break;
        if (*pos == '"') {
            pos++;
            size_t i = 0;
            while (*pos && *pos != '"' && i < sizeof(buf) - 1) {
                if (*pos == '\\') pos++;
                buf[i++] = *pos++;
            }
            buf[i] = '\0';
            if (*pos == '"') pos++;
            if (cb) cb(buf, user_data);
            count++;
        } else {
            pos++;
        }
    }
    return count;
}

/* ── Bundle helpers ──────────────────────────────────────────────────────── */

/* Reads a file from a .wasp bundle into a heap buffer. Caller must free. */
static uint8_t* bundle_read_file(const char* bundle_path, const char* filename,
                                  size_t* out_size) {
    int err = 0;
    zip_t* zip = zip_open(bundle_path, ZIP_RDONLY, &err);
    if (!zip) {
        fprintf(stderr, "wasp_host: failed to open bundle '%s' (error %d)\n",
                bundle_path, err);
        return NULL;
    }

    zip_stat_t stat = {0};
    if (zip_stat(zip, filename, 0, &stat) != 0) {
        fprintf(stderr, "wasp_host: '%s' not found in bundle '%s'\n",
                filename, bundle_path);
        zip_close(zip);
        return NULL;
    }

    zip_file_t* file = zip_fopen(zip, filename, 0);
    if (!file) {
        fprintf(stderr, "wasp_host: failed to open '%s' in bundle\n", filename);
        zip_close(zip);
        return NULL;
    }

    uint8_t* buf = (uint8_t*)wasp_malloc(stat.size);
    if (!buf) { zip_fclose(file); zip_close(zip); return NULL; }

    zip_fread(file, buf, stat.size);
    zip_fclose(file);
    zip_close(zip);

    if (out_size) *out_size = (size_t)stat.size;
    return buf;
}

/* ── Error strings ───────────────────────────────────────────────────────── */

const char* wasp_error_string(WaspError error) {
    switch (error) {
        case WASP_OK:                     return "OK";
        case WASP_ERROR_BUNDLE_NOT_FOUND:  return "Bundle not found";
        case WASP_ERROR_INVALID_BUNDLE:    return "Invalid bundle";
        case WASP_ERROR_WASM_NOT_FOUND:    return "dsp.wasm not found in bundle";
        case WASP_ERROR_COMPILE_FAILED:    return "Failed to compile WASM module";
        case WASP_ERROR_INSTANTIATE_FAILED:return "Failed to instantiate WASM module";
        case WASP_ERROR_MISSING_EXPORT:    return "Required export missing from WASM module";
        case WASP_ERROR_PROCESS_FAILED:    return "wasp_process failed";
        case WASP_ERROR_OUT_OF_MEMORY:     return "Out of memory";
        case WASP_ERROR_INVALID_ARGUMENT:  return "Invalid argument";
        default:                           return "Unknown error";
    }
}

/* ── Engine ──────────────────────────────────────────────────────────────── */

struct WaspEngine {
    wasm_engine_t*   wasm_engine;
    wasmtime_store_t* store;
};

WaspEngine* wasp_engine_create() {
    WaspEngine* engine = (WaspEngine*)wasp_malloc(sizeof(WaspEngine));
    if (!engine) return NULL;

    engine->wasm_engine = wasm_engine_new();
    if (!engine->wasm_engine) {
        free(engine);
        return NULL;
    }

    engine->store = wasmtime_store_new(engine->wasm_engine, NULL, NULL);
    if (!engine->store) {
        wasm_engine_delete(engine->wasm_engine);
        free(engine);
        return NULL;
    }

    return engine;
}

void wasp_engine_destroy(WaspEngine* engine) {
    if (!engine) return;
    if (engine->store)       wasmtime_store_delete(engine->store);
    if (engine->wasm_engine) wasm_engine_delete(engine->wasm_engine);
    free(engine);
}

/* ── Manifest ────────────────────────────────────────────────────────────── */

struct WaspManifest {
    char* name;
    char* unique_id;
    char* version;
    char* author;
    char* type;
    char* category;
    char* ui_path;
    char* description;
    char* icon;

    uint32_t input_count;
    uint32_t output_count;

    char*    extensions[WASP_MAX_EXTENSIONS];
    uint32_t extension_count;

    char*    permissions[WASP_MAX_PERMISSIONS];
    uint32_t permission_count;

    WaspParamDescriptor params[WASP_MAX_PARAMS];
    uint32_t            param_count;
};

/* Callback context for collecting string arrays */
typedef struct {
    char**   dest;
    uint32_t* count;
    uint32_t  max;
} StringArrayCtx;

static void collect_string(const char* value, void* user_data) {
    StringArrayCtx* ctx = (StringArrayCtx*)user_data;
    if (*ctx->count < ctx->max) {
        ctx->dest[(*ctx->count)++] = wasp_strdup(value);
    }
}

/* Parse a single parameter object from JSON.
 * pos should point just past the opening '{'. */
static bool parse_param(const char* param_json, WaspParamDescriptor* p) {
    memset(p, 0, sizeof(*p));
    p->visible = true; /* default */

    char buf[256];
    int  ival;
    float fval;
    bool bval;

    if (json_get_int(param_json, "id", &ival))   p->id = (uint32_t)ival;
    if (json_get_string(param_json, "name", buf, sizeof(buf)))
        p->name = wasp_strdup(buf);
    if (json_get_string(param_json, "description", buf, sizeof(buf)))
        p->description = wasp_strdup(buf);
    if (json_get_bool(param_json, "visible", &bval)) p->visible = bval;
    if (json_get_float(param_json, "min",     &fval)) p->min           = fval;
    if (json_get_float(param_json, "max",     &fval)) p->max           = fval;
    if (json_get_float(param_json, "default", &fval)) p->default_value = fval;

    if (json_get_string(param_json, "type", buf, sizeof(buf))) {
        if (strcmp(buf, "float") == 0) p->type = WASP_PARAM_FLOAT;
        else if (strcmp(buf, "enum") == 0)  p->type = WASP_PARAM_ENUM;
        else if (strcmp(buf, "bool") == 0)  p->type = WASP_PARAM_BOOL;
    }

    /* parse enum values array if present */
    if (p->type == WASP_PARAM_ENUM) {
        const char* arr = json_find_key(param_json, "values");
        if (arr) {
            /* allocate a temporary enum values array */
            char**   enum_vals = (char**)wasp_malloc(64 * sizeof(char*));
            uint32_t enum_count = 0;
            if (enum_vals) {
                StringArrayCtx ctx = { enum_vals, &enum_count, 64 };
                json_iter_string_array(arr, collect_string, &ctx);
                p->enum_values = enum_vals;
                p->enum_count  = enum_count;
            }
        }
    }

    return p->name != NULL;
}

WaspManifest* wasp_manifest_load(const char* bundle_path) {
    size_t   json_size = 0;
    uint8_t* json_bytes = bundle_read_file(bundle_path, "manifest.json", &json_size);
    if (!json_bytes) return NULL;

    /* null-terminate */
    char* json = (char*)wasp_malloc(json_size + 1);
    if (!json) { free(json_bytes); return NULL; }
    memcpy(json, json_bytes, json_size);
    json[json_size] = '\0';
    free(json_bytes);

    WaspManifest* m = (WaspManifest*)wasp_malloc(sizeof(WaspManifest));
    if (!m) { free(json); return NULL; }
    memset(m, 0, sizeof(*m));

    char buf[512];
    int  ival;

    if (json_get_string(json, "name",        buf, sizeof(buf))) m->name        = wasp_strdup(buf);
    if (json_get_string(json, "uniqueId",    buf, sizeof(buf))) m->unique_id   = wasp_strdup(buf);
    if (json_get_string(json, "version",     buf, sizeof(buf))) m->version     = wasp_strdup(buf);
    if (json_get_string(json, "author",      buf, sizeof(buf))) m->author      = wasp_strdup(buf);
    if (json_get_string(json, "type",        buf, sizeof(buf))) m->type        = wasp_strdup(buf);
    if (json_get_string(json, "category",    buf, sizeof(buf))) m->category    = wasp_strdup(buf);
    if (json_get_string(json, "ui",          buf, sizeof(buf))) m->ui_path     = wasp_strdup(buf);
    if (json_get_string(json, "description", buf, sizeof(buf))) m->description = wasp_strdup(buf);
    if (json_get_string(json, "icon",        buf, sizeof(buf))) m->icon        = wasp_strdup(buf);

    if (json_get_int(json, "inputs",  &ival)) m->input_count  = (uint32_t)ival;
    if (json_get_int(json, "outputs", &ival)) m->output_count = (uint32_t)ival;

    /* extensions array */
    const char* ext_arr = json_find_key(json, "extensions");
    if (ext_arr) {
        StringArrayCtx ctx = { m->extensions, &m->extension_count, WASP_MAX_EXTENSIONS };
        json_iter_string_array(ext_arr, collect_string, &ctx);
    }

    /* permissions array */
    const char* perm_arr = json_find_key(json, "permissions");
    if (perm_arr) {
        StringArrayCtx ctx = { m->permissions, &m->permission_count, WASP_MAX_PERMISSIONS };
        json_iter_string_array(perm_arr, collect_string, &ctx);
    }

    /* parameters array — find each '{' object and parse it */
    const char* params_arr = json_find_key(json, "parameters");
    if (params_arr && *params_arr == '[') {
        const char* p = params_arr + 1;
        while (*p && m->param_count < WASP_MAX_PARAMS) {
            while (*p == ' ' || *p == '\t' || *p == '\n' ||
                   *p == '\r' || *p == ',') p++;
            if (*p == ']') break;
            if (*p == '{') {
                if (parse_param(p, &m->params[m->param_count]))
                    m->param_count++;
                /* advance past this object */
                int depth = 0;
                while (*p) {
                    if (*p == '{') depth++;
                    else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
                    p++;
                }
            } else {
                p++;
            }
        }
    }

    free(json);
    return m;
}

void wasp_manifest_destroy(WaspManifest* m) {
    if (!m) return;
    free(m->name); free(m->unique_id); free(m->version); free(m->author);
    free(m->type); free(m->category);  free(m->ui_path); free(m->description);
    free(m->icon);
    for (uint32_t i = 0; i < m->extension_count;  i++) free(m->extensions[i]);
    for (uint32_t i = 0; i < m->permission_count; i++) free(m->permissions[i]);
    for (uint32_t i = 0; i < m->param_count; i++) {
        free(m->params[i].name);
        free(m->params[i].description);
        if (m->params[i].enum_values) {
            for (uint32_t j = 0; j < m->params[i].enum_count; j++)
                free(m->params[i].enum_values[j]);
            free(m->params[i].enum_values);
        }
    }
    free(m);
}

bool wasp_manifest_has_extension(const WaspManifest* m, const char* ext) {
    if (!m || !ext) return false;
    for (uint32_t i = 0; i < m->extension_count; i++)
        if (strcmp(m->extensions[i], ext) == 0) return true;
    return false;
}

bool wasp_manifest_has_permission(const WaspManifest* m, const char* perm) {
    if (!m || !perm) return false;
    for (uint32_t i = 0; i < m->permission_count; i++)
        if (strcmp(m->permissions[i], perm) == 0) return true;
    return false;
}

const char* wasp_manifest_name(const WaspManifest* m)        { return m ? m->name        : NULL; }
const char* wasp_manifest_unique_id(const WaspManifest* m)   { return m ? m->unique_id   : NULL; }
const char* wasp_manifest_version(const WaspManifest* m)     { return m ? m->version     : NULL; }
const char* wasp_manifest_author(const WaspManifest* m)      { return m ? m->author      : NULL; }
const char* wasp_manifest_type(const WaspManifest* m)        { return m ? m->type        : NULL; }
const char* wasp_manifest_category(const WaspManifest* m)    { return m ? m->category    : NULL; }
const char* wasp_manifest_ui_path(const WaspManifest* m)     { return m ? m->ui_path     : NULL; }
uint32_t    wasp_manifest_input_count(const WaspManifest* m) { return m ? m->input_count  : 0; }
uint32_t    wasp_manifest_output_count(const WaspManifest* m){ return m ? m->output_count : 0; }
uint32_t    wasp_manifest_param_count(const WaspManifest* m) { return m ? m->param_count  : 0; }

const WaspParamDescriptor* wasp_manifest_get_param(const WaspManifest* m, uint32_t index) {
    if (!m || index >= m->param_count) return NULL;
    return &m->params[index];
}

/* ── Wasmtime call helper ─────────────────────────────────────────────────── */

static bool wasp_call_fn(wasmtime_store_t* store, wasmtime_func_t fn,
                          wasmtime_val_t* args, size_t nargs,
                          wasmtime_val_t* results, size_t nresults,
                          const char* label) {
    wasmtime_context_t* ctx = wasmtime_store_context(store);
    wasm_trap_t* trap = NULL;
    wasmtime_error_t* error = wasmtime_func_call(
        ctx, &fn, args, nargs, results, nresults, &trap);
    if (error) {
        wasm_byte_vec_t msg;
        wasmtime_error_message(error, &msg);
        fprintf(stderr, "wasp_host: %s error: %.*s\n",
                label, (int)msg.size, msg.data);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(error);
        return false;
    }
    if (trap) {
        wasm_byte_vec_t msg;
        wasm_trap_message(trap, &msg);
        fprintf(stderr, "wasp_host: %s trap: %.*s\n",
                label, (int)msg.size, msg.data);
        wasm_byte_vec_delete(&msg);
        wasm_trap_delete(trap);
        return false;
    }
    return true;
}

/* ── Instance ────────────────────────────────────────────────────────────── */

struct WaspInstance {
    WaspEngine*         engine;
    WaspManifest*       manifest;
    wasmtime_module_t*  module;
    wasmtime_instance_t instance;
    wasmtime_memory_t   memory;
    uint32_t sample_rate;

    /* required exports */
    wasmtime_func_t fn_initialize;
    wasmtime_func_t fn_process;
    wasmtime_func_t fn_get_process_buffer;
    wasmtime_func_t fn_terminate;

    /* optional exports */
    wasmtime_func_t fn_get_request_buffer;
    wasmtime_func_t fn_save_state;
    wasmtime_func_t fn_load_state;
    wasmtime_func_t fn_get_latency;
    wasmtime_func_t fn_get_tail;

    bool has_get_request_buffer;
    bool has_save_state;
    bool has_load_state;
    bool has_get_latency;
    bool has_get_tail;

    uint32_t  ctx_offset;
    WaspError last_error;
};

static bool get_export_func(wasmtime_store_t* store, wasmtime_instance_t* inst,
                              const char* name, wasmtime_func_t* out) {
    wasmtime_context_t* ctx = wasmtime_store_context(store);
    wasmtime_extern_t   ext;
    if (!wasmtime_instance_export_get(ctx, inst, name, strlen(name), &ext))
        return false;
    if (ext.kind != WASMTIME_EXTERN_FUNC) return false;
    *out = ext.of.func;
    return true;
}

static bool get_export_memory(wasmtime_store_t* store, wasmtime_instance_t* inst,
                               const char* name, wasmtime_memory_t* out) {
    wasmtime_context_t* ctx = wasmtime_store_context(store);
    wasmtime_extern_t   ext;
    if (!wasmtime_instance_export_get(ctx, inst, name, strlen(name), &ext))
        return false;
    if (ext.kind != WASMTIME_EXTERN_MEMORY) return false;
    *out = ext.of.memory;
    return true;
}

WaspInstance* wasp_instance_create(WaspEngine* engine, const char* bundle_path) {
    if (!engine || !bundle_path) return NULL;

    WaspInstance* inst = (WaspInstance*)wasp_malloc(sizeof(WaspInstance));
    if (!inst) return NULL;
    memset(inst, 0, sizeof(*inst));
    inst->engine = engine;

    /* load manifest */
    inst->manifest = wasp_manifest_load(bundle_path);
    if (!inst->manifest) {
        fprintf(stderr, "wasp_host: failed to load manifest from '%s'\n", bundle_path);
        inst->last_error = WASP_ERROR_INVALID_BUNDLE;
        free(inst);
        return NULL;
    }

    /* load dsp.wasm from bundle */
    size_t   wasm_size = 0;
    uint8_t* wasm_bytes = bundle_read_file(bundle_path, "dsp.wasm", &wasm_size);
    if (!wasm_bytes) {
        inst->last_error = WASP_ERROR_WASM_NOT_FOUND;
        wasp_manifest_destroy(inst->manifest);
        free(inst);
        return NULL;
    }

    /* compile */
    wasmtime_error_t* error = wasmtime_module_new(
        engine->wasm_engine, wasm_bytes, wasm_size, &inst->module);
    free(wasm_bytes);
    if (error) {
        wasm_byte_vec_t msg;
        wasmtime_error_message(error, &msg);
        fprintf(stderr, "wasp_host: compile failed: %.*s\n",
                (int)msg.size, msg.data);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(error);
        inst->last_error = WASP_ERROR_COMPILE_FAILED;
        wasp_manifest_destroy(inst->manifest);
        free(inst);
        return NULL;
    }

    /* instantiate */
    wasmtime_context_t* ctx = wasmtime_store_context(engine->store);
    wasm_trap_t* trap = NULL;
    error = wasmtime_instance_new(
        ctx, inst->module, NULL, 0, &inst->instance, &trap);
    if (error || trap) {
        if (error) {
            wasm_byte_vec_t msg;
            wasmtime_error_message(error, &msg);
            fprintf(stderr, "wasp_host: instantiate failed: %.*s\n",
                    (int)msg.size, msg.data);
            wasm_byte_vec_delete(&msg);
            wasmtime_error_delete(error);
        }
        if (trap) wasm_trap_delete(trap);
        inst->last_error = WASP_ERROR_INSTANTIATE_FAILED;
        wasmtime_module_delete(inst->module);
        wasp_manifest_destroy(inst->manifest);
        free(inst);
        return NULL;
    }

    /* required exports */
    bool ok = true;
    ok &= get_export_func(engine->store, &inst->instance,
                          "wasp_initialize",         &inst->fn_initialize);
    ok &= get_export_func(engine->store, &inst->instance,
                          "wasp_process",            &inst->fn_process);
    ok &= get_export_func(engine->store, &inst->instance,
                          "wasp_get_process_buffer", &inst->fn_get_process_buffer);
    ok &= get_export_func(engine->store, &inst->instance,
                          "wasp_terminate",          &inst->fn_terminate);
    ok &= get_export_memory(engine->store, &inst->instance,
                            "memory",                &inst->memory);
    if (!ok) {
        fprintf(stderr, "wasp_host: missing required exports\n");
        inst->last_error = WASP_ERROR_MISSING_EXPORT;
        wasmtime_module_delete(inst->module);
        wasp_manifest_destroy(inst->manifest);
        free(inst);
        return NULL;
    }

    /* optional exports — gated by declared extensions */
    if (wasp_manifest_has_extension(inst->manifest, WASP_EXT_REQUESTS)) {
        inst->has_get_request_buffer = get_export_func(
            engine->store, &inst->instance,
            "wasp_get_request_buffer", &inst->fn_get_request_buffer);
    }
    if (wasp_manifest_has_extension(inst->manifest, WASP_EXT_STATE)) {
        inst->has_save_state = get_export_func(
            engine->store, &inst->instance,
            "wasp_save_state", &inst->fn_save_state);
        inst->has_load_state = get_export_func(
            engine->store, &inst->instance,
            "wasp_load_state", &inst->fn_load_state);
    }
    if (wasp_manifest_has_extension(inst->manifest, WASP_EXT_LATENCY)) {
        inst->has_get_latency = get_export_func(
            engine->store, &inst->instance,
            "wasp_get_latency", &inst->fn_get_latency);
    }
    if (wasp_manifest_has_extension(inst->manifest, WASP_EXT_TAIL)) {
        inst->has_get_tail = get_export_func(
            engine->store, &inst->instance,
            "wasp_get_tail", &inst->fn_get_tail);
    }

    return inst;
}

void wasp_instance_destroy(WaspInstance* inst) {
    if (!inst) return;
    if (inst->module)   wasmtime_module_delete(inst->module);
    if (inst->manifest) wasp_manifest_destroy(inst->manifest);
    free(inst);
}

WaspError wasp_instance_last_error(const WaspInstance* inst) {
    return inst ? inst->last_error : WASP_ERROR_INVALID_ARGUMENT;
}

const WaspManifest* wasp_instance_manifest(const WaspInstance* inst) {
    return inst ? inst->manifest : NULL;
}

WaspError wasp_instance_initialize(WaspInstance* inst,
                                    uint32_t sample_rate,
                                    uint32_t max_block_size) {
    if (!inst) return WASP_ERROR_INVALID_ARGUMENT;

    inst->sample_rate = sample_rate;

    wasmtime_val_t args[2];
    args[0].kind = WASMTIME_I32; args[0].of.i32 = (int32_t)sample_rate;
    args[1].kind = WASMTIME_I32; args[1].of.i32 = (int32_t)max_block_size;

    if (!wasp_call_fn(inst->engine->store, inst->fn_initialize,
                      args, 2, NULL, 0, "wasp_initialize")) {
        inst->last_error = WASP_ERROR_PROCESS_FAILED;
        return WASP_ERROR_PROCESS_FAILED;
    }

    /* get the process context buffer offset */
    wasmtime_val_t result;
    result.kind = WASMTIME_I32;
    if (!wasp_call_fn(inst->engine->store, inst->fn_get_process_buffer,
                      NULL, 0, &result, 1, "wasp_get_process_buffer")) {
        inst->last_error = WASP_ERROR_PROCESS_FAILED;
        return WASP_ERROR_PROCESS_FAILED;
    }
    inst->ctx_offset = (uint32_t)result.of.i32;

    inst->last_error = WASP_OK;
    return WASP_OK;
}

void wasp_instance_terminate(WaspInstance* inst) {
    if (!inst) return;
    wasp_call_fn(inst->engine->store, inst->fn_terminate,
                 NULL, 0, NULL, 0, "wasp_terminate");
}

WaspError wasp_instance_process(WaspInstance* inst, WaspProcessBuffer* buffer);
/* implemented after WaspProcessBuffer */

uint32_t wasp_instance_get_latency(WaspInstance* inst) {
    if (!inst || !inst->has_get_latency) return 0;
    wasmtime_val_t result; result.kind = WASMTIME_I32;
    if (!wasp_call_fn(inst->engine->store, inst->fn_get_latency,
                      NULL, 0, &result, 1, "wasp_get_latency")) return 0;
    return (uint32_t)result.of.i32;
}

uint32_t wasp_instance_get_tail(WaspInstance* inst) {
    if (!inst || !inst->has_get_tail) return 0;
    wasmtime_val_t result; result.kind = WASMTIME_I32;
    if (!wasp_call_fn(inst->engine->store, inst->fn_get_tail,
                      NULL, 0, &result, 1, "wasp_get_tail")) return 0;
    return (uint32_t)result.of.i32;
}

uint8_t* wasp_instance_save_state(WaspInstance* inst, uint32_t* size_out) {
    if (!inst || !inst->has_save_state || !size_out) return NULL;

    /* pass a pointer to a uint32 in WASM memory for size_out */
    /* we use a small scratch area just before the base offset */
    uint32_t size_offset = WASP_WASM_BASE_OFFSET - 8;

    wasmtime_val_t args[1], result;
    args[0].kind   = WASMTIME_I32;
    args[0].of.i32 = (int32_t)size_offset;
    result.kind    = WASMTIME_I32;

    if (!wasp_call_fn(inst->engine->store, inst->fn_save_state,
                      args, 1, &result, 1, "wasp_save_state")) return NULL;

    uint32_t data_offset = (uint32_t)result.of.i32;

    wasmtime_context_t* ctx = wasmtime_store_context(inst->engine->store);
    uint8_t* wasm_mem = wasmtime_memory_data(ctx, &inst->memory);

    uint32_t data_size;
    memcpy(&data_size, wasm_mem + size_offset, sizeof(uint32_t));

    if (data_size == 0) return NULL;

    uint8_t* copy = (uint8_t*)wasp_malloc(data_size);
    if (!copy) return NULL;
    memcpy(copy, wasm_mem + data_offset, data_size);
    *size_out = data_size;
    return copy;
}

WaspError wasp_instance_load_state(WaspInstance* inst,
                                    const uint8_t* data, uint32_t size) {
    if (!inst || !inst->has_load_state || !data || size == 0)
        return WASP_ERROR_INVALID_ARGUMENT;

    /* write data into WASM memory at the base offset area */
    uint32_t data_offset = WASP_WASM_BASE_OFFSET - 8 - size;

    wasmtime_context_t* ctx = wasmtime_store_context(inst->engine->store);
    uint8_t* wasm_mem = wasmtime_memory_data(ctx, &inst->memory);
    memcpy(wasm_mem + data_offset, data, size);

    wasmtime_val_t args[2];
    args[0].kind = WASMTIME_I32; args[0].of.i32 = (int32_t)data_offset;
    args[1].kind = WASMTIME_I32; args[1].of.i32 = (int32_t)size;

    if (!wasp_call_fn(inst->engine->store, inst->fn_load_state,
                      args, 2, NULL, 0, "wasp_load_state"))
        return WASP_ERROR_PROCESS_FAILED;

    return WASP_OK;
}

/* ── Process Buffer ──────────────────────────────────────────────────────── */

struct WaspProcessBuffer {
    WaspInstance* instance;

    uint32_t max_block_size;
    uint32_t max_events;
    uint32_t num_channels;
    uint32_t sample_rate;

    /* WASM memory offsets */
    uint32_t ch_offsets[16];    /* per-channel buffer offsets in WASM memory */
    uint32_t ch_table_offset;   /* offset of the channel offset table */
    uint32_t events_offset;     /* offset of the events array */

    /* host-side staging for events before commit */
    WaspEvent* pending_events;
    uint32_t   pending_count;

    /* current block state */
    uint32_t     current_frames;
    WaspTransport current_transport;
};

/* Simple insertion sort — event counts are always small */
static void sort_events(WaspEvent* events, uint32_t count) {
    for (uint32_t i = 1; i < count; i++) {
        WaspEvent key = events[i];
        int j = (int)i - 1;
        while (j >= 0 && events[j].sample_offset > key.sample_offset) {
            events[j + 1] = events[j];
            j--;
        }
        events[j + 1] = key;
    }
}

WaspProcessBuffer* wasp_process_buffer_create(WaspInstance* instance,
                                               uint32_t max_block_size,
                                               uint32_t max_events,
                                               uint32_t num_channels) {
    if (!instance || num_channels == 0 || num_channels > 16) return NULL;

    WaspProcessBuffer* buf = (WaspProcessBuffer*)wasp_malloc(sizeof(WaspProcessBuffer));
    if (!buf) return NULL;
    memset(buf, 0, sizeof(*buf));

    buf->instance       = instance;
    buf->max_block_size = max_block_size;
    buf->max_events     = max_events;
    buf->num_channels   = num_channels;
    buf->sample_rate    = instance->sample_rate;

    buf->pending_events = (WaspEvent*)wasp_malloc(max_events * sizeof(WaspEvent));
    if (!buf->pending_events) { free(buf); return NULL; }

    /* lay out WASM memory starting at WASP_WASM_BASE_OFFSET:
     *   [ch0 floats] [ch1 floats] ... [channel offset table] [events] */
    uint32_t offset = WASP_WASM_BASE_OFFSET;

    for (uint32_t i = 0; i < num_channels; i++) {
        buf->ch_offsets[i] = offset;
        offset += max_block_size * sizeof(float);
    }

    buf->ch_table_offset = offset;
    offset += num_channels * sizeof(uint32_t);

    buf->events_offset = offset;

    /* write the channel offset table into WASM memory once */
    wasmtime_context_t* ctx = wasmtime_store_context(instance->engine->store);
    uint8_t* wasm_mem = wasmtime_memory_data(ctx, &instance->memory);
    uint32_t* ch_table = (uint32_t*)(wasm_mem + buf->ch_table_offset);
    for (uint32_t i = 0; i < num_channels; i++)
        ch_table[i] = buf->ch_offsets[i];

    return buf;
}

void wasp_process_buffer_destroy(WaspProcessBuffer* buf) {
    if (!buf) return;
    free(buf->pending_events);
    free(buf);
}

void wasp_process_buffer_begin(WaspProcessBuffer* buf,
                                uint32_t frames,
                                WaspTransport transport) {
    if (!buf) return;
    buf->current_frames    = frames;
    buf->current_transport = transport;
    buf->pending_count     = 0;
}

bool wasp_process_buffer_add_event(WaspProcessBuffer* buf, WaspEvent event) {
    if (!buf || buf->pending_count >= buf->max_events) return false;
    buf->pending_events[buf->pending_count++] = event;
    return true;
}

void wasp_process_buffer_commit(WaspProcessBuffer* buf) {
    if (!buf) return;

    WaspInstance* inst = buf->instance;

    /* sort events by sample offset */
    sort_events(buf->pending_events, buf->pending_count);

    /* write events into WASM memory */
    wasmtime_context_t* ctx = wasmtime_store_context(inst->engine->store);
    uint8_t* wasm_mem = wasmtime_memory_data(ctx, &inst->memory);

    WaspEvent* wasm_events = (WaspEvent*)(wasm_mem + buf->events_offset);
    memcpy(wasm_events, buf->pending_events,
           buf->pending_count * sizeof(WaspEvent));

    /* write the WaspProcessContext */
    WaspProcessContext* pctx = (WaspProcessContext*)(wasm_mem + inst->ctx_offset);
    pctx->inputs_offset     = 0;
    pctx->outputs_offset    = buf->ch_table_offset;
    pctx->input_count       = 0;
    pctx->output_count      = buf->num_channels;
    pctx->frames            = buf->current_frames;
    pctx->sample_rate       = buf->sample_rate;
    pctx->events_offset     = buf->events_offset;
    pctx->event_count       = buf->pending_count;
    pctx->transport         = buf->current_transport;
}

float* wasp_process_buffer_get_channel(WaspProcessBuffer* buf, uint32_t channel) {
    if (!buf || channel >= buf->num_channels) return NULL;
    wasmtime_context_t* ctx = wasmtime_store_context(buf->instance->engine->store);
    uint8_t* wasm_mem = wasmtime_memory_data(ctx, &buf->instance->memory);
    return (float*)(wasm_mem + buf->ch_offsets[channel]);
}

uint32_t wasp_process_buffer_channel_count(const WaspProcessBuffer* buf) {
    return buf ? buf->num_channels : 0;
}

/* ── Instance process (defined here so it can use WaspProcessBuffer) ─────── */

WaspError wasp_instance_process(WaspInstance* inst, WaspProcessBuffer* buffer) {
    if (!inst || !buffer) return WASP_ERROR_INVALID_ARGUMENT;

    wasmtime_val_t args[1];
    args[0].kind   = WASMTIME_I32;
    args[0].of.i32 = (int32_t)inst->ctx_offset;

    if (!wasp_call_fn(inst->engine->store, inst->fn_process,
                      args, 1, NULL, 0, "wasp_process")) {
        inst->last_error = WASP_ERROR_PROCESS_FAILED;
        return WASP_ERROR_PROCESS_FAILED;
    }

    inst->last_error = WASP_OK;
    return WASP_OK;
}
