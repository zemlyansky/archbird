#include "sha256.h"
#include <archbird/archbird.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define AB_WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define AB_WASM_EXPORT
#endif

#ifndef ARCHBIRD_VERSION
#define ARCHBIRD_VERSION "0.0.1"
#endif

typedef struct WasmOutput {
  uint8_t *data;
  size_t length;
  size_t capacity;
} WasmOutput;

typedef struct WasmProject {
  uint32_t magic;
  ArchbirdEngine *engine;
  ArchbirdProject *project;
} WasmProject;

typedef struct WasmDiscovery {
  uint32_t magic;
  ArchbirdEngine *engine;
  ArchbirdDiscovery *discovery;
} WasmDiscovery;

enum {
  WASM_PROJECT_MAGIC = UINT32_C(0x4150524a),
  WASM_DISCOVERY_MAGIC = UINT32_C(0x41444953)
};

static WasmOutput wasm_output;
static char *wasm_error;
static size_t wasm_error_length;
static size_t wasm_error_offset = ARCHBIRD_NO_OFFSET;
static ArchbirdStatus wasm_status = ARCHBIRD_OK;

static void result_reset(void) {
  free(wasm_output.data);
  wasm_output.data = NULL;
  wasm_output.length = 0;
  wasm_output.capacity = 0;
  free(wasm_error);
  wasm_error = NULL;
  wasm_error_length = 0;
  wasm_error_offset = ARCHBIRD_NO_OFFSET;
  wasm_status = ARCHBIRD_OK;
}

static int output_write(void *user_data, const uint8_t *bytes, size_t length) {
  WasmOutput *output = (WasmOutput *)user_data;
  uint8_t *resized;
  size_t needed;
  size_t capacity;
  if (length > SIZE_MAX - output->length)
    return 1;
  needed = output->length + length;
  if (needed > output->capacity) {
    capacity = output->capacity ? output->capacity : 256;
    while (capacity < needed) {
      if (capacity > SIZE_MAX / 2) {
        capacity = needed;
        break;
      }
      capacity *= 2;
    }
    resized = (uint8_t *)realloc(output->data, capacity);
    if (!resized)
      return 1;
    output->data = resized;
    output->capacity = capacity;
  }
  if (length)
    memcpy(output->data + output->length, bytes, length);
  output->length = needed;
  return 0;
}

static ArchbirdStatus result_finish(ArchbirdEngine *engine,
                                    ArchbirdStatus status) {
  const char *message;
  size_t message_length;
  char suffix[96];
  int suffix_length;
  wasm_status = status;
  if (status == ARCHBIRD_OK)
    return status;
  free(wasm_output.data);
  wasm_output.data = NULL;
  wasm_output.length = 0;
  wasm_output.capacity = 0;
  message = engine ? archbird_engine_error(engine) : NULL;
  wasm_error_offset =
      engine ? archbird_engine_error_offset(engine) : ARCHBIRD_NO_OFFSET;
  if (!message || !message[0])
    message = "native Archbird operation failed";
  message_length = strlen(message);
  if (wasm_error_offset == ARCHBIRD_NO_OFFSET)
    suffix_length =
        snprintf(suffix, sizeof(suffix), " (status=%d)", (int)status);
  else
    suffix_length = snprintf(suffix, sizeof(suffix), " (status=%d, byte=%zu)",
                             (int)status, wasm_error_offset);
  if (suffix_length < 0 || (size_t)suffix_length >= sizeof(suffix))
    suffix_length = 0;
  wasm_error =
      (char *)malloc(message_length + (size_t)suffix_length + (size_t)1);
  if (!wasm_error) {
    wasm_error_length = 0;
    return status;
  }
  memcpy(wasm_error, message, message_length);
  if (suffix_length)
    memcpy(wasm_error + message_length, suffix, (size_t)suffix_length);
  wasm_error_length = message_length + (size_t)suffix_length;
  wasm_error[wasm_error_length] = '\0';
  return status;
}

static ArchbirdStatus invalid_argument(const char *message) {
  result_reset();
  wasm_error_length = strlen(message);
  wasm_error = (char *)malloc(wasm_error_length + 1);
  if (wasm_error) {
    memcpy(wasm_error, message, wasm_error_length + 1);
  } else {
    wasm_error_length = 0;
  }
  wasm_status = ARCHBIRD_INVALID_ARGUMENT;
  return wasm_status;
}

