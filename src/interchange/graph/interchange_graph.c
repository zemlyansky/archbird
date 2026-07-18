#include <archbird/archbird.h>

#include "archbird_internal.h"
#include "json_value.h"
#include "model.h"
#include "render_internal.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct GraphNode {
  AbString identity;
  AbString kind;
  AbString label;
  AbString parent;
  AbString attributes;
  AbString evidence;
} GraphNode;

typedef struct GraphEdge {
  AbString kind;
  AbString source;
  AbString target;
  AbString classification;
  AbString evidence;
  AbString *names;
  size_t name_count;
  size_t name_capacity;
} GraphEdge;

typedef struct Membership {
  AbString path;
  AbString component;
} Membership;

typedef struct GraphData {
  ArchbirdEngine *engine;
  AbString project;
  AbString input_sha256;
  AbString source_artifact;
  AbString tool_sha256;
  uint64_t source_schema;
  ArchbirdGraphView view;
  GraphNode *nodes;
  size_t node_count;
  size_t node_capacity;
  GraphEdge *edges;
  size_t edge_count;
  size_t edge_capacity;
} GraphData;

static ArchbirdStatus graph_error(GraphData *graph, const char *message) {
  return archbird_error_set(graph->engine, ARCHBIRD_INVALID_SCHEMA,
                            ARCHBIRD_NO_OFFSET, "%s", message);
}

static int text_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || !memcmp(value->data, literal, length));
}

static const AbString *required_text(GraphData *graph, const AbValue *object,
                                     const char *name, const char *where) {
  const AbValue *value = ab_value_member(object, name);
  if (!value || value->kind != AB_VALUE_STRING) {
    archbird_error_set(graph->engine, ARCHBIRD_INVALID_SCHEMA,
                       ARCHBIRD_NO_OFFSET, "%s.%s must be a string", where,
                       name);
    return NULL;
  }
  return &value->as.text;
}

static const AbValue *required_array(GraphData *graph, const AbValue *object,
                                     const char *name, const char *where) {
  const AbValue *value = ab_value_member(object, name);
  if (!value || value->kind != AB_VALUE_ARRAY) {
    archbird_error_set(graph->engine, ARCHBIRD_INVALID_SCHEMA,
                       ARCHBIRD_NO_OFFSET, "%s.%s must be an array", where,
                       name);
    return NULL;
  }
  return value;
}

static ArchbirdStatus copy_text(GraphData *graph, AbString *out,
                                const AbString *value) {
  return ab_string_copy(graph->engine, out, value->data, value->length);
}

static ArchbirdStatus copy_literal(GraphData *graph, AbString *out,
                                   const char *value) {
  return ab_string_copy(graph->engine, out, value, strlen(value));
}

static void node_free(ArchbirdEngine *engine, GraphNode *node) {
  ab_string_free(engine, &node->identity);
  ab_string_free(engine, &node->kind);
  ab_string_free(engine, &node->label);
  ab_string_free(engine, &node->parent);
  ab_string_free(engine, &node->attributes);
  ab_string_free(engine, &node->evidence);
  memset(node, 0, sizeof(*node));
}

static void edge_free(ArchbirdEngine *engine, GraphEdge *edge) {
  size_t index;
  ab_string_free(engine, &edge->kind);
  ab_string_free(engine, &edge->source);
  ab_string_free(engine, &edge->target);
  ab_string_free(engine, &edge->classification);
  ab_string_free(engine, &edge->evidence);
  for (index = 0; index < edge->name_count; index++)
    ab_string_free(engine, &edge->names[index]);
  ab_free(engine, edge->names);
  memset(edge, 0, sizeof(*edge));
}

static void graph_free(GraphData *graph) {
  size_t index;
  ab_string_free(graph->engine, &graph->project);
  ab_string_free(graph->engine, &graph->input_sha256);
  ab_string_free(graph->engine, &graph->source_artifact);
  ab_string_free(graph->engine, &graph->tool_sha256);
  for (index = 0; index < graph->node_count; index++)
    node_free(graph->engine, &graph->nodes[index]);
  for (index = 0; index < graph->edge_count; index++)
    edge_free(graph->engine, &graph->edges[index]);
  ab_free(graph->engine, graph->nodes);
  ab_free(graph->engine, graph->edges);
  memset(graph, 0, sizeof(*graph));
}

