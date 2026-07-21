#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <archbird/archbird.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct PyArchbirdProject {
  ArchbirdEngine *engine;
  ArchbirdProject *project;
} PyArchbirdProject;

typedef struct PyOutput {
  uint8_t *data;
  size_t length;
  size_t capacity;
} PyOutput;

static PyObject *archbird_error_type;
static const char *project_capsule_name = "archbird.native.Project";

static int output_write(void *user_data, const uint8_t *bytes, size_t length) {
  PyOutput *output = (PyOutput *)user_data;
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

static PyObject *raise_status(ArchbirdEngine *engine, ArchbirdStatus status) {
  const char *message = engine ? archbird_engine_error(engine) : NULL;
  size_t offset =
      engine ? archbird_engine_error_offset(engine) : ARCHBIRD_NO_OFFSET;
  PyObject *detail = NULL;
  PyObject *exception = NULL;
  PyObject *status_value = NULL;
  PyObject *offset_value = NULL;
  if (status == ARCHBIRD_OUT_OF_MEMORY)
    return PyErr_NoMemory();
  if (!message || !message[0])
    message = "native Archbird operation failed";
  if (offset == ARCHBIRD_NO_OFFSET)
    detail = PyUnicode_FromFormat("%s (status=%d)", message, (int)status);
  else
    detail = PyUnicode_FromFormat("%s (status=%d, byte=%zu)", message,
                                  (int)status, offset);
  if (!detail)
    return NULL;
  exception = PyObject_CallFunctionObjArgs(archbird_error_type, detail, NULL);
  Py_DECREF(detail);
  if (!exception)
    return NULL;
  status_value = PyLong_FromLong((long)status);
  if (!status_value ||
      PyObject_SetAttrString(exception, "status", status_value) < 0)
    goto failed;
  if (offset != ARCHBIRD_NO_OFFSET) {
    offset_value = PyLong_FromSize_t(offset);
    if (!offset_value ||
        PyObject_SetAttrString(exception, "offset", offset_value) < 0)
      goto failed;
  }
  PyErr_SetObject(archbird_error_type, exception);
  Py_XDECREF(offset_value);
  Py_DECREF(status_value);
  Py_DECREF(exception);
  return NULL;

failed:
  Py_XDECREF(offset_value);
  Py_XDECREF(status_value);
  Py_DECREF(exception);
  return NULL;
}

static PyObject *render_result(ArchbirdEngine *engine, ArchbirdStatus status,
                               PyOutput *output) {
  PyObject *result;
  if (status != ARCHBIRD_OK) {
    free(output->data);
    return raise_status(engine, status);
  }
  if (output->length > (size_t)PY_SSIZE_T_MAX) {
    free(output->data);
    PyErr_SetString(PyExc_OverflowError, "native output exceeds Python size");
    return NULL;
  }
  result = PyBytes_FromStringAndSize((const char *)output->data,
                                     (Py_ssize_t)output->length);
  free(output->data);
  return result;
}

static PyArchbirdProject *get_project(PyObject *capsule) {
  return (PyArchbirdProject *)PyCapsule_GetPointer(capsule,
                                                   project_capsule_name);
}

static void project_capsule_destroy(PyObject *capsule) {
  PyArchbirdProject *owned =
      (PyArchbirdProject *)PyCapsule_GetPointer(capsule, project_capsule_name);
  if (!owned) {
    PyErr_Clear();
    return;
  }
  archbird_project_destroy(owned->project);
  archbird_engine_destroy(owned->engine);
  free(owned);
}

static int parse_mode(const char *value, ArchbirdProviderMode *out) {
  if (strcmp(value, "primary") == 0)
    *out = ARCHBIRD_PROVIDER_PRIMARY;
  else if (strcmp(value, "augment") == 0)
    *out = ARCHBIRD_PROVIDER_AUGMENT;
  else if (strcmp(value, "audit") == 0)
    *out = ARCHBIRD_PROVIDER_AUDIT;
  else {
    PyErr_SetString(PyExc_ValueError,
                    "provider mode must be primary, augment, or audit");
    return 0;
  }
  return 1;
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

static PyObject *py_project_create(PyObject *self, PyObject *args) {
  const char *manifest;
  Py_ssize_t manifest_length;
  PyArchbirdProject *owned;
  ArchbirdStatus status;
  PyObject *capsule;
  (void)self;
  if (!PyArg_ParseTuple(args, "y#:project_create", &manifest, &manifest_length))
    return NULL;
  owned = (PyArchbirdProject *)calloc(1, sizeof(*owned));
  if (!owned)
    return PyErr_NoMemory();
  status = archbird_engine_create(NULL, &owned->engine);
  if (status == ARCHBIRD_OK)
    status = archbird_project_create(owned->engine, (const uint8_t *)manifest,
                                     (size_t)manifest_length, &owned->project);
  if (status != ARCHBIRD_OK) {
    PyObject *raised = raise_status(owned->engine, status);
    archbird_project_destroy(owned->project);
    archbird_engine_destroy(owned->engine);
    free(owned);
    return raised;
  }
  capsule = PyCapsule_New(owned, project_capsule_name, project_capsule_destroy);
  if (!capsule) {
    archbird_project_destroy(owned->project);
    archbird_engine_destroy(owned->engine);
    free(owned);
  }
  return capsule;
}

static PyObject *py_project_add_source(PyObject *self, PyObject *args) {
  PyObject *capsule;
  const char *path;
  const char *bytes;
  Py_ssize_t path_length;
  Py_ssize_t byte_length;
  PyArchbirdProject *owned;
  ArchbirdStatus status;
  (void)self;
  if (!PyArg_ParseTuple(args, "Os#y#:project_add_source", &capsule, &path,
                        &path_length, &bytes, &byte_length))
    return NULL;
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  status = archbird_project_add_source(
      owned->engine, owned->project, path, (size_t)path_length,
      (const uint8_t *)bytes, (size_t)byte_length);
  if (status != ARCHBIRD_OK)
    return raise_status(owned->engine, status);
  Py_RETURN_NONE;
}

static PyObject *py_project_finalize_sources(PyObject *self,
                                             PyObject *capsule) {
  PyArchbirdProject *owned;
  ArchbirdStatus status;
  (void)self;
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  status = archbird_project_finalize_sources(owned->engine, owned->project);
  if (status != ARCHBIRD_OK)
    return raise_status(owned->engine, status);
  Py_RETURN_NONE;
}

static PyObject *py_project_set_config(PyObject *self, PyObject *args) {
  PyObject *capsule;
  const char *config;
  Py_ssize_t config_length;
  PyArchbirdProject *owned;
  ArchbirdStatus status;
  (void)self;
  if (!PyArg_ParseTuple(args, "Oy#:project_set_config", &capsule, &config,
                        &config_length))
    return NULL;
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  status = archbird_project_set_config(owned->engine, owned->project,
                                       (const uint8_t *)config,
                                       (size_t)config_length);
  if (status != ARCHBIRD_OK)
    return raise_status(owned->engine, status);
  Py_RETURN_NONE;
}

static PyObject *py_project_config_sha256(PyObject *self, PyObject *capsule) {
  PyArchbirdProject *owned;
  const char *digest;
  (void)self;
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  digest = archbird_project_config_sha256(owned->project);
  if (!digest) {
    PyErr_SetString(archbird_error_type,
                    "project configuration has not been supplied");
    return NULL;
  }
  return PyUnicode_FromStringAndSize(digest, 64);
}

static PyObject *py_project_add_provider(PyObject *self, PyObject *args) {
  PyObject *capsule;
  const char *mode_text;
  const char *provider;
  Py_ssize_t provider_length;
  PyArchbirdProject *owned;
  ArchbirdProviderMode mode;
  ArchbirdStatus status;
  (void)self;
  if (!PyArg_ParseTuple(args, "Osy#:project_add_provider", &capsule, &mode_text,
                        &provider, &provider_length))
    return NULL;
  if (!parse_mode(mode_text, &mode))
    return NULL;
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  status = archbird_project_add_provider_facts(owned->engine, owned->project,
                                               mode, (const uint8_t *)provider,
                                               (size_t)provider_length);
  if (status != ARCHBIRD_OK)
    return raise_status(owned->engine, status);
  Py_RETURN_NONE;
}

static PyObject *py_project_add_test_symbol_observations(PyObject *self,
                                                         PyObject *args) {
  PyObject *capsule;
  const char *observations;
  Py_ssize_t observations_length;
  PyArchbirdProject *owned;
  ArchbirdStatus status;
  (void)self;
  if (!PyArg_ParseTuple(args, "Oy#:project_add_test_symbol_observations",
                        &capsule, &observations, &observations_length))
    return NULL;
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  status = archbird_project_add_test_symbol_observations(
      owned->engine, owned->project, (const uint8_t *)observations,
      (size_t)observations_length);
  if (status != ARCHBIRD_OK)
    return raise_status(owned->engine, status);
  Py_RETURN_NONE;
}

static PyObject *py_project_scan_builtin(PyObject *self, PyObject *args) {
  PyObject *capsule;
  const char *mode_text = "primary";
  PyArchbirdProject *owned;
  ArchbirdProviderMode mode;
  ArchbirdStatus status;
  (void)self;
  if (!PyArg_ParseTuple(args, "O|s:project_scan_builtin", &capsule, &mode_text))
    return NULL;
  if (!parse_mode(mode_text, &mode))
    return NULL;
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  status = archbird_project_scan_builtin(owned->engine, owned->project, mode);
  if (status != ARCHBIRD_OK)
    return raise_status(owned->engine, status);
  Py_RETURN_NONE;
}

static PyObject *py_project_scan_builtin_provider(PyObject *self,
                                                  PyObject *args) {
  PyObject *capsule;
  const char *provider_id;
  Py_ssize_t provider_id_length;
  const char *mode_text = "primary";
  PyArchbirdProject *owned;
  ArchbirdProviderMode mode;
  ArchbirdStatus status;
  (void)self;
  if (!PyArg_ParseTuple(args, "Os#|s:project_scan_builtin_provider", &capsule,
                        &provider_id, &provider_id_length, &mode_text))
    return NULL;
  if (!parse_mode(mode_text, &mode))
    return NULL;
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  status = archbird_project_scan_builtin_provider(
      owned->engine, owned->project, provider_id, (size_t)provider_id_length,
      mode);
  if (status != ARCHBIRD_OK)
    return raise_status(owned->engine, status);
  Py_RETURN_NONE;
}

static PyObject *py_project_scan_builtin_provider_file(PyObject *self,
                                                       PyObject *args) {
  PyObject *capsule;
  const char *provider_id;
  Py_ssize_t provider_id_length;
  const char *path;
  Py_ssize_t path_length;
  const char *mode_text = "primary";
  PyArchbirdProject *owned;
  ArchbirdProviderMode mode;
  ArchbirdStatus status;
  (void)self;
  if (!PyArg_ParseTuple(args, "Os#s#|s:project_scan_builtin_provider_file",
                        &capsule, &provider_id, &provider_id_length, &path,
                        &path_length, &mode_text))
    return NULL;
  if (!parse_mode(mode_text, &mode))
    return NULL;
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  status = archbird_project_scan_builtin_provider_file(
      owned->engine, owned->project, provider_id, (size_t)provider_id_length,
      path, (size_t)path_length, mode);
  if (status != ARCHBIRD_OK)
    return raise_status(owned->engine, status);
  Py_RETURN_NONE;
}

static PyObject *py_project_finalize_providers(PyObject *self,
                                               PyObject *capsule) {
  PyArchbirdProject *owned;
  ArchbirdStatus status;
  (void)self;
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  status = archbird_project_finalize_providers(owned->engine, owned->project);
  if (status != ARCHBIRD_OK)
    return raise_status(owned->engine, status);
  Py_RETURN_NONE;
}

static PyObject *py_project_manifest_sha256(PyObject *self, PyObject *capsule) {
  PyArchbirdProject *owned;
  const char *digest;
  (void)self;
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  digest = archbird_project_manifest_sha256(owned->project);
  if (!digest) {
    PyErr_SetString(archbird_error_type, "project has no manifest digest");
    return NULL;
  }
  return PyUnicode_FromStringAndSize(digest, 64);
}

static PyObject *py_project_map_input_sha256(PyObject *self,
                                             PyObject *capsule) {
  PyArchbirdProject *owned;
  const char *digest;
  (void)self;
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  digest = archbird_project_map_input_sha256(owned->project);
  if (!digest) {
    PyErr_SetString(archbird_error_type, "project has no Map-input digest");
    return NULL;
  }
  return PyUnicode_FromStringAndSize(digest, 64);
}

static PyObject *py_project_counts(PyObject *self, PyObject *capsule) {
  PyArchbirdProject *owned;
  (void)self;
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  return Py_BuildValue(
      "{s:n,s:n,s:n}", "sources",
      (Py_ssize_t)archbird_project_source_count(owned->project), "providers",
      (Py_ssize_t)archbird_project_provider_count(owned->project), "facts",
      (Py_ssize_t)archbird_project_provider_fact_count(owned->project));
}

static PyObject *py_project_merge_summary(PyObject *self, PyObject *capsule) {
  PyArchbirdProject *owned;
  ArchbirdMergeSummary summary;
  ArchbirdStatus status;
  (void)self;
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  memset(&summary, 0, sizeof(summary));
  summary.struct_size = sizeof(summary);
  status = archbird_project_merge_summary(owned->project, &summary);
  if (status != ARCHBIRD_OK)
    return raise_status(owned->engine, status);
  return Py_BuildValue("{s:n,s:n,s:n,s:n,s:n,s:n,s:n,s:n,s:n,s:n}", "providers",
                       (Py_ssize_t)summary.providers, "selections",
                       (Py_ssize_t)summary.selections, "selected_facts",
                       (Py_ssize_t)summary.selected_facts, "contributed",
                       (Py_ssize_t)summary.contributed, "deduplicated",
                       (Py_ssize_t)summary.deduplicated, "enriched",
                       (Py_ssize_t)summary.enriched, "variations",
                       (Py_ssize_t)summary.variations, "conflicts",
                       (Py_ssize_t)summary.conflicts, "audit_matches",
                       (Py_ssize_t)summary.audit_matches, "audit_differences",
                       (Py_ssize_t)summary.audit_differences);
}

typedef ArchbirdStatus (*ProjectRenderFn)(ArchbirdEngine *,
                                          const ArchbirdProject *, uint32_t,
                                          ArchbirdWriteFn, void *);

static PyObject *render_project(PyObject *capsule, ProjectRenderFn function,
                                uint32_t flags) {
  PyArchbirdProject *owned = get_project(capsule);
  PyOutput output = {0};
  ArchbirdStatus status;
  if (!owned)
    return NULL;
  status =
      function(owned->engine, owned->project, flags, output_write, &output);
  return render_result(owned->engine, status, &output);
}

static PyObject *py_project_file_facts(PyObject *self, PyObject *args,
                                       PyObject *kwargs) {
  static char *keywords[] = {"project", "pretty", NULL};
  PyObject *capsule;
  int pretty = 0;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|p:project_file_facts",
                                   keywords, &capsule, &pretty))
    return NULL;
  return render_project(capsule, archbird_project_render_file_facts,
                        pretty ? ARCHBIRD_JSON_PRETTY : 0);
}