static WasmProject *checked_project(uintptr_t handle) {
  WasmProject *owned = (WasmProject *)handle;
  if (!owned || owned->magic != WASM_PROJECT_MAGIC || !owned->engine ||
      !owned->project)
    return NULL;
  return owned;
}

static WasmDiscovery *checked_discovery(uintptr_t handle) {
  WasmDiscovery *owned = (WasmDiscovery *)handle;
  if (!owned || owned->magic != WASM_DISCOVERY_MAGIC || !owned->engine ||
      !owned->discovery)
    return NULL;
  return owned;
}

static ArchbirdProviderMode provider_mode(uint32_t mode, int *valid) {
  if (mode <= (uint32_t)ARCHBIRD_PROVIDER_AUDIT) {
    *valid = 1;
    return (ArchbirdProviderMode)mode;
  }
  *valid = 0;
  return ARCHBIRD_PROVIDER_PRIMARY;
}

static ArchbirdStatus
stateless_begin_input_profile(size_t input_length, ArchbirdInputProfile profile,
                              ArchbirdEngine **out_engine) {
  ArchbirdEngineOptions options;
  ArchbirdStatus status;
  result_reset();
  status =
      archbird_engine_options_init_for_input(&options, profile, input_length);
  if (status != ARCHBIRD_OK)
    return status;
  return archbird_engine_create(&options, out_engine);
}

static ArchbirdStatus stateless_begin_input(size_t input_length,
                                            ArchbirdEngine **out_engine) {
  return stateless_begin_input_profile(input_length, ARCHBIRD_INPUT_DEFAULT,
                                       out_engine);
}

static ArchbirdStatus
stateless_begin_saved_artifact(size_t input_length,
                               ArchbirdEngine **out_engine) {
  return stateless_begin_input_profile(
      input_length, ARCHBIRD_INPUT_SAVED_ARTIFACT, out_engine);
}

static size_t larger_input(size_t left, size_t right) {
  return left > right ? left : right;
}

static int stateless_end(ArchbirdEngine *engine, ArchbirdStatus status) {
  int result = (int)result_finish(engine, status);
  archbird_engine_destroy(engine);
  return result;
}

AB_WASM_EXPORT const uint8_t *ab_wasm_output_data(void) {
  return wasm_output.data;
}

AB_WASM_EXPORT size_t ab_wasm_output_length(void) { return wasm_output.length; }

AB_WASM_EXPORT const char *ab_wasm_error_data(void) { return wasm_error; }

AB_WASM_EXPORT size_t ab_wasm_error_length(void) { return wasm_error_length; }

AB_WASM_EXPORT size_t ab_wasm_error_offset(void) { return wasm_error_offset; }

AB_WASM_EXPORT int ab_wasm_last_status(void) { return (int)wasm_status; }

AB_WASM_EXPORT uint32_t ab_wasm_native_abi_version(void) {
  return ARCHBIRD_NATIVE_ABI_VERSION;
}

AB_WASM_EXPORT uint32_t ab_wasm_pattern_contract_version(void) {
  return ARCHBIRD_PATTERN_CONTRACT_VERSION;
}

AB_WASM_EXPORT const char *ab_wasm_version(void) { return ARCHBIRD_VERSION; }

AB_WASM_EXPORT const char *ab_wasm_implementation_sha256(void) {
  return archbird_implementation_sha256();
}

AB_WASM_EXPORT const char *ab_wasm_pattern_contract(void) {
  return ARCHBIRD_PATTERN_CONTRACT;
}

AB_WASM_EXPORT const char *ab_wasm_pattern_engine(void) {
  return ARCHBIRD_PATTERN_ENGINE;
}

AB_WASM_EXPORT const char *ab_wasm_pattern_unicode(void) {
  return ARCHBIRD_PATTERN_UNICODE;
}

AB_WASM_EXPORT const char *ab_wasm_pattern_options(void) {
  return ARCHBIRD_PATTERN_OPTIONS;
}

AB_WASM_EXPORT int ab_wasm_sha256(const uint8_t *bytes, size_t length) {
  uint8_t digest[32];
  char hex[65];
  ArchbirdStatus status;
  result_reset();
  status = archbird_sha256(bytes, length, digest);
  if (status == ARCHBIRD_OK) {
    archbird_sha256_hex(digest, hex);
    if (output_write(&wasm_output, (const uint8_t *)hex, 64))
      status = ARCHBIRD_OUT_OF_MEMORY;
  }
  return (int)result_finish(NULL, status);
}

