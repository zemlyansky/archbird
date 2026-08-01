#include <node_api.h>

#include <archbird/archbird.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ARCHBIRD_VERSION
#define ARCHBIRD_VERSION "0.0.2"
#endif

typedef struct NodeProject {
  ArchbirdEngine *engine;
  ArchbirdProject *project;
} NodeProject;

typedef struct NodeOutput {
  uint8_t *data;
  size_t length;
  size_t capacity;
} NodeOutput;

#define NAPI_TRY(call)                                                         \
  do {                                                                         \
    napi_status napi_call_status = (call);                                     \
    if (napi_call_status != napi_ok) {                                         \
      const napi_extended_error_info *napi_error_info = NULL;                  \
      (void)napi_get_last_error_info(env, &napi_error_info);                   \
      napi_throw_error(env, "ARCHBIRD_NAPI",                                   \
                       napi_error_info && napi_error_info->error_message       \
                           ? napi_error_info->error_message                    \
                           : "Node-API call failed");                          \
      return NULL;                                                             \
    }                                                                          \
  } while (0)

static int output_write(void *user_data, const uint8_t *bytes, size_t length) {
  NodeOutput *output = (NodeOutput *)user_data;
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

static napi_value throw_status(napi_env env, ArchbirdEngine *engine,
                               ArchbirdStatus status) {
  const char *message = engine ? archbird_engine_error(engine) : NULL;
  size_t offset =
      engine ? archbird_engine_error_offset(engine) : ARCHBIRD_NO_OFFSET;
  char code[48];
  char detail[1024];
  if (!message || !message[0])
    message = "native Archbird operation failed";
  (void)snprintf(code, sizeof(code), "ARCHBIRD_STATUS_%d", (int)status);
  if (offset == ARCHBIRD_NO_OFFSET)
    (void)snprintf(detail, sizeof(detail), "%s (status=%d)", message,
                   (int)status);
  else
    (void)snprintf(detail, sizeof(detail), "%s (status=%d, byte=%zu)", message,
                   (int)status, offset);
  napi_throw_error(env, code, detail);
  return NULL;
}

static napi_value render_result(napi_env env, ArchbirdEngine *engine,
                                ArchbirdStatus status, NodeOutput *output) {
  napi_value result;
  napi_status napi_result;
  if (status != ARCHBIRD_OK) {
    free(output->data);
    return throw_status(env, engine, status);
  }
  napi_result =
      napi_create_buffer_copy(env, output->length, output->data, NULL, &result);
  free(output->data);
  if (napi_result != napi_ok) {
    napi_throw_error(env, "ARCHBIRD_NAPI", "could not allocate output Buffer");
    return NULL;
  }
  return result;
}

static ArchbirdStatus input_engine_profile(size_t input_length,
                                           ArchbirdInputProfile profile,
                                           ArchbirdEngine **out_engine) {
  ArchbirdEngineOptions options;
  ArchbirdStatus status =
      archbird_engine_options_init_for_input(&options, profile, input_length);
  if (status != ARCHBIRD_OK)
    return status;
  return archbird_engine_create(&options, out_engine);
}

static ArchbirdStatus input_engine(size_t input_length,
                                   ArchbirdEngine **out_engine) {
  return input_engine_profile(input_length, ARCHBIRD_INPUT_DEFAULT, out_engine);
}

static ArchbirdStatus saved_artifact_engine(size_t input_length,
                                            ArchbirdEngine **out_engine) {
  return input_engine_profile(input_length, ARCHBIRD_INPUT_SAVED_ARTIFACT,
                              out_engine);
}

static size_t larger_input(size_t left, size_t right) {
  return left > right ? left : right;
}

static void project_release(NodeProject *owned) {
  if (!owned)
    return;
  archbird_project_destroy(owned->project);
  archbird_engine_destroy(owned->engine);
  owned->project = NULL;
  owned->engine = NULL;
}

static void project_finalize(napi_env env, void *data, void *hint) {
  NodeProject *owned = (NodeProject *)data;
  (void)env;
  (void)hint;
  project_release(owned);
  free(owned);
}

static NodeProject *get_project(napi_env env, napi_value value) {
  NodeProject *owned = NULL;
  if (napi_get_value_external(env, value, (void **)&owned) != napi_ok ||
      !owned || !owned->engine || !owned->project) {
    napi_throw_type_error(env, "ARCHBIRD_PROJECT", "expected native Project");
    return NULL;
  }
  return owned;
}

static napi_value project_dispose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  NodeProject *owned = NULL;
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc != 1 ||
      napi_get_value_external(env, argv[0], (void **)&owned) != napi_ok ||
      !owned) {
    napi_throw_type_error(env, "ARCHBIRD_PROJECT", "expected native Project");
    return NULL;
  }
  project_release(owned);
  NAPI_TRY(napi_get_undefined(env, &result));
  return result;
}

static int get_buffer(napi_env env, napi_value value, const uint8_t **bytes,
                      size_t *length) {
  void *data = NULL;
  if (napi_get_buffer_info(env, value, &data, length) != napi_ok) {
    napi_throw_type_error(env, "ARCHBIRD_BUFFER", "expected Buffer");
    return 0;
  }
  *bytes = (const uint8_t *)data;
  return 1;
}

static char *get_string(napi_env env, napi_value value, size_t *out_length) {
  size_t length = 0;
  size_t written = 0;
  char *text;
  if (napi_get_value_string_utf8(env, value, NULL, 0, &length) != napi_ok) {
    napi_throw_type_error(env, "ARCHBIRD_STRING", "expected string");
    return NULL;
  }
  text = (char *)malloc(length + 1);
  if (!text) {
    napi_throw_error(env, "ARCHBIRD_OOM", "out of memory reading string");
    return NULL;
  }
  if (napi_get_value_string_utf8(env, value, text, length + 1, &written) !=
          napi_ok ||
      written != length) {
    free(text);
    napi_throw_error(env, "ARCHBIRD_NAPI", "could not read string");
    return NULL;
  }
  *out_length = length;
  return text;
}

static char *get_nullable_string(napi_env env, napi_value value,
                                 size_t *out_length, int *out_is_null) {
  napi_value null_value;
  bool is_null = false;
  if (napi_get_null(env, &null_value) != napi_ok ||
      napi_strict_equals(env, value, null_value, &is_null) != napi_ok) {
    napi_throw_error(env, "ARCHBIRD_NAPI", "could not inspect nullable string");
    return NULL;
  }
  if (is_null) {
    *out_length = 0;
    *out_is_null = 1;
    return NULL;
  }
  *out_is_null = 0;
  return get_string(env, value, out_length);
}

static int get_optional_bool(napi_env env, size_t argc, napi_value *argv,
                             size_t index, int default_value, int *out_value) {
  bool value;
  if (argc <= index) {
    *out_value = default_value;
    return 1;
  }
  if (napi_get_value_bool(env, argv[index], &value) != napi_ok) {
    napi_throw_type_error(env, "ARCHBIRD_BOOL", "expected boolean");
    return 0;
  }
  *out_value = value ? 1 : 0;
  return 1;
}

static int get_optional_size(napi_env env, size_t argc, napi_value *argv,
                             size_t index, size_t default_value,
                             const char *name, size_t *out_value) {
  napi_valuetype type;
  double value;
  if (argc <= index) {
    *out_value = default_value;
    return 1;
  }
  if (napi_typeof(env, argv[index], &type) != napi_ok || type != napi_number ||
      napi_get_value_double(env, argv[index], &value) != napi_ok ||
      !isfinite(value) || value < 0.0 || value > 9007199254740991.0 ||
      value > (double)SIZE_MAX || value != (double)(size_t)value) {
    char message[128];
    (void)snprintf(message, sizeof(message),
                   "%s must be a nonnegative safe integer", name);
    napi_throw_type_error(env, "ARCHBIRD_NUMBER", message);
    return 0;
  }
  *out_value = (size_t)value;
  return 1;
}

static int parse_mode(napi_env env, napi_value value,
                      ArchbirdProviderMode *out) {
  size_t length;
  char *text = get_string(env, value, &length);
  int valid = 1;
  if (!text)
    return 0;
  if (length == 7 && memcmp(text, "primary", 7) == 0)
    *out = ARCHBIRD_PROVIDER_PRIMARY;
  else if (length == 7 && memcmp(text, "augment", 7) == 0)
    *out = ARCHBIRD_PROVIDER_AUGMENT;
  else if (length == 5 && memcmp(text, "audit", 5) == 0)
    *out = ARCHBIRD_PROVIDER_AUDIT;
  else {
    napi_throw_range_error(env, "ARCHBIRD_MODE",
                           "mode must be primary, augment, or audit");
    valid = 0;
  }
  free(text);
  return valid;
}

static napi_value project_create(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  const uint8_t *manifest;
  size_t manifest_length;
  NodeProject *owned;
  ArchbirdStatus status;
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc != 1 || !get_buffer(env, argv[0], &manifest, &manifest_length))
    return NULL;
  owned = (NodeProject *)calloc(1, sizeof(*owned));
  if (!owned) {
    napi_throw_error(env, "ARCHBIRD_OOM", "out of memory creating Project");
    return NULL;
  }
  status = archbird_engine_create(NULL, &owned->engine);
  if (status == ARCHBIRD_OK)
    status = archbird_project_create(owned->engine, manifest, manifest_length,
                                     &owned->project);
  if (status != ARCHBIRD_OK) {
    napi_value raised = throw_status(env, owned->engine, status);
    project_finalize(env, owned, NULL);
    return raised;
  }
  if (napi_create_external(env, owned, project_finalize, NULL, &result) !=
      napi_ok) {
    project_finalize(env, owned, NULL);
    napi_throw_error(env, "ARCHBIRD_NAPI", "could not create Project handle");
    return NULL;
  }
  return result;
}

