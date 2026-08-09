#include "act/act_source.h"

#include "base/artifact_validation.h"

#include <string.h>

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "act source metadata: %s", message);
}

const AbValue *ab_act_source_file(const AbValue *metadata,
                                  const AbString *path) {
  const AbValue *files = field(metadata, "files");
  size_t low = 0;
  size_t high =
      files && files->kind == AB_VALUE_ARRAY ? files->as.array.count : 0;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    const AbValue *row = &files->as.array.items[middle];
    const AbValue *row_path = field(row, "path");
    int compared = ab_string_compare(&row_path->as.text, path);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return row;
  }
  return NULL;
}

int ab_act_source_path_absent(const AbValue *metadata, const AbString *path) {
  const AbValue *absent = field(metadata, "absent");
  size_t low = 0;
  size_t high =
      absent && absent->kind == AB_VALUE_ARRAY ? absent->as.array.count : 0;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared =
        ab_string_compare(&absent->as.array.items[middle].as.text, path);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return 1;
  }
  return 0;
}

static ArchbirdStatus validate_metadata(ArchbirdEngine *engine,
                                        const AbValue *metadata) {
  static const char *const root_fields[] = {"files", "absent"};
  static const char *const file_fields[] = {"path", "sha256", "executable"};
  const AbValue *files;
  const AbValue *absent;
  size_t index;
  if (!ab_artifact_object_exact(metadata, root_fields, 2))
    return invalid(engine, "document must contain files and absent");
  files = field(metadata, "files");
  absent = field(metadata, "absent");
  if (!files || files->kind != AB_VALUE_ARRAY ||
      files->as.array.count > AB_ACT_MAX_OBSERVED_PATHS || !absent ||
      absent->kind != AB_VALUE_ARRAY ||
      absent->as.array.count > AB_ACT_MAX_OBSERVED_PATHS)
    return invalid(engine, "file inventories exceed the Act limit");
  for (index = 0; index < files->as.array.count; index++) {
    const AbValue *row = &files->as.array.items[index];
    const AbValue *path = field(row, "path");
    if (!ab_artifact_object_exact(row, file_fields, 3) ||
        !ab_artifact_repository_path(path) ||
        !ab_artifact_sha256(field(row, "sha256")) ||
        !ab_artifact_boolean(field(row, "executable")))
      return invalid(engine, "file row is invalid");
    if (index && ab_string_compare(
                     &field(&files->as.array.items[index - 1], "path")->as.text,
                     &path->as.text) >= 0)
      return invalid(engine, "files are not uniquely sorted");
  }
  for (index = 0; index < absent->as.array.count; index++) {
    const AbValue *path = &absent->as.array.items[index];
    if (!ab_artifact_repository_path(path) ||
        (index && ab_string_compare(&absent->as.array.items[index - 1].as.text,
                                    &path->as.text) >= 0))
      return invalid(engine, "absent paths are not uniquely sorted");
    if (ab_act_source_file(metadata, &path->as.text))
      return invalid(engine, "one path is both present and absent");
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_act_source_metadata_load(ArchbirdEngine *engine,
                                           const uint8_t *json, size_t length,
                                           AbValue *out) {
  ArchbirdStatus status;
  if (!engine || !json || !length || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  status = ab_json_value_decode(engine, json, length, out);
  if (status == ARCHBIRD_OK)
    status = validate_metadata(engine, out);
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}