AB_WASM_EXPORT uintptr_t ab_wasm_project_create(const uint8_t *manifest,
                                                size_t manifest_length) {
  WasmProject *owned;
  ArchbirdStatus status;
  result_reset();
  owned = (WasmProject *)calloc(1, sizeof(*owned));
  if (!owned) {
    (void)result_finish(NULL, ARCHBIRD_OUT_OF_MEMORY);
    return (uintptr_t)0;
  }
  status = archbird_engine_create(NULL, &owned->engine);
  if (status == ARCHBIRD_OK)
    status = archbird_project_create(owned->engine, manifest, manifest_length,
                                     &owned->project);
  if (status != ARCHBIRD_OK) {
    (void)result_finish(owned->engine, status);
    archbird_project_destroy(owned->project);
    archbird_engine_destroy(owned->engine);
    free(owned);
    return (uintptr_t)0;
  }
  owned->magic = WASM_PROJECT_MAGIC;
  return (uintptr_t)owned;
}

AB_WASM_EXPORT void ab_wasm_project_destroy(uintptr_t handle) {
  WasmProject *owned = checked_project(handle);
  if (!owned)
    return;
  owned->magic = 0;
  archbird_project_destroy(owned->project);
  archbird_engine_destroy(owned->engine);
  free(owned);
}

AB_WASM_EXPORT int ab_wasm_project_add_source(uintptr_t handle,
                                              const char *path,
                                              size_t path_length,
                                              const uint8_t *bytes,
                                              size_t byte_length) {
  WasmProject *owned = checked_project(handle);
  ArchbirdStatus status;
  result_reset();
  if (!owned)
    return (int)invalid_argument("invalid Wasm project handle");
  status = archbird_project_add_source(owned->engine, owned->project, path,
                                       path_length, bytes, byte_length);
  return (int)result_finish(owned->engine, status);
}

AB_WASM_EXPORT int ab_wasm_project_finalize_sources(uintptr_t handle) {
  WasmProject *owned = checked_project(handle);
  ArchbirdStatus status;
  result_reset();
  if (!owned)
    return (int)invalid_argument("invalid Wasm project handle");
  status = archbird_project_finalize_sources(owned->engine, owned->project);
  return (int)result_finish(owned->engine, status);
}

AB_WASM_EXPORT int ab_wasm_project_set_config(uintptr_t handle,
                                              const uint8_t *config,
                                              size_t config_length) {
  WasmProject *owned = checked_project(handle);
  ArchbirdStatus status;
  result_reset();
  if (!owned)
    return (int)invalid_argument("invalid Wasm project handle");
  status = archbird_project_set_config(owned->engine, owned->project, config,
                                       config_length);
  return (int)result_finish(owned->engine, status);
}

AB_WASM_EXPORT int ab_wasm_project_add_provider(uintptr_t handle, uint32_t mode,
                                                const uint8_t *provider,
                                                size_t provider_length) {
  WasmProject *owned = checked_project(handle);
  ArchbirdProviderMode native_mode;
  ArchbirdStatus status;
  int valid;
  result_reset();
  if (!owned)
    return (int)invalid_argument("invalid Wasm project handle");
  native_mode = provider_mode(mode, &valid);
  if (!valid)
    return (int)invalid_argument("invalid provider mode");
  status = archbird_project_add_provider_facts(
      owned->engine, owned->project, native_mode, provider, provider_length);
  return (int)result_finish(owned->engine, status);
}

AB_WASM_EXPORT int ab_wasm_project_add_test_symbol_observations(
    uintptr_t handle, const uint8_t *observations, size_t observations_length) {
  WasmProject *owned = checked_project(handle);
  ArchbirdStatus status;
  result_reset();
  if (!owned)
    return (int)invalid_argument("invalid Wasm project handle");
  status = archbird_project_add_test_symbol_observations(
      owned->engine, owned->project, observations, observations_length);
  return (int)result_finish(owned->engine, status);
}

AB_WASM_EXPORT int ab_wasm_project_scan_builtin(uintptr_t handle,
                                                uint32_t mode) {
  WasmProject *owned = checked_project(handle);
  ArchbirdProviderMode native_mode;
  ArchbirdStatus status;
  int valid;
  result_reset();
  if (!owned)
    return (int)invalid_argument("invalid Wasm project handle");
  native_mode = provider_mode(mode, &valid);
  if (!valid)
    return (int)invalid_argument("invalid provider mode");
  status =
      archbird_project_scan_builtin(owned->engine, owned->project, native_mode);
  return (int)result_finish(owned->engine, status);
}