static napi_value project_add_source(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  NodeProject *owned;
  char *path;
  size_t path_length;
  const uint8_t *bytes;
  size_t byte_length;
  ArchbirdStatus status;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc != 3)
    return throw_status(env, NULL, ARCHBIRD_INVALID_ARGUMENT);
  owned = get_project(env, argv[0]);
  if (!owned)
    return NULL;
  path = get_string(env, argv[1], &path_length);
  if (!path)
    return NULL;
  if (!get_buffer(env, argv[2], &bytes, &byte_length)) {
    free(path);
    return NULL;
  }
  status = archbird_project_add_source(owned->engine, owned->project, path,
                                       path_length, bytes, byte_length);
  free(path);
  if (status != ARCHBIRD_OK)
    return throw_status(env, owned->engine, status);
  return NULL;
}

static napi_value project_finalize_sources(napi_env env,
                                           napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  NodeProject *owned;
  ArchbirdStatus status;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc != 1 || !(owned = get_project(env, argv[0])))
    return NULL;
  status = archbird_project_finalize_sources(owned->engine, owned->project);
  if (status != ARCHBIRD_OK)
    return throw_status(env, owned->engine, status);
  return NULL;
}

static napi_value project_set_config(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  NodeProject *owned;
  const uint8_t *config;
  size_t config_length;
  ArchbirdStatus status;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc != 2 || !(owned = get_project(env, argv[0])) ||
      !get_buffer(env, argv[1], &config, &config_length))
    return NULL;
  status = archbird_project_set_config(owned->engine, owned->project, config,
                                       config_length);
  if (status != ARCHBIRD_OK)
    return throw_status(env, owned->engine, status);
  return NULL;
}

static napi_value project_config_sha256(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  NodeProject *owned;
  const char *digest;
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc != 1 || !(owned = get_project(env, argv[0])))
    return NULL;
  digest = archbird_project_config_sha256(owned->project);
  if (!digest)
    return throw_status(env, owned->engine, ARCHBIRD_CONFLICT);
  NAPI_TRY(napi_create_string_utf8(env, digest, 64, &result));
  return result;
}

static napi_value project_add_provider(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  NodeProject *owned;
  ArchbirdProviderMode mode;
  const uint8_t *provider;
  size_t provider_length;
  ArchbirdStatus status;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc != 3 || !(owned = get_project(env, argv[0])) ||
      !parse_mode(env, argv[1], &mode) ||
      !get_buffer(env, argv[2], &provider, &provider_length))
    return NULL;
  status = archbird_project_add_provider_facts(owned->engine, owned->project,
                                               mode, provider, provider_length);
  if (status != ARCHBIRD_OK)
    return throw_status(env, owned->engine, status);
  return NULL;
}

static napi_value
project_add_test_symbol_observations(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  NodeProject *owned;
  const uint8_t *observations;
  size_t observations_length;
  ArchbirdStatus status;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc != 2 || !(owned = get_project(env, argv[0])) ||
      !get_buffer(env, argv[1], &observations, &observations_length))
    return NULL;
  status = archbird_project_add_test_symbol_observations(
      owned->engine, owned->project, observations, observations_length);
  if (status != ARCHBIRD_OK)
    return throw_status(env, owned->engine, status);
  return NULL;
}

static napi_value project_scan_builtin(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  NodeProject *owned;
  ArchbirdProviderMode mode = ARCHBIRD_PROVIDER_PRIMARY;
  ArchbirdStatus status;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 1 || !(owned = get_project(env, argv[0])))
    return NULL;
  if (argc >= 2 && !parse_mode(env, argv[1], &mode))
    return NULL;
  status = archbird_project_scan_builtin(owned->engine, owned->project, mode);
  if (status != ARCHBIRD_OK)
    return throw_status(env, owned->engine, status);
  return NULL;
}

static napi_value project_scan_builtin_provider(napi_env env,
                                                napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  NodeProject *owned;
  char *provider_id = NULL;
  size_t provider_id_length = 0;
  ArchbirdProviderMode mode = ARCHBIRD_PROVIDER_PRIMARY;
  ArchbirdStatus status;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 2 || !(owned = get_project(env, argv[0])))
    return NULL;
  provider_id = get_string(env, argv[1], &provider_id_length);
  if (!provider_id)
    return NULL;
  if (argc >= 3 && !parse_mode(env, argv[2], &mode)) {
    free(provider_id);
    return NULL;
  }
  status = archbird_project_scan_builtin_provider(
      owned->engine, owned->project, provider_id, provider_id_length, mode);
  free(provider_id);
  if (status != ARCHBIRD_OK)
    return throw_status(env, owned->engine, status);
  return NULL;
}

static napi_value project_scan_builtin_provider_file(napi_env env,
                                                     napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  NodeProject *owned;
  char *provider_id = NULL;
  size_t provider_id_length = 0;
  char *path = NULL;
  size_t path_length = 0;
  ArchbirdProviderMode mode = ARCHBIRD_PROVIDER_PRIMARY;
  ArchbirdStatus status;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 3 || !(owned = get_project(env, argv[0])))
    return NULL;
  provider_id = get_string(env, argv[1], &provider_id_length);
  if (!provider_id)
    return NULL;
  path = get_string(env, argv[2], &path_length);
  if (!path) {
    free(provider_id);
    return NULL;
  }
  if (argc >= 4 && !parse_mode(env, argv[3], &mode)) {
    free(path);
    free(provider_id);
    return NULL;
  }
  status = archbird_project_scan_builtin_provider_file(
      owned->engine, owned->project, provider_id, provider_id_length, path,
      path_length, mode);
  free(path);
  free(provider_id);
  if (status != ARCHBIRD_OK)
    return throw_status(env, owned->engine, status);
  return NULL;
}

static napi_value project_finalize_providers(napi_env env,
                                             napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  NodeProject *owned;
  ArchbirdStatus status;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc != 1 || !(owned = get_project(env, argv[0])))
    return NULL;
  status = archbird_project_finalize_providers(owned->engine, owned->project);
  if (status != ARCHBIRD_OK)
    return throw_status(env, owned->engine, status);
  return NULL;
}

static napi_value project_manifest_sha256(napi_env env,
                                          napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  NodeProject *owned;
  const char *digest;
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc != 1 || !(owned = get_project(env, argv[0])))
    return NULL;
  digest = archbird_project_manifest_sha256(owned->project);
  if (!digest)
    return throw_status(env, owned->engine, ARCHBIRD_CONFLICT);
  NAPI_TRY(napi_create_string_utf8(env, digest, 64, &result));
  return result;
}

static napi_value project_map_input_sha256(napi_env env,
                                           napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  NodeProject *owned;
  const char *digest;
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc != 1 || !(owned = get_project(env, argv[0])))
    return NULL;
  digest = archbird_project_map_input_sha256(owned->project);
  if (!digest)
    return throw_status(env, owned->engine, ARCHBIRD_CONFLICT);
  NAPI_TRY(napi_create_string_utf8(env, digest, 64, &result));
  return result;
}

static napi_value size_value(napi_env env, size_t value) {
  napi_value result;
  if (napi_create_bigint_uint64(env, (uint64_t)value, &result) != napi_ok) {
    napi_throw_error(env, "ARCHBIRD_NAPI", "could not create exact count");
    return NULL;
  }
  return result;
}

static int set_size(napi_env env, napi_value object, const char *name,
                    size_t value) {
  napi_value number = size_value(env, value);
  return number &&
         napi_set_named_property(env, object, name, number) == napi_ok;
}

static napi_value project_counts(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  NodeProject *owned;
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc != 1 || !(owned = get_project(env, argv[0])))
    return NULL;
  NAPI_TRY(napi_create_object(env, &result));
  if (!set_size(env, result, "sources",
                archbird_project_source_count(owned->project)) ||
      !set_size(env, result, "providers",
                archbird_project_provider_count(owned->project)) ||
      !set_size(env, result, "facts",
                archbird_project_provider_fact_count(owned->project)))
    return NULL;
  return result;
}

static napi_value project_merge_summary(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  NodeProject *owned;
  ArchbirdMergeSummary summary;
  ArchbirdStatus status;
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc != 1 || !(owned = get_project(env, argv[0])))
    return NULL;
  memset(&summary, 0, sizeof(summary));
  summary.struct_size = sizeof(summary);
  status = archbird_project_merge_summary(owned->project, &summary);
  if (status != ARCHBIRD_OK)
    return throw_status(env, owned->engine, status);
  NAPI_TRY(napi_create_object(env, &result));
