#include <archbird/archbird.h>

#include "json_value.h"
#include "path_internal.h"
#include "path_match.h"
#include "projection_internal.h"
#include "projection_model.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

#define TRY(expression)                                                        \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

typedef struct AbPathEndpoint {
  const AbString *kind;
  const AbValue *patterns;
} AbPathEndpoint;

typedef struct AbPathRequest {
  AbPathEndpoint source;
  AbPathEndpoint target;
  const AbString *level;
  const AbString *direction;
  const AbString *producer_policy;
  const AbValue *relations;
  size_t max_depth;
  size_t max_paths;
} AbPathRequest;

typedef struct AbPathNode {
  const AbProjectionItem *item;
  const AbString *id;
} AbPathNode;

typedef struct AbPathEdge {
  const AbProjectionItem *item;
  size_t source;
  size_t target;
} AbPathEdge;

typedef struct AbPathArc {
  size_t edge;
  size_t next;
  int reverse;
} AbPathArc;

typedef struct AbPathGraph {
  AbPathNode *nodes;
  size_t node_count;
  AbPathEdge *edges;
  size_t edge_count;
  AbPathArc *arcs;
  size_t arc_count;
  size_t *arc_offsets;
} AbPathGraph;

typedef struct AbPathCandidates {
  size_t *items;
  size_t count;
} AbPathCandidates;

typedef struct AbPathEnumeration {
  const AbPathGraph *graph;
  const size_t *distance;
  const uint8_t *reachable;
  const uint8_t *targets;
  size_t shortest;
  size_t max_paths;
  size_t count;
  int truncated;
  size_t *nodes;
  size_t *arcs;
  AbBuffer *output;
} AbPathEnumeration;

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "path request: %s", message);
}

static int string_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static const AbValue *attribute(const AbProjectionItem *item,
                                const char *name) {
  size_t index;
  size_t length = strlen(name);
  for (index = 0; item && index < item->attribute_count; index++)
    if (item->attributes[index].name.length == length &&
        !memcmp(item->attributes[index].name.data, name, length))
      return &item->attributes[index].value;
  return NULL;
}

static int field_allowed(const AbString *name, const char *const *allowed,
                         size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    if (string_is(name, allowed[index]))
      return 1;
  return 0;
}

static int stable_kind(const AbString *value) {
  size_t index;
  if (!value || !value->length)
    return 0;
  for (index = 0; index < value->length; index++) {
    unsigned char byte = (unsigned char)value->data[index];
    if (!(byte >= 'a' && byte <= 'z') &&
        (!index || !((byte >= '0' && byte <= '9') || byte == '-')))
      return 0;
  }
  return 1;
}

static int string_array(const AbValue *value, int require_nonempty) {
  size_t index;
  size_t other;
  if (!value || value->kind != AB_VALUE_ARRAY ||
      (require_nonempty && !value->as.array.count))
    return 0;
  for (index = 0; index < value->as.array.count; index++)
    if (value->as.array.items[index].kind != AB_VALUE_STRING ||
        !value->as.array.items[index].as.text.length)
      return 0;
  for (index = 0; index < value->as.array.count; index++)
    for (other = index + 1; other < value->as.array.count; other++)
      if (ab_string_equal(&value->as.array.items[index].as.text,
                          &value->as.array.items[other].as.text))
        return 0;
  return 1;
}

static ArchbirdStatus decode_endpoint(ArchbirdEngine *engine,
                                      const AbValue *value,
                                      AbPathEndpoint *out) {
  static const char *const allowed[] = {"kind", "patterns"};
  const AbValue *kind;
  const AbValue *patterns;
  size_t index;
  if (!value || value->kind != AB_VALUE_OBJECT)
    return invalid(engine, "source and target must be endpoint objects");
  for (index = 0; index < value->as.object.count; index++)
    if (!field_allowed(&value->as.object.fields[index].name, allowed,
                       sizeof(allowed) / sizeof(allowed[0])))
      return invalid(engine, "endpoint contains an unknown field");
  kind = ab_value_member(value, "kind");
  patterns = ab_value_member(value, "patterns");
  if (!kind || kind->kind != AB_VALUE_STRING ||
      (!stable_kind(&kind->as.text) && !string_is(&kind->as.text, "any")))
    return invalid(engine, "endpoint kind must be any or a stable entity kind");
  if (!string_array(patterns, 1))
    return invalid(engine, "endpoint patterns must be a nonempty string array");
  out->kind = &kind->as.text;
  out->patterns = patterns;
  return ARCHBIRD_OK;
}

static int relation_allowed(const AbString *value) {
  static const char *const allowed[] = {
      "bridges", "builds",   "calls",      "declarations",
      "imports", "packages", "references", "tests",
  };
  size_t index;
  for (index = 0; index < sizeof(allowed) / sizeof(allowed[0]); index++)
    if (string_is(value, allowed[index]))
      return 1;
  return 0;
}