AB_WASM_EXPORT int
ab_wasm_project_scan_builtin_provider(uintptr_t handle, const char *provider_id,
                                      size_t provider_id_length,
                                      uint32_t mode) {
  WasmProject *owned = checked_project(handle);
  ArchbirdProviderMode native_mode;
  ArchbirdStatus status;
  int valid;
  result_reset();
  if (!owned)
    return (int)invalid_argument("invalid Wasm project handle");
  native_mode = provider_mode(mode, &valid);
  if (!valid)
    return (int)invalid_argument("invalid provider mode");
  status = archbird_project_scan_builtin_provider(
      owned->engine, owned->project, provider_id, provider_id_length,
      native_mode);
  return (int)result_finish(owned->engine, status);
}

AB_WASM_EXPORT int ab_wasm_project_scan_builtin_provider_file(
    uintptr_t handle, const char *provider_id, size_t provider_id_length,
    const char *path, size_t path_length, uint32_t mode) {
  WasmProject *owned = checked_project(handle);
  ArchbirdProviderMode native_mode;
  ArchbirdStatus status;
  int valid;
  result_reset();
  if (!owned)
    return (int)invalid_argument("invalid Wasm project handle");
  native_mode = provider_mode(mode, &valid);
  if (!valid)
    return (int)invalid_argument("invalid provider mode");
  status = archbird_project_scan_builtin_provider_file(
      owned->engine, owned->project, provider_id, provider_id_length, path,
      path_length, native_mode);
  return (int)result_finish(owned->engine, status);
}

AB_WASM_EXPORT int ab_wasm_project_finalize_providers(uintptr_t handle) {
  WasmProject *owned = checked_project(handle);
  ArchbirdStatus status;
  result_reset();
  if (!owned)
    return (int)invalid_argument("invalid Wasm project handle");
  status = archbird_project_finalize_providers(owned->engine, owned->project);
  return (int)result_finish(owned->engine, status);
}

static int project_digest(uintptr_t handle, int configuration) {
  WasmProject *owned = checked_project(handle);
  const char *digest;
  ArchbirdStatus status = ARCHBIRD_OK;
  result_reset();
  if (!owned)
    return (int)invalid_argument("invalid Wasm project handle");
  digest = configuration ? archbird_project_config_sha256(owned->project)
                         : archbird_project_manifest_sha256(owned->project);
  if (!digest)
    status = ARCHBIRD_CONFLICT;
  else if (output_write(&wasm_output, (const uint8_t *)digest, 64))
    status = ARCHBIRD_OUT_OF_MEMORY;
  return (int)result_finish(owned->engine, status);
}

AB_WASM_EXPORT int ab_wasm_project_map_input_sha256(uintptr_t handle) {
  WasmProject *owned = checked_project(handle);
  const char *digest;
  ArchbirdStatus status = ARCHBIRD_OK;
  result_reset();
  if (!owned)
    return (int)invalid_argument("invalid Wasm project handle");
  digest = archbird_project_map_input_sha256(owned->project);
  if (!digest)
    status = ARCHBIRD_CONFLICT;
  else if (output_write(&wasm_output, (const uint8_t *)digest, 64))
    status = ARCHBIRD_OUT_OF_MEMORY;
  return (int)result_finish(owned->engine, status);
}

AB_WASM_EXPORT int ab_wasm_project_manifest_sha256(uintptr_t handle) {
  return project_digest(handle, 0);
}

AB_WASM_EXPORT int ab_wasm_project_config_sha256(uintptr_t handle) {
  return project_digest(handle, 1);
}

AB_WASM_EXPORT int ab_wasm_project_counts(uintptr_t handle) {
  WasmProject *owned = checked_project(handle);
  char encoded[256];
  int length;
  ArchbirdStatus status = ARCHBIRD_OK;
  result_reset();
  if (!owned)
    return (int)invalid_argument("invalid Wasm project handle");
  length =
      snprintf(encoded, sizeof(encoded),
               "{\"facts\":\"%" PRIuMAX "\",\"providers\":\"%" PRIuMAX
               "\",\"sources\":\"%" PRIuMAX "\"}",
               (uintmax_t)archbird_project_provider_fact_count(owned->project),
               (uintmax_t)archbird_project_provider_count(owned->project),
               (uintmax_t)archbird_project_source_count(owned->project));
  if (length < 0 || (size_t)length >= sizeof(encoded))
    status = ARCHBIRD_LIMIT_EXCEEDED;
  else if (output_write(&wasm_output, (const uint8_t *)encoded, (size_t)length))
    status = ARCHBIRD_OUT_OF_MEMORY;
  return (int)result_finish(owned->engine, status);
}

