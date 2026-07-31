#include "artifact_validation.h"

#include "render_internal.h"
#include "sha256.h"

#include <string.h>

typedef struct ArtifactDigestWriter {
  ArchbirdSha256Context context;
  ArchbirdStatus status;
} ArtifactDigestWriter;

static int digest_write(void *user_data, const uint8_t *bytes, size_t length) {
  ArtifactDigestWriter *writer = (ArtifactDigestWriter *)user_data;
  writer->status = archbird_sha256_update(&writer->context, bytes, length);
  return writer->status == ARCHBIRD_OK ? 0 : 1;
}

int ab_artifact_text_is(const AbValue *value, const char *literal) {
  return ab_value_string_is(value, literal);
}

int ab_artifact_bounded_text(const AbValue *value, size_t maximum,
                             int nonempty) {
  return value && value->kind == AB_VALUE_STRING &&
         (!nonempty || value->as.text.length) &&
         value->as.text.length <= maximum;
}

int ab_artifact_sha256(const AbValue *value) {
  size_t index;
  if (!ab_artifact_bounded_text(value, 64, 1) || value->as.text.length != 64)
    return 0;
  for (index = 0; index < 64; index++)
    if (!((value->as.text.data[index] >= '0' &&
           value->as.text.data[index] <= '9') ||
          (value->as.text.data[index] >= 'a' &&
           value->as.text.data[index] <= 'f')))
      return 0;
  return 1;
}

int ab_artifact_stable_id(const AbValue *value) {
  size_t index;
  if (!ab_artifact_bounded_text(value, 256, 1))
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
          (byte >= '0' && byte <= '9') ||
          (index &&
           (byte == '_' || byte == '.' || byte == ':' || byte == '-'))))
      return 0;
  }
  return 1;
}

int ab_artifact_repository_path(const AbValue *value) {
  size_t index;
  size_t segment = 0;
  if (!ab_artifact_bounded_text(value, 4096, 1) ||
      value->as.text.data[0] == '/' || value->as.text.data[0] == '\\')
    return 0;
  if (value->as.text.length >= 2 &&
      ((value->as.text.data[0] >= 'A' && value->as.text.data[0] <= 'Z') ||
       (value->as.text.data[0] >= 'a' && value->as.text.data[0] <= 'z')) &&
      value->as.text.data[1] == ':')
    return 0;
  for (index = 0; index <= value->as.text.length; index++) {
    unsigned char byte = index < value->as.text.length
                             ? (unsigned char)value->as.text.data[index]
                             : 0;
    int end =
        index == value->as.text.length || value->as.text.data[index] == '/';
    if (!end &&
        (value->as.text.data[index] == '\\' || byte < 0x20 || byte == 0x7f))
      return 0;
    if (!end)
      continue;
    if (index == segment ||
        (index - segment == 1 && value->as.text.data[segment] == '.') ||
        (index - segment == 2 && value->as.text.data[segment] == '.' &&
         value->as.text.data[segment + 1] == '.'))
      return 0;
    segment = index + 1;
  }
  return 1;
}

int ab_artifact_repository_literal_path(const AbValue *value) {
  static const char pattern_bytes[] = "*?[]{}";
  size_t index;
  if (!ab_artifact_repository_path(value))
    return 0;
  for (index = 0; index < value->as.text.length; index++)
    if (memchr(pattern_bytes, value->as.text.data[index],
               sizeof(pattern_bytes) - 1))
      return 0;
  return 1;
}

int ab_artifact_safe_integer(const AbValue *value, uint64_t *out) {
  uint64_t number;
  if (!ab_value_u64(value, &number) || number > AB_ARTIFACT_MAX_SAFE_INTEGER)
    return 0;
  if (out)
    *out = number;
  return 1;
}

int ab_artifact_boolean(const AbValue *value) {
  return value && value->kind == AB_VALUE_BOOL;
}

static int field_named(const AbObjectField *field, const char *name) {
  size_t length = strlen(name);
  return field->name.length == length &&
         memcmp(field->name.data, name, length) == 0;
}

int ab_artifact_object_exact(const AbValue *value, const char *const *fields,
                             size_t count) {
  size_t index;
  size_t expected;
  if (!value || value->kind != AB_VALUE_OBJECT ||
      value->as.object.count != count)
    return 0;
  for (index = 0; index < value->as.object.count; index++) {
    int found = 0;
    for (expected = 0; expected < count; expected++)
      if (field_named(&value->as.object.fields[index], fields[expected])) {
        found = 1;
        break;
      }
    if (!found)
      return 0;
  }
  return 1;
}

ArchbirdStatus ab_artifact_json_sha256(ArchbirdEngine *engine,
                                       const uint8_t *json, size_t length,
                                       char out[65]) {
  ArtifactDigestWriter writer;
  uint8_t digest[32];
  ArchbirdStatus status;
  if (!engine || !json || !length || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  archbird_sha256_init(&writer.context);
  writer.status = ARCHBIRD_OK;
  status = archbird_json_canonicalize(engine, json, length, 0, digest_write,
                                      &writer);
  if (status == ARCHBIRD_OK)
    status = writer.status;
  if (status == ARCHBIRD_OK) {
    archbird_sha256_final(&writer.context, digest);
    archbird_sha256_hex(digest, out);
  }
  return status;
}