static ArchbirdStatus decode_request(ArchbirdEngine *engine,
                                     const AbValue *request,
                                     AbPathRequest *out) {
  static const char *const allowed[] = {
      "artifact",  "direction",       "level",     "max_depth",
      "max_paths", "producer_policy", "relations", "schema_version",
      "source",    "target",
  };
  static const AbString file = {(char *)"file", 4};
  static const AbString downstream = {(char *)"downstream", 10};
  static const AbString compatible = {(char *)"compatible", 10};
  const AbValue *schema;
  const AbValue *value;
  uint64_t number;
  size_t index;
  memset(out, 0, sizeof(*out));
  if (!request || request->kind != AB_VALUE_OBJECT)
    return invalid(engine, "request must be an object");
  for (index = 0; index < request->as.object.count; index++)
    if (!field_allowed(&request->as.object.fields[index].name, allowed,
                       sizeof(allowed) / sizeof(allowed[0])))
      return invalid(engine, "request contains an unknown field");
  if (!ab_value_string_is(ab_value_member(request, "artifact"), "path-request"))
    return invalid(engine, "artifact must be path-request");
  schema = ab_value_member(request, "schema_version");
  if (!schema || !ab_value_u64(schema, &number) ||
      number != ARCHBIRD_PATH_SCHEMA_CURRENT)
    return invalid(engine, "schema_version must be 1");
  TRY(decode_endpoint(engine, ab_value_member(request, "source"),
                      &out->source));
  TRY(decode_endpoint(engine, ab_value_member(request, "target"),
                      &out->target));
  value = ab_value_member(request, "level");
  out->level =
      value && value->kind == AB_VALUE_STRING ? &value->as.text : &file;
  if (!string_is(out->level, "component") && !string_is(out->level, "file") &&
      !string_is(out->level, "symbol"))
    return invalid(engine, "level must be component, file, or symbol");
  value = ab_value_member(request, "direction");
  out->direction =
      value && value->kind == AB_VALUE_STRING ? &value->as.text : &downstream;
  if (!string_is(out->direction, "downstream") &&
      !string_is(out->direction, "upstream") &&
      !string_is(out->direction, "both"))
    return invalid(engine, "direction must be downstream, upstream, or both");
  value = ab_value_member(request, "producer_policy");
  out->producer_policy =
      value && value->kind == AB_VALUE_STRING ? &value->as.text : &compatible;
  if (!string_is(out->producer_policy, "compatible") &&
      !string_is(out->producer_policy, "current"))
    return invalid(engine, "producer_policy must be compatible or current");
  out->relations = ab_value_member(request, "relations");
  if (out->relations && !string_array(out->relations, 1))
    return invalid(engine,
                   "relations must be a nonempty array of unique strings");
  for (index = 0; out->relations && index < out->relations->as.array.count;
       index++)
    if (!relation_allowed(&out->relations->as.array.items[index].as.text))
      return invalid(engine, "relations contains an unsupported family");
  out->max_depth = 8;
  value = ab_value_member(request, "max_depth");
  if (value) {
    if (!ab_value_u64(value, &number) || number > 64)
      return invalid(engine, "max_depth must be an integer from 0 to 64");
    out->max_depth = (size_t)number;
  }
  out->max_paths = 8;
  value = ab_value_member(request, "max_paths");
  if (value) {
    if (!ab_value_u64(value, &number) || !number || number > 100)
      return invalid(engine, "max_paths must be an integer from 1 to 100");
    out->max_paths = (size_t)number;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_producer(ArchbirdEngine *engine,
                                        const AbValue *map,
                                        const AbPathRequest *request,
                                        const char **out_compatibility) {
  const AbValue *tool = ab_value_member(map, "tool");
  const AbValue *digest = tool && tool->kind == AB_VALUE_OBJECT
                              ? ab_value_member(tool, "implementation_sha256")
                              : NULL;
  int current =
      digest && digest->kind == AB_VALUE_STRING &&
      digest->as.text.length == 64 &&
      !memcmp(digest->as.text.data, ARCHBIRD_IMPLEMENTATION_SHA256, 64);
  *out_compatibility = current ? "current"
                               : (digest && digest->kind == AB_VALUE_STRING &&
                                          digest->as.text.length == 64
                                      ? "different"
                                      : "unknown");
  if (!string_is(request->producer_policy, "current"))
    return ARCHBIRD_OK;
  if (!current)
    return archbird_error_set(
        engine, ARCHBIRD_POLICY_REJECTED, ARCHBIRD_NO_OFFSET,
        "saved Map core does not match the active core implementation");
  return ARCHBIRD_OK;
}

static ArchbirdStatus append_default_relations(AbBuffer *buffer,
                                               const AbString *level) {
  if (string_is(level, "symbol"))
    return ab_buffer_literal(buffer, "[\"calls\",\"references\"]");
  return ab_buffer_literal(
      buffer, "[\"bridges\",\"builds\",\"calls\",\"declarations\",\"imports\","
              "\"packages\",\"references\",\"tests\"]");
}

static ArchbirdStatus append_relations(AbBuffer *buffer,
                                       const AbPathRequest *request) {
  static const char *const order[] = {
      "bridges", "builds",   "calls",      "declarations",
      "imports", "packages", "references", "tests",
  };
  size_t family;
  int first = 1;
  if (!request->relations)
    return append_default_relations(buffer, request->level);
  TRY(ab_buffer_literal(buffer, "["));
  for (family = 0; family < sizeof(order) / sizeof(order[0]); family++) {
    size_t index;
    for (index = 0; index < request->relations->as.array.count; index++) {
      if (!string_is(&request->relations->as.array.items[index].as.text,
                     order[family]))
        continue;
      if (!first)
        TRY(ab_buffer_literal(buffer, ","));
      TRY(ab_buffer_json_string(buffer, order[family], strlen(order[family])));
      first = 0;
    }
  }
  return ab_buffer_literal(buffer, "]");
}

static int string_pointer_compare(const void *left_raw, const void *right_raw) {
  const AbString *const *left = (const AbString *const *)left_raw;
  const AbString *const *right = (const AbString *const *)right_raw;
  return ab_string_compare(*left, *right);
}

static ArchbirdStatus render_sorted_string_array(AbBuffer *buffer,
                                                 const AbValue *values) {
  const AbString **items = NULL;
  size_t index;
  ArchbirdStatus status;
  if (!values || values->kind != AB_VALUE_ARRAY ||
      values->as.array.count > SIZE_MAX / sizeof(*items))
    return ARCHBIRD_INVALID_SCHEMA;
  if (values->as.array.count)
    items = (const AbString **)ab_calloc(buffer->engine, values->as.array.count,
                                         sizeof(*items));
  if (values->as.array.count && !items)
    return ARCHBIRD_OUT_OF_MEMORY;
  for (index = 0; index < values->as.array.count; index++)
    items[index] = &values->as.array.items[index].as.text;
  if (values->as.array.count > 1)
    qsort(items, values->as.array.count, sizeof(*items),
          string_pointer_compare);
  status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < values->as.array.count;
       index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, items[index]->data,
                                     items[index]->length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  ab_free(buffer->engine, items);
  return status;
}

static ArchbirdStatus graph_definition(ArchbirdEngine *engine,
                                       const AbPathRequest *request,
                                       AbValue *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_literal(&buffer,
                             "{\"id\":\"connection_path_graph\",\"level\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, request->level->data,
                                   request->level->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"relations\":");
  if (status == ARCHBIRD_OK)
    status = append_relations(&buffer, request);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"select\":\"graph\"}");
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, buffer.data, buffer.length, out);
  ab_buffer_free(&buffer);
  return status;
}

