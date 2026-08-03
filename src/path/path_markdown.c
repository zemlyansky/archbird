#include <archbird/archbird.h>

#include "json_value.h"
#include "path_internal.h"
#include "render_internal.h"

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
    const AbValue *steps = ab_value_member(path, "steps");
    size_t node_index;
    if (!nodes || nodes->kind != AB_VALUE_ARRAY || !steps ||
        steps->kind != AB_VALUE_ARRAY)
      return ARCHBIRD_INVALID_SCHEMA;
    TRY(ab_buffer_literal(out, "## Witness "));
    TRY(ab_buffer_u64(out, index + 1));
    TRY(ab_buffer_literal(out, "\n\n"));
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
      const AbValue *family =
          attributes ? ab_value_member(attributes, "family") : NULL;
      const AbValue *kind =
          attributes ? ab_value_member(attributes, "relation_kind") : NULL;
      if (!traversal || traversal->kind != AB_VALUE_STRING || !family ||
          family->kind != AB_VALUE_STRING || !kind ||
          kind->kind != AB_VALUE_STRING)
        return ARCHBIRD_INVALID_SCHEMA;
      TRY(ab_buffer_literal(out, "- `"));
      TRY(ab_buffer_append(out, family->as.text.data, family->as.text.length));
      TRY(ab_buffer_literal(out, ":"));
      TRY(ab_buffer_append(out, kind->as.text.data, kind->as.text.length));
      TRY(ab_buffer_literal(out, "` traversed "));
      TRY(ab_buffer_append(out, traversal->as.text.data,
                           traversal->as.text.length));
      TRY(ab_buffer_literal(out, "\n"));
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
    status =
        ab_path_render_markdown_value(engine, &artifact, max_chars, &markdown);
  if (status == ARCHBIRD_OK)
    status = write_fn(user_data, markdown.data, markdown.length);
  ab_value_free(engine, &artifact);
  ab_value_free(engine, &request);
  ab_value_free(engine, &resolution);
  ab_value_free(engine, &map);
  ab_buffer_free(&markdown);
  ab_buffer_free(&json);
  return status;
}

#undef TRY
