#include "evidence/manifests/npm_workspace_manifest.h"

#include "base/archbird_internal.h"
#include "base/json_value.h"

#include <string.h>

static int nonblank_string(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || !value->as.text.length)
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n')
      return 1;
  }
  return 0;
}

static ArchbirdStatus copy_optional_string(ArchbirdEngine *engine,
                                           const AbValue *object,
                                           const char *name, AbString *out) {
  const AbValue *value = ab_value_member(object, name);
  if (!nonblank_string(value))
    return ARCHBIRD_OK;
  return ab_string_copy(engine, out, value->as.text.data,
                        value->as.text.length);
}

static ArchbirdStatus copy_workspace_patterns(ArchbirdEngine *engine,
                                              const AbValue *value,
                                              AbNpmDiscoveryMetadata *out) {
  size_t index;
  size_t capacity = 0;
  if (!value || value->kind != AB_VALUE_ARRAY) {
    out->workspaces_supported = 0;
    return ARCHBIRD_OK;
  }
  for (index = 0; index < value->as.array.count; index++) {
    const AbValue *item = &value->as.array.items[index];
    AbString *resized;
    ArchbirdStatus status;
    if (!nonblank_string(item)) {
      size_t previous;
      for (previous = 0; previous < out->workspace_count; previous++)
        ab_string_free(engine, &out->workspaces[previous]);
      ab_free(engine, out->workspaces);
      out->workspaces = NULL;
      out->workspace_count = 0;
      out->workspaces_supported = 0;
      return ARCHBIRD_OK;
    }
    if (out->workspace_count == capacity) {
      size_t next = capacity ? capacity * 2 : 8;
      if (next < capacity || next > SIZE_MAX / sizeof(*out->workspaces))
        return ARCHBIRD_LIMIT_EXCEEDED;
      resized = (AbString *)ab_realloc(engine, out->workspaces,
                                       next * sizeof(*out->workspaces));
      if (!resized)
        return ARCHBIRD_OUT_OF_MEMORY;
      out->workspaces = resized;
      capacity = next;
    }
    memset(&out->workspaces[out->workspace_count], 0, sizeof(*out->workspaces));
    status = ab_string_copy(engine, &out->workspaces[out->workspace_count],
                            item->as.text.data, item->as.text.length);
    if (status != ARCHBIRD_OK)
      return status;
    out->workspace_count++;
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_npm_discovery_metadata(ArchbirdEngine *engine,
                                         const uint8_t *json,
                                         size_t json_length,
                                         AbNpmDiscoveryMetadata *out) {
  AbValue document = {0};
  const AbValue *workspaces;
  ArchbirdStatus status;
  if (!engine || (!json && json_length) || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  out->workspaces_supported = 1;
  status = ab_json_value_decode(engine, json, json_length, &document);
  if (status != ARCHBIRD_OK)
    return status;
  if (document.kind != AB_VALUE_OBJECT) {
    status = ARCHBIRD_INVALID_SCHEMA;
    goto done;
  }
  status = copy_optional_string(engine, &document, "name", &out->name);
  if (status == ARCHBIRD_OK)
    status = copy_optional_string(engine, &document, "version", &out->version);
  workspaces = ab_value_member(&document, "workspaces");
  if (status == ARCHBIRD_OK && workspaces) {
    out->workspaces_present = 1;
    if (workspaces->kind == AB_VALUE_OBJECT)
      workspaces = ab_value_member(workspaces, "packages");
    status = copy_workspace_patterns(engine, workspaces, out);
  }
done:
  ab_value_free(engine, &document);
  if (status != ARCHBIRD_OK)
    ab_npm_discovery_metadata_free(engine, out);
  return status;
}

void ab_npm_discovery_metadata_free(ArchbirdEngine *engine,
                                    AbNpmDiscoveryMetadata *metadata) {
  size_t index;
  if (!metadata)
    return;
  ab_string_free(engine, &metadata->name);
  ab_string_free(engine, &metadata->version);
  for (index = 0; index < metadata->workspace_count; index++)
    ab_string_free(engine, &metadata->workspaces[index]);
  ab_free(engine, metadata->workspaces);
  memset(metadata, 0, sizeof(*metadata));
}
