#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <archbird/archbird.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(ARCHBIRD_CONFIGURED_PYTHON_MAJOR) &&                               \
    defined(ARCHBIRD_CONFIGURED_PYTHON_MINOR)
_Static_assert(PY_MAJOR_VERSION == ARCHBIRD_CONFIGURED_PYTHON_MAJOR,
               "configured Python interpreter and development headers differ");
_Static_assert(PY_MINOR_VERSION == ARCHBIRD_CONFIGURED_PYTHON_MINOR,
               "configured Python interpreter and development headers differ");
#endif

typedef struct PyArchbirdProject {
  ArchbirdEngine *engine;
  ArchbirdProject *project;
} PyArchbirdProject;

typedef struct PyOutput {
  uint8_t *data;
  size_t length;
  size_t capacity;
} PyOutput;

typedef struct PySink {
  PyObject *callable;
  PyObject *error_type;
  PyObject *error_value;
  PyObject *error_traceback;
} PySink;

static PyObject *archbird_error_type;
static const char *project_capsule_name = "archbird.native.Project";
static const char *closed_project_capsule_name =
    "archbird.native.Project.closed";

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

static int sink_capture_error(PySink *sink) {
  PyErr_Fetch(&sink->error_type, &sink->error_value, &sink->error_traceback);
  return 1;
}