static ArchbirdStatus reserve_nodes(GraphData *graph, size_t count) {
  GraphNode *resized;
  size_t capacity = graph->node_capacity ? graph->node_capacity : 16;
  if (count > graph->engine->options.max_values)
    return archbird_error_set(graph->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET, "graph node limit exceeded");
  if (count <= graph->node_capacity)
    return ARCHBIRD_OK;
  while (capacity < count) {
    if (capacity > SIZE_MAX / 2)
      return archbird_error_set(graph->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "graph node capacity overflow");
    capacity *= 2;
  }
  resized = (GraphNode *)ab_realloc(graph->engine, graph->nodes,
                                    capacity * sizeof(*resized));
  if (!resized)
    return archbird_error_set(graph->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory building graph nodes");
  memset(resized + graph->node_capacity, 0,
         (capacity - graph->node_capacity) * sizeof(*resized));
  graph->nodes = resized;
  graph->node_capacity = capacity;
  return ARCHBIRD_OK;
}

static ArchbirdStatus reserve_edges(GraphData *graph, size_t count) {
  GraphEdge *resized;
  size_t capacity = graph->edge_capacity ? graph->edge_capacity : 16;
  if (count > graph->engine->options.max_values)
    return archbird_error_set(graph->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET, "graph edge limit exceeded");
  if (count <= graph->edge_capacity)
    return ARCHBIRD_OK;
  while (capacity < count) {
    if (capacity > SIZE_MAX / 2)
      return archbird_error_set(graph->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "graph edge capacity overflow");
    capacity *= 2;
  }
  resized = (GraphEdge *)ab_realloc(graph->engine, graph->edges,
                                    capacity * sizeof(*resized));
  if (!resized)
    return archbird_error_set(graph->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory building graph edges");
  memset(resized + graph->edge_capacity, 0,
         (capacity - graph->edge_capacity) * sizeof(*resized));
  graph->edges = resized;
  graph->edge_capacity = capacity;
  return ARCHBIRD_OK;
}

static ArchbirdStatus buffer_to_string(GraphData *graph, AbBuffer *buffer,
                                       AbString *out) {
  return ab_string_copy(graph->engine, out, (const char *)buffer->data,
                        buffer->length);
}

static ArchbirdStatus file_attributes(GraphData *graph, const AbValue *row,
                                      AbString *out) {
  const AbString *language = required_text(graph, row, "language", "map.files");
  const AbString *layer = required_text(graph, row, "layer", "map.files");
  const AbString *sha256 = required_text(graph, row, "sha256", "map.files");
  const AbValue *symbols = required_array(graph, row, "symbols", "map.files");
  AbBuffer buffer;
  ArchbirdStatus status;
  if (!language || !layer || !sha256 || !symbols)
    return ARCHBIRD_INVALID_SCHEMA;
  ab_buffer_init(&buffer, graph->engine);
  status = ab_buffer_literal(&buffer, "{\"language\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, language->data, language->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"layer\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, layer->data, layer->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, sha256->data, sha256->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"symbols\":\"");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&buffer, (uint64_t)symbols->as.array.count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "\"}");
  if (status == ARCHBIRD_OK)
    status = buffer_to_string(graph, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus component_attributes(GraphData *graph, const AbValue *row,
                                           AbString *out) {
  const AbString *description =
      required_text(graph, row, "description", "map.components");
  const AbValue *files = required_array(graph, row, "files", "map.components");
  const AbValue *symbols = ab_value_member(row, "symbol_count");
  uint64_t symbol_count;
  AbBuffer buffer;
  ArchbirdStatus status;
  if (!description || !files || !ab_value_u64(symbols, &symbol_count))
    return graph_error(graph, "map.components symbol_count must be an integer");
  ab_buffer_init(&buffer, graph->engine);
  status = ab_buffer_literal(&buffer, "{\"description\":");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_json_string(&buffer, description->data, description->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"files\":\"");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&buffer, (uint64_t)files->as.array.count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "\",\"symbols\":\"");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&buffer, symbol_count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "\"}");
  if (status == ARCHBIRD_OK)
    status = buffer_to_string(graph, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus add_node_full(GraphData *graph, const AbString *identity,
                                    const char *kind, const AbString *label,
                                    const AbString *parent,
                                    const AbString *attributes,
                                    const AbString *evidence) {
  GraphNode *node;
  ArchbirdStatus status = reserve_nodes(graph, graph->node_count + 1);
  if (status != ARCHBIRD_OK)
    return status;
  node = &graph->nodes[graph->node_count];
  status = copy_text(graph, &node->identity, identity);
  if (status == ARCHBIRD_OK)
    status = copy_literal(graph, &node->kind, kind);
  if (status == ARCHBIRD_OK)
    status = copy_text(graph, &node->label, label);
  if (status == ARCHBIRD_OK && parent)
    status = copy_text(graph, &node->parent, parent);
  if (status == ARCHBIRD_OK)
    status = attributes ? copy_text(graph, &node->attributes, attributes)
                        : copy_literal(graph, &node->attributes, "{}");
  if (status == ARCHBIRD_OK)
    status = evidence ? copy_text(graph, &node->evidence, evidence)
                      : copy_literal(graph, &node->evidence, "[]");
  if (status != ARCHBIRD_OK) {
    node_free(graph->engine, node);
    return status;
  }
  graph->node_count++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_node(GraphData *graph, const AbString *identity,
                               const char *kind, const AbString *label,
                               const AbString *attributes) {
  return add_node_full(graph, identity, kind, label, NULL, attributes, NULL);
}

static ArchbirdStatus edge_add_name(GraphData *graph, GraphEdge *edge,
                                    const AbString *name) {
  size_t index;
  AbString *resized;
  size_t capacity;
  for (index = 0; index < edge->name_count; index++) {
    if (ab_string_equal(&edge->names[index], name))
      return ARCHBIRD_OK;
  }
  if (edge->name_count == edge->name_capacity) {
    capacity = edge->name_capacity ? edge->name_capacity * 2 : 4;
    if (capacity > graph->engine->options.max_values)
      return archbird_error_set(graph->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "graph edge-name limit exceeded");
    resized = (AbString *)ab_realloc(graph->engine, edge->names,
                                     capacity * sizeof(*edge->names));
    if (!resized)
      return archbird_error_set(graph->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory building graph edge names");
    memset(resized + edge->name_capacity, 0,
           (capacity - edge->name_capacity) * sizeof(*resized));
    edge->names = resized;
    edge->name_capacity = capacity;
  }
  if (copy_text(graph, &edge->names[edge->name_count], name) != ARCHBIRD_OK)
    return graph->engine->error_status;
  edge->name_count++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus
add_edge_full(GraphData *graph, const AbString *kind, const AbString *source,
              const AbString *target, const AbValue *names,
              const AbString *classification, const AbString *evidence) {
  GraphEdge *edge;
  size_t index;
  ArchbirdStatus status;
  if (!names || names->kind != AB_VALUE_ARRAY)
    return graph_error(graph, "map.edges.names must be an array");
  status = reserve_edges(graph, graph->edge_count + 1);
  if (status != ARCHBIRD_OK)
    return status;
  edge = &graph->edges[graph->edge_count];
  status = copy_text(graph, &edge->kind, kind);
  if (status == ARCHBIRD_OK)
    status = copy_text(graph, &edge->source, source);
  if (status == ARCHBIRD_OK)
    status = copy_text(graph, &edge->target, target);
  if (status == ARCHBIRD_OK)
    status = classification
                 ? copy_text(graph, &edge->classification, classification)
                 : copy_literal(graph, &edge->classification, "direct");
  if (status == ARCHBIRD_OK)
    status = evidence ? copy_text(graph, &edge->evidence, evidence)
                      : copy_literal(graph, &edge->evidence, "[]");
  for (index = 0; status == ARCHBIRD_OK && index < names->as.array.count;
       index++) {
    const AbValue *name = &names->as.array.items[index];
    if (name->kind != AB_VALUE_STRING)
      status = graph_error(graph, "map.edges.names must contain strings");
    else
      status = edge_add_name(graph, edge, &name->as.text);
  }
  if (status != ARCHBIRD_OK) {
    edge_free(graph->engine, edge);
    return status;
  }
  graph->edge_count++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_edge(GraphData *graph, const AbString *kind,
                               const AbString *source, const AbString *target,
                               const AbValue *names) {
  return add_edge_full(graph, kind, source, target, names, NULL, NULL);
}

static int node_compare(const void *left, const void *right) {
  return ab_string_compare(&((const GraphNode *)left)->identity,
                           &((const GraphNode *)right)->identity);
}

static int edge_compare(const void *left, const void *right) {
  const GraphEdge *a = (const GraphEdge *)left;
  const GraphEdge *b = (const GraphEdge *)right;
  int compared = ab_string_compare(&a->kind, &b->kind);
  if (!compared)
    compared = ab_string_compare(&a->source, &b->source);
  if (!compared)
    compared = ab_string_compare(&a->target, &b->target);
  if (!compared)
    compared = ab_string_compare(&a->classification, &b->classification);
  if (!compared)
    compared = ab_string_compare(&a->evidence, &b->evidence);
  return compared;
}

static int string_compare_qsort(const void *left, const void *right) {
  return ab_string_compare((const AbString *)left, (const AbString *)right);
}

static GraphNode *find_node(GraphData *graph, const AbString *identity) {
  GraphNode key = {0};
  if (!graph->node_count)
    return NULL;
  key.identity = *identity;
  return (GraphNode *)bsearch(&key, graph->nodes, graph->node_count,
                              sizeof(*graph->nodes), node_compare);
}

static const char *external_kind(const AbString *identity) {
  static const char external[] = "external:";
  static const char package[] = "package:";
  if (identity->length >= sizeof(external) - 1 &&
      !memcmp(identity->data, external, sizeof(external) - 1))
    return "external";
  if (identity->length >= sizeof(package) - 1 &&
      !memcmp(identity->data, package, sizeof(package) - 1))
    return "package";
  return "unmapped";
}

static ArchbirdStatus build_file_graph(GraphData *graph, const AbValue *map) {
  const AbValue *files = required_array(graph, map, "files", "map");
  const AbValue *edges = required_array(graph, map, "edges", "map");
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!files || !edges)
    return ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; status == ARCHBIRD_OK && index < files->as.array.count;
       index++) {
    const AbValue *row = &files->as.array.items[index];
    const AbString *path;
    AbString attributes = {0};
    if (row->kind != AB_VALUE_OBJECT)
      return graph_error(graph, "map.files entries must be objects");
    path = required_text(graph, row, "path", "map.files");
    if (!path)
      return ARCHBIRD_INVALID_SCHEMA;
    status = file_attributes(graph, row, &attributes);
    if (status == ARCHBIRD_OK)
      status = add_node(graph, path, "file", path, &attributes);
    ab_string_free(graph->engine, &attributes);
  }
  if (status != ARCHBIRD_OK)
    return status;
  if (graph->node_count > 1)
    qsort(graph->nodes, graph->node_count, sizeof(*graph->nodes), node_compare);
  for (index = 0; status == ARCHBIRD_OK && index < edges->as.array.count;
       index++) {
    const AbValue *row = &edges->as.array.items[index];
    const AbString *kind;
    const AbString *source;
    const AbString *target;
    const AbValue *names;
    if (row->kind != AB_VALUE_OBJECT)
      return graph_error(graph, "map.edges entries must be objects");
    kind = required_text(graph, row, "kind", "map.edges");
    source = required_text(graph, row, "source", "map.edges");
    target = required_text(graph, row, "target", "map.edges");
    names = required_array(graph, row, "names", "map.edges");
    if (!kind || !source || !target || !names)
      return ARCHBIRD_INVALID_SCHEMA;
    if (!find_node(graph, source)) {
      status = add_node(graph, source, external_kind(source), source, NULL);
      if (status == ARCHBIRD_OK && graph->node_count > 1)
        qsort(graph->nodes, graph->node_count, sizeof(*graph->nodes),
              node_compare);
    }
    if (status == ARCHBIRD_OK && !find_node(graph, target))
      status = add_node(graph, target, external_kind(target), target, NULL);
    if (status == ARCHBIRD_OK)
      status = add_edge(graph, kind, source, target, names);
    if (status == ARCHBIRD_OK && graph->node_count > 1)
      qsort(graph->nodes, graph->node_count, sizeof(*graph->nodes),
            node_compare);
  }
  if (status == ARCHBIRD_OK && graph->edge_count > 1)
    qsort(graph->edges, graph->edge_count, sizeof(*graph->edges), edge_compare);
  for (index = 0; status == ARCHBIRD_OK && index < graph->edge_count; index++)
    if (graph->edges[index].name_count > 1)
      qsort(graph->edges[index].names, graph->edges[index].name_count,
            sizeof(*graph->edges[index].names), string_compare_qsort);
  return status;
}

static int membership_compare(const void *left, const void *right) {
  const Membership *a = (const Membership *)left;
  const Membership *b = (const Membership *)right;
  int compared = ab_string_compare(&a->path, &b->path);
  return compared ? compared : ab_string_compare(&a->component, &b->component);
}

static size_t membership_lower(const Membership *rows, size_t count,
                               const AbString *path) {
  size_t first = 0;
  size_t length = count;
  while (length) {
    size_t half = length / 2;
    size_t middle = first + half;
    if (ab_string_compare(&rows[middle].path, path) < 0) {
      first = middle + 1;
      length -= half + 1;
    } else {
      length = half;
    }
  }
  return first;
}

static void memberships_free(ArchbirdEngine *engine, Membership *rows,
                             size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    ab_string_free(engine, &rows[index].path);
    ab_string_free(engine, &rows[index].component);
  }
  ab_free(engine, rows);
}

static ArchbirdStatus component_identity(GraphData *graph, const AbString *name,
                                         AbString *identity) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, graph->engine);
  status = ab_buffer_literal(&buffer, "component:");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, name->data, name->length);
  if (status == ARCHBIRD_OK)
    status = buffer_to_string(graph, &buffer, identity);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus consolidate_edges(GraphData *graph) {
  size_t read;
  size_t write = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (graph->edge_count > 1)
    qsort(graph->edges, graph->edge_count, sizeof(*graph->edges), edge_compare);
  for (read = 0; status == ARCHBIRD_OK && read < graph->edge_count; read++) {
    GraphEdge *source = &graph->edges[read];
    if (write && !edge_compare(&graph->edges[write - 1], source)) {
      size_t name;
      for (name = 0; status == ARCHBIRD_OK && name < source->name_count; name++)
        status = edge_add_name(graph, &graph->edges[write - 1],
                               &source->names[name]);
      edge_free(graph->engine, source);
    } else {
      if (write != read) {
        graph->edges[write] = *source;
        memset(source, 0, sizeof(*source));
      }
      write++;
    }
  }
  graph->edge_count = write;
  for (read = 0; status == ARCHBIRD_OK && read < graph->edge_count; read++)
    if (graph->edges[read].name_count > 1)
      qsort(graph->edges[read].names, graph->edges[read].name_count,
            sizeof(*graph->edges[read].names), string_compare_qsort);
  return status;
}

static ArchbirdStatus build_component_graph(GraphData *graph,
                                            const AbValue *map) {
  const AbValue *components = required_array(graph, map, "components", "map");
  const AbValue *edges = required_array(graph, map, "edges", "map");
  Membership *memberships = NULL;
  size_t membership_count = 0;
  size_t component_index;
  size_t edge_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!components || !edges)
    return ARCHBIRD_INVALID_SCHEMA;
  for (component_index = 0; component_index < components->as.array.count;
       component_index++) {
    const AbValue *row = &components->as.array.items[component_index];
    const AbValue *files;
    if (row->kind != AB_VALUE_OBJECT)
      return graph_error(graph, "map.components entries must be objects");
    files = required_array(graph, row, "files", "map.components");
    if (!files)
      return ARCHBIRD_INVALID_SCHEMA;
    if (files->as.array.count >
        graph->engine->options.max_values - membership_count)
      return archbird_error_set(graph->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "component membership limit exceeded");
    membership_count += files->as.array.count;
  }
  if (membership_count) {
    memberships = (Membership *)ab_calloc(graph->engine, membership_count,
                                          sizeof(*memberships));
    if (!memberships)
      return archbird_error_set(graph->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory building component membership");
  }
  membership_count = 0;
  for (component_index = 0;
       status == ARCHBIRD_OK && component_index < components->as.array.count;
       component_index++) {
    const AbValue *row = &components->as.array.items[component_index];
    const AbString *name = required_text(graph, row, "name", "map.components");
    const AbValue *files =
        required_array(graph, row, "files", "map.components");
    AbString identity = {0};
    AbString attributes = {0};
    size_t file_index;
    if (!name || !files) {
      status = ARCHBIRD_INVALID_SCHEMA;
      break;
    }
    status = component_identity(graph, name, &identity);
    if (status == ARCHBIRD_OK)
      status = component_attributes(graph, row, &attributes);
    if (status == ARCHBIRD_OK)
      status = add_node(graph, &identity, "component", name, &attributes);
    for (file_index = 0;
         status == ARCHBIRD_OK && file_index < files->as.array.count;
         file_index++) {
      const AbValue *path = &files->as.array.items[file_index];
      Membership *membership = &memberships[membership_count];
      if (path->kind != AB_VALUE_STRING) {
        status =
            graph_error(graph, "map.components.files must contain strings");
        break;
      }
      status = copy_text(graph, &membership->path, &path->as.text);
      if (status == ARCHBIRD_OK)
        status = copy_text(graph, &membership->component, &identity);
      if (status == ARCHBIRD_OK) {
        membership_count++;
      } else {
        ab_string_free(graph->engine, &membership->path);
        ab_string_free(graph->engine, &membership->component);
      }
    }
    ab_string_free(graph->engine, &attributes);
    ab_string_free(graph->engine, &identity);
  }
  if (status == ARCHBIRD_OK) {
    if (membership_count > 1)
      qsort(memberships, membership_count, sizeof(*memberships),
            membership_compare);
    if (graph->node_count > 1)
      qsort(graph->nodes, graph->node_count, sizeof(*graph->nodes),
            node_compare);
  }
  for (edge_index = 0;
       status == ARCHBIRD_OK && edge_index < edges->as.array.count;
       edge_index++) {
    const AbValue *row = &edges->as.array.items[edge_index];
    const AbString *kind;
    const AbString *source;
    const AbString *target;
    const AbValue *names;
    size_t source_first;
    size_t target_first;
    size_t source_index;
    if (row->kind != AB_VALUE_OBJECT) {
      status = graph_error(graph, "map.edges entries must be objects");
      break;
    }
    kind = required_text(graph, row, "kind", "map.edges");
    source = required_text(graph, row, "source", "map.edges");
    target = required_text(graph, row, "target", "map.edges");
    names = required_array(graph, row, "names", "map.edges");
    if (!kind || !source || !target || !names) {
      status = ARCHBIRD_INVALID_SCHEMA;
      break;
    }
    source_first = membership_lower(memberships, membership_count, source);
    target_first = membership_lower(memberships, membership_count, target);
    for (source_index = source_first;
         status == ARCHBIRD_OK && source_index < membership_count &&
         ab_string_equal(&memberships[source_index].path, source);
         source_index++) {
      size_t target_index;
      for (target_index = target_first;
           status == ARCHBIRD_OK && target_index < membership_count &&
           ab_string_equal(&memberships[target_index].path, target);
           target_index++) {
        if (!ab_string_equal(&memberships[source_index].component,
                             &memberships[target_index].component))
          status = add_edge(graph, kind, &memberships[source_index].component,
                            &memberships[target_index].component, names);
      }
    }
  }
  if (status == ARCHBIRD_OK)
    status = consolidate_edges(graph);
  memberships_free(graph->engine, memberships, membership_count);
  return status;
}

static ArchbirdStatus value_json(GraphData *graph, const AbValue *value,
                                 AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, graph->engine);
  status = value ? ab_value_render(&buffer, value)
                 : ab_buffer_literal(&buffer, "null");
  if (status == ARCHBIRD_OK)
    status = buffer_to_string(graph, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus query_file_attributes(GraphData *graph,
                                            const AbValue *row, AbString *out) {
  const AbString *language =
      required_text(graph, row, "language", "query.files");
  const AbString *layer = required_text(graph, row, "layer", "query.files");
  const AbString *sha256 = required_text(graph, row, "sha256", "query.files");
  const AbValue *symbols = required_array(graph, row, "symbols", "query.files");
  uint64_t distance;
  AbBuffer buffer;
  ArchbirdStatus status;
  if (!language || !layer || !sha256 || !symbols ||
      !ab_value_u64(ab_value_member(row, "distance"), &distance))
    return graph_error(graph,
                       "query.files distance must be a nonnegative integer");
  ab_buffer_init(&buffer, graph->engine);
  status = ab_buffer_literal(&buffer, "{\"distance\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&buffer, distance);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"language\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, language->data, language->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"layer\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, layer->data, layer->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, sha256->data, sha256->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"symbol_count\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&buffer, (uint64_t)symbols->as.array.count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "}");
  if (status == ARCHBIRD_OK)
    status = buffer_to_string(graph, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus symbol_identity(GraphData *graph, const AbString *path,
                                      const AbString *symbol, AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, graph->engine);
  status = ab_buffer_literal(&buffer, "symbol:[");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, path->data, path->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, symbol->data, symbol->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "]");
  if (status == ARCHBIRD_OK)
    status = buffer_to_string(graph, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus special_identity(GraphData *graph, const char *kind,
                                       const AbString *name, AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, graph->engine);
  status = ab_buffer_append(&buffer, kind, strlen(kind));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, name->data, name->length);
  if (status == ARCHBIRD_OK)
    status = buffer_to_string(graph, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus unresolved_identity(GraphData *graph,
                                          const AbString *name, AbString *out) {
  return special_identity(graph, "unresolved", name, out);
}

static void sort_nodes(GraphData *graph) {
  if (graph->node_count > 1)
    qsort(graph->nodes, graph->node_count, sizeof(*graph->nodes), node_compare);
}

static ArchbirdStatus ensure_file_node(GraphData *graph, const AbString *path,
                                       const AbValue *attributes) {
  AbString rendered = {0};
  ArchbirdStatus status;
  if (find_node(graph, path))
    return ARCHBIRD_OK;
  status = attributes ? query_file_attributes(graph, attributes, &rendered)
                      : copy_literal(graph, &rendered, "{}");
  if (status == ARCHBIRD_OK)
    status = add_node(graph, path, "file", path, &rendered);
  ab_string_free(graph->engine, &rendered);
  if (status == ARCHBIRD_OK)
    sort_nodes(graph);
  return status;
}

static ArchbirdStatus ensure_symbol_node(GraphData *graph, const AbString *path,
                                         const AbString *symbol,
                                         const AbValue *attributes,
                                         AbString *out_identity) {
  AbString rendered = {0};
  ArchbirdStatus status = symbol_identity(graph, path, symbol, out_identity);
  if (status != ARCHBIRD_OK)
    return status;
  status = ensure_file_node(graph, path, NULL);
  if (status == ARCHBIRD_OK && !find_node(graph, out_identity)) {
    status = attributes ? value_json(graph, attributes, &rendered)
                        : copy_literal(graph, &rendered, "{}");
    if (status == ARCHBIRD_OK)
      status = add_node_full(graph, out_identity, "symbol", symbol, path,
                             &rendered, NULL);
    if (status == ARCHBIRD_OK)
      sort_nodes(graph);
  }
  ab_string_free(graph->engine, &rendered);
  return status;
}

static ArchbirdStatus add_relation_edge(GraphData *graph, const AbString *kind,
                                        const AbString *source,
                                        const AbString *target,
                                        const AbString *name,
                                        const AbString *classification,
                                        const AbString *evidence) {
  AbValue name_value = {0};
  AbValue names = {0};
  name_value.kind = AB_VALUE_STRING;
  name_value.as.text = *name;
  names.kind = AB_VALUE_ARRAY;
  names.as.array.items = &name_value;
  names.as.array.count = 1;
  return add_edge_full(graph, kind, source, target, &names, classification,
                       evidence);
}

static ArchbirdStatus add_symbol_relation(GraphData *graph, const AbValue *row,
                                          const char *fallback_kind) {
  const AbValue *source = ab_value_member(row, "source");
  const AbValue *candidates =
      required_array(graph, row, "candidates", "query.symbol_relation");
  const AbValue *evidence_value =
      required_array(graph, row, "evidence", "query.symbol_relation");
  const AbString *name =
      required_text(graph, row, "name", "query.symbol_relation");
  const AbString *resolution =
      required_text(graph, row, "resolution", "query.symbol_relation");
  const AbString *source_path;
  const AbString *source_symbol;
  const AbString *relation =
      ab_value_member(row, "relation") &&
              ab_value_member(row, "relation")->kind == AB_VALUE_STRING
          ? &ab_value_member(row, "relation")->as.text
          : NULL;
  AbString kind = {0};
  AbString source_identity = {0};
  AbString evidence = {0};
  size_t index;
  ArchbirdStatus status;
  if (!source || source->kind != AB_VALUE_OBJECT || !candidates ||
      !evidence_value || !name || !resolution)
    return graph_error(graph, "query symbol relation shape is invalid");
  source_path =
      required_text(graph, source, "path", "query.symbol_relation.source");
  source_symbol =
      ab_value_member(source, "symbol") &&
              ab_value_member(source, "symbol")->kind == AB_VALUE_STRING
          ? &ab_value_member(source, "symbol")->as.text
          : NULL;
  if (!source_path)
    return ARCHBIRD_INVALID_SCHEMA;
  status = copy_literal(graph, &kind, fallback_kind);
  if (status == ARCHBIRD_OK && relation) {
    AbBuffer buffer;
    ab_buffer_init(&buffer, graph->engine);
    ab_string_free(graph->engine, &kind);
    status = ab_buffer_literal(&buffer, "reference:");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&buffer, relation->data, relation->length);
    if (status == ARCHBIRD_OK)
      status = buffer_to_string(graph, &buffer, &kind);
    ab_buffer_free(&buffer);
  }
  if (status == ARCHBIRD_OK && source_symbol)
    status = ensure_symbol_node(graph, source_path, source_symbol, NULL,
                                &source_identity);
  else if (status == ARCHBIRD_OK) {
    status = ensure_file_node(graph, source_path, NULL);
    if (status == ARCHBIRD_OK)
      status = copy_text(graph, &source_identity, source_path);
  }
  if (status == ARCHBIRD_OK)
    status = value_json(graph, evidence_value, &evidence);
  for (index = 0; status == ARCHBIRD_OK && index < candidates->as.array.count;
       index++) {
    const AbValue *candidate = &candidates->as.array.items[index];
    const AbString *path;
    const AbString *symbol;
    AbString target_identity = {0};
    if (candidate->kind != AB_VALUE_OBJECT) {
      status = graph_error(graph, "query relation candidate must be an object");
      break;
    }
    path = required_text(graph, candidate, "path", "query.relation.candidate");
    symbol =
        required_text(graph, candidate, "symbol", "query.relation.candidate");
    if (!path || !symbol) {
      status = ARCHBIRD_INVALID_SCHEMA;
      break;
    }
    status =
        ensure_symbol_node(graph, path, symbol, candidate, &target_identity);
    if (status == ARCHBIRD_OK)
      status = add_relation_edge(graph, &kind, &source_identity,
                                 &target_identity, name, resolution, &evidence);
    ab_string_free(graph->engine, &target_identity);
  }
  if (status == ARCHBIRD_OK && candidates->as.array.count == 0) {
    AbString target_identity = {0};
    const int builtin = text_is(resolution, "builtin");
    status = builtin
                 ? special_identity(graph, "builtin", name, &target_identity)
                 : unresolved_identity(graph, name, &target_identity);
    if (status == ARCHBIRD_OK && !find_node(graph, &target_identity)) {
      status = add_node(graph, &target_identity,
                        builtin ? "builtin" : "unresolved", name, NULL);
      if (status == ARCHBIRD_OK)
        sort_nodes(graph);
    }
    if (status == ARCHBIRD_OK)
      status = add_relation_edge(graph, &kind, &source_identity,
                                 &target_identity, name, resolution, &evidence);
    ab_string_free(graph->engine, &target_identity);
  }
  ab_string_free(graph->engine, &evidence);
  ab_string_free(graph->engine, &source_identity);
  ab_string_free(graph->engine, &kind);
  return status;
}

static ArchbirdStatus build_symbol_graph(GraphData *graph,
                                         const AbValue *query) {
  const AbValue *files = required_array(graph, query, "files", "query");
  const AbValue *matched =
      required_array(graph, query, "matched_symbols", "query");
  const AbValue *calls = required_array(graph, query, "symbol_calls", "query");
  const AbValue *references =
      required_array(graph, query, "symbol_references", "query");
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!files || !matched || !calls || !references)
    return ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; status == ARCHBIRD_OK && index < files->as.array.count;
       index++) {
    const AbValue *file = &files->as.array.items[index];
    const AbString *path;
    if (file->kind != AB_VALUE_OBJECT)
      return graph_error(graph, "query.files entries must be objects");
    path = required_text(graph, file, "path", "query.files");
    if (!path)
      return ARCHBIRD_INVALID_SCHEMA;
    status = ensure_file_node(graph, path, file);
  }
  for (index = 0; status == ARCHBIRD_OK && index < matched->as.array.count;
       index++) {
    const AbValue *symbol = &matched->as.array.items[index];
    const AbString *path;
    const AbString *name;
    AbString identity = {0};
    if (symbol->kind != AB_VALUE_OBJECT)
      return graph_error(graph,
                         "query.matched_symbols entries must be objects");
    path = required_text(graph, symbol, "path", "query.matched_symbols");
    name = required_text(graph, symbol, "name", "query.matched_symbols");
    if (!path || !name)
      return ARCHBIRD_INVALID_SCHEMA;
    status = ensure_symbol_node(graph, path, name, symbol, &identity);
    ab_string_free(graph->engine, &identity);
  }
  for (index = 0; status == ARCHBIRD_OK && index < calls->as.array.count;
       index++) {
    if (calls->as.array.items[index].kind != AB_VALUE_OBJECT)
      return graph_error(graph, "query.symbol_calls entries must be objects");
    status = add_symbol_relation(graph, &calls->as.array.items[index], "call");
  }
  for (index = 0; status == ARCHBIRD_OK && index < references->as.array.count;
       index++) {
    if (references->as.array.items[index].kind != AB_VALUE_OBJECT)
      return graph_error(graph,
                         "query.symbol_references entries must be objects");
    status = add_symbol_relation(graph, &references->as.array.items[index],
                                 "reference");
  }
  if (status == ARCHBIRD_OK)
    status = consolidate_edges(graph);
  return status;
}

static ArchbirdStatus build_graph(GraphData *graph, const AbValue *input) {
  const AbValue *evidence;
  const AbValue *tool;
  const AbString *artifact;
  const AbString *project;
  const AbString *input_sha256;
  uint64_t version;
  if (!input || input->kind != AB_VALUE_OBJECT ||
      !(artifact = required_text(graph, input, "artifact", "graph input")) ||
      (!text_is(artifact, "map") && !text_is(artifact, "query")) ||
      !ab_value_u64(ab_value_member(input, "schema_version"), &version) ||
      version < ARCHBIRD_MAP_SCHEMA_MIN ||
      version > ARCHBIRD_MAP_SCHEMA_CURRENT)
    return graph_error(graph,
                       "graph input must be an Archbird map "
                       "or query schema " ARCHBIRD_MAP_SCHEMA_SUPPORTED_TEXT);
  if ((text_is(artifact, "map") && graph->view == ARCHBIRD_GRAPH_SYMBOLS) ||
      (text_is(artifact, "query") && graph->view != ARCHBIRD_GRAPH_SYMBOLS))
    return graph_error(graph,
                       "component/file views require Map input; symbol view "
                       "requires Query input");
  project = required_text(graph, input, "project", "graph input");
  evidence = ab_value_member(input, "evidence");
  if (!project || !evidence || evidence->kind != AB_VALUE_OBJECT)
    return graph_error(graph, "graph input evidence must be an object");
  input_sha256 =
      required_text(graph, evidence, "input_sha256", "graph.evidence");
  tool = ab_value_member(input,
                         text_is(artifact, "query") ? "source_tool" : "tool");
  if (!input_sha256 || !tool || tool->kind != AB_VALUE_OBJECT)
    return ARCHBIRD_INVALID_SCHEMA;
  {
    const AbString *tool_sha =
        required_text(graph, tool, "implementation_sha256", "graph.tool");
    if (!tool_sha)
      return ARCHBIRD_INVALID_SCHEMA;
    if (copy_text(graph, &graph->tool_sha256, tool_sha) != ARCHBIRD_OK)
      return graph->engine->error_status;
  }
  if (copy_text(graph, &graph->project, project) != ARCHBIRD_OK ||
      copy_text(graph, &graph->input_sha256, input_sha256) != ARCHBIRD_OK ||
      copy_text(graph, &graph->source_artifact, artifact) != ARCHBIRD_OK)
    return graph->engine->error_status;
  graph->source_schema = version;
  if (graph->view == ARCHBIRD_GRAPH_COMPONENTS)
    return build_component_graph(graph, input);
  if (graph->view == ARCHBIRD_GRAPH_FILES)
    return build_file_graph(graph, input);
  return build_symbol_graph(graph, input);
}

static void identity_hash(const AbString *identity, char output[67]) {
  uint8_t digest[32];
  output[0] = 'n';
  output[1] = '_';
  (void)archbird_sha256((const uint8_t *)identity->data, identity->length,
                        digest);
  archbird_sha256_hex(digest, output + 2);
}

static ArchbirdStatus render_names_json(AbBuffer *buffer,
                                        const GraphEdge *edge) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < edge->name_count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, edge->names[index].data,
                                     edge->names[index].length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus edge_hash_full(GraphData *graph, const GraphEdge *edge,
                                     char output[67], int include_evidence) {
  AbBuffer buffer;
  uint8_t digest[32];
  ArchbirdStatus status;
  ab_buffer_init(&buffer, graph->engine);
  status = ab_buffer_literal(&buffer, "[");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, edge->kind.data, edge->kind.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_json_string(&buffer, edge->source.data, edge->source.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_json_string(&buffer, edge->target.data, edge->target.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",");
  if (status == ARCHBIRD_OK)
    status = render_names_json(&buffer, edge);
  if (status == ARCHBIRD_OK && include_evidence)
    status = ab_buffer_literal(&buffer, ",");
  if (status == ARCHBIRD_OK && include_evidence)
    status = ab_buffer_json_string(&buffer, edge->classification.data,
                                   edge->classification.length);
  if (status == ARCHBIRD_OK && include_evidence)
    status = ab_buffer_literal(&buffer, ",");
  if (status == ARCHBIRD_OK && include_evidence)
    status =
        ab_buffer_append(&buffer, edge->evidence.data, edge->evidence.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "]");
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(buffer.data, buffer.length, digest);
  if (status == ARCHBIRD_OK) {
    output[0] = 'e';
    output[1] = '_';
    archbird_sha256_hex(digest, output + 2);
  }
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus edge_hash(GraphData *graph, const GraphEdge *edge,
                                char output[67]) {
  return edge_hash_full(graph, edge, output, 0);
}

static ArchbirdStatus
graph_json_edge_hash(GraphData *graph, const GraphEdge *edge, char output[67]) {
  return edge_hash_full(graph, edge, output, 1);
}

static const char *graph_view_name(ArchbirdGraphView view) {
  if (view == ARCHBIRD_GRAPH_COMPONENTS)
    return "components";
  if (view == ARCHBIRD_GRAPH_FILES)
    return "files";
  return "symbols";
}

static ArchbirdStatus
render_names_json_limit(AbBuffer *buffer, const GraphEdge *edge, size_t limit) {
  size_t index;
  size_t count = !limit || limit > edge->name_count ? edge->name_count : limit;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, edge->names[index].data,
                                     edge->names[index].length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static size_t omitted_edge_names(const GraphData *graph, size_t limit) {
  size_t index;
  size_t omitted = 0;
  if (!limit)
    return 0;
  for (index = 0; index < graph->edge_count; index++)
    if (graph->edges[index].name_count > limit)
      omitted += graph->edges[index].name_count - limit;
  return omitted;
}

static ArchbirdStatus render_graph_json(GraphData *graph, const AbValue *input,
                                        const ArchbirdGraphOptions *options,
                                        AbBuffer *buffer) {
  const AbValue *diagnostics = ab_value_member(input, "diagnostics");
  const AbValue *query = ab_value_member(input, "query");
  const AbValue *test_matches = ab_value_member(input, "test_matches");
  const char *view = graph_view_name(graph->view);
  size_t omitted = omitted_edge_names(graph, options->max_edge_names);
  size_t omitted_tests = test_matches && test_matches->kind == AB_VALUE_ARRAY
                             ? test_matches->as.array.count
                             : 0;
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(
      buffer, "{\"artifact\":\"archbird-graph-view\",\"diagnostics\":");
  if (status == ARCHBIRD_OK)
    status = diagnostics && diagnostics->kind == AB_VALUE_ARRAY
                 ? ab_value_render(buffer, diagnostics)
                 : ab_buffer_literal(buffer, "[]");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"edges\":[");
  for (index = 0; status == ARCHBIRD_OK && index < graph->edge_count; index++) {
    const GraphEdge *edge = &graph->edges[index];
    char id[67];
    char source_id[67];
    char target_id[67];
    size_t visible =
        !options->max_edge_names || options->max_edge_names > edge->name_count
            ? edge->name_count
            : options->max_edge_names;
    if (!find_node(graph, &edge->source) || !find_node(graph, &edge->target))
      return graph_error(graph, "graph edge endpoint is missing");
    status = graph_json_edge_hash(graph, edge, id);
    identity_hash(&edge->source, source_id);
    identity_hash(&edge->target, target_id);
    if (status == ARCHBIRD_OK && index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"classification\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, edge->classification.data,
                                     edge->classification.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"evidence\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(buffer, edge->evidence.data, edge->evidence.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"id\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, id, 66);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"kind\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, edge->kind.data, edge->kind.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"names\":");
    if (status == ARCHBIRD_OK)
      status = render_names_json_limit(buffer, edge, options->max_edge_names);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"omitted_names\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)(edge->name_count - visible));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"source\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, source_id, 66);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"target\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, target_id, 66);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "],\"nodes\":[");
  for (index = 0; status == ARCHBIRD_OK && index < graph->node_count; index++) {
    const GraphNode *node = &graph->nodes[index];
    char id[67];
    identity_hash(&node->identity, id);
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"attributes\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(buffer, node->attributes.data,
                                node->attributes.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"evidence\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(buffer, node->evidence.data, node->evidence.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"id\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, id, 66);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"identity\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, node->identity.data,
                                     node->identity.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"kind\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, node->kind.data, node->kind.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"label\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, node->label.data, node->label.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"parent\":");
    if (status == ARCHBIRD_OK && node->parent.length) {
      char parent_id[67];
      if (!find_node(graph, &node->parent))
        return graph_error(graph, "graph parent node is missing");
      identity_hash(&node->parent, parent_id);
      status = ab_buffer_json_string(buffer, parent_id, 66);
    } else if (status == ARCHBIRD_OK) {
      status = ab_buffer_literal(buffer, "null");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "],\"omissions\":[");
  if (status == ARCHBIRD_OK && omitted) {
    status = ab_buffer_literal(buffer, "{\"count\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)omitted);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(
          buffer, ",\"kind\":\"edge-names\",\"reason\":\"max_edge_names\"}");
  }
  if (status == ARCHBIRD_OK && omitted_tests) {
    if (omitted)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"count\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)omitted_tests);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(
          buffer,
          ",\"kind\":\"test-matches\",\"reason\":\"not-symbol-edges\"}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "],\"project\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, graph->project.data,
                                   graph->project.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"request\":{\"max_edge_names\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(buffer, (uint64_t)options->max_edge_names);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"max_nodes\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(buffer, (uint64_t)options->max_nodes);
  if (status == ARCHBIRD_OK && query) {
    status = ab_buffer_literal(buffer, ",\"query\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, query);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"view\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, view, strlen(view));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        buffer, "},\"schema_version\":1,\"source\":{\"artifact\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, graph->source_artifact.data,
                                   graph->source_artifact.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"input_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, graph->input_sha256.data,
                                   graph->input_sha256.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"schema_version\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(buffer, graph->source_schema);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"tool_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, graph->tool_sha256.data,
                                   graph->tool_sha256.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "},\"summary\":{\"edges\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(buffer, (uint64_t)graph->edge_count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"nodes\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(buffer, (uint64_t)graph->node_count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        buffer,
        "},\"tool\":{\"implementation_sha256\":"
        "\"" ARCHBIRD_IMPLEMENTATION_SHA256
        "\",\"name\":\"archbird\",\"version\":\"" ARCHBIRD_VERSION "\"}}");
  return status;
}

static ArchbirdStatus xml_text(AbBuffer *buffer, const char *data,
                               size_t length) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < length; index++) {
    switch ((unsigned char)data[index]) {
    case '&':
      status = ab_buffer_literal(buffer, "&amp;");
      break;
    case '<':
      status = ab_buffer_literal(buffer, "&lt;");
      break;
    case '>':
      status = ab_buffer_literal(buffer, "&gt;");
      break;
    default:
      status = ab_buffer_append(buffer, data + index, 1);
      break;
    }
  }
  return status;
}

