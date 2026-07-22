#include <node_api.h>

#include <archbird/archbird.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ARCHBIRD_VERSION
#define ARCHBIRD_VERSION "0.0.1"
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
  size_t argc = 3;
  napi_value argv[3];
  const uint8_t *map;
  const uint8_t *query;
  size_t map_length;
  size_t query_length;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 2 || !get_buffer(env, argv[0], &map, &map_length) ||
      !get_buffer(env, argv[1], &query, &query_length) ||
      !get_optional_bool(env, argc, argv, 2, 0, &pretty))
    return NULL;
  status =
      saved_artifact_engine(larger_input(map_length, query_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_query(engine, map, map_length, query, query_length,
                                pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write,
                                &output);
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
  size_t argc = 3;
  napi_value argv[3];
  const uint8_t *map;
  const uint8_t *query;
  size_t map_length;
  size_t query_length;
  size_t max_chars;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 2 || !get_buffer(env, argv[0], &map, &map_length) ||
      !get_buffer(env, argv[1], &query, &query_length) ||
      !get_optional_size(env, argc, argv, 2, 0, "maxChars", &max_chars))
    return NULL;
  status =
      saved_artifact_engine(larger_input(map_length, query_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_query_markdown(engine, map, map_length, query,
                                         query_length, max_chars, output_write,
                                         &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value map_query_markdown_view(napi_env env,
                                          napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6];
  const uint8_t *map;
  const uint8_t *query;
  const uint8_t *verification = NULL;
  size_t map_length;
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
  if (argc < 4 || !get_buffer(env, argv[0], &map, &map_length) ||
      !get_buffer(env, argv[1], &query, &query_length) ||
      !get_optional_size(env, argc, argv, 2, 0, "view", &view) ||
      !get_optional_size(env, argc, argv, 3, 1, "detail", &detail) ||
      !get_optional_size(env, argc, argv, 4, 0, "maxChars", &max_chars) ||
      (argc > 5 &&
       !get_buffer(env, argv[5], &verification, &verification_length)))
    return NULL;
  status = saved_artifact_engine(
      larger_input(larger_input(map_length, query_length), verification_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_query_markdown_view_with_verification(
        engine, map, map_length, query, query_length, verification,
        verification_length, (ArchbirdQueryView)view,
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
  size_t argc = 7;
  napi_value argv[7];
  const uint8_t *map;
  const uint8_t *verification;
  const uint8_t *proposal;
  const uint8_t *contract;
  const uint8_t *change_result;
  const uint8_t *normalization;
  size_t map_length;
  size_t verification_length;
  size_t proposal_length;
  size_t contract_length;
  size_t result_length;
  size_t normalization_length;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 6 || !get_buffer(env, argv[0], &map, &map_length) ||
      !get_buffer(env, argv[1], &verification, &verification_length) ||
      !get_buffer(env, argv[2], &proposal, &proposal_length) ||
      !get_buffer(env, argv[3], &contract, &contract_length) ||
      !get_buffer(env, argv[4], &change_result, &result_length) ||
      !get_buffer(env, argv[5], &normalization, &normalization_length) ||
      !get_optional_bool(env, argc, argv, 6, 0, &pretty))
    return NULL;
  status = saved_artifact_engine(
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

static napi_value query_plan_compile(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6];
  const uint8_t *config;
  const uint8_t *map;
  const uint8_t *resolution;
  const uint8_t *overrides;
  size_t config_length;
  size_t map_length;
  size_t resolution_length;
  size_t overrides_length;
  char *query_id = NULL;
  size_t query_id_length = 0;
  int pretty;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 5 || !get_buffer(env, argv[0], &config, &config_length) ||
      !get_buffer(env, argv[1], &map, &map_length) ||
      !get_buffer(env, argv[2], &resolution, &resolution_length) ||
      !get_buffer(env, argv[4], &overrides, &overrides_length))
    return NULL;
  query_id = get_string(env, argv[3], &query_id_length);
  if (!query_id || !get_optional_bool(env, argc, argv, 5, 0, &pretty)) {
    free(query_id);
    return NULL;
  }
  status = saved_artifact_engine(
      larger_input(larger_input(config_length, map_length),
                   larger_input(resolution_length, overrides_length)),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_query_plan_compile(
        engine, config, config_length, map, map_length,
        resolution_length ? resolution : NULL, resolution_length, query_id,
        query_id_length, overrides_length ? overrides : NULL, overrides_length,
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

static napi_value constraints_report(napi_env env, napi_callback_info info) {
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
  ArchbirdVerificationFormat native_format;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
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
  if (status == ARCHBIRD_OK)
    status = archbird_constraints_report(
        engine, config, config_length, map, map_length,
        resolution_length ? resolution : NULL, resolution_length,
        request_length ? request : NULL, request_length, native_format,
        max_findings,
        (pretty ? ARCHBIRD_JSON_PRETTY : 0) |
            (native_format == ARCHBIRD_VERIFICATION_SARIF
                 ? ARCHBIRD_JSON_TRAILING_NEWLINE
                 : 0),
        output_write, &output);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
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

static napi_value change_proposal(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6];
  const uint8_t *verification;
  size_t verification_length;
  char *fingerprint = NULL;
  size_t fingerprint_length = 0;
  char *format = NULL;
  size_t format_length = 0;
  uint32_t max_candidates = 100;
  int full = 0;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 3 ||
      !get_buffer(env, argv[0], &verification, &verification_length))
    return NULL;
  fingerprint = get_string(env, argv[1], &fingerprint_length);
  format = get_string(env, argv[2], &format_length);
  if (!fingerprint || !format) {
    free(fingerprint);
    free(format);
    return NULL;
  }
  if (!get_optional_bool(env, argc, argv, 3, 0, &full)) {
    free(fingerprint);
    free(format);
    return NULL;
  }
  if (argc >= 5 &&
      napi_get_value_uint32(env, argv[4], &max_candidates) != napi_ok) {
    free(fingerprint);
    free(format);
    napi_throw_type_error(env, "ARCHBIRD_NUMBER",
                          "maxCandidates must be a nonnegative integer");
    return NULL;
  }
  if (!get_optional_bool(env, argc, argv, 5, 0, &pretty)) {
    free(fingerprint);
    free(format);
    return NULL;
  }
  if (!((format_length == 4 && !memcmp(format, "json", 4)) ||
        (format_length == 8 && !memcmp(format, "markdown", 8)))) {
    free(fingerprint);
    free(format);
    napi_throw_range_error(env, "ARCHBIRD_FORMAT",
                           "change proposal format must be json or markdown");
    return NULL;
  }
  status = saved_artifact_engine(verification_length, &engine);
  if (status == ARCHBIRD_OK && format_length == 4)
    status = archbird_change_proposal(engine, verification, verification_length,
                                      fingerprint, fingerprint_length,
                                      pretty ? ARCHBIRD_JSON_PRETTY : 0,
                                      output_write, &output);
  else if (status == ARCHBIRD_OK)
    status = archbird_change_proposal_report(
        engine, verification, verification_length, fingerprint,
        fingerprint_length, full, (size_t)max_candidates, output_write,
        &output);
  free(fingerprint);
  free(format);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value change_contract(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  const uint8_t *proposal;
  const uint8_t *review;
  size_t proposal_length;
  size_t review_length;
  char *format = NULL;
  size_t format_length = 0;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 3 || !get_buffer(env, argv[0], &proposal, &proposal_length) ||
      !get_buffer(env, argv[1], &review, &review_length))
    return NULL;
  format = get_string(env, argv[2], &format_length);
  if (!format || !get_optional_bool(env, argc, argv, 3, 0, &pretty)) {
    free(format);
    return NULL;
  }
  if (!((format_length == 4 && !memcmp(format, "json", 4)) ||
        (format_length == 8 && !memcmp(format, "markdown", 8)))) {
    free(format);
    napi_throw_range_error(env, "ARCHBIRD_FORMAT",
                           "change contract format must be json or markdown");
    return NULL;
  }
  status = saved_artifact_engine(larger_input(proposal_length, review_length),
                                 &engine);
  if (status == ARCHBIRD_OK && format_length == 4)
    status = archbird_change_contract(
        engine, proposal, proposal_length, review, review_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  else if (status == ARCHBIRD_OK)
    status = archbird_change_contract_report(engine, proposal, proposal_length,
                                             review, review_length,
                                             output_write, &output);
  free(format);
  result = render_result(env, engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static napi_value change_verify(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6];
  const uint8_t *proposal;
  const uint8_t *contract;
  const uint8_t *before;
  const uint8_t *after;
  size_t proposal_length;
  size_t contract_length;
  size_t before_length;
  size_t after_length;
  char *format = NULL;
  size_t format_length = 0;
  int pretty = 0;
  ArchbirdChangeFormat native_format;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  NodeOutput output = {0};
  napi_value result;
  NAPI_TRY(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  if (argc < 5 || !get_buffer(env, argv[0], &proposal, &proposal_length) ||
      !get_buffer(env, argv[1], &contract, &contract_length) ||
      !get_buffer(env, argv[2], &before, &before_length) ||
      !get_buffer(env, argv[3], &after, &after_length))
    return NULL;
  format = get_string(env, argv[4], &format_length);
  if (!format || !get_optional_bool(env, argc, argv, 5, 0, &pretty)) {
    free(format);
    return NULL;
  }
  if (format_length == 4 && !memcmp(format, "json", 4))
    native_format = ARCHBIRD_CHANGE_JSON;
  else if (format_length == 8 && !memcmp(format, "markdown", 8))
    native_format = ARCHBIRD_CHANGE_MARKDOWN;
  else if (format_length == 5 && !memcmp(format, "sarif", 5))
    native_format = ARCHBIRD_CHANGE_SARIF;
  else if (format_length == 5 && !memcmp(format, "junit", 5))
    native_format = ARCHBIRD_CHANGE_JUNIT;
  else {
    free(format);
    napi_throw_range_error(env, "ARCHBIRD_FORMAT",
                           "change result format must be json, markdown, "
                           "sarif, or junit");
    return NULL;
  }
  free(format);
  status = saved_artifact_engine(
      larger_input(larger_input(proposal_length, contract_length),
                   larger_input(before_length, after_length)),
      &engine);
  if (status == ARCHBIRD_OK && native_format == ARCHBIRD_CHANGE_JSON)
    status = archbird_change_verify(
        engine, proposal, proposal_length, contract, contract_length, before,
        before_length, after, after_length, pretty ? ARCHBIRD_JSON_PRETTY : 0,
        output_write, &output);
  else if (status == ARCHBIRD_OK)
    status = archbird_change_verify_report(
        engine, proposal, proposal_length, contract, contract_length, before,
        before_length, after, after_length, native_format,
        (pretty ? ARCHBIRD_JSON_PRETTY : 0) |
            (native_format == ARCHBIRD_CHANGE_SARIF
                 ? ARCHBIRD_JSON_TRAILING_NEWLINE
                 : 0),
        output_write, &output);
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
      {"constraintsEvaluate", NULL, constraints_evaluate, NULL, NULL, NULL,
       napi_default, NULL},
      {"queryPlanCompile", NULL, query_plan_compile, NULL, NULL, NULL,
       napi_default, NULL},
      {"projectionEvaluate", NULL, projection_evaluate, NULL, NULL, NULL,
       napi_default, NULL},
      {"projectConfigurationCompile", NULL, project_configuration_compile, NULL,
       NULL, NULL, napi_default, NULL},
      {"changeVerify", NULL, change_verify, NULL, NULL, NULL, napi_default,
       NULL},
      {"changeContract", NULL, change_contract, NULL, NULL, NULL, napi_default,
       NULL},
      {"changeProposal", NULL, change_proposal, NULL, NULL, NULL, napi_default,
       NULL},
      {"workspaceAnalyze", NULL, workspace_analyze, NULL, NULL, NULL,
       napi_default, NULL},
      {"workspacePlan", NULL, workspace_plan, NULL, NULL, NULL, napi_default,
       NULL},
      {"mapDiff", NULL, map_diff, NULL, NULL, NULL, napi_default, NULL},
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