AB_WASM_EXPORT int ab_wasm_project_merge_summary(uintptr_t handle) {
  WasmProject *owned = checked_project(handle);
  ArchbirdMergeSummary summary;
  ArchbirdStatus status;
  char encoded[768];
  int length;
  result_reset();
  if (!owned)
    return (int)invalid_argument("invalid Wasm project handle");
  memset(&summary, 0, sizeof(summary));
  summary.struct_size = sizeof(summary);
  status = archbird_project_merge_summary(owned->project, &summary);
  if (status == ARCHBIRD_OK) {
    length = snprintf(
        encoded, sizeof(encoded),
        "{\"audit_differences\":\"%" PRIuMAX "\",\"audit_matches\":\"%" PRIuMAX
        "\",\"conflicts\":\"%" PRIuMAX "\",\"contributed\":\"%" PRIuMAX
        "\",\"deduplicated\":\"%" PRIuMAX "\",\"enriched\":\"%" PRIuMAX
        "\",\"providers\":\"%" PRIuMAX "\",\"selected_facts\":\"%" PRIuMAX
        "\",\"selections\":\"%" PRIuMAX "\",\"variations\":\"%" PRIuMAX "\"}",
        (uintmax_t)summary.audit_differences, (uintmax_t)summary.audit_matches,
        (uintmax_t)summary.conflicts, (uintmax_t)summary.contributed,
        (uintmax_t)summary.deduplicated, (uintmax_t)summary.enriched,
        (uintmax_t)summary.providers, (uintmax_t)summary.selected_facts,
        (uintmax_t)summary.selections, (uintmax_t)summary.variations);
    if (length < 0 || (size_t)length >= sizeof(encoded))
      status = ARCHBIRD_LIMIT_EXCEEDED;
    else if (output_write(&wasm_output, (const uint8_t *)encoded,
                          (size_t)length))
      status = ARCHBIRD_OUT_OF_MEMORY;
  }
  return (int)result_finish(owned->engine, status);
}

typedef ArchbirdStatus (*ProjectRenderFn)(ArchbirdEngine *,
                                          const ArchbirdProject *, uint32_t,
                                          ArchbirdWriteFn, void *);

static int project_render(uintptr_t handle, uint32_t flags,
                          ProjectRenderFn function) {
  WasmProject *owned = checked_project(handle);
  ArchbirdStatus status;
  result_reset();
  if (!owned)
    return (int)invalid_argument("invalid Wasm project handle");
  status = function(owned->engine, owned->project, flags, output_write,
                    &wasm_output);
  return (int)result_finish(owned->engine, status);
}

AB_WASM_EXPORT int ab_wasm_project_file_facts(uintptr_t handle,
                                              uint32_t flags) {
  return project_render(handle, flags, archbird_project_render_file_facts);
}

AB_WASM_EXPORT int ab_wasm_project_merge_ledger(uintptr_t handle,
                                                uint32_t flags) {
  return project_render(handle, flags, archbird_project_render_merge_ledger);
}

AB_WASM_EXPORT int ab_wasm_project_merge_conflicts(uintptr_t handle,
                                                   uint32_t flags) {
  return project_render(handle, flags, archbird_project_render_merge_conflicts);
}

AB_WASM_EXPORT int ab_wasm_project_map(uintptr_t handle, uint32_t flags) {
  return project_render(handle, flags, archbird_project_render_map);
}

AB_WASM_EXPORT int
ab_wasm_project_provider_facts(uintptr_t handle, size_t index, uint32_t flags) {
  WasmProject *owned = checked_project(handle);
  ArchbirdStatus status;
  result_reset();
  if (!owned)
    return (int)invalid_argument("invalid Wasm project handle");
  status = archbird_project_render_provider_facts(
      owned->engine, owned->project, index, flags, output_write, &wasm_output);
  return (int)result_finish(owned->engine, status);
}

AB_WASM_EXPORT uintptr_t ab_wasm_discovery_create(const uint8_t *config,
                                                  size_t config_length) {
  WasmDiscovery *owned;
  ArchbirdStatus status;
  result_reset();
  owned = (WasmDiscovery *)calloc(1, sizeof(*owned));
  if (!owned) {
    (void)result_finish(NULL, ARCHBIRD_OUT_OF_MEMORY);
    return (uintptr_t)0;
  }
  status = archbird_engine_create(NULL, &owned->engine);
  if (status == ARCHBIRD_OK)
    status = archbird_discovery_create(owned->engine, config, config_length,
                                       &owned->discovery);
  if (status != ARCHBIRD_OK) {
    (void)result_finish(owned->engine, status);
    archbird_discovery_destroy(owned->discovery);
    archbird_engine_destroy(owned->engine);
    free(owned);
    return (uintptr_t)0;
  }
  owned->magic = WASM_DISCOVERY_MAGIC;
  return (uintptr_t)owned;
}