static ArchbirdStatus graphml_data(AbBuffer *buffer, const char *indent,
                                   const char *key, const AbString *value) {
  ArchbirdStatus status = ab_buffer_append(buffer, indent, strlen(indent));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "<data key=\"");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, key);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "\">");
  if (status == ARCHBIRD_OK)
    status = xml_text(buffer, value->data, value->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "</data>\n");
  return status;
}

static ArchbirdStatus render_graphml(GraphData *graph, AbBuffer *buffer) {
  static const char header[] =
      "<?xml version='1.0' encoding='utf-8'?>\n"
      "<graphml xmlns=\"http://graphml.graphdrawing.org/xmlns\">\n"
      "  <key id=\"g_project\" for=\"graph\" attr.name=\"project\" "
      "attr.type=\"string\" />\n"
      "  <key id=\"g_input\" for=\"graph\" attr.name=\"input_sha256\" "
      "attr.type=\"string\" />\n"
      "  <key id=\"g_view\" for=\"graph\" attr.name=\"view\" "
      "attr.type=\"string\" />\n"
      "  <key id=\"n_identity\" for=\"node\" attr.name=\"identity\" "
      "attr.type=\"string\" />\n"
      "  <key id=\"n_kind\" for=\"node\" attr.name=\"kind\" "
      "attr.type=\"string\" />\n"
      "  <key id=\"n_label\" for=\"node\" attr.name=\"label\" "
      "attr.type=\"string\" />\n"
      "  <key id=\"n_attributes\" for=\"node\" attr.name=\"attributes\" "
      "attr.type=\"string\" />\n"
      "  <key id=\"e_kind\" for=\"edge\" attr.name=\"kind\" "
      "attr.type=\"string\" />\n"
      "  <key id=\"e_names\" for=\"edge\" attr.name=\"names\" "
      "attr.type=\"string\" />\n"
      "  <graph id=\"archbird\" edgedefault=\"directed\">\n";
  static const AbString components = {(char *)"components", 10};
  static const AbString files = {(char *)"files", 5};
  size_t index;
  ArchbirdStatus status = ab_buffer_append(buffer, header, sizeof(header) - 1);
  if (status == ARCHBIRD_OK)
    status = graphml_data(buffer, "    ", "g_project", &graph->project);
  if (status == ARCHBIRD_OK)
    status = graphml_data(buffer, "    ", "g_input", &graph->input_sha256);
  if (status == ARCHBIRD_OK)
    status = graphml_data(buffer, "    ", "g_view",
                          graph->view == ARCHBIRD_GRAPH_COMPONENTS ? &components
                                                                   : &files);
  for (index = 0; status == ARCHBIRD_OK && index < graph->node_count; index++) {
    GraphNode *node = &graph->nodes[index];
    char id[67];
    identity_hash(&node->identity, id);
    status = ab_buffer_literal(buffer, "    <node id=\"");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(buffer, id, 66);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "\">\n");
    if (status == ARCHBIRD_OK)
      status = graphml_data(buffer, "      ", "n_identity", &node->identity);
    if (status == ARCHBIRD_OK)
      status = graphml_data(buffer, "      ", "n_kind", &node->kind);
    if (status == ARCHBIRD_OK)
      status = graphml_data(buffer, "      ", "n_label", &node->label);
    if (status == ARCHBIRD_OK)
      status =
          graphml_data(buffer, "      ", "n_attributes", &node->attributes);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "    </node>\n");
  }
  for (index = 0; status == ARCHBIRD_OK && index < graph->edge_count; index++) {
    GraphEdge *edge = &graph->edges[index];
    GraphNode *source = find_node(graph, &edge->source);
    GraphNode *target = find_node(graph, &edge->target);
    char id[67];
    char source_id[67];
    char target_id[67];
    AbBuffer names;
    if (!source || !target)
      return graph_error(graph, "graph edge endpoint is missing");
    status = edge_hash(graph, edge, id);
    identity_hash(&source->identity, source_id);
    identity_hash(&target->identity, target_id);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "    <edge id=\"");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(buffer, id, 66);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "\" source=\"");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(buffer, source_id, 66);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "\" target=\"");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(buffer, target_id, 66);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "\">\n");
    if (status == ARCHBIRD_OK)
      status = graphml_data(buffer, "      ", "e_kind", &edge->kind);
    ab_buffer_init(&names, graph->engine);
    if (status == ARCHBIRD_OK)
      status = render_names_json(&names, edge);
    if (status == ARCHBIRD_OK) {
      AbString value = {(char *)names.data, names.length};
      status = graphml_data(buffer, "      ", "e_names", &value);
    }
    ab_buffer_free(&names);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "    </edge>\n");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "  </graph>\n</graphml>\n");
  return status;
}