static int node_compare(const void *left_raw, const void *right_raw) {
  const AbPathNode *left = (const AbPathNode *)left_raw;
  const AbPathNode *right = (const AbPathNode *)right_raw;
  return ab_string_compare(left->id, right->id);
}

static size_t find_node(const AbPathGraph *graph, const AbString *id) {
  size_t low = 0;
  size_t high = graph->node_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(graph->nodes[middle].id, id);
    if (compared < 0)
      low = middle + 1;
    else
      high = middle;
  }
  return low < graph->node_count && ab_string_equal(graph->nodes[low].id, id)
             ? low
             : SIZE_MAX;
}

static void graph_free(ArchbirdEngine *engine, AbPathGraph *graph) {
  ab_free(engine, graph->nodes);
  ab_free(engine, graph->edges);
  ab_free(engine, graph->arcs);
  ab_free(engine, graph->arc_offsets);
  memset(graph, 0, sizeof(*graph));
}

static ArchbirdStatus graph_decode(ArchbirdEngine *engine,
                                   const AbProjectionData *projection,
                                   const AbPathRequest *request,
                                   AbPathGraph *out) {
  size_t index;
  size_t node_count = 0;
  size_t edge_count = 0;
  size_t direction_factor = string_is(request->direction, "both") ? 2 : 1;
  size_t *cursor = NULL;
  memset(out, 0, sizeof(*out));
  for (index = 0; index < projection->item_count; index++) {
    const AbValue *record = attribute(&projection->items[index], "record_kind");
    node_count += ab_projection_value_is(record, "node");
    edge_count += ab_projection_value_is(record, "relation");
  }
  if (node_count > SIZE_MAX / sizeof(*out->nodes) ||
      edge_count > SIZE_MAX / sizeof(*out->edges) ||
      edge_count > SIZE_MAX / direction_factor ||
      edge_count * direction_factor > SIZE_MAX / sizeof(*out->arcs) ||
      node_count == SIZE_MAX)
    return ARCHBIRD_LIMIT_EXCEEDED;
  out->nodes = (AbPathNode *)ab_calloc(engine, node_count, sizeof(*out->nodes));
  out->edges = (AbPathEdge *)ab_calloc(engine, edge_count, sizeof(*out->edges));
  out->arcs = (AbPathArc *)ab_calloc(engine, edge_count * direction_factor,
                                     sizeof(*out->arcs));
  out->arc_offsets =
      (size_t *)ab_calloc(engine, node_count + 1, sizeof(*out->arc_offsets));
  if ((node_count && !out->nodes) || (edge_count && !out->edges) ||
      (edge_count && !out->arcs) || !out->arc_offsets) {
    graph_free(engine, out);
    return ARCHBIRD_OUT_OF_MEMORY;
  }
  for (index = 0; index < projection->item_count; index++) {
    const AbProjectionItem *item = &projection->items[index];
    if (ab_projection_value_is(attribute(item, "record_kind"), "node")) {
      const AbValue *id = attribute(item, "id");
      if (!id || id->kind != AB_VALUE_STRING) {
        graph_free(engine, out);
        return ARCHBIRD_INVALID_SCHEMA;
      }
      out->nodes[out->node_count].item = item;
      out->nodes[out->node_count].id = &id->as.text;
      out->node_count++;
    }
  }
  if (out->node_count > 1)
    qsort(out->nodes, out->node_count, sizeof(*out->nodes), node_compare);
  for (index = 0; index < projection->item_count; index++) {
    const AbProjectionItem *item = &projection->items[index];
    if (ab_projection_value_is(attribute(item, "record_kind"), "relation")) {
      const AbValue *source = attribute(item, "source");
      const AbValue *target = attribute(item, "target");
      size_t source_index;
      size_t target_index;
      if (!source || source->kind != AB_VALUE_STRING || !target ||
          target->kind != AB_VALUE_STRING) {
        graph_free(engine, out);
        return ARCHBIRD_INVALID_SCHEMA;
      }
      source_index = find_node(out, &source->as.text);
      target_index = find_node(out, &target->as.text);
      if (source_index == SIZE_MAX || target_index == SIZE_MAX) {
        graph_free(engine, out);
        return ARCHBIRD_INVALID_SCHEMA;
      }
      out->edges[out->edge_count].item = item;
      out->edges[out->edge_count].source = source_index;
      out->edges[out->edge_count].target = target_index;
      out->edge_count++;
    }
  }
  for (index = 0; index < out->edge_count; index++) {
    if (!string_is(request->direction, "upstream"))
      out->arc_offsets[out->edges[index].source + 1]++;
    if (!string_is(request->direction, "downstream"))
      out->arc_offsets[out->edges[index].target + 1]++;
  }
  for (index = 1; index <= out->node_count; index++)
    out->arc_offsets[index] += out->arc_offsets[index - 1];
  out->arc_count = out->arc_offsets[out->node_count];
  cursor = (size_t *)ab_calloc(engine, out->node_count, sizeof(*cursor));
  if (out->node_count && !cursor) {
    graph_free(engine, out);
    return ARCHBIRD_OUT_OF_MEMORY;
  }
  for (index = 0; index < out->edge_count; index++) {
    const AbPathEdge *edge = &out->edges[index];
    if (!string_is(request->direction, "upstream")) {
      size_t slot = out->arc_offsets[edge->source] + cursor[edge->source]++;
      out->arcs[slot].edge = index;
      out->arcs[slot].next = edge->target;
      out->arcs[slot].reverse = 0;
    }
    if (!string_is(request->direction, "downstream")) {
      size_t slot = out->arc_offsets[edge->target] + cursor[edge->target]++;
      out->arcs[slot].edge = index;
      out->arcs[slot].next = edge->source;
      out->arcs[slot].reverse = 1;
    }
  }
  ab_free(engine, cursor);
  return ARCHBIRD_OK;
}

