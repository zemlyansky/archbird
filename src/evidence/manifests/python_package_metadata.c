#include "evidence/manifests/python_package_metadata.h"

#include "base/archbird_internal.h"

#include <string.h>

void ab_python_package_metadata_init(AbPythonPackageMetadata *metadata) {
  if (!metadata)
    return;
  memset(metadata, 0, sizeof(*metadata));
  metadata->module_hints_supported = 1;
  metadata->source_root_supported = 1;
}

void ab_python_package_metadata_free(ArchbirdEngine *engine,
                                     AbPythonPackageMetadata *metadata) {
  if (!metadata)
    return;
  ab_string_free(engine, &metadata->name);
  ab_string_free(engine, &metadata->version);
  ab_string_free(engine, &metadata->module);
  ab_string_free(engine, &metadata->source_root);
  memset(metadata, 0, sizeof(*metadata));
}

int ab_python_identifier_valid(const char *data, size_t length) {
  size_t index;
  if (!length || !((data[0] >= 'A' && data[0] <= 'Z') ||
                   (data[0] >= 'a' && data[0] <= 'z') || data[0] == '_'))
    return 0;
  for (index = 1; index < length; index++)
    if (!((data[index] >= 'A' && data[index] <= 'Z') ||
          (data[index] >= 'a' && data[index] <= 'z') ||
          (data[index] >= '0' && data[index] <= '9') || data[index] == '_'))
      return 0;
  return 1;
}

int ab_python_distribution_name_valid(const AbString *value) {
  size_t index;
  if (!value || !value->length)
    return 0;
  for (index = 0; index < value->length; index++) {
    unsigned char byte = (unsigned char)value->data[index];
    if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
        (byte >= '0' && byte <= '9'))
      continue;
    if (!index || index + 1 == value->length ||
        (byte != '-' && byte != '_' && byte != '.'))
      return 0;
  }
  return 1;
}

int ab_python_module_candidate(const AbString *value, int pattern,
                               const char **data, size_t *length) {
  size_t end = 0;
  size_t segment = 0;
  size_t index;
  if (!value)
    return 0;
  while (end < value->length && value->data[end] != '.' &&
         (!pattern || (value->data[end] != '*' && value->data[end] != '?' &&
                       value->data[end] != '[')))
    end++;
  if (!end || !ab_python_identifier_valid(value->data, end))
    return 0;
  if (pattern) {
    for (index = 0; index < value->length; index++) {
      unsigned char byte = (unsigned char)value->data[index];
      if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '_' || byte == '.' ||
            byte == '*' || byte == '?' || byte == '[' || byte == ']' ||
            byte == '!' || byte == '-'))
        return 0;
    }
  } else {
    for (index = 0; index <= value->length; index++) {
      if (index < value->length && value->data[index] != '.')
        continue;
      if (!ab_python_identifier_valid(value->data + segment, index - segment))
        return 0;
      segment = index + 1;
    }
  }
  *data = value->data;
  *length = end;
  return 1;
}

int ab_python_source_root_valid(const AbString *value) {
  size_t segment = 0;
  size_t index;
  if (!value || !value->length || value->data[0] == '/' ||
      value->data[value->length - 1] == '/')
    return 0;
  if (value->length == 1 && value->data[0] == '.')
    return 1;
  for (index = 0; index <= value->length; index++) {
    if (index < value->length && value->data[index] != '/') {
      if (value->data[index] == '\\' || value->data[index] == '\0')
        return 0;
      continue;
    }
    if (index == segment ||
        (index - segment == 1 && value->data[segment] == '.') ||
        (index - segment == 2 && value->data[segment] == '.' &&
         value->data[segment + 1] == '.'))
      return 0;
    segment = index + 1;
  }
  return 1;
}

ArchbirdStatus
ab_python_module_hint_merge(ArchbirdEngine *engine, AbString *module,
                            int *present, int *supported, const char *candidate,
                            size_t candidate_length, int candidate_supported) {
  if (!engine || !module || !present || !supported ||
      (!candidate && candidate_length))
    return ARCHBIRD_INVALID_ARGUMENT;
  *present = 1;
  if (!candidate_supported || !candidate_length || !*supported) {
    *supported = 0;
    ab_string_free(engine, module);
    return ARCHBIRD_OK;
  }
  if (!module->length)
    return ab_string_copy(engine, module, candidate, candidate_length);
  if (module->length != candidate_length ||
      memcmp(module->data, candidate, candidate_length)) {
    *supported = 0;
    ab_string_free(engine, module);
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_python_source_root_merge(ArchbirdEngine *engine,
                                           AbString *source_root, int *present,
                                           int *supported,
                                           const AbString *candidate,
                                           int candidate_supported) {
  if (!engine || !source_root || !present || !supported)
    return ARCHBIRD_INVALID_ARGUMENT;
  *present = 1;
  candidate_supported = candidate_supported && candidate &&
                        ab_python_source_root_valid(candidate);
  if (!candidate_supported || !*supported) {
    *supported = 0;
    ab_string_free(engine, source_root);
    return ARCHBIRD_OK;
  }
  if (!source_root->length)
    return ab_string_copy(engine, source_root, candidate->data,
                          candidate->length);
  if (!ab_string_equal(source_root, candidate)) {
    *supported = 0;
    ab_string_free(engine, source_root);
  }
  return ARCHBIRD_OK;
}