static ArchbirdStatus html_label(AbBuffer *buffer, const char *data,
                                 size_t length, int edge) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < length; index++) {
    switch ((unsigned char)data[index]) {
    case '&':
      status = ab_buffer_literal(buffer, "&amp;");
      break;
    case '<':
      status = ab_buffer_literal(buffer, "&lt;");
      break;
    case '>':
      status = ab_buffer_literal(buffer, "&gt;");
      break;
    case '"':
      status = ab_buffer_literal(buffer, "&quot;");
      break;
    case '\'':
      status = ab_buffer_literal(buffer, "&#x27;");
      break;
    case '|':
      status = edge ? ab_buffer_literal(buffer, "&#124;")
                    : ab_buffer_append(buffer, data + index, 1);
      break;
    case '\n':
    case '\r':
      status = ab_buffer_literal(buffer, " ");
      break;
    default:
      status = ab_buffer_append(buffer, data + index, 1);
      break;
    }
  }
  return status;
}

typedef struct ClassId {
  char value[67];
} ClassId;

static int class_id_compare(const void *left, const void *right) {
  return strcmp(((const ClassId *)left)->value,
                ((const ClassId *)right)->value);
}

static ArchbirdStatus render_mermaid_edge_label(AbBuffer *buffer,
                                                const GraphEdge *edge,
                                                size_t max_names) {
  size_t shown = edge->name_count < max_names ? edge->name_count : max_names;
  size_t index;
  ArchbirdStatus status =
      html_label(buffer, edge->kind.data, edge->kind.length, 1);
  if (status == ARCHBIRD_OK && shown)
    status = ab_buffer_literal(buffer, ": ");
  for (index = 0; status == ARCHBIRD_OK && index < shown; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ", ");
    if (status == ARCHBIRD_OK)
      status = html_label(buffer, edge->names[index].data,
                          edge->names[index].length, 1);
  }
  if (status == ARCHBIRD_OK && edge->name_count > shown) {
    status = ab_buffer_literal(buffer, " …+");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)(edge->name_count - shown));
  }
  return status;
}