static const char *
path_graph_classification(const AbProjectionData *projection) {
  const char *classification = ab_projection_data_classification(projection);
  size_t index;
  if (strcmp(classification, "complete"))
    return classification;
  for (index = 0; index < projection->item_count; index++) {
    const AbProjectionItem *item = &projection->items[index];
    if (ab_projection_value_is(attribute(item, "record_kind"), "coverage") &&
        !string_is(&item->state, "current"))
      return "incomplete";
  }
  return classification;
}

static const AbProjectionItem *
path_graph_coverage(const AbProjectionData *projection) {
  size_t index;
  for (index = 0; index < projection->item_count; index++)
    if (ab_projection_value_is(
            attribute(&projection->items[index], "record_kind"), "coverage"))
      return &projection->items[index];
  return NULL;
}

static AbString node_identity(const AbPathNode *node) {
  const char *colon =
      (const char *)memchr(node->id->data, ':', node->id->length);
  AbString identity;
  if (!colon || colon + 1 > node->id->data + node->id->length)
    return *node->id;
  identity.data = (char *)colon + 1;
  identity.length = node->id->length - (size_t)(colon + 1 - node->id->data);
  return identity;
}

static int endpoint_matches(const AbPathEndpoint *endpoint,
                            const AbPathNode *node) {
  const AbValue *kind = attribute(node->item, "entity_kind");
  const AbValue *path = attribute(node->item, "path");
  const AbString *label = &node->item->label;
  AbString identity = node_identity(node);
  size_t index;
  if (!kind || kind->kind != AB_VALUE_STRING ||
      (!string_is(endpoint->kind, "any") &&
       !ab_string_equal(endpoint->kind, &kind->as.text)))
    return 0;
  for (index = 0; index < endpoint->patterns->as.array.count; index++) {
    const AbString *pattern =
        &endpoint->patterns->as.array.items[index].as.text;
    if (ab_string_equal(pattern, &identity) ||
        ab_map_glob_match(pattern, &identity) ||
        ab_string_equal(pattern, label) || ab_map_glob_match(pattern, label) ||
        (path && path->kind == AB_VALUE_STRING &&
         (ab_string_equal(pattern, &path->as.text) ||
          ab_map_glob_match(pattern, &path->as.text))))
      return 1;
  }
  return 0;
}