#define SET_SUMMARY(name)                                                      \
  if (!set_size(env, result, #name, summary.name))                             \
  return NULL
  SET_SUMMARY(providers);
  SET_SUMMARY(selections);
  SET_SUMMARY(selected_facts);
  SET_SUMMARY(contributed);
  SET_SUMMARY(deduplicated);
  SET_SUMMARY(enriched);
  SET_SUMMARY(variations);
  SET_SUMMARY(conflicts);
  SET_SUMMARY(audit_matches);
  SET_SUMMARY(audit_differences);
#undef SET_SUMMARY
  return result;
}

typedef ArchbirdStatus (*ProjectRenderFn)(ArchbirdEngine *,
                                          const ArchbirdProject *, uint32_t,
                                          ArchbirdWriteFn, void *);

static napi_value render_project(napi_env env, napi_callback_info info,
                                 ProjectRenderFn function) {
  size_t argc = 2;
  napi_value argv[2];
  NodeProject *owned;
  int pretty;
  NodeOutput output = {0};
  ArchbirdStatus status;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 1 || !(owned = get_project(env, argv[0])) ||
      !get_optional_bool(env, argc, argv, 1, 0, &pretty))
    return NULL;
  status = function(owned->engine, owned->project,
                    pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  return render_result(env, owned->engine, status, &output);
}

static napi_value project_file_facts(napi_env env, napi_callback_info info) {
  return render_project(env, info, archbird_project_render_file_facts);
}

static napi_value project_merge_ledger(napi_env env, napi_callback_info info) {
  return render_project(env, info, archbird_project_render_merge_ledger);
}

static napi_value project_merge_conflicts(napi_env env,
                                          napi_callback_info info) {
  return render_project(env, info, archbird_project_render_merge_conflicts);
}

static napi_value project_map(napi_env env, napi_callback_info info) {
  return render_project(env, info, archbird_project_render_map);
}

static napi_value project_provider_facts(napi_env env,
                                         napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  NodeProject *owned;
  uint32_t index;
  int pretty;
  NodeOutput output = {0};
  ArchbirdStatus status;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 2 || !(owned = get_project(env, argv[0])) ||
      napi_get_value_uint32(env, argv[1], &index) != napi_ok ||
      !get_optional_bool(env, argc, argv, 2, 0, &pretty))
    return NULL;
  status = archbird_project_render_provider_facts(
      owned->engine, owned->project, index, pretty ? ARCHBIRD_JSON_PRETTY : 0,
      output_write, &output);
  return render_result(env, owned->engine, status, &output);
}

static napi_value json_canonicalize(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  const uint8_t *input;
  size_t input_length;
  int pretty;
  int trailing;
  uint32_t flags = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 1 || !get_buffer(env, argv[0], &input, &input_length) ||
      !get_optional_bool(env, argc, argv, 1, 0, &pretty) ||
      !get_optional_bool(env, argc, argv, 2, 0, &trailing))
    return NULL;
  if (pretty)
    flags |= ARCHBIRD_JSON_PRETTY;
  if (trailing)
    flags |= ARCHBIRD_JSON_TRAILING_NEWLINE;
  status = input_engine(input_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, input, input_length, flags,
                                        output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value test_symbol_observations_validate(napi_env env,
                                                    napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  const uint8_t *input;
  size_t input_length;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 1 || !get_buffer(env, argv[0], &input, &input_length))
    return NULL;
  status = saved_artifact_engine(input_length, &engine);
  if (status == ARCHBIRD_OK)
    status =
        archbird_test_symbol_observations_validate(engine, input, input_length);
  if (status != ARCHBIRD_OK) {
    result = throw_status(env, engine, status);
    archbird_engine_destroy(engine);
    return result;
  }
  archbird_engine_destroy(engine);
  NAPI_TRY(napi_get_undefined(env, &result));
  return result;
}

static napi_value plan_validate(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  const uint8_t *input;
  size_t input_length;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 1 || !get_buffer(env, argv[0], &input, &input_length))
    return NULL;
  status = saved_artifact_engine(input_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_plan_validate(engine, input, input_length);
  if (status != ARCHBIRD_OK) {
    result = throw_status(env, engine, status);
    archbird_engine_destroy(engine);
    return result;
  }
  archbird_engine_destroy(engine);
  NAPI_TRY(napi_get_undefined(env, &result));
  return result;
}