AB_WASM_EXPORT void ab_wasm_discovery_destroy(uintptr_t handle) {
  WasmDiscovery *owned = checked_discovery(handle);
  if (!owned)
    return;
  owned->magic = 0;
  archbird_discovery_destroy(owned->discovery);
  archbird_engine_destroy(owned->engine);
  free(owned);
}

AB_WASM_EXPORT int ab_wasm_discovery_add_path(uintptr_t handle,
                                              const char *path,
                                              size_t path_length) {
  WasmDiscovery *owned = checked_discovery(handle);
  ArchbirdStatus status;
  result_reset();
  if (!owned)
    return (int)invalid_argument("invalid Wasm discovery handle");
  status = archbird_discovery_add_path(owned->engine, owned->discovery, path,
                                       path_length);
  return (int)result_finish(owned->engine, status);
}

AB_WASM_EXPORT int ab_wasm_discovery_should_descend(uintptr_t handle,
                                                    const char *path,
                                                    size_t path_length) {
  WasmDiscovery *owned = checked_discovery(handle);
  ArchbirdStatus status;
  int should_descend = 0;
  result_reset();
  if (!owned) {
    (void)invalid_argument("invalid Wasm discovery handle");
    return -1;
  }
  status = archbird_discovery_should_descend(
      owned->engine, owned->discovery, path, path_length, &should_descend);
  (void)result_finish(owned->engine, status);
  return status == ARCHBIRD_OK ? should_descend : -1;
}

AB_WASM_EXPORT int ab_wasm_discovery_render(uintptr_t handle, uint32_t flags) {
  WasmDiscovery *owned = checked_discovery(handle);
  ArchbirdStatus status;
  result_reset();
  if (!owned)
    return (int)invalid_argument("invalid Wasm discovery handle");
  status = archbird_discovery_render(owned->engine, owned->discovery, flags,
                                     output_write, &wasm_output);
  return (int)result_finish(owned->engine, status);
}