static PyObject *py_project_merge_ledger(PyObject *self, PyObject *args,
                                         PyObject *kwargs) {
  static char *keywords[] = {"project", "pretty", NULL};
  PyObject *capsule;
  int pretty = 0;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|p:project_merge_ledger",
                                   keywords, &capsule, &pretty))
    return NULL;
  return render_project(capsule, archbird_project_render_merge_ledger,
                        pretty ? ARCHBIRD_JSON_PRETTY : 0);
}

static PyObject *py_project_merge_conflicts(PyObject *self, PyObject *args,
                                            PyObject *kwargs) {
  static char *keywords[] = {"project", "pretty", NULL};
  PyObject *capsule;
  int pretty = 0;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|p:project_merge_conflicts",
                                   keywords, &capsule, &pretty))
    return NULL;
  return render_project(capsule, archbird_project_render_merge_conflicts,
                        pretty ? ARCHBIRD_JSON_PRETTY : 0);
}

static PyObject *py_project_map(PyObject *self, PyObject *args,
                                PyObject *kwargs) {
  static char *keywords[] = {"project", "pretty", NULL};
  PyObject *capsule;
  int pretty = 0;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|p:project_map", keywords,
                                   &capsule, &pretty))
    return NULL;
  return render_project(capsule, archbird_project_render_map,
                        pretty ? ARCHBIRD_JSON_PRETTY : 0);
}

