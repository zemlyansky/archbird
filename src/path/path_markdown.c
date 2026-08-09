#include <archbird/archbird.h>

#include "base/json_value.h"
#include "base/render_internal.h"
#include "path/path_artifact.h"
#include "path/path_internal.h"

#include <string.h>

#define TRY(expression)                                                        \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

static ArchbirdStatus markdown_string(AbBuffer *buffer, const AbValue *object,
                                      const char *name) {
  const AbValue *value = ab_value_member(object, name);
  if (!value || value->kind != AB_VALUE_STRING)
    return ARCHBIRD_INVALID_SCHEMA;
  return ab_buffer_append(buffer, value->as.text.data, value->as.text.length);
}

static ArchbirdStatus markdown_text(AbBuffer *buffer, const AbString *value) {
  size_t index;
  for (index = 0; index < value->length; index++) {
    unsigned char byte = (unsigned char)value->data[index];
    if (byte == '\r')
      continue;
    if (byte == '\n' || byte == '\t')
      TRY(ab_buffer_literal(buffer, " "));
    else {
      if (byte == '\\' || byte == '`' || byte == '*' || byte == '_' ||
          byte == '[' || byte == ']' || byte == '<' || byte == '>' ||
          byte == '#')
        TRY(ab_buffer_literal(buffer, "\\"));
      TRY(ab_buffer_append(buffer, &byte, 1));
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus markdown_evidence(AbBuffer *buffer,
                                        const AbValue *evidence) {
  size_t index;
  if (!evidence || evidence->kind != AB_VALUE_ARRAY)
    return ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; index < evidence->as.array.count; index++) {
    const AbValue *row = &evidence->as.array.items[index];
    const AbValue *provenance;
    const AbValue *path;
    const AbValue *line_value;
    const AbValue *detail;
    uint64_t line = 0;
    if (row->kind != AB_VALUE_OBJECT)
      return ARCHBIRD_INVALID_SCHEMA;
    provenance = ab_value_member(row, "provenance");
    path = ab_value_member(row, "path");
    line_value = ab_value_member(row, "line");
    detail = ab_value_member(row, "detail");
    if (!provenance || provenance->kind != AB_VALUE_STRING || !path ||
        path->kind != AB_VALUE_STRING || !line_value ||
        !ab_value_u64(line_value, &line) || !detail ||
        detail->kind != AB_VALUE_STRING)
      return ARCHBIRD_INVALID_SCHEMA;
    TRY(ab_buffer_literal(buffer, "  - provenance="));
    TRY(markdown_text(buffer, &provenance->as.text));
    if (path->as.text.length) {
      TRY(ab_buffer_literal(buffer, "; source="));
      TRY(markdown_text(buffer, &path->as.text));
      if (line) {
        TRY(ab_buffer_literal(buffer, ":"));
        TRY(ab_buffer_u64(buffer, line));
      }
    }
    if (detail->as.text.length) {
      TRY(ab_buffer_literal(buffer, "; detail="));
      TRY(markdown_text(buffer, &detail->as.text));
    }
    TRY(ab_buffer_literal(buffer, "\n"));
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_path_render_markdown_value(ArchbirdEngine *engine,
                                             const AbValue *artifact,
                                             size_t max_chars, AbBuffer *out) {
  const AbValue *paths;
  const AbValue *source;
  const AbValue *target;
  const AbValue *search;
  size_t index;
  (void)engine;
  if (!artifact || artifact->kind != AB_VALUE_OBJECT ||
      !ab_value_string_is(ab_value_member(artifact, "artifact"), "path"))
    return ARCHBIRD_INVALID_SCHEMA;
  paths = ab_value_member(artifact, "paths");
  source = ab_value_member(artifact, "source");
  target = ab_value_member(artifact, "target");
  search = ab_value_member(artifact, "search");
  if (!paths || paths->kind != AB_VALUE_ARRAY || !source ||
      source->kind != AB_VALUE_OBJECT || !target ||
      target->kind != AB_VALUE_OBJECT || !search ||
      search->kind != AB_VALUE_OBJECT)
    return ARCHBIRD_INVALID_SCHEMA;
  TRY(ab_buffer_literal(out, "# Connection paths: "));
  TRY(markdown_string(out, artifact, "project"));
  TRY(ab_buffer_literal(out, "\n\nOutcome: `"));
  TRY(markdown_string(out, artifact, "outcome"));
  TRY(ab_buffer_literal(out, "` (`"));
  TRY(markdown_string(out, artifact, "reason"));
  TRY(ab_buffer_literal(out, "`). Source `"));
  TRY(markdown_string(out, source, "state"));
  TRY(ab_buffer_literal(out, "`; target `"));
  TRY(markdown_string(out, target, "state"));
  TRY(ab_buffer_literal(out, "`.\n\n"));
  if (!paths->as.array.count)
    TRY(ab_buffer_literal(out, "No connection witness is established.\n"));
  for (index = 0; index < paths->as.array.count; index++) {
    const AbValue *path = &paths->as.array.items[index];
    const AbValue *nodes = ab_value_member(path, "nodes");
    const AbValue *path_state = ab_value_member(path, "state");
    const AbValue *steps = ab_value_member(path, "steps");
    size_t node_index;
    if (!nodes || nodes->kind != AB_VALUE_ARRAY || !steps ||
        steps->kind != AB_VALUE_ARRAY || !path_state ||
        path_state->kind != AB_VALUE_STRING)
      return ARCHBIRD_INVALID_SCHEMA;
    TRY(ab_buffer_literal(out, "## Witness "));
    TRY(ab_buffer_u64(out, index + 1));
    TRY(ab_buffer_literal(out, " · state `"));
    TRY(ab_buffer_append(out, path_state->as.text.data,
                         path_state->as.text.length));
    TRY(ab_buffer_literal(out, "`\n\n"));
    for (node_index = 0; node_index < nodes->as.array.count; node_index++) {
      const AbValue *node = &nodes->as.array.items[node_index];
      if (node->kind != AB_VALUE_STRING)
        return ARCHBIRD_INVALID_SCHEMA;
      TRY(ab_buffer_literal(out, node_index ? " → `" : "`"));
      TRY(ab_buffer_append(out, node->as.text.data, node->as.text.length));
      TRY(ab_buffer_literal(out, "`"));
    }
    TRY(ab_buffer_literal(out, "\n\n"));
    for (node_index = 0; node_index < steps->as.array.count; node_index++) {
      const AbValue *step = &steps->as.array.items[node_index];
      const AbValue *traversal = ab_value_member(step, "traversal");
      const AbValue *relation = ab_value_member(step, "relation");
      const AbValue *attributes =
          relation ? ab_value_member(relation, "attributes") : NULL;
      const AbValue *state =
          relation ? ab_value_member(relation, "state") : NULL;
      const AbValue *evidence =
          relation ? ab_value_member(relation, "evidence") : NULL;
      const AbValue *family =
          attributes ? ab_value_member(attributes, "family") : NULL;
      const AbValue *kind =
          attributes ? ab_value_member(attributes, "relation_kind") : NULL;
      const AbValue *resolution =
          attributes ? ab_value_member(attributes, "resolution") : NULL;
      if (!traversal || traversal->kind != AB_VALUE_STRING || !family ||
          family->kind != AB_VALUE_STRING || !kind ||
          kind->kind != AB_VALUE_STRING || !state ||
          state->kind != AB_VALUE_STRING || !evidence ||
          evidence->kind != AB_VALUE_ARRAY)
        return ARCHBIRD_INVALID_SCHEMA;
      TRY(ab_buffer_literal(out, "- `"));
      TRY(ab_buffer_append(out, family->as.text.data, family->as.text.length));
      TRY(ab_buffer_literal(out, ":"));
      TRY(ab_buffer_append(out, kind->as.text.data, kind->as.text.length));
      TRY(ab_buffer_literal(out, "` traversed "));
      TRY(ab_buffer_append(out, traversal->as.text.data,
                           traversal->as.text.length));
      TRY(ab_buffer_literal(out, "; state `"));
      TRY(ab_buffer_append(out, state->as.text.data, state->as.text.length));
      TRY(ab_buffer_literal(out, "`; resolution `"));
      if (resolution && resolution->kind == AB_VALUE_STRING)
        TRY(ab_buffer_append(out, resolution->as.text.data,
                             resolution->as.text.length));
      else if (!resolution)
        TRY(ab_buffer_literal(out, "not-applicable"));
      else
        return ARCHBIRD_INVALID_SCHEMA;
      TRY(ab_buffer_literal(out, "`; evidence="));
      TRY(ab_buffer_u64(out, evidence->as.array.count));
      TRY(ab_buffer_literal(out, "\n"));
      TRY(markdown_evidence(out, evidence));
    }
    TRY(ab_buffer_literal(out, "\n"));
  }
  if (max_chars && out->length > max_chars)
    return archbird_error_set(
        out->engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
        "path Markdown exceeds max_chars; request canonical JSON or raise the "
        "presentation budget");
  return ARCHBIRD_OK;
}

ArchbirdStatus archbird_path_render_markdown(ArchbirdEngine *engine,
                                             const uint8_t *artifact_json,
                                             size_t artifact_length,
                                             size_t max_chars,
                                             ArchbirdWriteFn write_fn,
                                             void *user_data) {
  AbPathArtifact artifact = {0};
  AbBuffer markdown;
  ArchbirdStatus status;
  if (!engine || !artifact_json || !artifact_length || !write_fn)
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&markdown, engine);
  status =
      ab_path_artifact_load(engine, artifact_json, artifact_length, &artifact);
  if (status == ARCHBIRD_OK)
    status = ab_path_render_markdown_value(engine, &artifact.root, max_chars,
                                           &markdown);
  if (status == ARCHBIRD_OK &&
      write_fn(user_data, markdown.data, markdown.length))
    status =
        archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                           "Path report callback failed");
  ab_buffer_free(&markdown);
  ab_path_artifact_free(&artifact);
  return status;
}

ArchbirdStatus archbird_map_path_markdown(
    ArchbirdEngine *engine, const uint8_t *map_json, size_t map_length,
    const uint8_t *resolution_json, size_t resolution_length,
    const uint8_t *request_json, size_t request_length, size_t max_chars,
    ArchbirdWriteFn write_fn, void *user_data) {
  AbValue map = {0};
  AbValue resolution = {0};
  AbValue request = {0};
  AbValue artifact = {0};
  AbBuffer json;
  AbBuffer markdown;
  ArchbirdStatus status;
  if (!engine || !map_json || !map_length ||
      (!resolution_json && resolution_length) || !request_json ||
      !request_length || !write_fn)
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&json, engine);
  ab_buffer_init(&markdown, engine);
  status = ab_json_value_decode(engine, map_json, map_length, &map);
  if (status == ARCHBIRD_OK && resolution_length)
    status = ab_json_value_decode(engine, resolution_json, resolution_length,
                                  &resolution);
  if (status == ARCHBIRD_OK)
    status =
        ab_json_value_decode(engine, request_json, request_length, &request);
  if (status == ARCHBIRD_OK)
    status = ab_path_execute_value(
        engine, &map, resolution_length ? &resolution : NULL, &request, &json);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, json.data, json.length, &artifact);
  if (status == ARCHBIRD_OK)
    status = ab_path_artifact_validate(engine, &artifact);
  if (status == ARCHBIRD_OK)
    status =
        ab_path_render_markdown_value(engine, &artifact, max_chars, &markdown);
  if (status == ARCHBIRD_OK &&
      write_fn(user_data, markdown.data, markdown.length))
    status =
        archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                           "Path report callback failed");
  ab_value_free(engine, &artifact);
  ab_value_free(engine, &request);
  ab_value_free(engine, &resolution);
  ab_value_free(engine, &map);
  ab_buffer_free(&markdown);
  ab_buffer_free(&json);
  return status;
}

#undef TRY