static ArchbirdStatus candidates_build(ArchbirdEngine *engine,
                                       const AbPathGraph *graph,
                                       const AbPathEndpoint *endpoint,
                                       AbPathCandidates *out) {
  size_t index;
  memset(out, 0, sizeof(*out));
  for (index = 0; index < graph->node_count; index++)
    out->count += endpoint_matches(endpoint, &graph->nodes[index]);
  if (out->count > SIZE_MAX / sizeof(*out->items))
    return ARCHBIRD_LIMIT_EXCEEDED;
  out->items = (size_t *)ab_calloc(engine, out->count, sizeof(*out->items));
  if (out->count && !out->items)
    return ARCHBIRD_OUT_OF_MEMORY;
  out->count = 0;
  for (index = 0; index < graph->node_count; index++)
    if (endpoint_matches(endpoint, &graph->nodes[index]))
      out->items[out->count++] = index;
  return ARCHBIRD_OK;
}

static const char *endpoint_state(const AbPathCandidates *candidates) {
  return !candidates->count
             ? "unresolved"
             : (candidates->count == 1 ? "resolved" : "ambiguous");
}

static ArchbirdStatus render_endpoint(AbBuffer *buffer,
                                      const AbPathCandidates *candidates,
                                      const AbPathGraph *graph) {
  size_t index;
  TRY(ab_buffer_literal(buffer, "{\"candidates\":["));
  for (index = 0; index < candidates->count; index++) {
    if (index)
      TRY(ab_buffer_literal(buffer, ","));
    TRY(ab_projection_item_render(buffer,
                                  graph->nodes[candidates->items[index]].item));
  }
  TRY(ab_buffer_literal(buffer, "],\"state\":"));
  TRY(ab_buffer_json_string(buffer, endpoint_state(candidates),
                            strlen(endpoint_state(candidates))));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus bfs(ArchbirdEngine *engine, const AbPathGraph *graph,
                          const AbPathCandidates *sources, size_t max_depth,
                          size_t *distance, int *frontier) {
  size_t *queue;
  size_t head = 0;
  size_t tail = 0;
  size_t index;
  if (graph->node_count > SIZE_MAX / sizeof(*queue))
    return ARCHBIRD_LIMIT_EXCEEDED;
  queue = (size_t *)ab_calloc(engine, graph->node_count, sizeof(*queue));
  if (graph->node_count && !queue)
    return ARCHBIRD_OUT_OF_MEMORY;
  for (index = 0; index < graph->node_count; index++)
    distance[index] = SIZE_MAX;
  for (index = 0; index < sources->count; index++) {
    distance[sources->items[index]] = 0;
    queue[tail++] = sources->items[index];
  }
  *frontier = 0;
  while (head < tail) {
    size_t node = queue[head++];
    size_t arc_index;
    if (distance[node] == max_depth) {
      for (arc_index = graph->arc_offsets[node];
           arc_index < graph->arc_offsets[node + 1]; arc_index++)
        if (distance[graph->arcs[arc_index].next] == SIZE_MAX)
          *frontier = 1;
      continue;
    }
    for (arc_index = graph->arc_offsets[node];
         arc_index < graph->arc_offsets[node + 1]; arc_index++) {
      size_t next = graph->arcs[arc_index].next;
      if (distance[next] != SIZE_MAX)
        continue;
      distance[next] = distance[node] + 1;
      queue[tail++] = next;
    }
  }
  ab_free(engine, queue);
  return ARCHBIRD_OK;
}

static size_t nearest_targets(const AbPathCandidates *targets,
                              const size_t *distance, uint8_t *target_mask) {
  size_t shortest = SIZE_MAX;
  size_t index;
  for (index = 0; index < targets->count; index++) {
    size_t target = targets->items[index];
    if (distance[target] < shortest)
      shortest = distance[target];
  }
  if (shortest == SIZE_MAX)
    return shortest;
  for (index = 0; index < targets->count; index++) {
    size_t target = targets->items[index];
    if (distance[target] == shortest)
      target_mask[target] = 1;
  }
  return shortest;
}

static void shortest_reachable(const AbPathGraph *graph, const size_t *distance,
                               const uint8_t *target_mask, size_t shortest,
                               uint8_t *reachable) {
  size_t node;
  size_t depth;
  for (node = 0; node < graph->node_count; node++)
    reachable[node] = target_mask[node];
  for (depth = shortest; depth; depth--) {
    for (node = 0; node < graph->node_count; node++) {
      size_t arc_index;
      if (distance[node] != depth - 1)
        continue;
      for (arc_index = graph->arc_offsets[node];
           arc_index < graph->arc_offsets[node + 1]; arc_index++) {
        size_t next = graph->arcs[arc_index].next;
        if (distance[next] == depth && reachable[next]) {
          reachable[node] = 1;
          break;
        }
      }
    }
  }
}

static ArchbirdStatus render_path(AbPathEnumeration *enumeration,
                                  size_t depth) {
  size_t index;
  if (enumeration->count)
    TRY(ab_buffer_literal(enumeration->output, ","));
  TRY(ab_buffer_literal(enumeration->output, "{\"length\":"));
  TRY(ab_buffer_u64(enumeration->output, depth));
  TRY(ab_buffer_literal(enumeration->output, ",\"nodes\":["));
  for (index = 0; index <= depth; index++) {
    const AbString *id =
        enumeration->graph->nodes[enumeration->nodes[index]].id;
    if (index)
      TRY(ab_buffer_literal(enumeration->output, ","));
    TRY(ab_buffer_json_string(enumeration->output, id->data, id->length));
  }
  TRY(ab_buffer_literal(enumeration->output, "],\"source\":"));
  TRY(ab_buffer_json_string(
      enumeration->output,
      enumeration->graph->nodes[enumeration->nodes[0]].id->data,
      enumeration->graph->nodes[enumeration->nodes[0]].id->length));
  TRY(ab_buffer_literal(enumeration->output, ",\"steps\":["));
  for (index = 0; index < depth; index++) {
    const AbPathArc *arc = &enumeration->graph->arcs[enumeration->arcs[index]];
    const AbPathEdge *edge = &enumeration->graph->edges[arc->edge];
    if (index)
      TRY(ab_buffer_literal(enumeration->output, ","));
    TRY(ab_buffer_literal(enumeration->output, "{\"relation\":"));
    TRY(ab_projection_item_render(enumeration->output, edge->item));
    TRY(ab_buffer_literal(enumeration->output, ",\"traversal\":"));
    TRY(ab_buffer_json_string(enumeration->output,
                              arc->reverse ? "reverse" : "forward",
                              arc->reverse ? 7 : 7));
    TRY(ab_buffer_literal(enumeration->output, "}"));
  }
  TRY(ab_buffer_literal(enumeration->output, "],\"target\":"));
  TRY(ab_buffer_json_string(
      enumeration->output,
      enumeration->graph->nodes[enumeration->nodes[depth]].id->data,
      enumeration->graph->nodes[enumeration->nodes[depth]].id->length));
  TRY(ab_buffer_literal(enumeration->output, "}"));
  enumeration->count++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus enumerate_shortest(AbPathEnumeration *enumeration,
                                         size_t node, size_t depth) {
  size_t arc_index;
  if (depth == enumeration->shortest) {
    if (!enumeration->targets[node])
      return ARCHBIRD_INVALID_SCHEMA;
    if (enumeration->count == enumeration->max_paths) {
      enumeration->truncated = 1;
      return ARCHBIRD_OK;
    }
    return render_path(enumeration, depth);
  }
  for (arc_index = enumeration->graph->arc_offsets[node];
       !enumeration->truncated &&
       arc_index < enumeration->graph->arc_offsets[node + 1];
       arc_index++) {
    const AbPathArc *arc = &enumeration->graph->arcs[arc_index];
    if (enumeration->distance[arc->next] != depth + 1 ||
        !enumeration->reachable[arc->next])
      continue;
    enumeration->arcs[depth] = arc_index;
    enumeration->nodes[depth + 1] = arc->next;
    TRY(enumerate_shortest(enumeration, arc->next, depth + 1));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_normalized_request(AbBuffer *buffer,
                                                const AbPathRequest *request,
                                                const AbValue *source,
                                                const AbValue *target) {
  const AbValue *source_kind = ab_value_member(source, "kind");
  const AbValue *source_patterns = ab_value_member(source, "patterns");
  const AbValue *target_kind = ab_value_member(target, "kind");
  const AbValue *target_patterns = ab_value_member(target, "patterns");
  if (!source_kind || source_kind->kind != AB_VALUE_STRING ||
      !source_patterns || source_patterns->kind != AB_VALUE_ARRAY ||
      !target_kind || target_kind->kind != AB_VALUE_STRING ||
      !target_patterns || target_patterns->kind != AB_VALUE_ARRAY)
    return ARCHBIRD_INVALID_SCHEMA;
  TRY(ab_buffer_literal(buffer,
                        "{\"artifact\":\"path-request\",\"direction\":"));
  TRY(ab_buffer_json_string(buffer, request->direction->data,
                            request->direction->length));
  TRY(ab_buffer_literal(buffer, ",\"level\":"));
  TRY(ab_buffer_json_string(buffer, request->level->data,
                            request->level->length));
  TRY(ab_buffer_literal(buffer, ",\"max_depth\":"));
  TRY(ab_buffer_u64(buffer, request->max_depth));
  TRY(ab_buffer_literal(buffer, ",\"max_paths\":"));
  TRY(ab_buffer_u64(buffer, request->max_paths));
  TRY(ab_buffer_literal(buffer, ",\"producer_policy\":"));
  TRY(ab_buffer_json_string(buffer, request->producer_policy->data,
                            request->producer_policy->length));
  TRY(ab_buffer_literal(buffer, ",\"relations\":"));
  TRY(append_relations(buffer, request));
  TRY(ab_buffer_literal(buffer, ",\"schema_version\":1,\"source\":{\"kind\":"));
  TRY(ab_value_render(buffer, source_kind));
  TRY(ab_buffer_literal(buffer, ",\"patterns\":"));
  TRY(render_sorted_string_array(buffer, source_patterns));
  TRY(ab_buffer_literal(buffer, "},\"target\":{\"kind\":"));
  TRY(ab_value_render(buffer, target_kind));
  TRY(ab_buffer_literal(buffer, ",\"patterns\":"));
  TRY(render_sorted_string_array(buffer, target_patterns));
  return ab_buffer_literal(buffer, "}}");
}

static ArchbirdStatus append_path_digest(AbBuffer *base, AbBuffer *out) {
  uint8_t digest[32];
  char hex[65];
  if (!base->length || base->data[base->length - 1] != '}')
    return ARCHBIRD_INVALID_SCHEMA;
  TRY(archbird_sha256(base->data, base->length, digest));
  archbird_sha256_hex(digest, hex);
  TRY(ab_buffer_append(out, base->data, base->length - 1));
  TRY(ab_buffer_literal(out, ",\"path_sha256\":"));
  TRY(ab_buffer_json_string(out, hex, 64));
  return ab_buffer_literal(out, "}");
}

ArchbirdStatus ab_path_execute_value(ArchbirdEngine *engine, const AbValue *map,
                                     const AbValue *resolution,
                                     const AbValue *request, AbBuffer *out) {
  AbPathRequest decoded;
  AbValue definition = {0};
  AbProjectionPlan plan = {0};
  AbProjectionResult projection = {0};
  AbPathGraph graph = {0};
  AbPathCandidates sources = {0};
  AbPathCandidates targets = {0};
  AbBuffer paths;
  AbBuffer base;
  const AbValue *project;
  const AbValue *source_tool;
  const AbValue *evidence;
  const AbValue *source_request;
  const AbValue *target_request;
  const char *compatibility = NULL;
  const char *classification;
  const char *outcome;
  const char *reason;
  size_t *distance = NULL;
  size_t *path_nodes = NULL;
  size_t *path_arcs = NULL;
  uint8_t *reachable = NULL;
  uint8_t *target_mask = NULL;
  size_t source_index;
  size_t shortest = SIZE_MAX;
  size_t path_count = 0;
  int frontier = 0;
  int paths_truncated = 0;
  ArchbirdStatus status;
  ab_buffer_init(&paths, engine);
  ab_buffer_init(&base, engine);
  status = decode_request(engine, request, &decoded);
  if (status == ARCHBIRD_OK)
    status = validate_producer(engine, map, &decoded, &compatibility);
  if (status == ARCHBIRD_OK)
    status = graph_definition(engine, &decoded, &definition);
  if (status == ARCHBIRD_OK) {
    const AbValue *id = ab_value_member(&definition, "id");
    status =
        ab_projection_plan_compile(engine, &definition, &id->as.text, &plan);
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_evaluate(engine, &plan, map, resolution,
                                         &projection);
  if (status == ARCHBIRD_OK)
    status = graph_decode(engine, &projection.data, &decoded, &graph);
  if (status == ARCHBIRD_OK)
    status = candidates_build(engine, &graph, &decoded.source, &sources);
  if (status == ARCHBIRD_OK)
    status = candidates_build(engine, &graph, &decoded.target, &targets);
  if (status == ARCHBIRD_OK && graph.node_count > SIZE_MAX / sizeof(size_t))
    status = ARCHBIRD_LIMIT_EXCEEDED;
  if (status == ARCHBIRD_OK) {
    distance = (size_t *)ab_calloc(engine, graph.node_count, sizeof(*distance));
    path_nodes =
        (size_t *)ab_calloc(engine, decoded.max_depth + 1, sizeof(*path_nodes));
    path_arcs = (size_t *)ab_calloc(
        engine, decoded.max_depth ? decoded.max_depth : 1, sizeof(*path_arcs));
    reachable =
        (uint8_t *)ab_calloc(engine, graph.node_count, sizeof(*reachable));
    target_mask =
        (uint8_t *)ab_calloc(engine, graph.node_count, sizeof(*target_mask));
    if ((graph.node_count && (!distance || !reachable || !target_mask)) ||
        !path_nodes || !path_arcs)
      status = ARCHBIRD_OUT_OF_MEMORY;
  }
  if (status == ARCHBIRD_OK && sources.count && targets.count)
    status =
        bfs(engine, &graph, &sources, decoded.max_depth, distance, &frontier);
  if (status == ARCHBIRD_OK && sources.count && targets.count) {
    shortest = nearest_targets(&targets, distance, target_mask);
    if (shortest != SIZE_MAX)
      shortest_reachable(&graph, distance, target_mask, shortest, reachable);
  }
  for (source_index = 0; status == ARCHBIRD_OK && shortest != SIZE_MAX &&
                         source_index < sources.count;
       source_index++) {
    AbPathEnumeration enumeration = {
        .graph = &graph,
        .distance = distance,
        .reachable = reachable,
        .targets = target_mask,
        .shortest = shortest,
        .max_paths = decoded.max_paths,
        .count = path_count,
        .nodes = path_nodes,
        .arcs = path_arcs,
        .output = &paths,
    };
    if (!reachable[sources.items[source_index]])
      continue;
    path_nodes[0] = sources.items[source_index];
    status = enumerate_shortest(&enumeration, sources.items[source_index], 0);
    path_count = enumeration.count;
    if (enumeration.truncated) {
      paths_truncated = 1;
      break;
    }
  }
  classification = status == ARCHBIRD_OK
                       ? path_graph_classification(&projection.data)
                       : "unknown";
  if (status == ARCHBIRD_OK) {
    if (path_count) {
      outcome = "found";
      reason = paths_truncated ? "path-limit" : "witnesses";
    } else if (!sources.count || !targets.count) {
      outcome = "unknown";
      reason = "endpoint-unresolved";
    } else if (strcmp(classification, "complete")) {
      outcome = "unknown";
      reason = "graph-incomplete";
    } else if (frontier) {
      outcome = "unknown";
      reason = "depth-frontier";
    } else {
      outcome = "absent";
      reason = "exhaustive";
    }
    project = ab_value_member(map, "project");
    source_tool = ab_value_member(map, "tool");
    evidence = ab_value_member(map, "evidence");
    source_request = ab_value_member(request, "source");
    target_request = ab_value_member(request, "target");
    if (!project || project->kind != AB_VALUE_STRING || !source_tool ||
        source_tool->kind != AB_VALUE_OBJECT || !evidence ||
        evidence->kind != AB_VALUE_OBJECT)
      status = invalid(engine, "Map identity fields are invalid");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, "{\"artifact\":\"path\",\"evidence\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&base, evidence);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, ",\"graph\":{\"classification\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(&base, classification, strlen(classification));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, ",\"completeness\":");
    if (status == ARCHBIRD_OK)
      status = ab_projection_completeness_render(&base, &projection.data);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, ",\"coverage\":");
    if (status == ARCHBIRD_OK) {
      const AbProjectionItem *coverage = path_graph_coverage(&projection.data);
      status = coverage ? ab_projection_item_render(&base, coverage)
                        : ab_buffer_literal(&base, "null");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, ",\"projection_definition_sha256\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&base, plan.definition_sha256, 64);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, ",\"projection_result_sha256\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&base, projection.result_sha256, 64);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, "},\"outcome\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&base, outcome, strlen(outcome));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, ",\"paths\":[");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&base, paths.data, paths.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, "],\"producer_compatibility\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(&base, compatibility, strlen(compatibility));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, ",\"project\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&base, project);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, ",\"reason\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&base, reason, strlen(reason));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, ",\"request\":");
    if (status == ARCHBIRD_OK)
      status = render_normalized_request(&base, &decoded, source_request,
                                         target_request);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, ",\"schema_version\":1,\"search\":{");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, "\"frontier\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, frontier ? "true" : "false");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, ",\"max_depth\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&base, decoded.max_depth);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, ",\"max_paths\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&base, decoded.max_paths);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, ",\"path_count\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&base, path_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, ",\"shortest_length\":");
    if (status == ARCHBIRD_OK)
      status = shortest == SIZE_MAX ? ab_buffer_literal(&base, "null")
                                    : ab_buffer_u64(&base, shortest);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, ",\"truncated\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, paths_truncated ? "true" : "false");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, "},\"source\":");
    if (status == ARCHBIRD_OK)
      status = render_endpoint(&base, &sources, &graph);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, ",\"source_tool\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&base, source_tool);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, ",\"target\":");
    if (status == ARCHBIRD_OK)
      status = render_endpoint(&base, &targets, &graph);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&base, "}");
    if (status == ARCHBIRD_OK)
      status = append_path_digest(&base, out);
  }
  ab_free(engine, target_mask);
  ab_free(engine, reachable);
  ab_free(engine, path_arcs);
  ab_free(engine, path_nodes);
  ab_free(engine, distance);
  ab_free(engine, targets.items);
  ab_free(engine, sources.items);
  graph_free(engine, &graph);
  ab_projection_result_free(engine, &projection);
  ab_projection_plan_free(engine, &plan);
  ab_value_free(engine, &definition);
  ab_buffer_free(&base);
  ab_buffer_free(&paths);
  return status;
}

