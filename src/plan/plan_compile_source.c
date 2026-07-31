#include "plan_compile_internal.h"

#include "artifact_validation.h"
#include "sha256.h"

#include <string.h>

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

ArchbirdStatus ab_plan_source_lock(ArchbirdEngine *engine,
                                   const ArchbirdProject *project,
                                   const AbValue *map, const AbString *path,
                                   AbPlanSourceLock *out) {
  const AbValue *map_files = field(map, "files");
  const AbValue *map_inputs = field(map, "inputs");
  const AbValue *map_file = NULL;
  size_t index;
  uint8_t digest[32];
  char actual[65];
  ArchbirdStatus status;
  if (!engine || !project || !map || map->kind != AB_VALUE_OBJECT ||
      !map_files || map_files->kind != AB_VALUE_ARRAY || !map_inputs ||
      map_inputs->kind != AB_VALUE_ARRAY || !path || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  for (index = 0; index < map_files->as.array.count; index++) {
    const AbValue *row = &map_files->as.array.items[index];
    const AbValue *candidate = field(row, "path");
    if (!candidate || candidate->kind != AB_VALUE_STRING ||
        !ab_string_equal(&candidate->as.text, path))
      continue;
    if (map_file)
      return archbird_error_set(
          engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
          "plan compilation: Map has duplicate source rows for %.*s",
          (int)path->length, path->data);
    map_file = row;
  }
  if (!map_file) {
    for (index = 0; index < map_inputs->as.array.count; index++) {
      const AbValue *row = &map_inputs->as.array.items[index];
      const AbValue *candidate = field(row, "path");
      if (!candidate || candidate->kind != AB_VALUE_STRING ||
          !ab_string_equal(&candidate->as.text, path))
        continue;
      if (map_file)
        return archbird_error_set(
            engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
            "plan compilation: Map has duplicate input rows for %.*s",
            (int)path->length, path->data);
      map_file = row;
    }
  }
  out->sha256 = field(map_file, "sha256");
  if (!map_file || !ab_artifact_sha256(out->sha256))
    return archbird_error_set(
        engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
        "plan compilation: Map has no unique source lock for %.*s",
        (int)path->length, path->data);
  out->source.struct_size = sizeof(out->source);
  for (index = 0; index < archbird_project_source_count(project); index++) {
    ArchbirdSourceView source = {0};
    source.struct_size = sizeof(source);
    if (archbird_project_source(project, index, &source) != ARCHBIRD_OK)
      continue;
    if (source.path_length == path->length &&
        memcmp(source.path, path->data, path->length) == 0) {
      if (out->source.path)
        return archbird_error_set(
            engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
            "plan compilation: project has duplicate source rows for %.*s",
            (int)path->length, path->data);
      out->source = source;
    }
  }
  if (!out->source.path)
    return archbird_error_set(
        engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
        "plan compilation: source %.*s is absent from the in-memory project",
        (int)path->length, path->data);
  status = archbird_sha256(out->source.bytes, out->source.byte_length, digest);
  if (status != ARCHBIRD_OK)
    return status;
  archbird_sha256_hex(digest, actual);
  if (memcmp(actual, out->sha256->as.text.data, 64) != 0)
    return archbird_error_set(
        engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
        "plan compilation: source %.*s does not match its Map source lock",
        (int)path->length, path->data);
  out->map_file = map_file;
  return ARCHBIRD_OK;
}