AB_WASM_EXPORT int
ab_wasm_discovery_resolve(const uint8_t *config, size_t config_length,
                          const uint8_t *request, size_t request_length,
                          const uint8_t *inventory, size_t inventory_length,
                          uint32_t flags) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_input(
      larger_input(larger_input(config_length, request_length),
                   inventory_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_discovery_resolve(
        engine, config, config_length, request, request_length, inventory,
        inventory_length, flags, output_write, &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_json_canonicalize(const uint8_t *input,
                                             size_t input_length,
                                             uint32_t flags) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_input(input_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, input, input_length, flags,
                                        output_write, &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_map_markdown(const uint8_t *map, size_t map_length,
                                        int full, size_t max_chars) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_saved_artifact(map_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_render_markdown(
        engine, map, map_length, full, max_chars, output_write, &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_map_markdown_view(const uint8_t *map,
                                             size_t map_length, int view,
                                             int detail, size_t max_chars) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_saved_artifact(map_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_render_markdown_view(
        engine, map, map_length, (ArchbirdMapView)view,
        (ArchbirdReportDetail)detail, max_chars, output_write, &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_map_query(const uint8_t *map, size_t map_length,
                                     const uint8_t *query, size_t query_length,
                                     uint32_t flags) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_saved_artifact(
      larger_input(map_length, query_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_query(engine, map, map_length, query, query_length,
                                flags, output_write, &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_map_query_markdown(const uint8_t *map,
                                              size_t map_length,
                                              const uint8_t *query,
                                              size_t query_length,
                                              size_t max_chars) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_saved_artifact(
      larger_input(map_length, query_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_query_markdown(engine, map, map_length, query,
                                         query_length, max_chars, output_write,
                                         &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int
ab_wasm_map_query_markdown_view(const uint8_t *map, size_t map_length,
                                const uint8_t *query, size_t query_length,
                                int view, int detail, size_t max_chars) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_saved_artifact(
      larger_input(map_length, query_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_query_markdown_view(
        engine, map, map_length, query, query_length, (ArchbirdQueryView)view,
        (ArchbirdReportDetail)detail, max_chars, output_write, &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_map_query_markdown_view_with_verification(
    const uint8_t *map, size_t map_length, const uint8_t *query,
    size_t query_length, const uint8_t *verification,
    size_t verification_length, int view, int detail, size_t max_chars) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_saved_artifact(
      larger_input(larger_input(map_length, query_length), verification_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_query_markdown_view_with_verification(
        engine, map, map_length, query, query_length, verification,
        verification_length, (ArchbirdQueryView)view,
        (ArchbirdReportDetail)detail, max_chars, output_write, &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_map_diff(const uint8_t *before, size_t before_length,
                                    const uint8_t *after, size_t after_length,
                                    uint32_t flags) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_saved_artifact(
      larger_input(before_length, after_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_diff(engine, before, before_length, after,
                               after_length, flags, output_write, &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_map_freshness(const uint8_t *snapshot,
                                         size_t snapshot_length,
                                         const uint8_t *current,
                                         size_t current_length,
                                         uint32_t flags) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_saved_artifact(
      larger_input(snapshot_length, current_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_freshness(engine, snapshot, snapshot_length, current,
                                    current_length, flags, output_write,
                                    &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_map_export_graph(const uint8_t *map,
                                            size_t map_length, uint32_t format,
                                            uint32_t view, uint32_t direction,
                                            size_t max_nodes,
                                            size_t max_edge_names) {
  ArchbirdEngine *engine = NULL;
  ArchbirdGraphOptions options;
  ArchbirdStatus status = stateless_begin_saved_artifact(map_length, &engine);
  if (format > (uint32_t)ARCHBIRD_GRAPH_JSON ||
      view > (uint32_t)ARCHBIRD_GRAPH_SYMBOLS ||
      direction > (uint32_t)ARCHBIRD_GRAPH_BT)
    status = ARCHBIRD_INVALID_ARGUMENT;
  if (status == ARCHBIRD_OK) {
    archbird_graph_options_init(&options);
    options.format = (ArchbirdGraphFormat)format;
    options.view = (ArchbirdGraphView)view;
    options.direction = (ArchbirdGraphDirection)direction;
    options.max_nodes = max_nodes;
    options.max_edge_names = max_edge_names;
    status = archbird_map_export_graph(engine, map, map_length, &options,
                                       output_write, &wasm_output);
  }
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_okf_analyze(const uint8_t *source,
                                       size_t source_length,
                                       const uint8_t *query,
                                       size_t query_length, uint32_t format,
                                       int include_body, uint32_t flags) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status =
      stateless_begin_input(larger_input(source_length, query_length), &engine);
  if (format > (uint32_t)ARCHBIRD_OKF_MARKDOWN)
    status = ARCHBIRD_INVALID_ARGUMENT;
  if (status == ARCHBIRD_OK)
    status = archbird_okf_analyze(engine, source, source_length,
                                  query_length ? query : NULL, query_length,
                                  (ArchbirdOkfFormat)format, include_body,
                                  flags, output_write, &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_okf_publish(
    const uint8_t *map, size_t map_length, const uint8_t *verification,
    size_t verification_length, const uint8_t *proposal, size_t proposal_length,
    const uint8_t *contract, size_t contract_length,
    const uint8_t *change_result, size_t result_length,
    const uint8_t *normalization, size_t normalization_length, uint32_t flags) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_saved_artifact(
      larger_input(larger_input(larger_input(map_length, verification_length),
                                larger_input(proposal_length, contract_length)),
                   larger_input(result_length, normalization_length)),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_okf_publish(
        engine, map, map_length, verification_length ? verification : NULL,
        verification_length, proposal_length ? proposal : NULL, proposal_length,
        contract_length ? contract : NULL, contract_length,
        result_length ? change_result : NULL, result_length,
        normalization_length ? normalization : NULL, normalization_length,
        flags, output_write, &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_workspace_plan(const uint8_t *config,
                                          size_t config_length,
                                          uint32_t flags) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_input(config_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_workspace_plan(engine, config, config_length, flags,
                                     output_write, &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_workspace_analyze(const uint8_t *config,
                                             size_t config_length,
                                             const uint8_t *maps,
                                             size_t maps_length,
                                             uint32_t flags) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_saved_artifact(
      larger_input(config_length, maps_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_workspace_analyze(engine, config, config_length, maps,
                                        maps_length, flags, output_write,
                                        &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_verification_plan(const uint8_t *suite,
                                             size_t suite_length,
                                             uint32_t flags) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_input(suite_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_verification_plan(engine, suite, suite_length, flags,
                                        output_write, &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_verification_analyze(const uint8_t *suite,
                                                size_t suite_length,
                                                const uint8_t *input,
                                                size_t input_length,
                                                uint32_t flags) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_saved_artifact(
      larger_input(suite_length, input_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_verification_analyze(engine, suite, suite_length, input,
                                           input_length, flags, output_write,
                                           &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_verification_draft(const uint8_t *map,
                                              size_t map_length,
                                              const char *project_config,
                                              size_t project_config_length,
                                              uint32_t flags) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_saved_artifact(map_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_verification_draft(engine, map, map_length,
                                         project_config, project_config_length,
                                         flags, output_write, &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_verification_freeze(
    const uint8_t *suite, size_t suite_length, const uint8_t *input,
    size_t input_length, const char *owner, size_t owner_length,
    const char *rationale, size_t rationale_length, uint32_t flags) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_saved_artifact(
      larger_input(suite_length, input_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_verification_freeze(
        engine, suite, suite_length, input, input_length, owner, owner_length,
        rationale, rationale_length, flags, output_write, &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_verification_report(
    const uint8_t *suite, size_t suite_length, const uint8_t *input,
    size_t input_length, uint32_t format, size_t max_findings, uint32_t flags) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_saved_artifact(
      larger_input(suite_length, input_length), &engine);
  if (format == (uint32_t)ARCHBIRD_VERIFICATION_JSON ||
      format > (uint32_t)ARCHBIRD_VERIFICATION_JUNIT)
    status = ARCHBIRD_INVALID_ARGUMENT;
  if (status == ARCHBIRD_OK)
    status = archbird_verification_analyze_report(
        engine, suite, suite_length, input, input_length,
        (ArchbirdVerificationFormat)format, max_findings, flags, output_write,
        &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int
ab_wasm_change_proposal(const uint8_t *verification, size_t verification_length,
                        const char *fingerprint, size_t fingerprint_length,
                        uint32_t format, int full, size_t max_candidates,
                        uint32_t flags) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status =
      stateless_begin_saved_artifact(verification_length, &engine);
  if (format > 1)
    status = ARCHBIRD_INVALID_ARGUMENT;
  if (status == ARCHBIRD_OK && format == 0)
    status = archbird_change_proposal(engine, verification, verification_length,
                                      fingerprint, fingerprint_length, flags,
                                      output_write, &wasm_output);
  else if (status == ARCHBIRD_OK)
    status = archbird_change_proposal_report(
        engine, verification, verification_length, fingerprint,
        fingerprint_length, full, max_candidates, output_write, &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int ab_wasm_change_contract(const uint8_t *proposal,
                                           size_t proposal_length,
                                           const uint8_t *review,
                                           size_t review_length,
                                           uint32_t format, uint32_t flags) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_saved_artifact(
      larger_input(proposal_length, review_length), &engine);
  if (format > 1)
    status = ARCHBIRD_INVALID_ARGUMENT;
  if (status == ARCHBIRD_OK && format == 0)
    status = archbird_change_contract(engine, proposal, proposal_length, review,
                                      review_length, flags, output_write,
                                      &wasm_output);
  else if (status == ARCHBIRD_OK)
    status = archbird_change_contract_report(engine, proposal, proposal_length,
                                             review, review_length,
                                             output_write, &wasm_output);
  return stateless_end(engine, status);
}

AB_WASM_EXPORT int
ab_wasm_change_verify(const uint8_t *proposal, size_t proposal_length,
                      const uint8_t *contract, size_t contract_length,
                      const uint8_t *before, size_t before_length,
                      const uint8_t *after, size_t after_length,
                      uint32_t format, uint32_t flags) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = stateless_begin_saved_artifact(
      larger_input(larger_input(proposal_length, contract_length),
                   larger_input(before_length, after_length)),
      &engine);
  if (format > (uint32_t)ARCHBIRD_CHANGE_JUNIT)
    status = ARCHBIRD_INVALID_ARGUMENT;
  if (status == ARCHBIRD_OK && format == (uint32_t)ARCHBIRD_CHANGE_JSON)
    status = archbird_change_verify(
        engine, proposal, proposal_length, contract, contract_length, before,
        before_length, after, after_length, flags, output_write, &wasm_output);
  else if (status == ARCHBIRD_OK)
    status = archbird_change_verify_report(
        engine, proposal, proposal_length, contract, contract_length, before,
        before_length, after, after_length, (ArchbirdChangeFormat)format, flags,
        output_write, &wasm_output);
  return stateless_end(engine, status);
}