static int sink_write(void *user_data, const uint8_t *bytes, size_t length) {
  PySink *sink = (PySink *)user_data;
  PyObject *chunk;
  PyObject *result;
  PyObject *expected;
  int equal;
  if (sink->error_type)
    return 1;
  if (length > (size_t)PY_SSIZE_T_MAX) {
    PyErr_SetString(PyExc_OverflowError,
                    "native output chunk exceeds Python size");
    return sink_capture_error(sink);
  }
  chunk = PyBytes_FromStringAndSize((const char *)bytes, (Py_ssize_t)length);
  if (!chunk)
    return sink_capture_error(sink);
  result = PyObject_CallOneArg(sink->callable, chunk);
  Py_DECREF(chunk);
  if (!result)
    return sink_capture_error(sink);
  if (result == Py_None) {
    Py_DECREF(result);
    return 0;
  }
  expected = PyLong_FromSize_t(length);
  if (!expected) {
    Py_DECREF(result);
    return sink_capture_error(sink);
  }
  equal = PyObject_RichCompareBool(result, expected, Py_EQ);
  Py_DECREF(expected);
  if (equal < 0) {
    Py_DECREF(result);
    return sink_capture_error(sink);
  }
  if (!equal) {
    PyErr_Format(PyExc_OSError, "output sink wrote %R of %zu bytes", result,
                 length);
    Py_DECREF(result);
    return sink_capture_error(sink);
  }
  Py_DECREF(result);
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

static void project_destroy_owned(PyArchbirdProject *owned) {
  if (!owned)
    return;
  archbird_project_destroy(owned->project);
  archbird_engine_destroy(owned->engine);
  free(owned);
}

static void project_capsule_destroy(PyObject *capsule) {
  PyArchbirdProject *owned =
      (PyArchbirdProject *)PyCapsule_GetPointer(capsule, project_capsule_name);
  if (!owned) {
    PyErr_Clear();
    return;
  }
  project_destroy_owned(owned);
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

static PyObject *py_project_close(PyObject *self, PyObject *capsule) {
  PyArchbirdProject *owned;
  (void)self;
  if (PyCapsule_IsValid(capsule, closed_project_capsule_name))
    Py_RETURN_NONE;
  if (!PyCapsule_IsValid(capsule, project_capsule_name)) {
    PyErr_SetString(PyExc_TypeError,
                    "project must be an Archbird project handle");
    return NULL;
  }
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  if (PyCapsule_SetDestructor(capsule, NULL) < 0)
    return NULL;
  if (PyCapsule_SetName(capsule, closed_project_capsule_name) < 0) {
    (void)PyCapsule_SetDestructor(capsule, project_capsule_destroy);
    return NULL;
  }
  project_destroy_owned(owned);
  Py_RETURN_NONE;
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

static PyObject *py_project_write_map(PyObject *self, PyObject *args,
                                      PyObject *kwargs) {
  static char *keywords[] = {"project", "sink", "pretty", NULL};
  PyObject *capsule;
  PyObject *sink_object;
  int pretty = 0;
  PyArchbirdProject *owned;
  PySink sink = {0};
  ArchbirdStatus status;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO|p:project_write_map",
                                   keywords, &capsule, &sink_object, &pretty))
    return NULL;
  if (!PyCallable_Check(sink_object)) {
    PyErr_SetString(PyExc_TypeError, "sink must be callable");
    return NULL;
  }
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  sink.callable = sink_object;
  status = archbird_project_render_map(owned->engine, owned->project,
                                       pretty ? ARCHBIRD_JSON_PRETTY : 0,
                                       sink_write, &sink);
  if (sink.error_type) {
    PyErr_Restore(sink.error_type, sink.error_value, sink.error_traceback);
    return NULL;
  }
  if (status != ARCHBIRD_OK)
    return raise_status(owned->engine, status);
  Py_RETURN_NONE;
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

static PyObject *py_plan_validate(PyObject *self, PyObject *args) {
  const char *input;
  Py_ssize_t input_length;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  (void)self;
  if (!PyArg_ParseTuple(args, "y#:plan_validate", &input, &input_length))
    return NULL;
  status = saved_artifact_engine((size_t)input_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_plan_validate(engine, (const uint8_t *)input,
                                    (size_t)input_length);
  if (status != ARCHBIRD_OK) {
    PyObject *result = raise_status(engine, status);
    archbird_engine_destroy(engine);
    return result;
  }
  archbird_engine_destroy(engine);
  Py_RETURN_NONE;
}

static PyObject *py_plan_render_markdown(PyObject *self, PyObject *args) {
  const char *input;
  Py_ssize_t input_length;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTuple(args, "y#:plan_render_markdown", &input, &input_length))
    return NULL;
  status = saved_artifact_engine((size_t)input_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_plan_render_markdown(engine, (const uint8_t *)input,
                                           (size_t)input_length, output_write,
                                           &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_plan_compile(PyObject *self, PyObject *args,
                                 PyObject *kwargs) {
  static char *keywords[] = {"project",
                             "map_json",
                             "verification_json",
                             "before_map_json",
                             "request_json",
                             "pretty",
                             NULL};
  PyObject *capsule;
  const char *map;
  const char *verification;
  const char *before_map = "";
  const char *request = "";
  Py_ssize_t map_length;
  Py_ssize_t verification_length;
  Py_ssize_t before_map_length = 0;
  Py_ssize_t request_length = 0;
  int pretty = 0;
  PyArchbirdProject *owned;
  PyOutput output = {0};
  ArchbirdStatus status;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "Oy#y#|y#y#p:plan_compile", keywords, &capsule, &map,
          &map_length, &verification, &verification_length, &before_map,
          &before_map_length, &request, &request_length, &pretty))
    return NULL;
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  status = archbird_plan_compile(
      owned->engine, owned->project, (const uint8_t *)map, (size_t)map_length,
      before_map_length ? (const uint8_t *)before_map : NULL,
      (size_t)before_map_length, (const uint8_t *)verification,
      (size_t)verification_length,
      request_length ? (const uint8_t *)request : NULL, (size_t)request_length,
      pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  return render_result(owned->engine, status, &output);
}

static PyObject *py_act_validate(PyObject *self, PyObject *args) {
  const char *input;
  Py_ssize_t input_length;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  (void)self;
  if (!PyArg_ParseTuple(args, "y#:act_validate", &input, &input_length))
    return NULL;
  status = saved_artifact_engine((size_t)input_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_act_validate(engine, (const uint8_t *)input,
                                   (size_t)input_length);
  if (status != ARCHBIRD_OK) {
    PyObject *result = raise_status(engine, status);
    archbird_engine_destroy(engine);
    return result;
  }
  archbird_engine_destroy(engine);
  Py_RETURN_NONE;
}

static PyObject *py_plan_source_requirements(PyObject *self, PyObject *args,
                                             PyObject *kwargs) {
  static char *keywords[] = {"plan_json", "executor_submissions_json", "pretty",
                             NULL};
  const char *plan;
  const char *submissions = "";
  Py_ssize_t plan_length;
  Py_ssize_t submissions_length = 0;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  PyOutput output = {0};
  PyObject *result;
  ArchbirdStatus status;
  size_t budget;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "y#|y#p:plan_source_requirements", keywords, &plan,
          &plan_length, &submissions, &submissions_length, &pretty))
    return NULL;
  budget = larger_input((size_t)plan_length, (size_t)submissions_length);
  status = saved_artifact_engine(budget, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_plan_source_requirements(
        engine, (const uint8_t *)plan, (size_t)plan_length,
        (const uint8_t *)submissions, (size_t)submissions_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_act_source_requirements(PyObject *self, PyObject *args,
                                            PyObject *kwargs) {
  static char *keywords[] = {"act_json", "pretty", NULL};
  const char *act;
  Py_ssize_t act_length;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  PyOutput output = {0};
  PyObject *result;
  ArchbirdStatus status;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#|p:act_source_requirements",
                                   keywords, &act, &act_length, &pretty))
    return NULL;
  status = saved_artifact_engine((size_t)act_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_act_source_requirements(
        engine, (const uint8_t *)act, (size_t)act_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_act_materialize(PyObject *self, PyObject *args,
                                    PyObject *kwargs) {
  static char *keywords[] = {"project",
                             "plan_json",
                             "map_json",
                             "verification_json",
                             "source_metadata_json",
                             "executor_submissions_json",
                             "pretty",
                             NULL};
  PyObject *capsule;
  const char *plan;
  const char *map;
  const char *verification;
  const char *metadata;
  const char *submissions;
  Py_ssize_t plan_length;
  Py_ssize_t map_length;
  Py_ssize_t verification_length;
  Py_ssize_t metadata_length;
  Py_ssize_t submissions_length;
  int pretty = 0;
  PyArchbirdProject *owned;
  PyOutput output = {0};
  ArchbirdStatus status;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "Oy#y#y#y#y#|p:act_materialize", keywords, &capsule,
          &plan, &plan_length, &map, &map_length, &verification,
          &verification_length, &metadata, &metadata_length, &submissions,
          &submissions_length, &pretty))
    return NULL;
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  status = archbird_act_materialize(
      owned->engine, owned->project, (const uint8_t *)plan, (size_t)plan_length,
      (const uint8_t *)map, (size_t)map_length, (const uint8_t *)verification,
      (size_t)verification_length, (const uint8_t *)metadata,
      (size_t)metadata_length, (const uint8_t *)submissions,
      (size_t)submissions_length, pretty ? ARCHBIRD_JSON_PRETTY : 0,
      output_write, &output);
  return render_result(owned->engine, status, &output);
}

static PyObject *py_act_accept(PyObject *self, PyObject *args,
                               PyObject *kwargs) {
  static char *keywords[] = {"act_json",
                             "before_map_json",
                             "after_map_json",
                             "verification_json",
                             "gate_results_json",
                             "pretty",
                             NULL};
  const char *act;
  const char *before_map;
  const char *after_map;
  const char *verification;
  const char *gate_results;
  Py_ssize_t act_length;
  Py_ssize_t before_map_length;
  Py_ssize_t after_map_length;
  Py_ssize_t verification_length;
  Py_ssize_t gate_results_length;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  PyOutput output = {0};
  PyObject *result;
  ArchbirdStatus status;
  size_t budget;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "y#y#y#y#y#|p:act_accept", keywords, &act, &act_length,
          &before_map, &before_map_length, &after_map, &after_map_length,
          &verification, &verification_length, &gate_results,
          &gate_results_length, &pretty))
    return NULL;
  budget =
      larger_input(larger_input((size_t)act_length, (size_t)before_map_length),
                   larger_input(larger_input((size_t)after_map_length,
                                             (size_t)verification_length),
                                (size_t)gate_results_length));
  status = saved_artifact_engine(budget, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_act_accept(
        engine, (const uint8_t *)act, (size_t)act_length,
        (const uint8_t *)before_map, (size_t)before_map_length,
        (const uint8_t *)after_map, (size_t)after_map_length,
        (const uint8_t *)verification, (size_t)verification_length,
        (const uint8_t *)gate_results, (size_t)gate_results_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_act_preflight_apply(PyObject *self, PyObject *args) {
  const char *act;
  const char *metadata;
  Py_ssize_t act_length;
  Py_ssize_t metadata_length;
  ArchbirdEngine *engine = NULL;
  ArchbirdActApplyState apply_state = ARCHBIRD_ACT_APPLY_READY;
  ArchbirdStatus status;
  (void)self;
  if (!PyArg_ParseTuple(args, "y#y#:act_preflight_apply", &act, &act_length,
                        &metadata, &metadata_length))
    return NULL;
  status = saved_artifact_engine(
      larger_input((size_t)act_length, (size_t)metadata_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_act_preflight_apply(
        engine, (const uint8_t *)act, (size_t)act_length,
        (const uint8_t *)metadata, (size_t)metadata_length, &apply_state);
  if (status != ARCHBIRD_OK) {
    PyObject *result = raise_status(engine, status);
    archbird_engine_destroy(engine);
    return result;
  }
  archbird_engine_destroy(engine);
  return PyLong_FromLong((long)apply_state);
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
  static char *keywords[] = {"map", "query", "pretty", "resolution", NULL};
  const char *map;
  const char *query;
  const char *resolution = "";
  Py_ssize_t map_length;
  Py_ssize_t query_length;
  Py_ssize_t resolution_length = 0;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#y#|py#:map_query", keywords,
                                   &map, &map_length, &query, &query_length,
                                   &pretty, &resolution, &resolution_length))
    return NULL;
  status = saved_artifact_engine(
      larger_input(larger_input((size_t)map_length, (size_t)query_length),
                   (size_t)resolution_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_query(
        engine, (const uint8_t *)map, (size_t)map_length,
        resolution_length ? (const uint8_t *)resolution : NULL,
        (size_t)resolution_length, (const uint8_t *)query, (size_t)query_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_map_path(PyObject *self, PyObject *args, PyObject *kwargs) {
  static char *keywords[] = {"map", "request", "pretty", "resolution", NULL};
  const char *map;
  const char *request;
  const char *resolution = "";
  Py_ssize_t map_length;
  Py_ssize_t request_length;
  Py_ssize_t resolution_length = 0;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#y#|py#:map_path", keywords,
                                   &map, &map_length, &request, &request_length,
                                   &pretty, &resolution, &resolution_length))
    return NULL;
  status = saved_artifact_engine(
      larger_input(larger_input((size_t)map_length, (size_t)request_length),
                   (size_t)resolution_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_path(
        engine, (const uint8_t *)map, (size_t)map_length,
        resolution_length ? (const uint8_t *)resolution : NULL,
        (size_t)resolution_length, (const uint8_t *)request,
        (size_t)request_length, pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write,
        &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_path_render_markdown(PyObject *self, PyObject *args,
                                         PyObject *kwargs) {
  static char *keywords[] = {"artifact", "max_chars", NULL};
  const char *artifact;
  Py_ssize_t artifact_length;
  Py_ssize_t max_chars = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#|n:path_render_markdown",
                                   keywords, &artifact, &artifact_length,
                                   &max_chars))
    return NULL;
  if (max_chars < 0) {
    PyErr_SetString(PyExc_ValueError,
                    "path max_chars must be a nonnegative integer");
    return NULL;
  }
  status = saved_artifact_engine((size_t)artifact_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_path_render_markdown(
        engine, (const uint8_t *)artifact, (size_t)artifact_length,
        (size_t)max_chars, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_map_path_markdown(PyObject *self, PyObject *args,
                                      PyObject *kwargs) {
  static char *keywords[] = {"map", "request", "max_chars", "resolution", NULL};
  const char *map;
  const char *request;
  const char *resolution = "";
  Py_ssize_t map_length;
  Py_ssize_t request_length;
  Py_ssize_t resolution_length = 0;
  Py_ssize_t max_chars = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#y#|ny#:map_path_markdown",
                                   keywords, &map, &map_length, &request,
                                   &request_length, &max_chars, &resolution,
                                   &resolution_length))
    return NULL;
  if (max_chars < 0) {
    PyErr_SetString(PyExc_ValueError,
                    "path max_chars must be a nonnegative integer");
    return NULL;
  }
  status = saved_artifact_engine(
      larger_input(larger_input((size_t)map_length, (size_t)request_length),
                   (size_t)resolution_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_path_markdown(
        engine, (const uint8_t *)map, (size_t)map_length,
        resolution_length ? (const uint8_t *)resolution : NULL,
        (size_t)resolution_length, (const uint8_t *)request,
        (size_t)request_length, (size_t)max_chars, output_write, &output);
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
  static char *keywords[] = {"map", "query", "max_chars", "resolution", NULL};
  const char *map;
  const char *query;
  const char *resolution = "";
  Py_ssize_t map_length;
  Py_ssize_t query_length;
  Py_ssize_t resolution_length = 0;
  Py_ssize_t max_chars = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#y#|ny#:map_query_markdown",
                                   keywords, &map, &map_length, &query,
                                   &query_length, &max_chars, &resolution,
                                   &resolution_length))
    return NULL;
  if (max_chars < 0) {
    PyErr_SetString(PyExc_ValueError,
                    "query max_chars must be a nonnegative integer");
    return NULL;
  }
  status = saved_artifact_engine(
      larger_input(larger_input((size_t)map_length, (size_t)query_length),
                   (size_t)resolution_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_query_markdown(
        engine, (const uint8_t *)map, (size_t)map_length,
        resolution_length ? (const uint8_t *)resolution : NULL,
        (size_t)resolution_length, (const uint8_t *)query, (size_t)query_length,
        (size_t)max_chars, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_map_query_markdown_view(PyObject *self, PyObject *args,
                                            PyObject *kwargs) {
  static char *keywords[] = {"map",        "query",     "view",
                             "detail",     "max_chars", "verification",
                             "resolution", NULL};
  const char *map;
  const char *query;
  const char *verification = NULL;
  const char *resolution = "";
  Py_ssize_t map_length;
  Py_ssize_t query_length;
  Py_ssize_t verification_length = 0;
  Py_ssize_t resolution_length = 0;
  Py_ssize_t max_chars = 0;
  int view;
  int detail;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "y#y#ii|ny#y#:map_query_markdown_view", keywords, &map,
          &map_length, &query, &query_length, &view, &detail, &max_chars,
          &verification, &verification_length, &resolution, &resolution_length))
    return NULL;
  if (max_chars < 0) {
    PyErr_SetString(PyExc_ValueError,
                    "query max_chars must be a nonnegative integer");
    return NULL;
  }
  status = saved_artifact_engine(
      larger_input(
          larger_input(larger_input((size_t)map_length, (size_t)query_length),
                       (size_t)verification_length),
          (size_t)resolution_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_query_markdown_view_with_verification(
        engine, (const uint8_t *)map, (size_t)map_length,
        resolution_length ? (const uint8_t *)resolution : NULL,
        (size_t)resolution_length, (const uint8_t *)query, (size_t)query_length,
        verification_length ? (const uint8_t *)verification : NULL,
        (size_t)verification_length, (ArchbirdQueryView)view,
        (ArchbirdReportDetail)detail, (size_t)max_chars, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_project_source_markdown(PyObject *self, PyObject *args,
                                            PyObject *kwargs) {
  static char *keywords[] = {"project", "artifact", "detail", "max_chars",
                             NULL};
  PyObject *capsule;
  PyArchbirdProject *owned;
  const char *artifact;
  Py_ssize_t artifact_length;
  Py_ssize_t max_chars = 0;
  int detail = ARCHBIRD_REPORT_DETAIL_STANDARD;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "Oy#|in:project_source_markdown", keywords, &capsule,
          &artifact, &artifact_length, &detail, &max_chars))
    return NULL;
  if (max_chars < 0) {
    PyErr_SetString(PyExc_ValueError,
                    "source max_chars must be a nonnegative integer");
    return NULL;
  }
  owned = get_project(capsule);
  if (!owned)
    return NULL;
  status = saved_artifact_engine((size_t)artifact_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_project_render_source_markdown(
        engine, owned->project, (const uint8_t *)artifact,
        (size_t)artifact_length, (ArchbirdReportDetail)detail,
        (size_t)max_chars, output_write, &output);
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

static PyObject *py_unified_diff(PyObject *self, PyObject *args,
                                 PyObject *kwargs) {
  static char *keywords[] = {
      "before",   "after",         "before_path",    "after_path",
      "metadata", "context_lines", "max_work_bytes", NULL,
  };
  const char *before;
  const char *after;
  const char *before_path;
  const char *after_path;
  const char *metadata = NULL;
  Py_ssize_t before_length;
  Py_ssize_t after_length;
  Py_ssize_t before_path_length;
  Py_ssize_t after_path_length;
  Py_ssize_t metadata_length = 0;
  Py_ssize_t context_lines = 3;
  Py_ssize_t max_work_bytes = 16 * 1024 * 1024;
  ArchbirdUnifiedDiffOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "y#y#z#z#|y#nn:unified_diff", keywords, &before,
          &before_length, &after, &after_length, &before_path,
          &before_path_length, &after_path, &after_path_length, &metadata,
          &metadata_length, &context_lines, &max_work_bytes))
    return NULL;
  if (context_lines < 0 || max_work_bytes <= 0) {
    PyErr_SetString(PyExc_ValueError,
                    "context_lines must be nonnegative and max_work_bytes "
                    "must be positive");
    return NULL;
  }
  archbird_unified_diff_options_init(&options);
  options.context_lines = (size_t)context_lines;
  options.max_work_bytes = (size_t)max_work_bytes;
  options.metadata = (const uint8_t *)metadata;
  options.metadata_length = (size_t)metadata_length;
  status = input_engine(
      larger_input((size_t)before_length, (size_t)after_length), &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_unified_diff(
        engine, (const uint8_t *)before, (size_t)before_length,
        (const uint8_t *)after, (size_t)after_length, before_path,
        (size_t)before_path_length, after_path, (size_t)after_path_length,
        &options, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_json_pointer_edit(PyObject *self, PyObject *args,
                                      PyObject *kwargs) {
  static char *keywords[] = {"source",   "source_sha256", "pointer",
                             "expected", "replacement",   NULL};
  const char *source;
  const char *source_sha256;
  const char *pointer;
  const char *replacement;
  Py_ssize_t source_length;
  Py_ssize_t source_sha256_length;
  Py_ssize_t pointer_length;
  Py_ssize_t replacement_length;
  PyObject *expected_object;
  Py_buffer expected = {0};
  int expected_absent;
  ArchbirdJsonPointerEditOptions options;
  ArchbirdJsonPointerEditResult edit_result;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *replacement_bytes;
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "y#s#s#Oy#:json_pointer_edit", keywords, &source,
          &source_length, &source_sha256, &source_sha256_length, &pointer,
          &pointer_length, &expected_object, &replacement, &replacement_length))
    return NULL;
  expected_absent = expected_object == Py_None;
  if (!expected_absent &&
      PyObject_GetBuffer(expected_object, &expected, PyBUF_SIMPLE) != 0)
    return NULL;
  archbird_json_pointer_edit_options_init(&options);
  options.source_sha256 = source_sha256;
  options.source_sha256_length = (size_t)source_sha256_length;
  options.pointer = (const uint8_t *)pointer;
  options.pointer_length = (size_t)pointer_length;
  options.expected_absent = expected_absent;
  options.expected_json =
      expected_absent ? NULL : (const uint8_t *)expected.buf;
  options.expected_json_length = expected_absent ? 0 : (size_t)expected.len;
  options.replacement_json = (const uint8_t *)replacement;
  options.replacement_json_length = (size_t)replacement_length;
  archbird_json_pointer_edit_result_init(&edit_result);
  status = input_engine((size_t)source_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_json_pointer_edit(engine, (const uint8_t *)source,
                                        (size_t)source_length, &options,
                                        &edit_result, output_write, &output);
  if (!expected_absent)
    PyBuffer_Release(&expected);
  replacement_bytes = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  if (!replacement_bytes)
    return NULL;
  result = Py_BuildValue(
      "{s:n,s:n,s:n,s:s,s:N}", "start_byte", (Py_ssize_t)edit_result.start_byte,
      "end_byte", (Py_ssize_t)edit_result.end_byte, "matched_values",
      (Py_ssize_t)edit_result.matched_values, "kind",
      edit_result.kind == ARCHBIRD_JSON_POINTER_INSERT ? "insert" : "replace",
      "replacement", replacement_bytes);
  return result;
}

static PyObject *py_make_variable_token_edit(PyObject *self, PyObject *args,
                                             PyObject *kwargs) {
  static char *keywords[] = {"source",         "source_sha256",     "variable",
                             "expected_token", "replacement_token", NULL};
  const char *source;
  const char *source_sha256;
  const char *variable;
  const char *expected_token;
  const char *replacement_token;
  Py_ssize_t source_length;
  Py_ssize_t source_sha256_length;
  Py_ssize_t variable_length;
  Py_ssize_t expected_token_length;
  Py_ssize_t replacement_token_length;
  ArchbirdMakeVariableTokenEditOptions options;
  ArchbirdMakeVariableTokenEditResult edit_result;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *replacement_bytes;
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "y#s#s#s#s#:make_variable_token_edit", keywords,
          &source, &source_length, &source_sha256, &source_sha256_length,
          &variable, &variable_length, &expected_token, &expected_token_length,
          &replacement_token, &replacement_token_length))
    return NULL;
  archbird_make_variable_token_edit_options_init(&options);
  options.source_sha256 = source_sha256;
  options.source_sha256_length = (size_t)source_sha256_length;
  options.variable = (const uint8_t *)variable;
  options.variable_length = (size_t)variable_length;
  options.expected_token = (const uint8_t *)expected_token;
  options.expected_token_length = (size_t)expected_token_length;
  options.replacement_token = (const uint8_t *)replacement_token;
  options.replacement_token_length = (size_t)replacement_token_length;
  archbird_make_variable_token_edit_result_init(&edit_result);
  status = input_engine((size_t)source_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_make_variable_token_edit(
        engine, (const uint8_t *)source, (size_t)source_length, &options,
        &edit_result, output_write, &output);
  replacement_bytes = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  if (!replacement_bytes)
    return NULL;
  result = Py_BuildValue(
      "{s:n,s:n,s:n,s:N}", "start_byte", (Py_ssize_t)edit_result.start_byte,
      "end_byte", (Py_ssize_t)edit_result.end_byte, "matched_tokens",
      (Py_ssize_t)edit_result.matched_tokens, "replacement", replacement_bytes);
  return result;
}

static PyObject *py_make_variable_token_insert(PyObject *self, PyObject *args,
                                               PyObject *kwargs) {
  static char *keywords[] = {"source", "source_sha256", "variable",
                             "token",  "anchor_token",  "position",
                             NULL};
  const char *source;
  const char *source_sha256;
  const char *variable;
  const char *token;
  const char *anchor_token;
  const char *position;
  Py_ssize_t source_length;
  Py_ssize_t source_sha256_length;
  Py_ssize_t variable_length;
  Py_ssize_t token_length;
  Py_ssize_t anchor_token_length;
  Py_ssize_t position_length;
  ArchbirdMakeVariableTokenInsertOptions options;
  ArchbirdMakeVariableTokenInsertResult insert_result;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *replacement_bytes;
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "y#s#s#s#s#s#:make_variable_token_insert", keywords,
          &source, &source_length, &source_sha256, &source_sha256_length,
          &variable, &variable_length, &token, &token_length, &anchor_token,
          &anchor_token_length, &position, &position_length))
    return NULL;
  archbird_make_variable_token_insert_options_init(&options);
  options.source_sha256 = source_sha256;
  options.source_sha256_length = (size_t)source_sha256_length;
  options.variable = (const uint8_t *)variable;
  options.variable_length = (size_t)variable_length;
  options.token = (const uint8_t *)token;
  options.token_length = (size_t)token_length;
  options.anchor_token = (const uint8_t *)anchor_token;
  options.anchor_token_length = (size_t)anchor_token_length;
  if (position_length == 6 && memcmp(position, "before", 6) == 0)
    options.position = ARCHBIRD_MAKE_TOKEN_BEFORE;
  else if (position_length == 5 && memcmp(position, "after", 5) == 0)
    options.position = ARCHBIRD_MAKE_TOKEN_AFTER;
  else {
    PyErr_SetString(PyExc_ValueError, "position must be before or after");
    return NULL;
  }
  archbird_make_variable_token_insert_result_init(&insert_result);
  status = input_engine((size_t)source_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_make_variable_token_insert(
        engine, (const uint8_t *)source, (size_t)source_length, &options,
        &insert_result, output_write, &output);
  replacement_bytes = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  if (!replacement_bytes)
    return NULL;
  result =
      Py_BuildValue("{s:n,s:n,s:n,s:n,s:N}", "start_byte",
                    (Py_ssize_t)insert_result.start_byte, "end_byte",
                    (Py_ssize_t)insert_result.end_byte, "matched_tokens",
                    (Py_ssize_t)insert_result.matched_tokens, "matched_anchors",
                    (Py_ssize_t)insert_result.matched_anchors, "replacement",
                    replacement_bytes);
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
  static char *keywords[] = {"map", "verification", "normalization", "pretty",
                             NULL};
  const char *map;
  const char *verification;
  const char *normalization;
  Py_ssize_t map_length;
  Py_ssize_t verification_length;
  Py_ssize_t normalization_length;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  PyOutput output = {0};
  PyObject *rendered;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#y#y#|p:okf_publish",
                                   keywords, &map, &map_length, &verification,
                                   &verification_length, &normalization,
                                   &normalization_length, &pretty))
    return NULL;
  status = saved_artifact_engine(
      larger_input(
          larger_input((size_t)map_length, (size_t)verification_length),
          (size_t)normalization_length),
      &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_okf_publish(
        engine, (const uint8_t *)map, (size_t)map_length,
        verification_length ? (const uint8_t *)verification : NULL,
        (size_t)verification_length,
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

static PyObject *py_project_configuration_compile(PyObject *self,
                                                  PyObject *args,
                                                  PyObject *kwargs) {
  static char *keywords[] = {"config", "pretty", NULL};
  const char *config;
  Py_ssize_t config_length;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  PyOutput output = {0};
  ArchbirdStatus status;
  PyObject *result;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                   "y#|p:project_configuration_compile",
                                   keywords, &config, &config_length, &pretty))
    return NULL;
  status = input_engine((size_t)config_length, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_project_configuration_compile(
        engine, (const uint8_t *)config, (size_t)config_length,
        pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_projection_evaluate(PyObject *self, PyObject *args,
                                        PyObject *kwargs) {
  static char *keywords[] = {"map_json", "projection_json", "resolution_json",
                             "pretty", NULL};
  const char *map;
  const char *projection;
  const char *resolution = "";
  Py_ssize_t map_length;
  Py_ssize_t projection_length;
  Py_ssize_t resolution_length = 0;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  PyOutput output = {0};
  ArchbirdStatus status;
  PyObject *result;
  size_t budget;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#y#|y#p:projection_evaluate",
                                   keywords, &map, &map_length, &projection,
                                   &projection_length, &resolution,
                                   &resolution_length, &pretty))
    return NULL;
  budget =
      larger_input(larger_input((size_t)map_length, (size_t)resolution_length),
                   (size_t)projection_length);
  status = saved_artifact_engine(budget, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_projection_evaluate(
        engine, (const uint8_t *)map, (size_t)map_length,
        resolution_length ? (const uint8_t *)resolution : NULL,
        (size_t)resolution_length, (const uint8_t *)projection,
        (size_t)projection_length, pretty ? ARCHBIRD_JSON_PRETTY : 0,
        output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_projection_render_markdown(PyObject *self, PyObject *args,
                                               PyObject *kwargs) {
  static char *keywords[] = {"map_json", "projection_json", "resolution_json",
                             "detail",   "max_chars",       NULL};
  const char *map;
  const char *projection;
  const char *resolution = "";
  Py_ssize_t map_length;
  Py_ssize_t projection_length;
  Py_ssize_t resolution_length = 0;
  int detail = ARCHBIRD_REPORT_DETAIL_STANDARD;
  Py_ssize_t max_chars = 0;
  ArchbirdEngine *engine = NULL;
  PyOutput output = {0};
  ArchbirdStatus status;
  PyObject *result;
  size_t budget;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "y#y#|y#in:projection_render_markdown", keywords, &map,
          &map_length, &projection, &projection_length, &resolution,
          &resolution_length, &detail, &max_chars))
    return NULL;
  if (max_chars < 0) {
    PyErr_SetString(PyExc_ValueError, "max_chars must be nonnegative");
    return NULL;
  }
  budget =
      larger_input(larger_input((size_t)map_length, (size_t)resolution_length),
                   (size_t)projection_length);
  status = saved_artifact_engine(budget, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_projection_render_markdown(
        engine, (const uint8_t *)map, (size_t)map_length,
        resolution_length ? (const uint8_t *)resolution : NULL,
        (size_t)resolution_length, (const uint8_t *)projection,
        (size_t)projection_length, (ArchbirdReportDetail)detail,
        (size_t)max_chars, output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_query_plan_compile(PyObject *self, PyObject *args,
                                       PyObject *kwargs) {
  static char *keywords[] = {"config", "query_id", "overrides_json", "pretty",
                             NULL};
  const char *config;
  const char *query_id;
  const char *overrides = "";
  Py_ssize_t config_length;
  Py_ssize_t query_id_length;
  Py_ssize_t overrides_length = 0;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  PyOutput output = {0};
  ArchbirdStatus status;
  PyObject *result;
  size_t budget;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "y#s#|y#p:query_plan_compile",
                                   keywords, &config, &config_length, &query_id,
                                   &query_id_length, &overrides,
                                   &overrides_length, &pretty))
    return NULL;
  budget = larger_input((size_t)config_length, (size_t)overrides_length);
  status = saved_artifact_engine(budget, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_query_plan_compile(
        engine, (const uint8_t *)config, (size_t)config_length, query_id,
        (size_t)query_id_length,
        overrides_length ? (const uint8_t *)overrides : NULL,
        (size_t)overrides_length, pretty ? ARCHBIRD_JSON_PRETTY : 0,
        output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyObject *py_constraints_evaluate(PyObject *self, PyObject *args,
                                         PyObject *kwargs) {
  static char *keywords[] = {"config",       "map_json", "resolution_json",
                             "request_json", "pretty",   NULL};
  const char *config;
  const char *map;
  const char *resolution = "";
  const char *request = "";
  Py_ssize_t config_length;
  Py_ssize_t map_length;
  Py_ssize_t resolution_length = 0;
  Py_ssize_t request_length = 0;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  PyOutput output = {0};
  ArchbirdStatus status;
  PyObject *result;
  size_t budget;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "y#y#|y#y#p:constraints_evaluate", keywords, &config,
          &config_length, &map, &map_length, &resolution, &resolution_length,
          &request, &request_length, &pretty))
    return NULL;
  budget = larger_input(
      larger_input((size_t)config_length, (size_t)map_length),
      larger_input((size_t)resolution_length, (size_t)request_length));
  status = saved_artifact_engine(budget, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_constraints_evaluate(
        engine, (const uint8_t *)config, (size_t)config_length,
        (const uint8_t *)map, (size_t)map_length,
        resolution_length ? (const uint8_t *)resolution : NULL,
        (size_t)resolution_length,
        request_length ? (const uint8_t *)request : NULL,
        (size_t)request_length, pretty ? ARCHBIRD_JSON_PRETTY : 0, output_write,
        &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static int constraint_format(const char *format,
                             ArchbirdVerificationFormat *out) {
  if (!strcmp(format, "markdown"))
    *out = ARCHBIRD_VERIFICATION_MARKDOWN;
  else if (!strcmp(format, "sarif"))
    *out = ARCHBIRD_VERIFICATION_SARIF;
  else if (!strcmp(format, "junit"))
    *out = ARCHBIRD_VERIFICATION_JUNIT;
  else
    return 0;
  return 1;
}

static PyObject *constraints_report_common(PyObject *args, PyObject *kwargs,
                                           int include_blocking) {
  static char *keywords[] = {
      "config",       "map_json",     "format", "resolution_json",
      "request_json", "max_findings", "pretty", NULL};
  const char *config;
  const char *map;
  const char *format;
  const char *resolution = "";
  const char *request = "";
  Py_ssize_t config_length;
  Py_ssize_t map_length;
  Py_ssize_t resolution_length = 0;
  Py_ssize_t request_length = 0;
  Py_ssize_t max_findings = 200;
  int pretty = 0;
  int blocking = 0;
  ArchbirdVerificationFormat native_format;
  ArchbirdEngine *engine = NULL;
  PyOutput output = {0};
  ArchbirdStatus status;
  PyObject *result;
  PyObject *blocking_result;
  PyObject *combined;
  size_t budget;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "y#y#s|y#y#np:constraints_report", keywords, &config,
          &config_length, &map, &map_length, &format, &resolution,
          &resolution_length, &request, &request_length, &max_findings,
          &pretty))
    return NULL;
  if (!constraint_format(format, &native_format)) {
    PyErr_SetString(PyExc_ValueError,
                    "constraint report format must be markdown, sarif, or "
                    "junit");
    return NULL;
  }
  budget = larger_input(
      larger_input((size_t)config_length, (size_t)map_length),
      larger_input((size_t)resolution_length, (size_t)request_length));
  status = saved_artifact_engine(budget, &engine);
  if (status == ARCHBIRD_OK) {
    uint32_t flags = (pretty ? ARCHBIRD_JSON_PRETTY : 0) |
                     (native_format == ARCHBIRD_VERIFICATION_SARIF
                          ? ARCHBIRD_JSON_TRAILING_NEWLINE
                          : 0);
    size_t finding_limit = max_findings < 0 ? SIZE_MAX : (size_t)max_findings;
    if (include_blocking)
      status = archbird_constraints_report_with_blocking(
          engine, (const uint8_t *)config, (size_t)config_length,
          (const uint8_t *)map, (size_t)map_length,
          resolution_length ? (const uint8_t *)resolution : NULL,
          (size_t)resolution_length,
          request_length ? (const uint8_t *)request : NULL,
          (size_t)request_length, native_format, finding_limit, flags,
          &blocking, output_write, &output);
    else
      status = archbird_constraints_report(
          engine, (const uint8_t *)config, (size_t)config_length,
          (const uint8_t *)map, (size_t)map_length,
          resolution_length ? (const uint8_t *)resolution : NULL,
          (size_t)resolution_length,
          request_length ? (const uint8_t *)request : NULL,
          (size_t)request_length, native_format, finding_limit, flags,
          output_write, &output);
  }
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  if (!include_blocking || !result)
    return result;
  blocking_result = PyBool_FromLong(blocking);
  if (!blocking_result) {
    Py_DECREF(result);
    return NULL;
  }
  combined = PyTuple_Pack(2, result, blocking_result);
  Py_DECREF(blocking_result);
  Py_DECREF(result);
  return combined;
}

static PyObject *py_constraints_report(PyObject *self, PyObject *args,
                                       PyObject *kwargs) {
  (void)self;
  return constraints_report_common(args, kwargs, 0);
}

static PyObject *py_constraints_report_with_blocking(PyObject *self,
                                                     PyObject *args,
                                                     PyObject *kwargs) {
  (void)self;
  return constraints_report_common(args, kwargs, 1);
}

static PyObject *py_constraints_freeze(PyObject *self, PyObject *args,
                                       PyObject *kwargs) {
  static char *keywords[] = {
      "config",          "map_json",     "owner",  "rationale",
      "resolution_json", "request_json", "pretty", NULL};
  const char *config;
  const char *map;
  const char *owner;
  const char *rationale;
  const char *resolution = "";
  const char *request = "";
  Py_ssize_t config_length;
  Py_ssize_t map_length;
  Py_ssize_t owner_length;
  Py_ssize_t rationale_length;
  Py_ssize_t resolution_length = 0;
  Py_ssize_t request_length = 0;
  int pretty = 0;
  ArchbirdEngine *engine = NULL;
  PyOutput output = {0};
  ArchbirdStatus status;
  PyObject *result;
  size_t budget;
  (void)self;
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "y#y#s#s#|y#y#p:constraints_freeze", keywords, &config,
          &config_length, &map, &map_length, &owner, &owner_length, &rationale,
          &rationale_length, &resolution, &resolution_length, &request,
          &request_length, &pretty))
    return NULL;
  budget = larger_input(
      larger_input((size_t)config_length, (size_t)map_length),
      larger_input((size_t)resolution_length, (size_t)request_length));
  status = saved_artifact_engine(budget, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_constraints_freeze(
        engine, (const uint8_t *)config, (size_t)config_length,
        (const uint8_t *)map, (size_t)map_length,
        resolution_length ? (const uint8_t *)resolution : NULL,
        (size_t)resolution_length,
        request_length ? (const uint8_t *)request : NULL,
        (size_t)request_length, owner, (size_t)owner_length, rationale,
        (size_t)rationale_length,
        (pretty ? ARCHBIRD_JSON_PRETTY : 0) | ARCHBIRD_JSON_TRAILING_NEWLINE,
        output_write, &output);
  result = render_result(engine, status, &output);
  archbird_engine_destroy(engine);
  return result;
}

static PyMethodDef archbird_methods[] = {
    {"constraints_freeze", (PyCFunction)py_constraints_freeze,
     METH_VARARGS | METH_KEYWORDS, "Freeze a reviewed constraint baseline."},
    {"constraints_report", (PyCFunction)py_constraints_report,
     METH_VARARGS | METH_KEYWORDS, "Render project constraint results."},
    {"constraints_report_with_blocking",
     (PyCFunction)py_constraints_report_with_blocking,
     METH_VARARGS | METH_KEYWORDS,
     "Render project constraints and return their blocking state."},
    {"constraints_evaluate", (PyCFunction)py_constraints_evaluate,
     METH_VARARGS | METH_KEYWORDS, "Evaluate project constraints."},
    {"query_plan_compile", (PyCFunction)py_query_plan_compile,
     METH_VARARGS | METH_KEYWORDS, "Compile one named query."},
    {"projection_evaluate", (PyCFunction)py_projection_evaluate,
     METH_VARARGS | METH_KEYWORDS, "Evaluate one exhaustive projection."},
    {"projection_render_markdown", (PyCFunction)py_projection_render_markdown,
     METH_VARARGS | METH_KEYWORDS, "Render one graph projection as Markdown."},
    {"project_configuration_compile",
     (PyCFunction)py_project_configuration_compile,
     METH_VARARGS | METH_KEYWORDS,
     "Validate and normalize project configuration."},
    {"workspace_analyze", (PyCFunction)py_workspace_analyze,
     METH_VARARGS | METH_KEYWORDS,
     "Join canonical project maps into a workspace artifact."},
    {"workspace_plan", (PyCFunction)py_workspace_plan,
     METH_VARARGS | METH_KEYWORDS,
     "Validate and expose a workspace host-loading plan."},
    {"map_diff", (PyCFunction)py_map_diff, METH_VARARGS | METH_KEYWORDS,
     "Structurally diff two canonical saved maps."},
    {"unified_diff", (PyCFunction)py_unified_diff, METH_VARARGS | METH_KEYWORDS,
     "Render one deterministic git-style unified diff."},
    {"json_pointer_edit", (PyCFunction)py_json_pointer_edit,
     METH_VARARGS | METH_KEYWORDS,
     "Preview one source-locked JSON Pointer edit."},
    {"make_variable_token_edit", (PyCFunction)py_make_variable_token_edit,
     METH_VARARGS | METH_KEYWORDS,
     "Preview one source-locked Make variable token edit."},
    {"make_variable_token_insert", (PyCFunction)py_make_variable_token_insert,
     METH_VARARGS | METH_KEYWORDS,
     "Preview one source-locked Make variable token insertion."},
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
    {"map_path", (PyCFunction)py_map_path, METH_VARARGS | METH_KEYWORDS,
     "Find typed connection paths in a canonical saved map."},
    {"path_render_markdown", (PyCFunction)py_path_render_markdown,
     METH_VARARGS | METH_KEYWORDS,
     "Render one canonical Path artifact as deterministic Markdown."},
    {"map_path_markdown", (PyCFunction)py_map_path_markdown,
     METH_VARARGS | METH_KEYWORDS,
     "Find typed connection paths and render deterministic Markdown."},
    {"map_query_markdown", (PyCFunction)py_map_query_markdown,
     METH_VARARGS | METH_KEYWORDS,
     "Query a canonical saved map and render ranked Markdown context."},
    {"map_query_markdown_view", (PyCFunction)py_map_query_markdown_view,
     METH_VARARGS | METH_KEYWORDS,
     "Project a canonical saved-map query by view and detail level."},
    {"project_source_markdown", (PyCFunction)py_project_source_markdown,
     METH_VARARGS | METH_KEYWORDS,
     "Render Map- or Query-selected source from project-owned bytes."},
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
    {"project_close", py_project_close, METH_O,
     "Release one native project handle."},
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
    {"project_write_map", (PyCFunction)py_project_write_map,
     METH_VARARGS | METH_KEYWORDS,
     "Stream the canonical native project map to a Python sink."},
    {"project_provider_facts", (PyCFunction)py_project_provider_facts,
     METH_VARARGS | METH_KEYWORDS, "Render one accepted provider artifact."},
    {"json_canonicalize", (PyCFunction)py_json_canonicalize,
     METH_VARARGS | METH_KEYWORDS, "Canonicalize strict Archbird JSON."},
    {"test_symbol_observations_validate", py_test_symbol_observations_validate,
     METH_VARARGS,
     "Validate strict project-owned test-to-symbol observations."},
    {"plan_validate", py_plan_validate, METH_VARARGS,
     "Validate one canonical editable Plan."},
    {"plan_render_markdown", py_plan_render_markdown, METH_VARARGS,
     "Render one canonical Plan as a Markdown task packet."},
    {"plan_compile", (PyCFunction)py_plan_compile, METH_VARARGS | METH_KEYWORDS,
     "Compile one native editable Plan from a Project, Map, and Verification."},
    {"act_validate", py_act_validate, METH_VARARGS,
     "Validate one materialized or accepted Act."},
    {"plan_source_requirements", (PyCFunction)py_plan_source_requirements,
     METH_VARARGS | METH_KEYWORDS,
     "Return exact source paths required to materialize a Plan."},
    {"act_source_requirements", (PyCFunction)py_act_source_requirements,
     METH_VARARGS | METH_KEYWORDS,
     "Return exact source paths required to apply an accepted Act."},
    {"act_materialize", (PyCFunction)py_act_materialize,
     METH_VARARGS | METH_KEYWORDS,
     "Materialize a Plan as an exact read-only Act."},
    {"act_accept", (PyCFunction)py_act_accept, METH_VARARGS | METH_KEYWORDS,
     "Seal a Act after independent Map and Verification acceptance."},
    {"act_preflight_apply", py_act_preflight_apply, METH_VARARGS,
     "Revalidate an accepted Act immediately before commit."},
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