static napi_value plan_render_markdown(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  const uint8_t *input;
  size_t input_length;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 1 || !get_buffer(env, argv[0], &input, &input_length))
    return NULL;
  status = saved_artifact_engine(input_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_plan_render_markdown(engine, input, input_length,
                                           output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value plan_compile(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6];
  NodeProject *owned;
  const uint8_t *map;
  const uint8_t *before_map;
  const uint8_t *verification;
  const uint8_t *request;
  size_t map_length;
  size_t before_map_length;
  size_t verification_length;
  size_t request_length;
  int pretty;
  ArchbirdStatus status;
  NodeOutput output = {0};
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 5 || !(owned = get_project(env, argv[0])) ||
      !get_buffer(env, argv[1], &map, &map_length) ||
      !get_buffer(env, argv[2], &verification, &verification_length) ||
      !get_buffer(env, argv[3], &before_map, &before_map_length) ||
      !get_buffer(env, argv[4], &request, &request_length) ||
      !get_optional_bool(env, argc, argv, 5, 0, &pretty))
    return NULL;
  status = archbird_plan_compile(
      owned->engine, owned->project, map, map_length,
      before_map_length ? before_map : NULL, before_map_length, verification,
      verification_length, request_length ? request : NULL, request_length,
      pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  return render_result(env, owned->engine, status, &output);
}

static napi_value act_validate(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  const uint8_t *input;
  size_t input_length;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 1 || !get_buffer(env, argv[0], &input, &input_length))
    return NULL;
  status = saved_artifact_engine(input_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_act_validate(engine, input, input_length);
  if (status != ARCHBIRD_OK) {
    result = throw_status(env, engine, status);
    archbird_engine_destroy(engine);
    return result;
  }
  archbird_engine_destroy(engine);
  NAPI_TRY(napi_get_undefined(env, &result));
  return result;
}

static napi_value plan_source_requirements(napi_env env,
                                           napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  const uint8_t *plan;
  const uint8_t *submissions;
  size_t plan_length;
  size_t submissions_length;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 2 || !get_buffer(env, argv[0], &plan, &plan_length) ||
      !get_buffer(env, argv[1], &submissions, &submissions_length) ||
      !get_optional_bool(env, argc, argv, 2, 0, &pretty))
    return NULL;
  status = saved_artifact_engine(larger_input(plan_length, submissions_length),
                                 &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_plan_source_requirements(
        engine, plan, plan_length, submissions, submissions_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value act_source_requirements(napi_env env,
                                          napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  const uint8_t *act;
  size_t act_length;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 1 || !get_buffer(env, argv[0], &act, &act_length) ||
      !get_optional_bool(env, argc, argv, 1, 0, &pretty))
    return NULL;
  status = saved_artifact_engine(act_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_act_source_requirements(engine, act, act_length,
                                              pretty ? ARCHBIRD_JSON_PRETTY : 0,
                                              output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value act_materialize(napi_env env, napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7];
  NodeProject *owned;
  const uint8_t *plan;
  const uint8_t *map;
  const uint8_t *verification;
  const uint8_t *metadata;
  const uint8_t *submissions;
  size_t plan_length;
  size_t map_length;
  size_t verification_length;
  size_t metadata_length;
  size_t submissions_length;
  int pretty;
  ArchbirdStatus status;
  NodeOutput output = {0};
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 6 || !(owned = get_project(env, argv[0])) ||
      !get_buffer(env, argv[1], &plan, &plan_length) ||
      !get_buffer(env, argv[2], &map, &map_length) ||
      !get_buffer(env, argv[3], &verification, &verification_length) ||
      !get_buffer(env, argv[4], &metadata, &metadata_length) ||
      !get_buffer(env, argv[5], &submissions, &submissions_length) ||
      !get_optional_bool(env, argc, argv, 6, 0, &pretty))
    return NULL;
  status = archbird_act_materialize(
      owned->engine, owned->project, plan, plan_length, map, map_length,
      verification, verification_length, metadata, metadata_length, submissions,
      submissions_length, pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write,
      &output);
  return render_result(env, owned->engine, status, &output);
}

static napi_value act_accept(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5];
  const uint8_t *act;
  const uint8_t *before_map;
  const uint8_t *after_map;
  const uint8_t *verification;
  size_t act_length;
  size_t before_map_length;
  size_t after_map_length;
  size_t verification_length;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  size_t budget;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 4 || !get_buffer(env, argv[0], &act, &act_length) ||
      !get_buffer(env, argv[1], &before_map, &before_map_length) ||
      !get_buffer(env, argv[2], &after_map, &after_map_length) ||
      !get_buffer(env, argv[3], &verification, &verification_length) ||
      !get_optional_bool(env, argc, argv, 4, 0, &pretty))
    return NULL;
  budget = larger_input(larger_input(act_length, before_map_length),
                        larger_input(after_map_length, verification_length));
  status = saved_artifact_engine(budget, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_act_accept(
        engine, act, act_length, before_map, before_map_length, after_map,
        after_map_length, verification, verification_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value act_preflight_apply(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  const uint8_t *act;
  const uint8_t *metadata;
  size_t act_length;
  size_t metadata_length;
  ArchbirdEngine *engine = NULL;
  ArchbirdActApplyState apply_state = ARCHBIRD_ACT_APPLY_READY;
  ArchbirdStatus status;
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 2 || !get_buffer(env, argv[0], &act, &act_length) ||
      !get_buffer(env, argv[1], &metadata, &metadata_length))
    return NULL;
  status =
      saved_artifact_engine(larger_input(act_length, metadata_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_act_preflight_apply(engine, act, act_length, metadata,
                                          metadata_length, &apply_state);
  if (status != ARCHBIRD_OK) {
    result = throw_status(env, engine, status);
    archbird_engine_destroy(engine);
    return result;
  }
  archbird_engine_destroy(engine);
  NAPI_TRY(napi_create_uint32(env, (uint32_t)apply_state, &result));
  return result;
}

static napi_value discovery_plan(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  const uint8_t *config;
  size_t config_length;
  bool is_array = false;
  uint32_t path_count = 0;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdDiscovery *discovery = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  uint32_t index;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 2 || !get_buffer(env, argv[0], &config, &config_length) ||
      !get_optional_bool(env, argc, argv, 2, 0, &pretty))
    return NULL;
  if (napi_is_array(env, argv[1], &is_array) != napi_ok || !is_array ||
      napi_get_array_length(env, argv[1], &path_count) != napi_ok) {
    napi_throw_type_error(env, "ARCHBIRD_NAPI",
                          "paths must be an array of strings");
    return NULL;
  }
  status = input_engine(config_length, &engine);
  if (status == ARCHBIRD_OK)
    status =
        archbird_discovery_create(engine, config, config_length, &discovery);
  for (index = 0; status == ARCHBIRD_OK && index < path_count; index++) {
    napi_value item;
    napi_valuetype type;
    size_t path_length = 0;
    char *path = NULL;
    if (napi_get_element(env, argv[1], index, &item) != napi_ok ||
        napi_typeof(env, item, &type) != napi_ok || type != napi_string ||
        napi_get_value_string_utf8(env, item, NULL, 0, &path_length) !=
            napi_ok) {
      napi_throw_type_error(env, "ARCHBIRD_NAPI",
                            "discovery paths must be strings");
      status = ARCHBIRD_INVALID_ARGUMENT;
      break;
    }
    path = (char *)malloc(path_length + 1);
    if (!path) {
      status = ARCHBIRD_OUT_OF_MEMORY;
      break;
    }
    if (napi_get_value_string_utf8(env, item, path, path_length + 1,
                                   &path_length) != napi_ok) {
      free(path);
      napi_throw_type_error(env, "ARCHBIRD_NAPI",
                            "could not decode a discovery path");
      status = ARCHBIRD_INVALID_ARGUMENT;
      break;
    }
    status = archbird_discovery_add_path(engine, discovery, path, path_length);
    free(path);
  }
  if (status == ARCHBIRD_OK)
    status = archbird_discovery_render(engine, discovery,
                                       pretty ? ARCHBIRD_JSON_PRETTY : 0,
                                       output_write, &output);
  archbird_discovery_destroy(discovery);
  if (status == ARCHBIRD_INVALID_ARGUMENT) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      free(output.data);
      archbird_engine_destroy(engine);
      return NULL;
    }
  }
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value discovery_descend(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  const uint8_t *config;
  size_t config_length;
  bool is_array = false;
  uint32_t path_count = 0;
  uint32_t ignore_count = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdDiscovery *discovery = NULL;
  ArchbirdStatus status;
  napi_value result;
  uint32_t index;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if ((argc != 2 && argc != 4) ||
      !get_buffer(env, argv[0], &config, &config_length))
    return NULL;
  if (napi_is_array(env, argv[1], &is_array) != napi_ok || !is_array ||
      napi_get_array_length(env, argv[1], &path_count) != napi_ok ||
      napi_create_array_with_length(env, path_count, &result) != napi_ok) {
    napi_throw_type_error(env, "ARCHBIRD_NAPI",
                          "paths must be an array of strings");
    return NULL;
  }
  if (argc == 4) {
    bool paths_are_array = false;
    bool contents_are_array = false;
    uint32_t content_count = 0;
    if (napi_is_array(env, argv[2], &paths_are_array) != napi_ok ||
        napi_is_array(env, argv[3], &contents_are_array) != napi_ok ||
        !paths_are_array || !contents_are_array ||
        napi_get_array_length(env, argv[2], &ignore_count) != napi_ok ||
        napi_get_array_length(env, argv[3], &content_count) != napi_ok ||
        ignore_count != content_count) {
      napi_throw_type_error(
          env, "ARCHBIRD_NAPI",
          "ignore paths and contents must be equal-length arrays");
      return NULL;
    }
  }
  status = input_engine(config_length, &engine);
  if (status == ARCHBIRD_OK)
    status =
        archbird_discovery_create(engine, config, config_length, &discovery);
  for (index = 0; status == ARCHBIRD_OK && index < ignore_count; index++) {
    napi_value path_item;
    napi_value content_item;
    napi_valuetype type;
    size_t path_length = 0;
    char *ignore_path = NULL;
    const uint8_t *ignore_bytes;
    size_t ignore_length;
    if (napi_get_element(env, argv[2], index, &path_item) != napi_ok ||
        napi_get_element(env, argv[3], index, &content_item) != napi_ok ||
        napi_typeof(env, path_item, &type) != napi_ok || type != napi_string ||
        napi_get_value_string_utf8(env, path_item, NULL, 0, &path_length) !=
            napi_ok ||
        !get_buffer(env, content_item, &ignore_bytes, &ignore_length)) {
      napi_throw_type_error(env, "ARCHBIRD_NAPI",
                            "ignore inputs must be string paths and buffers");
      status = ARCHBIRD_INVALID_ARGUMENT;
      break;
    }
    ignore_path = (char *)malloc(path_length + 1);
    if (!ignore_path) {
      status = ARCHBIRD_OUT_OF_MEMORY;
      break;
    }
    if (napi_get_value_string_utf8(env, path_item, ignore_path, path_length + 1,
                                   &path_length) != napi_ok) {
      free(ignore_path);
      napi_throw_type_error(env, "ARCHBIRD_NAPI",
                            "could not decode an ignore path");
      status = ARCHBIRD_INVALID_ARGUMENT;
      break;
    }
    status =
        archbird_discovery_add_ignore(engine, discovery, ignore_path,
                                      path_length, ignore_bytes, ignore_length);
    free(ignore_path);
  }
  for (index = 0; status == ARCHBIRD_OK && index < path_count; index++) {
    napi_value item;
    napi_value decision;
    napi_valuetype type;
    size_t path_length = 0;
    char *path = NULL;
    int should_descend;
    if (napi_get_element(env, argv[1], index, &item) != napi_ok ||
        napi_typeof(env, item, &type) != napi_ok || type != napi_string ||
        napi_get_value_string_utf8(env, item, NULL, 0, &path_length) !=
            napi_ok) {
      napi_throw_type_error(env, "ARCHBIRD_NAPI",
                            "discovery paths must be strings");
      status = ARCHBIRD_INVALID_ARGUMENT;
      break;
    }
    path = (char *)malloc(path_length + 1);
    if (!path) {
      status = ARCHBIRD_OUT_OF_MEMORY;
      break;
    }
    if (napi_get_value_string_utf8(env, item, path, path_length + 1,
                                   &path_length) != napi_ok) {
      free(path);
      napi_throw_type_error(env, "ARCHBIRD_NAPI",
                            "could not decode a discovery path");
      status = ARCHBIRD_INVALID_ARGUMENT;
      break;
    }
    status = archbird_discovery_should_descend(engine, discovery, path,
                                               path_length, &should_descend);
    free(path);
    if (status != ARCHBIRD_OK)
      break;
    if (napi_get_boolean(env, should_descend != 0, &decision) != napi_ok ||
        napi_set_element(env, result, index, decision) != napi_ok) {
      napi_throw_error(env, "ARCHBIRD_NAPI",
                       "could not construct discovery decisions");
      status = ARCHBIRD_INVALID_ARGUMENT;
      break;
    }
  }
  archbird_discovery_destroy(discovery);
  if (status != ARCHBIRD_OK) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      archbird_engine_destroy(engine);
      return NULL;
    }
    result = throw_status(env, engine, status);
  }
  archbird_engine_destroy(engine);
  return result;
}

static napi_value discovery_resolve(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  const uint8_t *config;
  const uint8_t *request;
  const uint8_t *inventory;
  size_t config_length;
  size_t request_length;
  size_t inventory_length;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 3 || !get_buffer(env, argv[0], &config, &config_length) ||
      !get_buffer(env, argv[1], &request, &request_length) ||
      !get_buffer(env, argv[2], &inventory, &inventory_length) ||
      !get_optional_bool(env, argc, argv, 3, 0, &pretty))
    return NULL;
  status =
      input_engine(larger_input(larger_input(config_length, request_length),
                                inventory_length),
                   &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_discovery_resolve(
        engine, config, config_length, request, request_length, inventory,
        inventory_length, pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write,
        &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value map_query(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  const uint8_t *map;
  const uint8_t *resolution;
  const uint8_t *query;
  size_t map_length;
  size_t resolution_length;
  size_t query_length;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 3 || !get_buffer(env, argv[0], &map, &map_length) ||
      !get_buffer(env, argv[1], &resolution, &resolution_length) ||
      !get_buffer(env, argv[2], &query, &query_length) ||
      !get_optional_bool(env, argc, argv, 3, 0, &pretty))
    return NULL;
  status = saved_artifact_engine(
      larger_input(larger_input(map_length, query_length), resolution_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_query(
        engine, map, map_length, resolution_length ? resolution : NULL,
        resolution_length, query, query_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value map_markdown(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  const uint8_t *map;
  size_t map_length;
  size_t max_chars;
  int full;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 1 || !get_buffer(env, argv[0], &map, &map_length) ||
      !get_optional_bool(env, argc, argv, 1, 0, &full) ||
      !get_optional_size(env, argc, argv, 2, 0, "maxChars", &max_chars))
    return NULL;
  status = saved_artifact_engine(map_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_render_markdown(engine, map, map_length, full,
                                          max_chars, output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value map_markdown_view(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  const uint8_t *map;
  size_t map_length;
  size_t view;
  size_t detail;
  size_t max_chars;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 3 || !get_buffer(env, argv[0], &map, &map_length) ||
      !get_optional_size(env, argc, argv, 1, 0, "view", &view) ||
      !get_optional_size(env, argc, argv, 2, 1, "detail", &detail) ||
      !get_optional_size(env, argc, argv, 3, 0, "maxChars", &max_chars))
    return NULL;
  status = saved_artifact_engine(map_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_render_markdown_view(
        engine, map, map_length, (ArchbirdMapView)view,
        (ArchbirdReportDetail)detail, max_chars, output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value map_query_markdown(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  const uint8_t *map;
  const uint8_t *resolution;
  const uint8_t *query;
  size_t map_length;
  size_t resolution_length;
  size_t query_length;
  size_t max_chars;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 3 || !get_buffer(env, argv[0], &map, &map_length) ||
      !get_buffer(env, argv[1], &resolution, &resolution_length) ||
      !get_buffer(env, argv[2], &query, &query_length) ||
      !get_optional_size(env, argc, argv, 3, 0, "maxChars", &max_chars))
    return NULL;
  status = saved_artifact_engine(
      larger_input(larger_input(map_length, query_length), resolution_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_query_markdown(engine, map, map_length,
                                         resolution_length ? resolution : NULL,
                                         resolution_length, query, query_length,
                                         max_chars, output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value map_query_markdown_view(napi_env env,
                                          napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7];
  const uint8_t *map;
  const uint8_t *resolution;
  const uint8_t *query;
  const uint8_t *verification = NULL;
  size_t map_length;
  size_t resolution_length;
  size_t query_length;
  size_t verification_length = 0;
  size_t view;
  size_t detail;
  size_t max_chars;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 5 || !get_buffer(env, argv[0], &map, &map_length) ||
      !get_buffer(env, argv[1], &resolution, &resolution_length) ||
      !get_buffer(env, argv[2], &query, &query_length) ||
      !get_optional_size(env, argc, argv, 3, 0, "view", &view) ||
      !get_optional_size(env, argc, argv, 4, 1, "detail", &detail) ||
      !get_optional_size(env, argc, argv, 5, 0, "maxChars", &max_chars) ||
      (argc > 6 &&
       !get_buffer(env, argv[6], &verification, &verification_length)))
    return NULL;
  status = saved_artifact_engine(
      larger_input(larger_input(larger_input(map_length, query_length),
                                verification_length),
                   resolution_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_query_markdown_view_with_verification(
        engine, map, map_length, resolution_length ? resolution : NULL,
        resolution_length, query, query_length, verification,
        verification_length, (ArchbirdQueryView)view,
        (ArchbirdReportDetail)detail, max_chars, output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value project_source_markdown(napi_env env,
                                          napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  NodeProject *owned;
  const uint8_t *artifact;
  size_t artifact_length;
  size_t detail;
  size_t max_chars;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 3 || !(owned = get_project(env, argv[0])) ||
      !get_buffer(env, argv[1], &artifact, &artifact_length) ||
      !get_optional_size(env, argc, argv, 2, 1, "detail", &detail) ||
      !get_optional_size(env, argc, argv, 3, 0, "maxChars", &max_chars))
    return NULL;
  status = saved_artifact_engine(artifact_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_project_render_source_markdown(
        engine, owned->project, artifact, artifact_length,
        (ArchbirdReportDetail)detail, max_chars, output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value map_diff(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  const uint8_t *before;
  const uint8_t *after;
  size_t before_length;
  size_t after_length;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 2 || !get_buffer(env, argv[0], &before, &before_length) ||
      !get_buffer(env, argv[1], &after, &after_length) ||
      !get_optional_bool(env, argc, argv, 2, 0, &pretty))
    return NULL;
  status =
      saved_artifact_engine(larger_input(before_length, after_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_diff(engine, before, before_length, after,
                               after_length, pretty ? ARCHBIRD_JSON_PRETTY : 0,
                               output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value unified_diff(napi_env env, napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7];
  const uint8_t *before;
  const uint8_t *after;
  const uint8_t *metadata = NULL;
  size_t before_length;
  size_t after_length;
  size_t metadata_length = 0;
  size_t before_path_length;
  size_t after_path_length;
  size_t context_lines;
  size_t max_work_bytes;
  char *before_path;
  char *after_path;
  int before_null;
  int after_null;
  ArchbirdUnifiedDiffOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 4 || !get_buffer(env, argv[0], &before, &before_length) ||
      !get_buffer(env, argv[1], &after, &after_length))
    return NULL;
  before_path =
      get_nullable_string(env, argv[2], &before_path_length, &before_null);
  if (!before_path && !before_null)
    return NULL;
  after_path =
      get_nullable_string(env, argv[3], &after_path_length, &after_null);
  if (!after_path && !after_null) {
    free(before_path);
    return NULL;
  }
  if (argc > 4 && !get_buffer(env, argv[4], &metadata, &metadata_length)) {
    free(after_path);
    free(before_path);
    return NULL;
  }
  if (!get_optional_size(env, argc, argv, 5, 3, "contextLines",
                         &context_lines) ||
      !get_optional_size(env, argc, argv, 6, 16 * 1024 * 1024, "maxWorkBytes",
                         &max_work_bytes)) {
    free(after_path);
    free(before_path);
    return NULL;
  }
  if (max_work_bytes == 0) {
    napi_throw_range_error(env, "ARCHBIRD_NUMBER",
                           "maxWorkBytes must be positive");
    free(after_path);
    free(before_path);
    return NULL;
  }
  archbird_unified_diff_options_init(&options);
  options.context_lines = context_lines;
  options.max_work_bytes = max_work_bytes;
  options.metadata = metadata;
  options.metadata_length = metadata_length;
  status = input_engine(larger_input(before_length, after_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_unified_diff(
        engine, before, before_length, after, after_length, before_path,
        before_path_length, after_path, after_path_length, &options,
        output_write, &output);
  free(after_path);
  free(before_path);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value json_pointer_edit(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6];
  const uint8_t *source;
  const uint8_t *expected;
  const uint8_t *replacement;
  size_t source_length;
  size_t expected_length;
  size_t replacement_length;
  size_t source_sha256_length;
  size_t pointer_length;
  char *source_sha256;
  char *pointer;
  int expected_absent;
  ArchbirdJsonPointerEditOptions options;
  ArchbirdJsonPointerEditResult edit_result;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value replacement_buffer;
  napi_value result;
  napi_value value;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc != 6 || !get_buffer(env, argv[0], &source, &source_length))
    return NULL;
  source_sha256 = get_string(env, argv[1], &source_sha256_length);
  if (!source_sha256)
    return NULL;
  pointer = get_string(env, argv[2], &pointer_length);
  if (!pointer) {
    free(source_sha256);
    return NULL;
  }
  if (!get_buffer(env, argv[3], &expected, &expected_length) ||
      !get_buffer(env, argv[4], &replacement, &replacement_length) ||
      !get_optional_bool(env, argc, argv, 5, 0, &expected_absent)) {
    free(pointer);
    free(source_sha256);
    return NULL;
  }
  archbird_json_pointer_edit_options_init(&options);
  options.source_sha256 = source_sha256;
  options.source_sha256_length = source_sha256_length;
  options.pointer = (const uint8_t *)pointer;
  options.pointer_length = pointer_length;
  options.expected_absent = expected_absent;
  options.expected_json = expected_absent ? NULL : expected;
  options.expected_json_length = expected_absent ? 0 : expected_length;
  options.replacement_json = replacement;
  options.replacement_json_length = replacement_length;
  archbird_json_pointer_edit_result_init(&edit_result);
  status = input_engine(source_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_json_pointer_edit(engine, source, source_length, &options,
                                        &edit_result, output_write, &output);
  free(pointer);
  free(source_sha256);
  replacement_buffer = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  if (!replacement_buffer)
    return NULL;
  NAPI_TRY(napi_create_object(env, &result));
  NAPI_TRY(napi_create_double(env, (double)edit_result.start_byte, &value));
  NAPI_TRY(napi_set_named_property(env, result, "startByte", value));
  NAPI_TRY(napi_create_double(env, (double)edit_result.end_byte, &value));
  NAPI_TRY(napi_set_named_property(env, result, "endByte", value));
  NAPI_TRY(napi_create_double(env, (double)edit_result.matched_values, &value));
  NAPI_TRY(napi_set_named_property(env, result, "matchedValues", value));
  NAPI_TRY(napi_create_string_utf8(
      env,
      edit_result.kind == ARCHBIRD_JSON_POINTER_INSERT ? "insert" : "replace",
      NAPI_AUTO_LENGTH, &value));
  NAPI_TRY(napi_set_named_property(env, result, "kind", value));
  NAPI_TRY(
      napi_set_named_property(env, result, "replacement", replacement_buffer));
  return result;
}

static napi_value make_variable_token_edit(napi_env env,
                                           napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5];
  const uint8_t *source;
  size_t source_length;
  size_t source_sha256_length;
  size_t variable_length;
  size_t expected_token_length;
  size_t replacement_token_length;
  char *source_sha256;
  char *variable;
  char *expected_token;
  char *replacement_token;
  ArchbirdMakeVariableTokenEditOptions options;
  ArchbirdMakeVariableTokenEditResult edit_result;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value replacement_buffer;
  napi_value result;
  napi_value value;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc != 5 || !get_buffer(env, argv[0], &source, &source_length))
    return NULL;
  source_sha256 = get_string(env, argv[1], &source_sha256_length);
  variable = get_string(env, argv[2], &variable_length);
  expected_token = get_string(env, argv[3], &expected_token_length);
  replacement_token = get_string(env, argv[4], &replacement_token_length);
  if (!source_sha256 || !variable || !expected_token || !replacement_token) {
    free(replacement_token);
    free(expected_token);
    free(variable);
    free(source_sha256);
    return NULL;
  }
  archbird_make_variable_token_edit_options_init(&options);
  options.source_sha256 = source_sha256;
  options.source_sha256_length = source_sha256_length;
  options.variable = (const uint8_t *)variable;
  options.variable_length = variable_length;
  options.expected_token = (const uint8_t *)expected_token;
  options.expected_token_length = expected_token_length;
  options.replacement_token = (const uint8_t *)replacement_token;
  options.replacement_token_length = replacement_token_length;
  archbird_make_variable_token_edit_result_init(&edit_result);
  status = input_engine(source_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_make_variable_token_edit(engine, source, source_length,
                                               &options, &edit_result,
                                               output_write, &output);
  free(replacement_token);
  free(expected_token);
  free(variable);
  free(source_sha256);
  replacement_buffer = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  if (!replacement_buffer)
    return NULL;
  NAPI_TRY(napi_create_object(env, &result));
  NAPI_TRY(napi_create_double(env, (double)edit_result.start_byte, &value));
  NAPI_TRY(napi_set_named_property(env, result, "startByte", value));
  NAPI_TRY(napi_create_double(env, (double)edit_result.end_byte, &value));
  NAPI_TRY(napi_set_named_property(env, result, "endByte", value));
  NAPI_TRY(napi_create_double(env, (double)edit_result.matched_tokens, &value));
  NAPI_TRY(napi_set_named_property(env, result, "matchedTokens", value));
  NAPI_TRY(
      napi_set_named_property(env, result, "replacement", replacement_buffer));
  return result;
}

static napi_value make_variable_token_insert(napi_env env,
                                             napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6];
  const uint8_t *source;
  size_t source_length;
  size_t lengths[5];
  char *values[5] = {NULL, NULL, NULL, NULL, NULL};
  ArchbirdMakeVariableTokenInsertOptions options;
  ArchbirdMakeVariableTokenInsertResult insert_result;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value replacement_buffer;
  napi_value result;
  napi_value value;
  size_t index;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc != 6 || !get_buffer(env, argv[0], &source, &source_length))
    return NULL;
  for (index = 0; index < 5; index++) {
    values[index] = get_string(env, argv[index + 1], &lengths[index]);
    if (!values[index]) {
      while (index)
        free(values[--index]);
      return NULL;
    }
  }
  archbird_make_variable_token_insert_options_init(&options);
  options.source_sha256 = values[0];
  options.source_sha256_length = lengths[0];
  options.variable = (const uint8_t *)values[1];
  options.variable_length = lengths[1];
  options.token = (const uint8_t *)values[2];
  options.token_length = lengths[2];
  options.anchor_token = (const uint8_t *)values[3];
  options.anchor_token_length = lengths[3];
  if (lengths[4] == 6 && memcmp(values[4], "before", 6) == 0)
    options.position = ARCHBIRD_MAKE_TOKEN_BEFORE;
  else if (lengths[4] == 5 && memcmp(values[4], "after", 5) == 0)
    options.position = ARCHBIRD_MAKE_TOKEN_AFTER;
  else {
    for (index = 0; index < 5; index++)
      free(values[index]);
    napi_throw_range_error(env, "ARCHBIRD_POSITION",
                           "position must be before or after");
    return NULL;
  }
  archbird_make_variable_token_insert_result_init(&insert_result);
  status = input_engine(source_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_make_variable_token_insert(engine, source, source_length,
                                                 &options, &insert_result,
                                                 output_write, &output);
  for (index = 0; index < 5; index++)
    free(values[index]);
  replacement_buffer = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  if (!replacement_buffer)
    return NULL;
  NAPI_TRY(napi_create_object(env, &result));
  NAPI_TRY(napi_create_double(env, (double)insert_result.start_byte, &value));
  NAPI_TRY(napi_set_named_property(env, result, "startByte", value));
  NAPI_TRY(napi_create_double(env, (double)insert_result.end_byte, &value));
  NAPI_TRY(napi_set_named_property(env, result, "endByte", value));
  NAPI_TRY(
      napi_create_double(env, (double)insert_result.matched_tokens, &value));
  NAPI_TRY(napi_set_named_property(env, result, "matchedTokens", value));
  NAPI_TRY(
      napi_create_double(env, (double)insert_result.matched_anchors, &value));
  NAPI_TRY(napi_set_named_property(env, result, "matchedAnchors", value));
  NAPI_TRY(
      napi_set_named_property(env, result, "replacement", replacement_buffer));
  return result;
}

static napi_value map_freshness(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  const uint8_t *snapshot;
  const uint8_t *current;
  size_t snapshot_length;
  size_t current_length;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 2 || !get_buffer(env, argv[0], &snapshot, &snapshot_length) ||
      !get_buffer(env, argv[1], &current, &current_length) ||
      !get_optional_bool(env, argc, argv, 2, 0, &pretty))
    return NULL;
  status = saved_artifact_engine(larger_input(snapshot_length, current_length),
                                 &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_freshness(
        engine, snapshot, snapshot_length, current, current_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value map_export_graph(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6];
  const uint8_t *map;
  size_t map_length;
  size_t format_length = 0;
  size_t view_length = 0;
  size_t direction_length = 0;
  char *format = NULL;
  char *view = NULL;
  char *direction = NULL;
  uint32_t max_nodes = 200;
  uint32_t max_edge_names = 3;
  ArchbirdGraphOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 3 || !get_buffer(env, argv[0], &map, &map_length))
    return NULL;
  format = get_string(env, argv[1], &format_length);
  view = get_string(env, argv[2], &view_length);
  if (!format || !view)
    goto invalid;
  if (argc >= 4) {
    direction = get_string(env, argv[3], &direction_length);
    if (!direction)
      goto invalid;
  }
  if (argc >= 5 && napi_get_value_uint32(env, argv[4], &max_nodes) != napi_ok) {
    napi_throw_type_error(env, "ARCHBIRD_NUMBER",
                          "maxNodes must be a uint32 integer");
    goto invalid;
  }
  if (argc >= 6 &&
      napi_get_value_uint32(env, argv[5], &max_edge_names) != napi_ok) {
    napi_throw_type_error(env, "ARCHBIRD_NUMBER",
                          "maxEdgeNames must be a uint32 integer");
    goto invalid;
  }
  archbird_graph_options_init(&options);
  if (!strcmp(format, "graphml"))
    options.format = ARCHBIRD_GRAPH_GRAPHML;
  else if (!strcmp(format, "mermaid"))
    options.format = ARCHBIRD_GRAPH_MERMAID;
  else if (!strcmp(format, "json"))
    options.format = ARCHBIRD_GRAPH_JSON;
  else {
    napi_throw_range_error(env, "ARCHBIRD_GRAPH",
                           "graph format must be graphml, json, or mermaid");
    goto invalid;
  }
  if (!strcmp(view, "components"))
    options.view = ARCHBIRD_GRAPH_COMPONENTS;
  else if (!strcmp(view, "files"))
    options.view = ARCHBIRD_GRAPH_FILES;
  else if (!strcmp(view, "symbols"))
    options.view = ARCHBIRD_GRAPH_SYMBOLS;
  else {
    napi_throw_range_error(env, "ARCHBIRD_GRAPH",
                           "graph view must be components, files, or symbols");
    goto invalid;
  }
  if (!direction || !strcmp(direction, "LR"))
    options.direction = ARCHBIRD_GRAPH_LR;
  else if (!strcmp(direction, "RL"))
    options.direction = ARCHBIRD_GRAPH_RL;
  else if (!strcmp(direction, "TB"))
    options.direction = ARCHBIRD_GRAPH_TB;
  else if (!strcmp(direction, "BT"))
    options.direction = ARCHBIRD_GRAPH_BT;
  else {
    napi_throw_range_error(env, "ARCHBIRD_GRAPH",
                           "graph direction must be BT, LR, RL, or TB");
    goto invalid;
  }
  options.max_nodes = max_nodes;
  options.max_edge_names = max_edge_names;
  status = saved_artifact_engine(map_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_export_graph(engine, map, map_length, &options,
                                       output_write, &output);
  free(direction);
  free(view);
  free(format);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;

invalid:
  free(direction);
  free(view);
  free(format);
  return NULL;
}

static napi_value okf_analyze(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5];
  const uint8_t *source;
  const uint8_t *query;
  size_t source_length;
  size_t query_length;
  size_t format_length = 0;
  char *format = NULL;
  int include_body;
  int pretty;
  ArchbirdOkfFormat native_format;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 3 || !get_buffer(env, argv[0], &source, &source_length) ||
      !get_buffer(env, argv[1], &query, &query_length))
    return NULL;
  format = get_string(env, argv[2], &format_length);
  if (!format || !get_optional_bool(env, argc, argv, 3, 0, &include_body) ||
      !get_optional_bool(env, argc, argv, 4, 1, &pretty)) {
    free(format);
    return NULL;
  }
  if (!strcmp(format, "json"))
    native_format = ARCHBIRD_OKF_JSON;
  else if (!strcmp(format, "markdown"))
    native_format = ARCHBIRD_OKF_MARKDOWN;
  else {
    free(format);
    napi_throw_range_error(env, "ARCHBIRD_OKF",
                           "OKF format must be json or markdown");
    return NULL;
  }
  free(format);
  status = input_engine(larger_input(source_length, query_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_okf_analyze(
        engine, source, source_length, query_length ? query : NULL,
        query_length, native_format, include_body,
        (pretty ? ARCHBIRD_JSON_PRETTY : 0) | ARCHBIRD_JSON_TRAILING_NEWLINE,
        output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value okf_publish(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  const uint8_t *map;
  const uint8_t *verification;
  const uint8_t *normalization;
  size_t map_length;
  size_t verification_length;
  size_t normalization_length;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 3 || !get_buffer(env, argv[0], &map, &map_length) ||
      !get_buffer(env, argv[1], &verification, &verification_length) ||
      !get_buffer(env, argv[2], &normalization, &normalization_length) ||
      !get_optional_bool(env, argc, argv, 3, 0, &pretty))
    return NULL;
  status = saved_artifact_engine(
      larger_input(larger_input(map_length, verification_length),
                   normalization_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_okf_publish(
        engine, map, map_length, verification_length ? verification : NULL,
        verification_length, normalization_length ? normalization : NULL,
        normalization_length,
        (pretty ? ARCHBIRD_JSON_PRETTY : 0) | ARCHBIRD_JSON_TRAILING_NEWLINE,
        output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value workspace_plan(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  const uint8_t *config;
  size_t config_length;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 1 || !get_buffer(env, argv[0], &config, &config_length) ||
      !get_optional_bool(env, argc, argv, 1, 0, &pretty))
    return NULL;
  status = input_engine(config_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_workspace_plan(engine, config, config_length,
                                     pretty ? ARCHBIRD_JSON_PRETTY : 0,
                                     output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value workspace_analyze(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  const uint8_t *config;
  const uint8_t *maps;
  size_t config_length;
  size_t maps_length;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 2 || !get_buffer(env, argv[0], &config, &config_length) ||
      !get_buffer(env, argv[1], &maps, &maps_length) ||
      !get_optional_bool(env, argc, argv, 2, 0, &pretty))
    return NULL;
  status =
      saved_artifact_engine(larger_input(config_length, maps_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_workspace_analyze(
        engine, config, config_length, maps, maps_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value project_configuration_compile(napi_env env,
                                                napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  const uint8_t *config;
  size_t config_length;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 1 || !get_buffer(env, argv[0], &config, &config_length) ||
      !get_optional_bool(env, argc, argv, 1, 0, &pretty))
    return NULL;
  status = input_engine(config_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_project_configuration_compile(
        engine, config, config_length, pretty ? ARCHBIRD_JSON_PRETTY : 0,
        output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value projection_evaluate(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  const uint8_t *map;
  const uint8_t *resolution;
  const uint8_t *projection;
  size_t map_length;
  size_t resolution_length;
  size_t projection_length;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 3 || !get_buffer(env, argv[0], &map, &map_length) ||
      !get_buffer(env, argv[1], &resolution, &resolution_length) ||
      !get_buffer(env, argv[2], &projection, &projection_length) ||
      !get_optional_bool(env, argc, argv, 3, 0, &pretty))
    return NULL;
  status = saved_artifact_engine(
      larger_input(larger_input(map_length, resolution_length),
                   projection_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_projection_evaluate(
        engine, map, map_length, resolution_length ? resolution : NULL,
        resolution_length, projection, projection_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value projection_render_markdown(napi_env env,
                                             napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5];
  const uint8_t *map;
  const uint8_t *resolution;
  const uint8_t *projection;
  size_t map_length;
  size_t resolution_length;
  size_t projection_length;
  int32_t detail;
  size_t max_chars;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 5 || !get_buffer(env, argv[0], &map, &map_length) ||
      !get_buffer(env, argv[1], &resolution, &resolution_length) ||
      !get_buffer(env, argv[2], &projection, &projection_length) ||
      napi_get_value_int32(env, argv[3], &detail) != napi_ok ||
      !get_optional_size(env, argc, argv, 4, 0, "maxChars", &max_chars))
    return NULL;
  status = saved_artifact_engine(
      larger_input(larger_input(map_length, resolution_length),
                   projection_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_projection_render_markdown(
        engine, map, map_length, resolution_length ? resolution : NULL,
        resolution_length, projection, projection_length,
        (ArchbirdReportDetail)detail, max_chars, output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value query_plan_compile(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  const uint8_t *config;
  const uint8_t *overrides;
  size_t config_length;
  size_t overrides_length;
  char *query_id = NULL;
  size_t query_id_length = 0;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 3 || !get_buffer(env, argv[0], &config, &config_length) ||
      !get_buffer(env, argv[2], &overrides, &overrides_length))
    return NULL;
  query_id = get_string(env, argv[1], &query_id_length);
  if (!query_id || !get_optional_bool(env, argc, argv, 3, 0, &pretty)) {
    free(query_id);
    return NULL;
  }
  status = input_engine(larger_input(config_length, overrides_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_query_plan_compile(
        engine, config, config_length, query_id, query_id_length,
        overrides_length ? overrides : NULL, overrides_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  free(query_id);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value constraints_evaluate(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5];
  const uint8_t *config;
  const uint8_t *map;
  const uint8_t *resolution;
  const uint8_t *request;
  size_t config_length;
  size_t map_length;
  size_t resolution_length;
  size_t request_length;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 4 || !get_buffer(env, argv[0], &config, &config_length) ||
      !get_buffer(env, argv[1], &map, &map_length) ||
      !get_buffer(env, argv[2], &resolution, &resolution_length) ||
      !get_buffer(env, argv[3], &request, &request_length) ||
      !get_optional_bool(env, argc, argv, 4, 0, &pretty))
    return NULL;
  status = saved_artifact_engine(
      larger_input(larger_input(config_length, map_length),
                   larger_input(resolution_length, request_length)),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_constraints_evaluate(
        engine, config, config_length, map, map_length,
        resolution_length ? resolution : NULL, resolution_length,
        request_length ? request : NULL, request_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static int node_constraint_format(const char *format, size_t length,
                                  ArchbirdVerificationFormat *out) {
  if (length == 8 && !memcmp(format, "markdown", 8))
    *out = ARCHBIRD_VERIFICATION_MARKDOWN;
  else if (length == 5 && !memcmp(format, "sarif", 5))
    *out = ARCHBIRD_VERIFICATION_SARIF;
  else if (length == 5 && !memcmp(format, "junit", 5))
    *out = ARCHBIRD_VERIFICATION_JUNIT;
  else
    return 0;
  return 1;
}

static napi_value constraints_report_common(napi_env env,
                                            napi_callback_info info,
                                            int include_blocking) {
  size_t argc = 7;
  napi_value argv[7];
  const uint8_t *config;
  const uint8_t *map;
  const uint8_t *resolution;
  const uint8_t *request;
  size_t config_length;
  size_t map_length;
  size_t resolution_length;
  size_t request_length;
  char *format = NULL;
  size_t format_length = 0;
  uint32_t max_findings = 200;
  int pretty;
  int blocking = 0;
  ArchbirdVerificationFormat native_format;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  napi_value combined;
  napi_value blocking_result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 5 || !get_buffer(env, argv[0], &config, &config_length) ||
      !get_buffer(env, argv[1], &map, &map_length) ||
      !get_buffer(env, argv[2], &resolution, &resolution_length) ||
      !get_buffer(env, argv[3], &request, &request_length))
    return NULL;
  format = get_string(env, argv[4], &format_length);
  if (!format ||
      !node_constraint_format(format, format_length, &native_format)) {
    free(format);
    napi_throw_range_error(env, "ARCHBIRD_FORMAT",
                           "constraint report format must be markdown, sarif, "
                           "or junit");
    return NULL;
  }
  free(format);
  if (argc >= 6 &&
      napi_get_value_uint32(env, argv[5], &max_findings) != napi_ok)
    return NULL;
  if (!get_optional_bool(env, argc, argv, 6, 0, &pretty))
    return NULL;
  status = saved_artifact_engine(
      larger_input(larger_input(config_length, map_length),
                   larger_input(resolution_length, request_length)),
      &engine);
  if (status == ARCHBIRD_OK) {
    uint32_t flags = (pretty ? ARCHBIRD_JSON_PRETTY : 0) |
                     (native_format == ARCHBIRD_VERIFICATION_SARIF
                          ? ARCHBIRD_JSON_TRAILING_NEWLINE
                          : 0);
    if (include_blocking)
      status = archbird_constraints_report_with_blocking(
          engine, config, config_length, map, map_length,
          resolution_length ? resolution : NULL, resolution_length,
          request_length ? request : NULL, request_length, native_format,
          max_findings, flags, &blocking, output_write, &output);
    else
      status = archbird_constraints_report(
          engine, config, config_length, map, map_length,
          resolution_length ? resolution : NULL, resolution_length,
          request_length ? request : NULL, request_length, native_format,
          max_findings, flags, output_write, &output);
  }
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  if (!include_blocking || !result)
    return result;
  NAPI_TRY(napi_create_object(env, &combined));
  NAPI_TRY(napi_set_named_property(env, combined, "report", result));
  NAPI_TRY(napi_get_boolean(env, blocking, &blocking_result));
  NAPI_TRY(napi_set_named_property(env, combined, "blocking", blocking_result));
  return combined;
}

static napi_value constraints_report(napi_env env, napi_callback_info info) {
  return constraints_report_common(env, info, 0);
}

static napi_value constraints_report_with_blocking(napi_env env,
                                                   napi_callback_info info) {
  return constraints_report_common(env, info, 1);
}

static napi_value constraints_freeze(napi_env env, napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7];
  const uint8_t *config;
  const uint8_t *map;
  const uint8_t *resolution;
  const uint8_t *request;
  size_t config_length;
  size_t map_length;
  size_t resolution_length;
  size_t request_length;
  char *owner = NULL;
  char *rationale = NULL;
  size_t owner_length = 0;
  size_t rationale_length = 0;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 6 || !get_buffer(env, argv[0], &config, &config_length) ||
      !get_buffer(env, argv[1], &map, &map_length) ||
      !get_buffer(env, argv[2], &resolution, &resolution_length) ||
      !get_buffer(env, argv[3], &request, &request_length))
    return NULL;
  owner = get_string(env, argv[4], &owner_length);
  rationale = get_string(env, argv[5], &rationale_length);
  if (!owner || !rationale ||
      !get_optional_bool(env, argc, argv, 6, 0, &pretty)) {
    free(owner);
    free(rationale);
    return NULL;
  }
  status = saved_artifact_engine(
      larger_input(larger_input(config_length, map_length),
                   larger_input(resolution_length, request_length)),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_constraints_freeze(
        engine, config, config_length, map, map_length,
        resolution_length ? resolution : NULL, resolution_length,
        request_length ? request : NULL, request_length, owner, owner_length,
        rationale, rationale_length,
        (pretty ? ARCHBIRD_JSON_PRETTY : 0) | ARCHBIRD_JSON_TRAILING_NEWLINE,
        output_write, &output);
  free(owner);
  free(rationale);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value init(napi_env env, napi_value exports) {
  static const napi_property_descriptor properties[] = {
      {"constraintsFreeze", NULL, constraints_freeze, NULL, NULL, NULL,
       napi_default, NULL},
      {"constraintsReport", NULL, constraints_report, NULL, NULL, NULL,
       napi_default, NULL},
      {"constraintsReportWithBlocking", NULL, constraints_report_with_blocking,
       NULL, NULL, NULL, napi_default, NULL},
      {"constraintsEvaluate", NULL, constraints_evaluate, NULL, NULL, NULL,
       napi_default, NULL},
      {"queryPlanCompile", NULL, query_plan_compile, NULL, NULL, NULL,
       napi_default, NULL},
      {"projectionEvaluate", NULL, projection_evaluate, NULL, NULL, NULL,
       napi_default, NULL},
      {"projectionRenderMarkdown", NULL, projection_render_markdown, NULL, NULL,
       NULL, napi_default, NULL},
      {"projectConfigurationCompile", NULL, project_configuration_compile, NULL,
       NULL, NULL, napi_default, NULL},
      {"workspaceAnalyze", NULL, workspace_analyze, NULL, NULL, NULL,
       napi_default, NULL},
      {"workspacePlan", NULL, workspace_plan, NULL, NULL, NULL, napi_default,
       NULL},
      {"mapDiff", NULL, map_diff, NULL, NULL, NULL, napi_default, NULL},
      {"unifiedDiff", NULL, unified_diff, NULL, NULL, NULL, napi_default, NULL},
      {"jsonPointerEdit", NULL, json_pointer_edit, NULL, NULL, NULL,
       napi_default, NULL},
      {"makeVariableTokenEdit", NULL, make_variable_token_edit, NULL, NULL,
       NULL, napi_default, NULL},
      {"makeVariableTokenInsert", NULL, make_variable_token_insert, NULL, NULL,
       NULL, napi_default, NULL},
      {"mapFreshness", NULL, map_freshness, NULL, NULL, NULL, napi_default,
       NULL},
      {"mapMarkdown", NULL, map_markdown, NULL, NULL, NULL, napi_default, NULL},
      {"mapMarkdownView", NULL, map_markdown_view, NULL, NULL, NULL,
       napi_default, NULL},
      {"mapExportGraph", NULL, map_export_graph, NULL, NULL, NULL, napi_default,
       NULL},
      {"okfAnalyze", NULL, okf_analyze, NULL, NULL, NULL, napi_default, NULL},
      {"okfPublish", NULL, okf_publish, NULL, NULL, NULL, napi_default, NULL},
      {"mapQuery", NULL, map_query, NULL, NULL, NULL, napi_default, NULL},
      {"mapQueryMarkdown", NULL, map_query_markdown, NULL, NULL, NULL,
       napi_default, NULL},
      {"mapQueryMarkdownView", NULL, map_query_markdown_view, NULL, NULL, NULL,
       napi_default, NULL},
      {"projectSourceMarkdown", NULL, project_source_markdown, NULL, NULL, NULL,
       napi_default, NULL},
      {"discoveryDescend", NULL, discovery_descend, NULL, NULL, NULL,
       napi_default, NULL},
      {"discoveryPlan", NULL, discovery_plan, NULL, NULL, NULL, napi_default,
       NULL},
      {"discoveryResolve", NULL, discovery_resolve, NULL, NULL, NULL,
       napi_default, NULL},
      {"projectCreate", NULL, project_create, NULL, NULL, NULL, napi_default,
       NULL},
      {"projectDestroy", NULL, project_dispose, NULL, NULL, NULL, napi_default,
       NULL},
      {"projectAddSource", NULL, project_add_source, NULL, NULL, NULL,
       napi_default, NULL},
      {"projectFinalizeSources", NULL, project_finalize_sources, NULL, NULL,
       NULL, napi_default, NULL},
      {"projectSetConfig", NULL, project_set_config, NULL, NULL, NULL,
       napi_default, NULL},
      {"projectConfigSha256", NULL, project_config_sha256, NULL, NULL, NULL,
       napi_default, NULL},
      {"projectAddProvider", NULL, project_add_provider, NULL, NULL, NULL,
       napi_default, NULL},
      {"projectAddTestSymbolObservations", NULL,
       project_add_test_symbol_observations, NULL, NULL, NULL, napi_default,
       NULL},
      {"projectScanBuiltin", NULL, project_scan_builtin, NULL, NULL, NULL,
       napi_default, NULL},
      {"projectScanBuiltinProvider", NULL, project_scan_builtin_provider, NULL,
       NULL, NULL, napi_default, NULL},
      {"projectScanBuiltinProviderFile", NULL,
       project_scan_builtin_provider_file, NULL, NULL, NULL, napi_default,
       NULL},
      {"projectFinalizeProviders", NULL, project_finalize_providers, NULL, NULL,
       NULL, napi_default, NULL},
      {"projectManifestSha256", NULL, project_manifest_sha256, NULL, NULL, NULL,
       napi_default, NULL},
      {"projectMapInputSha256", NULL, project_map_input_sha256, NULL, NULL,
       NULL, napi_default, NULL},
      {"projectCounts", NULL, project_counts, NULL, NULL, NULL, napi_default,
       NULL},
      {"projectMergeSummary", NULL, project_merge_summary, NULL, NULL, NULL,
       napi_default, NULL},
      {"projectFileFacts", NULL, project_file_facts, NULL, NULL, NULL,
       napi_default, NULL},
      {"projectMergeLedger", NULL, project_merge_ledger, NULL, NULL, NULL,
       napi_default, NULL},
      {"projectMergeConflicts", NULL, project_merge_conflicts, NULL, NULL, NULL,
       napi_default, NULL},
      {"projectMap", NULL, project_map, NULL, NULL, NULL, napi_default, NULL},
      {"projectProviderFacts", NULL, project_provider_facts, NULL, NULL, NULL,
       napi_default, NULL},
      {"jsonCanonicalize", NULL, json_canonicalize, NULL, NULL, NULL,
       napi_default, NULL},
      {"testSymbolObservationsValidate", NULL,
       test_symbol_observations_validate, NULL, NULL, NULL, napi_default, NULL},
      {"planValidate", NULL, plan_validate, NULL, NULL, NULL, napi_default,
       NULL},
      {"planRenderMarkdown", NULL, plan_render_markdown, NULL, NULL, NULL,
       napi_default, NULL},
      {"planCompile", NULL, plan_compile, NULL, NULL, NULL, napi_default, NULL},
      {"actValidate", NULL, act_validate, NULL, NULL, NULL, napi_default, NULL},
      {"planSourceRequirements", NULL, plan_source_requirements, NULL, NULL,
       NULL, napi_default, NULL},
      {"actSourceRequirements", NULL, act_source_requirements, NULL, NULL, NULL,
       napi_default, NULL},
      {"actMaterialize", NULL, act_materialize, NULL, NULL, NULL, napi_default,
       NULL},
      {"actAccept", NULL, act_accept, NULL, NULL, NULL, napi_default, NULL},
      {"actPreflightApply", NULL, act_preflight_apply, NULL, NULL, NULL,
       napi_default, NULL},
  };
  napi_value abi;
  napi_value implementation_sha256;
  napi_value pattern_contract_version;
  napi_value pattern_contract;
  napi_value pattern_engine;
  napi_value pattern_unicode;
  napi_value pattern_options;
  napi_value version;
  NAPI_TRY(napi_define_properties(
      env, exports, sizeof(properties) / sizeof(properties[0]), properties));
  NAPI_TRY(napi_create_uint32(env, ARCHBIRD_NATIVE_ABI_VERSION, &abi));
  NAPI_TRY(napi_set_named_property(env, exports, "NATIVE_ABI_VERSION", abi));
  NAPI_TRY(napi_create_string_utf8(env, archbird_implementation_sha256(),
                                   NAPI_AUTO_LENGTH, &implementation_sha256));
  NAPI_TRY(napi_set_named_property(env, exports, "IMPLEMENTATION_SHA256",
                                   implementation_sha256));
  NAPI_TRY(napi_create_uint32(env, ARCHBIRD_PATTERN_CONTRACT_VERSION,
                              &pattern_contract_version));
  NAPI_TRY(napi_set_named_property(env, exports, "PATTERN_CONTRACT_VERSION",
                                   pattern_contract_version));
  NAPI_TRY(napi_create_string_utf8(env, ARCHBIRD_PATTERN_CONTRACT,
                                   NAPI_AUTO_LENGTH, &pattern_contract));
  NAPI_TRY(napi_set_named_property(env, exports, "PATTERN_CONTRACT",
                                   pattern_contract));
  NAPI_TRY(napi_create_string_utf8(env, ARCHBIRD_PATTERN_ENGINE,
                                   NAPI_AUTO_LENGTH, &pattern_engine));
  NAPI_TRY(
      napi_set_named_property(env, exports, "PATTERN_ENGINE", pattern_engine));
  NAPI_TRY(napi_create_string_utf8(env, ARCHBIRD_PATTERN_UNICODE,
                                   NAPI_AUTO_LENGTH, &pattern_unicode));
  NAPI_TRY(napi_set_named_property(env, exports, "PATTERN_UNICODE",
                                   pattern_unicode));
  NAPI_TRY(napi_create_string_utf8(env, ARCHBIRD_PATTERN_OPTIONS,
                                   NAPI_AUTO_LENGTH, &pattern_options));
  NAPI_TRY(napi_set_named_property(env, exports, "PATTERN_OPTIONS",
                                   pattern_options));
  NAPI_TRY(napi_create_string_utf8(env, ARCHBIRD_VERSION, NAPI_AUTO_LENGTH,
                                   &version));
  NAPI_TRY(napi_set_named_property(env, exports, "VERSION", version));
  return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, init)