ArchbirdStatus archbird_map_path(ArchbirdEngine *engine,
                                 const uint8_t *map_json, size_t map_length,
                                 const uint8_t *resolution_json,
                                 size_t resolution_length,
                                 const uint8_t *request_json,
                                 size_t request_length, uint32_t json_flags,
                                 ArchbirdWriteFn write_fn, void *user_data) {
  AbValue map = {0};
  AbValue resolution = {0};
  AbValue request = {0};
  AbBuffer output;
  ArchbirdStatus status;
  if (!engine || !map_json || !map_length ||
      (!resolution_json && resolution_length) || !request_json ||
      !request_length || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&output, engine);
  status = ab_json_value_decode(engine, map_json, map_length, &map);
  if (status == ARCHBIRD_OK && resolution_length)
    status = ab_json_value_decode(engine, resolution_json, resolution_length,
                                  &resolution);
  if (status == ARCHBIRD_OK)
    status =
        ab_json_value_decode(engine, request_json, request_length, &request);
  if (status == ARCHBIRD_OK)
    status = ab_path_execute_value(engine, &map,
                                   resolution_length ? &resolution : NULL,
                                   &request, &output);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, output.data, output.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&output);
  ab_value_free(engine, &request);
  ab_value_free(engine, &resolution);
  ab_value_free(engine, &map);
  return status;
}

#undef TRY