static PyObject *py_project_provider_facts(PyObject *self, PyObject *args,
                                           PyObject *kwargs) {
  static char *keywords[] = {"project", "index", "pretty", NULL};
  PyObject *capsule;
  Py_ssize_t index;
  int pretty = 0;
  PyArchbirdProject *owned;
  PyOutput output = {0};
  ArchbirdStatus status;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "On|p:project_provider_facts",
                                   keywords, &capsule, &index, &pretty))
    return NULL;
  if (index < 0) {
    PyErr_SetString(PyExc_IndexError, "provider index cannot be negative");
    return NULL;
  }
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  status = archbird_project_render_provider_facts(
      owned->engine, owned->project, (size_t)index,
      pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  return render_result(owned->engine, status, &output);
}

static PyObject *py_json_canonicalize(PyObject *self, PyObject *args,
                                      PyObject *kwargs) {
  static char *keywords[] = {"input", "pretty", "trailing_newline", NULL};
  const char *input;
  Py_ssize_t input_length;
  int pretty = 0;
  int trailing = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  uint32_t flags = 0;
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#|pp:json_canonicalize",
                                   keywords, &input, &input_length, &pretty,
                                   &trailing))
    return NULL;
  if (pretty)
    flags |= ARCHBIRD_JSON_PRETTY;
  if (trailing)
    flags |= ARCHBIRD_JSON_TRAILING_NEWLINE;
  status = input_engine((size_t)input_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, (const uint8_t *)input,
                                        (size_t)input_length, flags,
                                        output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_test_symbol_observations_validate(PyObject *self,
                                                      PyObject *args) {
  const char *input;
  Py_ssize_t input_length;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  (void)self;
  if (!PyArg_ParseTuple(args, "y#:test_symbol_observations_validate", &input,
                        &input_length))
    return NULL;
  status = saved_artifact_engine((size_t)input_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_test_symbol_observations_validate(
        engine, (const uint8_t *)input, (size_t)input_length);
  if (status != ARCHBIRD_OK) {
    PyObject *result = raise_status(engine, status);
    archbird_engine_destroy(engine);
    return result;
  }
  archbird_engine_destroy(engine);
  Py_RETURN_NONE;
}

static PyObject *py_discovery_plan(PyObject *self, PyObject *args,
                                   PyObject *kwargs) {
  static char *keywords[] = {"config", "paths", "pretty", NULL};
  const char *config;
  Py_ssize_t config_length;
  PyObject *paths;
  PyObject *sequence = NULL;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdDiscovery *discovery = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  Py_ssize_t index;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#O|p:discovery_plan",
                                   keywords, &config, &config_length, &paths,
                                   &pretty))
    return NULL;
  sequence = PySequence_Fast(paths, "paths must be a sequence of strings");
  if (!sequence)
    return NULL;
  status = input_engine((size_t)config_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_discovery_create(engine, (const uint8_t *)config,
                                       (size_t)config_length, &discovery);
  for (index = 0;
       status == ARCHBIRD_OK && index < PySequence_Fast_GET_SIZE(sequence);
       index++) {
    PyObject *item = PySequence_Fast_GET_ITEM(sequence, index);
    Py_ssize_t path_length;
    const char *path;
    if (!PyUnicode_Check(item)) {
      PyErr_SetString(PyExc_TypeError, "discovery paths must be strings");
      status = ARCHBIRD_INVALID_ARGUMENT;
      break;
    }
    path = PyUnicode_AsUTF8AndSize(item, &path_length);
    if (!path) {
      status = ARCHBIRD_INVALID_ARGUMENT;
      break;
    }
    status = archbird_discovery_add_path(engine, discovery, path,
                                         (size_t)path_length);
  }
  if (status == ARCHBIRD_OK)
    status = archbird_discovery_render(engine, discovery,
                                       pretty ? ARCHBIRD_JSON_PRETTY : 0,
                                       output_write, &output);
  Py_DECREF(sequence);
  archbird_discovery_destroy(discovery);
  if (status == ARCHBIRD_INVALID_ARGUMENT && PyErr_Occurred()) {
    free(output.data);
    archbird_engine_destroy(engine);
    return NULL;
  }
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_discovery_resolve(PyObject *self, PyObject *args,
                                      PyObject *kwargs) {
  static char *keywords[] = {"config", "request", "inventory", "pretty", NULL};
  const char *config;
  const char *request;
  const char *inventory;
  Py_ssize_t config_length;
  Py_ssize_t request_length;
  Py_ssize_t inventory_length;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#y#y#|p:discovery_resolve",
                                   keywords, &config, &config_length, &request,
                                   &request_length, &inventory,
                                   &inventory_length, &pretty))
    return NULL;
  status = input_engine(
      larger_input(larger_input((size_t)config_length, (size_t)request_length),
                   (size_t)inventory_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_discovery_resolve(
        engine, (const uint8_t *)config, (size_t)config_length,
        (const uint8_t *)request, (size_t)request_length,
        (const uint8_t *)inventory, (size_t)inventory_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_discovery_descend(PyObject *self, PyObject *args) {
  const char *config;
  Py_ssize_t config_length;
  PyObject *paths;
  PyObject *ignore_paths = Py_None;
  PyObject *ignore_contents = Py_None;
  PyObject *sequence = NULL;
  PyObject *ignore_path_sequence = NULL;
  PyObject *ignore_content_sequence = NULL;
  PyObject *result = NULL;
  ArchbirdEngine *engine = NULL;
  ArchbirdDiscovery *discovery = NULL;
  ArchbirdStatus status;
  Py_ssize_t index;
  (void)self;
  if (!PyArg_ParseTuple(args, "y#O|OO:discovery_descend", &config,
                        &config_length, &paths, &ignore_paths,
                        &ignore_contents))
    return NULL;
  sequence = PySequence_Fast(paths, "paths must be a sequence of strings");
  if (!sequence)
    return NULL;
  if ((ignore_paths == Py_None) != (ignore_contents == Py_None)) {
    Py_DECREF(sequence);
    PyErr_SetString(PyExc_TypeError,
                    "ignore paths and contents must be supplied together");
    return NULL;
  }
  if (ignore_paths != Py_None) {
    ignore_path_sequence =
        PySequence_Fast(ignore_paths, "ignore paths must be a sequence");
    ignore_content_sequence =
        PySequence_Fast(ignore_contents, "ignore contents must be a sequence");
    if (!ignore_path_sequence || !ignore_content_sequence ||
        PySequence_Fast_GET_SIZE(ignore_path_sequence) !=
            PySequence_Fast_GET_SIZE(ignore_content_sequence)) {
      Py_DECREF(sequence);
      Py_XDECREF(ignore_path_sequence);
      Py_XDECREF(ignore_content_sequence);
      if (!PyErr_Occurred())
        PyErr_SetString(PyExc_TypeError,
                        "ignore paths and contents must have equal lengths");
      return NULL;
    }
  }
  status = input_engine((size_t)config_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_discovery_create(engine, (const uint8_t *)config,
                                       (size_t)config_length, &discovery);
  for (index = 0; status == ARCHBIRD_OK && ignore_path_sequence &&
                  index < PySequence_Fast_GET_SIZE(ignore_path_sequence);
       index++) {
    PyObject *path_item = PySequence_Fast_GET_ITEM(ignore_path_sequence, index);
    PyObject *content_item =
        PySequence_Fast_GET_ITEM(ignore_content_sequence, index);
    const char *ignore_path;
    char *ignore_bytes;
    Py_ssize_t ignore_path_length;
    Py_ssize_t ignore_byte_length;
    if (!PyUnicode_Check(path_item) || !PyBytes_Check(content_item)) {
      PyErr_SetString(PyExc_TypeError,
                      "ignore inputs must be string paths and bytes");
      status = ARCHBIRD_INVALID_ARGUMENT;
      break;
    }
    ignore_path = PyUnicode_AsUTF8AndSize(path_item, &ignore_path_length);
    if (!ignore_path || PyBytes_AsStringAndSize(content_item, &ignore_bytes,
                                                &ignore_byte_length) != 0) {
      status = ARCHBIRD_INVALID_ARGUMENT;
      break;
    }
    status = archbird_discovery_add_ignore(
        engine, discovery, ignore_path, (size_t)ignore_path_length,
        (const uint8_t *)ignore_bytes, (size_t)ignore_byte_length);
  }
  if (status == ARCHBIRD_OK)
    result = PyList_New(PySequence_Fast_GET_SIZE(sequence));
  if (status == ARCHBIRD_OK && !result)
    status = ARCHBIRD_OUT_OF_MEMORY;
  for (index = 0;
       status == ARCHBIRD_OK && index < PySequence_Fast_GET_SIZE(sequence);
       index++) {
    PyObject *item = PySequence_Fast_GET_ITEM(sequence, index);
    Py_ssize_t path_length;
    const char *path;
    int should_descend;
    PyObject *decision;
    if (!PyUnicode_Check(item)) {
      PyErr_SetString(PyExc_TypeError, "discovery paths must be strings");
      status = ARCHBIRD_INVALID_ARGUMENT;
      break;
    }
    path = PyUnicode_AsUTF8AndSize(item, &path_length);
    if (!path) {
      status = ARCHBIRD_INVALID_ARGUMENT;
      break;
    }
    status = archbird_discovery_should_descend(
        engine, discovery, path, (size_t)path_length, &should_descend);
    if (status != ARCHBIRD_OK)
      break;
    decision = should_descend ? Py_True : Py_False;
    Py_INCREF(decision);
    PyList_SET_ITEM(result, index, decision);
  }
  Py_DECREF(sequence);
  Py_XDECREF(ignore_path_sequence);
  Py_XDECREF(ignore_content_sequence);
  archbird_discovery_destroy(discovery);
  if (status != ARCHBIRD_OK) {
    Py_XDECREF(result);
    if (status == ARCHBIRD_INVALID_ARGUMENT && PyErr_Occurred()) {
      archbird_engine_destroy(engine);
      return NULL;
    }
    result = raise_status(engine, status);
  }
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_map_query(PyObject *self, PyObject *args,
                              PyObject *kwargs) {
  static char *keywords[] = {"map", "query", "pretty", NULL};
  const char *map;
  const char *query;
  Py_ssize_t map_length;
  Py_ssize_t query_length;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#y#|p:map_query", keywords,
                                   &map, &map_length, &query, &query_length,
                                   &pretty))
    return NULL;
  status = saved_artifact_engine(
      larger_input((size_t)map_length, (size_t)query_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_query(
        engine, (const uint8_t *)map, (size_t)map_length,
        (const uint8_t *)query, (size_t)query_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_map_markdown(PyObject *self, PyObject *args,
                                 PyObject *kwargs) {
  static char *keywords[] = {"map", "full", "max_chars", NULL};
  const char *map;
  Py_ssize_t map_length;
  Py_ssize_t max_chars = 0;
  int full = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#|pn:map_markdown", keywords,
                                   &map, &map_length, &full, &max_chars))
    return NULL;
  if (max_chars < 0) {
    PyErr_SetString(PyExc_ValueError,
                    "map max_chars must be a nonnegative integer");
    return NULL;
  }
  status = saved_artifact_engine((size_t)map_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_render_markdown(
        engine, (const uint8_t *)map, (size_t)map_length, full,
        (size_t)max_chars, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_map_markdown_view(PyObject *self, PyObject *args,
                                      PyObject *kwargs) {
  static char *keywords[] = {"map", "view", "detail", "max_chars", NULL};
  const char *map;
  Py_ssize_t map_length;
  Py_ssize_t max_chars = 0;
  int view;
  int detail;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#ii|n:map_markdown_view",
                                   keywords, &map, &map_length, &view, &detail,
                                   &max_chars))
    return NULL;
  if (max_chars < 0) {
    PyErr_SetString(PyExc_ValueError,
                    "map max_chars must be a nonnegative integer");
    return NULL;
  }
  status = saved_artifact_engine((size_t)map_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_render_markdown_view(
        engine, (const uint8_t *)map, (size_t)map_length, (ArchbirdMapView)view,
        (ArchbirdReportDetail)detail, (size_t)max_chars, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_map_query_markdown(PyObject *self, PyObject *args,
                                       PyObject *kwargs) {
  static char *keywords[] = {"map", "query", "max_chars", NULL};
  const char *map;
  const char *query;
  Py_ssize_t map_length;
  Py_ssize_t query_length;
  Py_ssize_t max_chars = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#y#|n:map_query_markdown",
                                   keywords, &map, &map_length, &query,
                                   &query_length, &max_chars))
    return NULL;
  if (max_chars < 0) {
    PyErr_SetString(PyExc_ValueError,
                    "query max_chars must be a nonnegative integer");
    return NULL;
  }
  status = saved_artifact_engine(
      larger_input((size_t)map_length, (size_t)query_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_query_markdown(
        engine, (const uint8_t *)map, (size_t)map_length,
        (const uint8_t *)query, (size_t)query_length, (size_t)max_chars,
        output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_map_query_markdown_view(PyObject *self, PyObject *args,
                                            PyObject *kwargs) {
  static char *keywords[] = {"map",       "query",        "view", "detail",
                             "max_chars", "verification", NULL};
  const char *map;
  const char *query;
  const char *verification = NULL;
  Py_ssize_t map_length;
  Py_ssize_t query_length;
  Py_ssize_t verification_length = 0;
  Py_ssize_t max_chars = 0;
  int view;
  int detail;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "y#y#ii|ny#:map_query_markdown_view", keywords, &map,
          &map_length, &query, &query_length, &view, &detail, &max_chars,
          &verification, &verification_length))
    return NULL;
  if (max_chars < 0) {
    PyErr_SetString(PyExc_ValueError,
                    "query max_chars must be a nonnegative integer");
    return NULL;
  }
  status = saved_artifact_engine(
      larger_input(larger_input((size_t)map_length, (size_t)query_length),
                   (size_t)verification_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_query_markdown_view_with_verification(
        engine, (const uint8_t *)map, (size_t)map_length,
        (const uint8_t *)query, (size_t)query_length,
        verification_length ? (const uint8_t *)verification : NULL,
        (size_t)verification_length, (ArchbirdQueryView)view,
        (ArchbirdReportDetail)detail, (size_t)max_chars, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_map_diff(PyObject *self, PyObject *args, PyObject *kwargs) {
  static char *keywords[] = {"before", "after", "pretty", NULL};
  const char *before;
  const char *after;
  Py_ssize_t before_length;
  Py_ssize_t after_length;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#y#|p:map_diff", keywords,
                                   &before, &before_length, &after,
                                   &after_length, &pretty))
    return NULL;
  status = saved_artifact_engine(
      larger_input((size_t)before_length, (size_t)after_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_diff(
        engine, (const uint8_t *)before, (size_t)before_length,
        (const uint8_t *)after, (size_t)after_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_map_freshness(PyObject *self, PyObject *args,
                                  PyObject *kwargs) {
  static char *keywords[] = {"snapshot", "current", "pretty", NULL};
  const char *snapshot;
  const char *current;
  Py_ssize_t snapshot_length;
  Py_ssize_t current_length;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#y#|p:map_freshness",
                                   keywords, &snapshot, &snapshot_length,
                                   &current, &current_length, &pretty))
    return NULL;
  status = saved_artifact_engine(
      larger_input((size_t)snapshot_length, (size_t)current_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_freshness(
        engine, (const uint8_t *)snapshot, (size_t)snapshot_length,
        (const uint8_t *)current, (size_t)current_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_map_export_graph(PyObject *self, PyObject *args,
                                     PyObject *kwargs) {
  static char *keywords[] = {"map",       "format",         "view", "direction",
                             "max_nodes", "max_edge_names", NULL};
  const char *map;
  const char *format;
  const char *view;
  const char *direction = "LR";
  Py_ssize_t map_length;
  Py_ssize_t max_nodes = 200;
  Py_ssize_t max_edge_names = 3;
  ArchbirdGraphOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#ss|snn:map_export_graph",
                                   keywords, &map, &map_length, &format, &view,
                                   &direction, &max_nodes, &max_edge_names))
    return NULL;
  if (max_nodes < 0 || max_edge_names < 0) {
    PyErr_SetString(PyExc_ValueError,
                    "graph limits must be nonnegative integers");
    return NULL;
  }
  archbird_graph_options_init(&options);
  if (!strcmp(format, "graphml"))
    options.format = ARCHBIRD_GRAPH_GRAPHML;
  else if (!strcmp(format, "mermaid"))
    options.format = ARCHBIRD_GRAPH_MERMAID;
  else if (!strcmp(format, "json"))
    options.format = ARCHBIRD_GRAPH_JSON;
  else {
    PyErr_SetString(PyExc_ValueError,
                    "graph format must be graphml, json, or mermaid");
    return NULL;
  }
  if (!strcmp(view, "components"))
    options.view = ARCHBIRD_GRAPH_COMPONENTS;
  else if (!strcmp(view, "files"))
    options.view = ARCHBIRD_GRAPH_FILES;
  else if (!strcmp(view, "symbols"))
    options.view = ARCHBIRD_GRAPH_SYMBOLS;
  else {
    PyErr_SetString(PyExc_ValueError,
                    "graph view must be components, files, or symbols");
    return NULL;
  }
  if (!strcmp(direction, "LR"))
    options.direction = ARCHBIRD_GRAPH_LR;
  else if (!strcmp(direction, "RL"))
    options.direction = ARCHBIRD_GRAPH_RL;
  else if (!strcmp(direction, "TB"))
    options.direction = ARCHBIRD_GRAPH_TB;
  else if (!strcmp(direction, "BT"))
    options.direction = ARCHBIRD_GRAPH_BT;
  else {
    PyErr_SetString(PyExc_ValueError,
                    "graph direction must be BT, LR, RL, or TB");
    return NULL;
  }
  options.max_nodes = (size_t)max_nodes;
  options.max_edge_names = (size_t)max_edge_names;
  status = saved_artifact_engine((size_t)map_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_export_graph(engine, (const uint8_t *)map,
                                       (size_t)map_length, &options,
                                       output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_okf_analyze(PyObject *self, PyObject *args,
                                PyObject *kwargs) {
  static char *keywords[] = {"source_bundle", "query",  "format",
                             "include_body",  "pretty", NULL};
  const char *source;
  const char *query = NULL;
  const char *format = "json";
  Py_ssize_t source_length;
  Py_ssize_t query_length = 0;
  int include_body = 0;
  int pretty = 1;
  ArchbirdOkfFormat native_format;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#|y#spp:okf_analyze",
                                   keywords, &source, &source_length, &query,
                                   &query_length, &format, &include_body,
                                   &pretty))
    return NULL;
  if (!strcmp(format, "json"))
    native_format = ARCHBIRD_OKF_JSON;
  else if (!strcmp(format, "markdown"))
    native_format = ARCHBIRD_OKF_MARKDOWN;
  else {
    PyErr_SetString(PyExc_ValueError, "OKF format must be json or markdown");
    return NULL;
  }
  status = input_engine(
      larger_input((size_t)source_length, (size_t)query_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_okf_analyze(
        engine, (const uint8_t *)source, (size_t)source_length,
        query_length ? (const uint8_t *)query : NULL,
        query_length ? (size_t)query_length : 0, native_format, include_body,
        (pretty ? ARCHBIRD_JSON_PRETTY : 0) | ARCHBIRD_JSON_TRAILING_NEWLINE,
        output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_okf_publish(PyObject *self, PyObject *args,
                                PyObject *kwargs) {
  static char *keywords[] = {"map",    "verification",  "proposal", "contract",
                             "result", "normalization", "pretty",   NULL};
  const char *map;
  const char *verification;
  const char *proposal;
  const char *contract;
  const char *change_result;
  const char *normalization;
  Py_ssize_t map_length;
  Py_ssize_t verification_length;
  Py_ssize_t proposal_length;
  Py_ssize_t contract_length;
  Py_ssize_t result_length;
  Py_ssize_t normalization_length;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *rendered;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "y#y#y#y#y#y#|p:okf_publish", keywords, &map,
          &map_length, &verification, &verification_length, &proposal,
          &proposal_length, &contract, &contract_length, &change_result,
          &result_length, &normalization, &normalization_length, &pretty))
    return NULL;
  status = saved_artifact_engine(
      larger_input(
          larger_input(
              larger_input((size_t)map_length, (size_t)verification_length),
              larger_input((size_t)proposal_length, (size_t)contract_length)),
          larger_input((size_t)result_length, (size_t)normalization_length)),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_okf_publish(
        engine, (const uint8_t *)map, (size_t)map_length,
        verification_length ? (const uint8_t *)verification : NULL,
        (size_t)verification_length,
        proposal_length ? (const uint8_t *)proposal : NULL,
        (size_t)proposal_length,
        contract_length ? (const uint8_t *)contract : NULL,
        (size_t)contract_length,
        result_length ? (const uint8_t *)change_result : NULL,
        (size_t)result_length,
        normalization_length ? (const uint8_t *)normalization : NULL,
        (size_t)normalization_length,
        (pretty ? ARCHBIRD_JSON_PRETTY : 0) | ARCHBIRD_JSON_TRAILING_NEWLINE,
        output_write, &output);
  rendered = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return rendered;
}

static PyObject *py_workspace_plan(PyObject *self, PyObject *args,
                                   PyObject *kwargs) {
  static char *keywords[] = {"config", "pretty", NULL};
  const char *config;
  Py_ssize_t config_length;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#|p:workspace_plan",
                                   keywords, &config, &config_length, &pretty))
    return NULL;
  status = input_engine((size_t)config_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_workspace_plan(
        engine, (const uint8_t *)config, (size_t)config_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_workspace_analyze(PyObject *self, PyObject *args,
                                      PyObject *kwargs) {
  static char *keywords[] = {"config", "maps", "pretty", NULL};
  const char *config;
  const char *maps;
  Py_ssize_t config_length;
  Py_ssize_t maps_length;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#y#|p:workspace_analyze",
                                   keywords, &config, &config_length, &maps,
                                   &maps_length, &pretty))
    return NULL;
  status = saved_artifact_engine(
      larger_input((size_t)config_length, (size_t)maps_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_workspace_analyze(
        engine, (const uint8_t *)config, (size_t)config_length,
        (const uint8_t *)maps, (size_t)maps_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_verification_plan(PyObject *self, PyObject *args,
                                      PyObject *kwargs) {
  static char *keywords[] = {"suite", "pretty", NULL};
  const char *suite;
  Py_ssize_t suite_length;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#|p:verification_plan",
                                   keywords, &suite, &suite_length, &pretty))
    return NULL;
  status = input_engine((size_t)suite_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_verification_plan(
        engine, (const uint8_t *)suite, (size_t)suite_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_verification_analyze(PyObject *self, PyObject *args,
                                         PyObject *kwargs) {
  static char *keywords[] = {"suite", "input", "pretty", NULL};
  const char *suite;
  const char *input;
  Py_ssize_t suite_length;
  Py_ssize_t input_length;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#y#|p:verification_analyze",
                                   keywords, &suite, &suite_length, &input,
                                   &input_length, &pretty))
    return NULL;
  status = saved_artifact_engine(
      larger_input((size_t)suite_length, (size_t)input_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_verification_analyze(
        engine, (const uint8_t *)suite, (size_t)suite_length,
        (const uint8_t *)input, (size_t)input_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_verification_debug(PyObject *self, PyObject *args,
                                       PyObject *kwargs) {
  static char *keywords[] = {"suite",  "input",  "request",
                             "format", "pretty", NULL};
  const char *suite;
  const char *input;
  const char *request;
  const char *format = "json";
  Py_ssize_t suite_length;
  Py_ssize_t input_length;
  Py_ssize_t request_length;
  int pretty = 0;
  ArchbirdVerificationFormat native_format;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#y#y#|sp:verification_debug",
                                   keywords, &suite, &suite_length, &input,
                                   &input_length, &request, &request_length,
                                   &format, &pretty))
    return NULL;
  if (!strcmp(format, "json"))
    native_format = ARCHBIRD_VERIFICATION_JSON;
  else if (!strcmp(format, "markdown"))
    native_format = ARCHBIRD_VERIFICATION_MARKDOWN;
  else {
    PyErr_SetString(PyExc_ValueError,
                    "verification debug format must be json or markdown");
    return NULL;
  }
  status = saved_artifact_engine(
      larger_input(larger_input((size_t)suite_length, (size_t)input_length),
                   (size_t)request_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_verification_debug(
        engine, (const uint8_t *)suite, (size_t)suite_length,
        (const uint8_t *)input, (size_t)input_length, (const uint8_t *)request,
        (size_t)request_length, native_format,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_verification_draft(PyObject *self, PyObject *args,
                                       PyObject *kwargs) {
  static char *keywords[] = {"map", "project_config", "pretty", NULL};
  const char *map;
  const char *project_config;
  Py_ssize_t map_length;
  Py_ssize_t project_config_length;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#s#|p:verification_draft",
                                   keywords, &map, &map_length, &project_config,
                                   &project_config_length, &pretty))
    return NULL;
  status = saved_artifact_engine((size_t)map_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_verification_draft(
        engine, (const uint8_t *)map, (size_t)map_length, project_config,
        (size_t)project_config_length,
        (pretty ? ARCHBIRD_JSON_PRETTY : 0) | ARCHBIRD_JSON_TRAILING_NEWLINE,
        output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_verification_recipe_catalog(PyObject *self, PyObject *args,
                                                PyObject *kwargs) {
  static char *keywords[] = {"recipe", "pretty", NULL};
  const char *recipe = "";
  Py_ssize_t recipe_length = 0;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                   "|s#p:verification_recipe_catalog", keywords,
                                   &recipe, &recipe_length, &pretty))
    return NULL;
  status = input_engine((size_t)recipe_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_verification_recipe_catalog(
        engine, recipe, (size_t)recipe_length,
        (pretty ? ARCHBIRD_JSON_PRETTY : 0) | ARCHBIRD_JSON_TRAILING_NEWLINE,
        output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_verification_recipe_compile(PyObject *self, PyObject *args,
                                                PyObject *kwargs) {
  static char *keywords[] = {"request", "pretty", NULL};
  const char *request;
  Py_ssize_t request_length;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                   "y#|p:verification_recipe_compile", keywords,
                                   &request, &request_length, &pretty))
    return NULL;
  status = input_engine((size_t)request_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_verification_recipe_compile(
        engine, (const uint8_t *)request, (size_t)request_length,
        (pretty ? ARCHBIRD_JSON_PRETTY : 0) | ARCHBIRD_JSON_TRAILING_NEWLINE,
        output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_verification_freeze(PyObject *self, PyObject *args,
                                        PyObject *kwargs) {
  static char *keywords[] = {"suite",     "input",  "owner",
                             "rationale", "pretty", NULL};
  const char *suite;
  const char *input;
  const char *owner;
  const char *rationale;
  Py_ssize_t suite_length;
  Py_ssize_t input_length;
  Py_ssize_t owner_length;
  Py_ssize_t rationale_length;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "y#y#s#s#|p:verification_freeze", keywords, &suite,
          &suite_length, &input, &input_length, &owner, &owner_length,
          &rationale, &rationale_length, &pretty))
    return NULL;
  status = saved_artifact_engine(
      larger_input((size_t)suite_length, (size_t)input_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_verification_freeze(
        engine, (const uint8_t *)suite, (size_t)suite_length,
        (const uint8_t *)input, (size_t)input_length, owner,
        (size_t)owner_length, rationale, (size_t)rationale_length,
        (pretty ? ARCHBIRD_JSON_PRETTY : 0) | ARCHBIRD_JSON_TRAILING_NEWLINE,
        output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_verification_report(PyObject *self, PyObject *args,
                                        PyObject *kwargs) {
  static char *keywords[] = {"suite",        "input",  "format",
                             "max_findings", "pretty", NULL};
  const char *suite;
  const char *input;
  const char *format;
  Py_ssize_t suite_length;
  Py_ssize_t input_length;
  Py_ssize_t max_findings = 200;
  int pretty = 0;
  ArchbirdVerificationFormat native_format;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#y#s|np:verification_report",
                                   keywords, &suite, &suite_length, &input,
                                   &input_length, &format, &max_findings,
                                   &pretty))
    return NULL;
  if (!strcmp(format, "markdown"))
    native_format = ARCHBIRD_VERIFICATION_MARKDOWN;
  else if (!strcmp(format, "sarif"))
    native_format = ARCHBIRD_VERIFICATION_SARIF;
  else if (!strcmp(format, "junit"))
    native_format = ARCHBIRD_VERIFICATION_JUNIT;
  else {
    PyErr_SetString(PyExc_ValueError,
                    "verification report format must be markdown, sarif, or "
                    "junit");
    return NULL;
  }
  status = saved_artifact_engine(
      larger_input((size_t)suite_length, (size_t)input_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_verification_analyze_report(
        engine, (const uint8_t *)suite, (size_t)suite_length,
        (const uint8_t *)input, (size_t)input_length, native_format,
        max_findings < 0 ? SIZE_MAX : (size_t)max_findings,
        (pretty ? ARCHBIRD_JSON_PRETTY : 0) |
            (native_format == ARCHBIRD_VERIFICATION_SARIF
                 ? ARCHBIRD_JSON_TRAILING_NEWLINE
                 : 0),
        output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_change_proposal(PyObject *self, PyObject *args,
                                    PyObject *kwargs) {
  static char *keywords[] = {"verification",   "fingerprint", "format", "full",
                             "max_candidates", "pretty",      NULL};
  const char *verification;
  const char *fingerprint;
  const char *format = "json";
  Py_ssize_t verification_length;
  Py_ssize_t fingerprint_length;
  Py_ssize_t max_candidates = 100;
  int full = 0;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "y#s#|spnp:change_proposal", keywords, &verification,
          &verification_length, &fingerprint, &fingerprint_length, &format,
          &full, &max_candidates, &pretty))
    return NULL;
  if (max_candidates < 0) {
    PyErr_SetString(PyExc_ValueError, "max_candidates must be nonnegative");
    return NULL;
  }
  if (strcmp(format, "json") != 0 && strcmp(format, "markdown") != 0) {
    PyErr_SetString(PyExc_ValueError,
                    "change proposal format must be json or markdown");
    return NULL;
  }
  status = saved_artifact_engine((size_t)verification_length, &engine);
  if (status == ARCHBIRD_OK && strcmp(format, "json") == 0)
    status = archbird_change_proposal(
        engine, (const uint8_t *)verification, (size_t)verification_length,
        fingerprint, (size_t)fingerprint_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  else if (status == ARCHBIRD_OK)
    status = archbird_change_proposal_report(
        engine, (const uint8_t *)verification, (size_t)verification_length,
        fingerprint, (size_t)fingerprint_length, full, (size_t)max_candidates,
        output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_change_contract(PyObject *self, PyObject *args,
                                    PyObject *kwargs) {
  static char *keywords[] = {"proposal", "review", "format", "pretty", NULL};
  const char *proposal;
  const char *review;
  const char *format = "json";
  Py_ssize_t proposal_length;
  Py_ssize_t review_length;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#y#|sp:change_contract",
                                   keywords, &proposal, &proposal_length,
                                   &review, &review_length, &format, &pretty))
    return NULL;
  if (strcmp(format, "json") != 0 && strcmp(format, "markdown") != 0) {
    PyErr_SetString(PyExc_ValueError,
                    "change contract format must be json or markdown");
    return NULL;
  }
  status = saved_artifact_engine(
      larger_input((size_t)proposal_length, (size_t)review_length), &engine);
  if (status == ARCHBIRD_OK && strcmp(format, "json") == 0)
    status = archbird_change_contract(
        engine, (const uint8_t *)proposal, (size_t)proposal_length,
        (const uint8_t *)review, (size_t)review_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  else if (status == ARCHBIRD_OK)
    status = archbird_change_contract_report(
        engine, (const uint8_t *)proposal, (size_t)proposal_length,
        (const uint8_t *)review, (size_t)review_length, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_change_verify(PyObject *self, PyObject *args,
                                  PyObject *kwargs) {
  static char *keywords[] = {"proposal", "contract", "before", "after",
                             "format",   "pretty",   NULL};
  const char *proposal;
  const char *contract;
  const char *before;
  const char *after;
  const char *format = "json";
  Py_ssize_t proposal_length;
  Py_ssize_t contract_length;
  Py_ssize_t before_length;
  Py_ssize_t after_length;
  int pretty = 0;
  ArchbirdChangeFormat native_format;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "y#y#y#y#|sp:change_verify", keywords, &proposal,
          &proposal_length, &contract, &contract_length, &before,
          &before_length, &after, &after_length, &format, &pretty))
    return NULL;
  if (!strcmp(format, "json"))
    native_format = ARCHBIRD_CHANGE_JSON;
  else if (!strcmp(format, "markdown"))
    native_format = ARCHBIRD_CHANGE_MARKDOWN;
  else if (!strcmp(format, "sarif"))
    native_format = ARCHBIRD_CHANGE_SARIF;
  else if (!strcmp(format, "junit"))
    native_format = ARCHBIRD_CHANGE_JUNIT;
  else {
    PyErr_SetString(PyExc_ValueError,
                    "change result format must be json, markdown, sarif, or "
                    "junit");
    return NULL;
  }
  status = saved_artifact_engine(
      larger_input(
          larger_input((size_t)proposal_length, (size_t)contract_length),
          larger_input((size_t)before_length, (size_t)after_length)),
      &engine);
  if (status == ARCHBIRD_OK && native_format == ARCHBIRD_CHANGE_JSON)
    status = archbird_change_verify(
        engine, (const uint8_t *)proposal, (size_t)proposal_length,
        (const uint8_t *)contract, (size_t)contract_length,
        (const uint8_t *)before, (size_t)before_length, (const uint8_t *)after,
        (size_t)after_length, pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write,
        &output);
  else if (status == ARCHBIRD_OK)
    status = archbird_change_verify_report(
        engine, (const uint8_t *)proposal, (size_t)proposal_length,
        (const uint8_t *)contract, (size_t)contract_length,
        (const uint8_t *)before, (size_t)before_length, (const uint8_t *)after,
        (size_t)after_length, native_format,
        (pretty ? ARCHBIRD_JSON_PRETTY : 0) |
            (native_format == ARCHBIRD_CHANGE_SARIF
                 ? ARCHBIRD_JSON_TRAILING_NEWLINE
                 : 0),
        output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyMethodDef archbird_methods[] = {
    {"change_verify", (PyCFunction)py_change_verify,
     METH_VARARGS | METH_KEYWORDS,
     "Judge a reviewed architecture change against before/after evidence."},
    {"change_contract", (PyCFunction)py_change_contract,
     METH_VARARGS | METH_KEYWORDS,
     "Seal explicit review metadata as an asserted change contract."},
    {"change_proposal", (PyCFunction)py_change_proposal,
     METH_VARARGS | METH_KEYWORDS,
     "Compile one derived change proposal from a verification finding."},
    {"verification_report", (PyCFunction)py_verification_report,
     METH_VARARGS | METH_KEYWORDS,
     "Render Markdown, SARIF, or JUnit from native verification evidence."},
    {"verification_freeze", (PyCFunction)py_verification_freeze,
     METH_VARARGS | METH_KEYWORDS,
     "Render an explicit violation and coverage baseline."},
    {"verification_draft", (PyCFunction)py_verification_draft,
     METH_VARARGS | METH_KEYWORDS,
     "Draft a candidate-only component dependency suite."},
    {"verification_recipe_catalog", (PyCFunction)py_verification_recipe_catalog,
     METH_VARARGS | METH_KEYWORDS,
     "List portable built-in verification recipes."},
    {"verification_recipe_compile", (PyCFunction)py_verification_recipe_compile,
     METH_VARARGS | METH_KEYWORDS,
     "Compile explicit recipe arguments into a verification suite."},
    {"verification_analyze", (PyCFunction)py_verification_analyze,
     METH_VARARGS | METH_KEYWORDS,
     "Evaluate a verification suite over host-supplied evidence."},
    {"verification_debug", (PyCFunction)py_verification_debug,
     METH_VARARGS | METH_KEYWORDS,
     "Explain selection completeness or unresolved verification evidence."},
    {"verification_plan", (PyCFunction)py_verification_plan,
     METH_VARARGS | METH_KEYWORDS,
     "Validate a verification suite and expose its host-loading plan."},
    {"workspace_analyze", (PyCFunction)py_workspace_analyze,
     METH_VARARGS | METH_KEYWORDS,
     "Join canonical project maps into a workspace artifact."},
    {"workspace_plan", (PyCFunction)py_workspace_plan,
     METH_VARARGS | METH_KEYWORDS,
     "Validate and expose a workspace host-loading plan."},
    {"map_diff", (PyCFunction)py_map_diff, METH_VARARGS | METH_KEYWORDS,
     "Structurally diff two canonical saved maps."},
    {"map_freshness", (PyCFunction)py_map_freshness,
     METH_VARARGS | METH_KEYWORDS,
     "Audit a saved Map or Query against a freshly derived current Map."},
    {"map_markdown", (PyCFunction)py_map_markdown, METH_VARARGS | METH_KEYWORDS,
     "Project a canonical saved map as compact or full Markdown."},
    {"map_markdown_view", (PyCFunction)py_map_markdown_view,
     METH_VARARGS | METH_KEYWORDS,
     "Project a canonical saved map by view and detail level."},
    {"map_export_graph", (PyCFunction)py_map_export_graph,
     METH_VARARGS | METH_KEYWORDS,
     "Project a canonical saved map as GraphML or Mermaid."},
    {"okf_analyze", (PyCFunction)py_okf_analyze, METH_VARARGS | METH_KEYWORDS,
     "Validate, index, or query host-decoded OKF syntax."},
    {"okf_publish", (PyCFunction)py_okf_publish, METH_VARARGS | METH_KEYWORDS,
     "Project canonical Map/Verify/Act artifacts into an OKF output bundle."},
    {"map_query", (PyCFunction)py_map_query, METH_VARARGS | METH_KEYWORDS,
     "Query a canonical saved map without reading repository sources."},
    {"map_query_markdown", (PyCFunction)py_map_query_markdown,
     METH_VARARGS | METH_KEYWORDS,
     "Query a canonical saved map and render ranked Markdown context."},
    {"map_query_markdown_view", (PyCFunction)py_map_query_markdown_view,
     METH_VARARGS | METH_KEYWORDS,
     "Project a canonical saved-map query by view and detail level."},
    {"discovery_descend", py_discovery_descend, METH_VARARGS,
     "Return C-owned safe traversal decisions for repository directories."},
    {"discovery_plan", (PyCFunction)py_discovery_plan,
     METH_VARARGS | METH_KEYWORDS,
     "Classify a host-provided repository file inventory."},
    {"discovery_resolve", (PyCFunction)py_discovery_resolve,
     METH_VARARGS | METH_KEYWORDS,
     "Resolve discovery, project configuration, and explicit overlays."},
    {"project_create", py_project_create, METH_VARARGS,
     "Create a native project from canonical source-manifest JSON."},
    {"project_add_source", py_project_add_source, METH_VARARGS,
     "Add one exact source byte sequence to a native project."},
    {"project_finalize_sources", py_project_finalize_sources, METH_O,
     "Require that every manifest source has been supplied."},
    {"project_set_config", py_project_set_config, METH_VARARGS,
     "Decode and bind one strict project configuration."},
    {"project_config_sha256", py_project_config_sha256, METH_O,
     "Return the canonical project-configuration digest."},
    {"project_add_provider", py_project_add_provider, METH_VARARGS,
     "Ingest one strict provider-facts artifact."},
    {"project_add_test_symbol_observations",
     py_project_add_test_symbol_observations, METH_VARARGS,
     "Ingest one strict runner-observed test-to-symbol artifact."},
    {"project_scan_builtin", py_project_scan_builtin, METH_VARARGS,
     "Run built-in lexical providers over supplied source bytes."},
    {"project_scan_builtin_provider", py_project_scan_builtin_provider,
     METH_VARARGS, "Run one built-in lexical provider by stable ID."},
    {"project_scan_builtin_provider_file",
     py_project_scan_builtin_provider_file, METH_VARARGS,
     "Run one file-local built-in provider for an exact manifest path."},
    {"project_finalize_providers", py_project_finalize_providers, METH_O,
     "Select and merge provider evidence."},
    {"project_manifest_sha256", py_project_manifest_sha256, METH_O,
     "Return the canonical source-manifest digest."},
    {"project_map_input_sha256", py_project_map_input_sha256, METH_O,
     "Return the content digest binding current Map source bytes."},
    {"project_counts", py_project_counts, METH_O,
     "Return native source/provider/fact counts."},
    {"project_merge_summary", py_project_merge_summary, METH_O,
     "Return the typed provider-merge summary."},
    {"project_file_facts", (PyCFunction)py_project_file_facts,
     METH_VARARGS | METH_KEYWORDS, "Render schema-6-compatible file evidence."},
    {"project_merge_ledger", (PyCFunction)py_project_merge_ledger,
     METH_VARARGS | METH_KEYWORDS, "Render the canonical provider ledger."},
    {"project_merge_conflicts", (PyCFunction)py_project_merge_conflicts,
     METH_VARARGS | METH_KEYWORDS,
     "Render the compact provider conflict ledger."},
    {"project_map", (PyCFunction)py_project_map, METH_VARARGS | METH_KEYWORDS,
     "Render the canonical native project map."},
    {"project_provider_facts", (PyCFunction)py_project_provider_facts,
     METH_VARARGS | METH_KEYWORDS, "Render one accepted provider artifact."},
    {"json_canonicalize", (PyCFunction)py_json_canonicalize,
     METH_VARARGS | METH_KEYWORDS, "Canonicalize strict Archbird JSON."},
    {"test_symbol_observations_validate", py_test_symbol_observations_validate,
     METH_VARARGS,
     "Validate strict project-owned test-to-symbol observations."},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef archbird_module = {
    PyModuleDef_HEAD_INIT,
    "_native",
    "Native language-neutral Archbird core.",
    -1,
    archbird_methods,
    NULL,
    NULL,
    NULL,
    NULL,
};

PyMODINIT_FUNC PyInit__native(void) {
  PyObject *module = PyModule_Create(&archbird_module);
  if (!module)
    return NULL;
  archbird_error_type =
      PyErr_NewException("archbird._native.Error", PyExc_RuntimeError, NULL);
  if (!archbird_error_type) {
    Py_DECREF(module);
    return NULL;
  }
  if (PyModule_AddObject(module, "Error", archbird_error_type) < 0) {
    Py_DECREF(archbird_error_type);
    Py_DECREF(module);
    return NULL;
  }
  if (PyModule_AddIntConstant(module, "NATIVE_ABI_VERSION",
                              ARCHBIRD_NATIVE_ABI_VERSION) < 0) {
    Py_DECREF(module);
    return NULL;
  }
  if (PyModule_AddStringConstant(module, "IMPLEMENTATION_SHA256",
                                 archbird_implementation_sha256()) < 0) {
    Py_DECREF(module);
    return NULL;
  }
  if (PyModule_AddIntConstant(module, "PATTERN_CONTRACT_VERSION",
                              ARCHBIRD_PATTERN_CONTRACT_VERSION) < 0) {
    Py_DECREF(module);
    return NULL;
  }
  if (PyModule_AddStringConstant(module, "PATTERN_CONTRACT",
                                 ARCHBIRD_PATTERN_CONTRACT) < 0 ||
      PyModule_AddStringConstant(module, "PATTERN_ENGINE",
                                 ARCHBIRD_PATTERN_ENGINE) < 0 ||
      PyModule_AddStringConstant(module, "PATTERN_UNICODE",
                                 ARCHBIRD_PATTERN_UNICODE) < 0 ||
      PyModule_AddStringConstant(module, "PATTERN_OPTIONS",
                                 ARCHBIRD_PATTERN_OPTIONS) < 0) {
    Py_DECREF(module);
    return NULL;
  }
  return module;
}
