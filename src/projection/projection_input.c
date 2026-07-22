#include "projection_internal.h"

#include "archbird_internal.h"

#include <string.h>

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *where,
                              const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "%s: %s", where, message);
}

static int value_is(const AbValue *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->kind == AB_VALUE_STRING &&
         value->as.text.length == length &&
         (!length || memcmp(value->as.text.data, literal, length) == 0);
}

static int nonblank(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || !value->as.text.length)
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    char byte = value->as.text.data[index];
    if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n')
      return 1;
  }
  return 0;
}

static int lowercase_sha256(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || value->as.text.length != 64)
    return 0;
  for (index = 0; index < 64; index++) {
    char byte = value->as.text.data[index];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return 0;
  }
  return 1;
}

static int repository_path(const AbValue *value) {
  const char *path;
  size_t length;
  size_t index;
  size_t segment = 0;
  if (!value || value->kind != AB_VALUE_STRING)
    return 0;
  path = value->as.text.data;
  length = value->as.text.length;
  if (!length || path[0] == '/' || path[length - 1] == '/' ||
      (length >= 2 &&
       ((path[0] >= 'A' && path[0] <= 'Z') ||
        (path[0] >= 'a' && path[0] <= 'z')) &&
       path[1] == ':'))
    return 0;
  for (index = 0; index <= length; index++) {
    if (index < length && path[index] != '/') {
      if (path[index] == '\\' || path[index] == '\0')
        return 0;
      continue;
    }
    if (index == segment || (index - segment == 1 && path[segment] == '.') ||
        (index - segment == 2 && path[segment] == '.' &&
         path[segment + 1] == '.'))
      return 0;
    segment = index + 1;
  }
  return 1;
}

ArchbirdStatus ab_projection_map_validate(ArchbirdEngine *engine,
                                          const AbValue *map,
                                          const char *where) {
  const AbValue *evidence;
  const AbValue *facts;
  uint64_t schema;
  if (!engine || !map || !where)
    return ARCHBIRD_INVALID_ARGUMENT;
  evidence = ab_value_member(map, "evidence");
  facts = ab_value_member(map, "facts");
  if (map->kind != AB_VALUE_OBJECT ||
      !value_is(ab_value_member(map, "artifact"), "map") ||
      !ab_value_u64(ab_value_member(map, "schema_version"), &schema) ||
      schema < ARCHBIRD_MAP_SCHEMA_MIN ||
      schema > ARCHBIRD_MAP_SCHEMA_CURRENT ||
      !nonblank(ab_value_member(map, "project")) || !evidence ||
      evidence->kind != AB_VALUE_OBJECT ||
      !lowercase_sha256(ab_value_member(evidence, "config_sha256")) ||
      !lowercase_sha256(ab_value_member(evidence, "input_sha256")) ||
      (schema >= 8 && (!facts || facts->kind != AB_VALUE_ARRAY)) ||
      (facts && facts->kind != AB_VALUE_ARRAY))
    return invalid(engine, where, "invalid Archbird Map artifact");
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_projection_resolution_validate(ArchbirdEngine *engine,
                                                 const AbValue *resolution,
                                                 const AbValue *map,
                                                 const char *where) {
  const AbValue *coverage;
  const AbValue *diagnostics;
  const AbValue *map_discovery;
  const AbValue *map_coverage;
  const AbValue *resolution_sha;
  const AbValue *map_resolution_sha;
  const AbValue *map_evidence;
  const AbValue *resolution_project;
  const AbValue *resolution_config;
  uint64_t schema;
  uint64_t declared_oversized;
  uint64_t mapped_oversized;
  size_t oversized_count = 0;
  size_t index;
  ArchbirdStatus status;
  if (!engine || !map || !where)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_projection_map_validate(engine, map, where);
  if (status != ARCHBIRD_OK || !resolution)
    return status;
  if (resolution->kind != AB_VALUE_OBJECT ||
      !value_is(ab_value_member(resolution, "artifact"),
                "archbird-config-resolution") ||
      !ab_value_u64(ab_value_member(resolution, "schema_version"), &schema) ||
      schema != 1)
    return invalid(engine, where, "invalid discovery resolution identity");
  coverage = ab_value_member(resolution, "coverage");
  diagnostics = ab_value_member(resolution, "diagnostics");
  resolution_sha = ab_value_member(resolution, "sha256");
  if (!coverage || coverage->kind != AB_VALUE_OBJECT ||
      !ab_value_u64(ab_value_member(coverage, "oversized"),
                    &declared_oversized) ||
      !diagnostics || diagnostics->kind != AB_VALUE_ARRAY ||
      !lowercase_sha256(resolution_sha))
    return invalid(engine, where, "invalid discovery resolution evidence");
  for (index = 0; index < diagnostics->as.array.count; index++) {
    const AbValue *row = &diagnostics->as.array.items[index];
    size_t previous;
    uint64_t bytes;
    if (!value_is(ab_value_member(row, "code"), "discovery-file-oversized"))
      continue;
    if (!repository_path(ab_value_member(row, "path")) ||
        !ab_value_u64(ab_value_member(row, "bytes"), &bytes))
      return invalid(engine, where, "invalid oversized-file evidence");
    for (previous = 0; previous < index; previous++) {
      const AbValue *old = &diagnostics->as.array.items[previous];
      if (value_is(ab_value_member(old, "code"), "discovery-file-oversized") &&
          ab_string_equal(&ab_value_member(row, "path")->as.text,
                          &ab_value_member(old, "path")->as.text))
        return invalid(engine, where, "duplicate oversized-file evidence");
    }
    oversized_count++;
  }
  if (oversized_count != declared_oversized)
    return invalid(engine, where, "incomplete oversized-file evidence");
  map_discovery = ab_value_member(map, "discovery");
  map_coverage =
      map_discovery ? ab_value_member(map_discovery, "coverage") : NULL;
  map_resolution_sha =
      map_discovery ? ab_value_member(map_discovery, "sha256") : NULL;
  if (!lowercase_sha256(map_resolution_sha) ||
      !ab_string_equal(&resolution_sha->as.text, &map_resolution_sha->as.text))
    return invalid(engine, where, "resolution digest does not match the Map");
  if (!map_coverage ||
      !ab_value_u64(ab_value_member(map_coverage, "oversized"),
                    &mapped_oversized) ||
      mapped_oversized != declared_oversized)
    return invalid(engine, where,
                   "resolution oversized count does not match the Map");
  map_evidence = ab_value_member(map, "evidence");
  resolution_project = ab_value_member(resolution, "project");
  resolution_config = ab_value_member(resolution, "configuration_sha256");
  if (!nonblank(resolution_project) || !lowercase_sha256(resolution_config) ||
      !ab_string_equal(&resolution_project->as.text,
                       &ab_value_member(map, "project")->as.text) ||
      !ab_string_equal(
          &resolution_config->as.text,
          &ab_value_member(map_evidence, "config_sha256")->as.text))
    return invalid(engine, where, "resolution does not match the Map");
  return ARCHBIRD_OK;
}