static ArchbirdStatus render_mermaid_classes(GraphData *graph,
                                             AbBuffer *buffer) {
  static const char *const kinds[] = {"component", "external", "file",
                                      "package", "unmapped"};
  size_t kind_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (kind_index = 0;
       status == ARCHBIRD_OK && kind_index < sizeof(kinds) / sizeof(kinds[0]);
       kind_index++) {
    ClassId *ids = NULL;
    size_t count = 0;
    size_t node_index;
    if (graph->node_count) {
      ids =
          (ClassId *)ab_calloc(graph->engine, graph->node_count, sizeof(*ids));
      if (!ids)
        return archbird_error_set(graph->engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory rendering Mermaid classes");
    }
    for (node_index = 0; node_index < graph->node_count; node_index++) {
      if (text_is(&graph->nodes[node_index].kind, kinds[kind_index]))
        identity_hash(&graph->nodes[node_index].identity, ids[count++].value);
    }
    if (count > 1)
      qsort(ids, count, sizeof(*ids), class_id_compare);
    if (count) {
      status = ab_buffer_literal(buffer, "  class ");
      for (node_index = 0; status == ARCHBIRD_OK && node_index < count;
           node_index++) {
        if (node_index)
          status = ab_buffer_literal(buffer, ",");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(buffer, ids[node_index].value, 66);
      }
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, " ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, kinds[kind_index]);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "\n");
    }
    ab_free(graph->engine, ids);
  }
  return status;
}

static const char *direction_name(ArchbirdGraphDirection direction) {
  switch (direction) {
  case ARCHBIRD_GRAPH_RL:
    return "RL";
  case ARCHBIRD_GRAPH_TB:
    return "TB";
  case ARCHBIRD_GRAPH_BT:
    return "BT";
  default:
    return "LR";
  }
}

static ArchbirdStatus render_mermaid(GraphData *graph, AbBuffer *buffer,
                                     const ArchbirdGraphOptions *options) {
  static const char class_definitions[] =
      "  classDef component fill:#e8f1ff,stroke:#315a9b,color:#10233f\n"
      "  classDef file fill:#f4f4f4,stroke:#666,color:#111\n"
      "  classDef external fill:#fff3cd,stroke:#8a6d3b,color:#3d310f\n"
      "  classDef package fill:#e9f7ef,stroke:#287a47,color:#173c26\n"
      "  classDef unmapped fill:#f8d7da,stroke:#9b3b45,color:#48191e\n";
  size_t index;
  ArchbirdStatus status;
  if (options->max_nodes && graph->node_count > options->max_nodes)
    return archbird_error_set(
        graph->engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
        "Mermaid view has %zu nodes, exceeding %zu; use the components view, "
        "raise max_nodes, or export GraphML",
        graph->node_count, options->max_nodes);
  status = ab_buffer_literal(buffer, "%% Archbird ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, graph->view == ARCHBIRD_GRAPH_COMPONENTS
                                           ? "components"
                                           : "files");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, " graph; input_sha256=");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(buffer, graph->input_sha256.data,
                              graph->input_sha256.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "\nflowchart ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, direction_name(options->direction));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "\n");
  for (index = 0; status == ARCHBIRD_OK && index < graph->node_count; index++) {
    char id[67];
    identity_hash(&graph->nodes[index].identity, id);
    status = ab_buffer_literal(buffer, "  ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(buffer, id, 66);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "[\"");
    if (status == ARCHBIRD_OK)
      status = html_label(buffer, graph->nodes[index].label.data,
                          graph->nodes[index].label.length, 0);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "\"]\n");
  }
  for (index = 0; status == ARCHBIRD_OK && index < graph->edge_count; index++) {
    char source[67];
    char target[67];
    identity_hash(&graph->edges[index].source, source);
    identity_hash(&graph->edges[index].target, target);
    status = ab_buffer_literal(buffer, "  ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(buffer, source, 66);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, " -->|\"");
    if (status == ARCHBIRD_OK)
      status = render_mermaid_edge_label(buffer, &graph->edges[index],
                                         options->max_edge_names);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "\"| ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(buffer, target, 66);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "\n");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(buffer, class_definitions,
                              sizeof(class_definitions) - 1);
  if (status == ARCHBIRD_OK)
    status = render_mermaid_classes(graph, buffer);
  return status;
}

void archbird_graph_options_init(ArchbirdGraphOptions *options) {
  if (!options)
    return;
  options->struct_size = sizeof(*options);
  options->format = ARCHBIRD_GRAPH_GRAPHML;
  options->view = ARCHBIRD_GRAPH_COMPONENTS;
  options->direction = ARCHBIRD_GRAPH_LR;
  options->max_nodes = 200;
  options->max_edge_names = 3;
}

ArchbirdStatus archbird_map_export_graph(ArchbirdEngine *engine,
                                         const uint8_t *artifact_json,
                                         size_t artifact_length,
                                         const ArchbirdGraphOptions *options,
                                         ArchbirdWriteFn write_fn,
                                         void *user_data) {
  ArchbirdGraphOptions resolved;
  GraphData graph = {0};
  AbValue input = {0};
  AbBuffer output;
  ArchbirdStatus status;
  if (!engine || (!artifact_json && artifact_length) || !write_fn)
    return ARCHBIRD_INVALID_ARGUMENT;
  archbird_graph_options_init(&resolved);
  if (options) {
    if (options->struct_size != sizeof(*options))
      return ARCHBIRD_INVALID_ARGUMENT;
    resolved = *options;
  }
  if ((resolved.format != ARCHBIRD_GRAPH_GRAPHML &&
       resolved.format != ARCHBIRD_GRAPH_MERMAID &&
       resolved.format != ARCHBIRD_GRAPH_JSON) ||
      (resolved.view != ARCHBIRD_GRAPH_COMPONENTS &&
       resolved.view != ARCHBIRD_GRAPH_FILES &&
       resolved.view != ARCHBIRD_GRAPH_SYMBOLS) ||
      resolved.direction < ARCHBIRD_GRAPH_LR ||
      resolved.direction > ARCHBIRD_GRAPH_BT)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (resolved.view == ARCHBIRD_GRAPH_SYMBOLS &&
      resolved.format != ARCHBIRD_GRAPH_JSON)
    return ARCHBIRD_INVALID_ARGUMENT;
  archbird_error_clear(engine);
  graph.engine = engine;
  graph.view = resolved.view;
  ab_buffer_init(&output, engine);
  status = ab_json_value_decode(engine, artifact_json, artifact_length, &input);
  if (status == ARCHBIRD_OK)
    status = build_graph(&graph, &input);
  if (status == ARCHBIRD_OK && resolved.format == ARCHBIRD_GRAPH_JSON &&
      resolved.max_nodes && graph.node_count > resolved.max_nodes)
    status = archbird_error_set(
        engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
        "JSON graph view has %zu nodes, exceeding %zu; narrow the Query or "
        "increase max_nodes",
        graph.node_count, resolved.max_nodes);
  if (status == ARCHBIRD_OK)
    status = resolved.format == ARCHBIRD_GRAPH_GRAPHML
                 ? render_graphml(&graph, &output)
             : resolved.format == ARCHBIRD_GRAPH_MERMAID
                 ? render_mermaid(&graph, &output, &resolved)
                 : render_graph_json(&graph, &input, &resolved, &output);
  if (status == ARCHBIRD_OK && write_fn(user_data, output.data, output.length))
    status =
        archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                           "graph output callback failed");
  ab_buffer_free(&output);
  graph_free(&graph);
  ab_value_free(engine, &input);
  return status;
}
