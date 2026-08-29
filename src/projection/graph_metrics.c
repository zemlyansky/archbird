#include "projection/graph_metrics.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static ArchbirdStatus graph_csr(ArchbirdEngine *engine,
                                const AbGraphMetricsEdge *edges,
                                size_t edge_count, size_t node_count,
                                int reverse, size_t **offsets_out,
                                size_t **adjacent_out) {
  size_t *offsets = NULL;
  size_t *adjacent = NULL;
  size_t *cursor = NULL;
  size_t index;
  if (node_count == SIZE_MAX)
    return ARCHBIRD_LIMIT_EXCEEDED;
  offsets = (size_t *)ab_calloc(engine, node_count + 1, sizeof(*offsets));
  if (!offsets)
    return ARCHBIRD_OUT_OF_MEMORY;
  if (edge_count)
    adjacent = (size_t *)ab_calloc(engine, edge_count, sizeof(*adjacent));
  if (edge_count && !adjacent) {
    ab_free(engine, offsets);
    return ARCHBIRD_OUT_OF_MEMORY;
  }
  for (index = 0; index < edge_count; index++) {
    size_t source = reverse ? edges[index].target : edges[index].source;
    size_t target = reverse ? edges[index].source : edges[index].target;
    if (source >= node_count || target >= node_count ||
        offsets[source + 1] == SIZE_MAX) {
      ab_free(engine, adjacent);
      ab_free(engine, offsets);
      return source >= node_count || target >= node_count
                 ? ARCHBIRD_INVALID_SCHEMA
                 : ARCHBIRD_LIMIT_EXCEEDED;
    }
    offsets[source + 1]++;
  }
  for (index = 1; index <= node_count; index++) {
    if (SIZE_MAX - offsets[index - 1] < offsets[index]) {
      ab_free(engine, adjacent);
      ab_free(engine, offsets);
      return ARCHBIRD_LIMIT_EXCEEDED;
    }
    offsets[index] += offsets[index - 1];
  }
  if (node_count)
    cursor = (size_t *)ab_calloc(engine, node_count, sizeof(*cursor));
  if (node_count && !cursor) {
    ab_free(engine, adjacent);
    ab_free(engine, offsets);
    return ARCHBIRD_OUT_OF_MEMORY;
  }
  if (node_count)
    memcpy(cursor, offsets, node_count * sizeof(*cursor));
  for (index = 0; index < edge_count; index++) {
    size_t source = reverse ? edges[index].target : edges[index].source;
    adjacent[cursor[source]++] =
        reverse ? edges[index].source : edges[index].target;
  }
  ab_free(engine, cursor);
  *offsets_out = offsets;
  *adjacent_out = adjacent;
  return ARCHBIRD_OK;
}

void ab_graph_metrics_free(ArchbirdEngine *engine,
                           AbGraphMetricsResult *result) {
  if (!result)
    return;
  ab_free(engine, result->cyclic);
  ab_free(engine, result->scc_size);
  ab_free(engine, result->in_relations);
  ab_free(engine, result->out_relations);
  ab_free(engine, result->in_neighbors);
  ab_free(engine, result->out_neighbors);
  memset(result, 0, sizeof(*result));
}

ArchbirdStatus ab_graph_metrics_evaluate(ArchbirdEngine *engine,
                                         size_t node_count,
                                         const AbGraphMetricsEdge *edges,
                                         size_t edge_count,
                                         AbGraphMetricsResult *out) {
  size_t *forward_offsets = NULL;
  size_t *forward = NULL;
  size_t *reverse_offsets = NULL;
  size_t *reverse = NULL;
  size_t *stack = NULL;
  size_t *cursor = NULL;
  size_t *order = NULL;
  size_t *component = NULL;
  size_t *component_sizes = NULL;
  unsigned char *visited = NULL;
  unsigned char *component_cyclic = NULL;
  size_t order_count = 0;
  size_t component_count = 0;
  size_t root;
  size_t index;
  ArchbirdStatus status;
  memset(out, 0, sizeof(*out));
  out->node_count = node_count;
  if (node_count) {
    out->out_neighbors =
        (uint64_t *)ab_calloc(engine, node_count, sizeof(*out->out_neighbors));
    out->in_neighbors =
        (uint64_t *)ab_calloc(engine, node_count, sizeof(*out->in_neighbors));
    out->out_relations =
        (uint64_t *)ab_calloc(engine, node_count, sizeof(*out->out_relations));
    out->in_relations =
        (uint64_t *)ab_calloc(engine, node_count, sizeof(*out->in_relations));
    out->scc_size =
        (uint64_t *)ab_calloc(engine, node_count, sizeof(*out->scc_size));
    out->cyclic = (unsigned char *)ab_calloc(engine, node_count, 1);
    if (!out->out_neighbors || !out->in_neighbors || !out->out_relations ||
        !out->in_relations || !out->scc_size || !out->cyclic) {
      ab_graph_metrics_free(engine, out);
      return ARCHBIRD_OUT_OF_MEMORY;
    }
  }
  for (index = 0; index < edge_count; index++) {
    if (edges[index].source >= node_count ||
        edges[index].target >= node_count || !edges[index].multiplicity) {
      status = ARCHBIRD_INVALID_SCHEMA;
      goto cleanup;
    }
    if (out->out_neighbors[edges[index].source] == UINT64_MAX ||
        out->in_neighbors[edges[index].target] == UINT64_MAX ||
        UINT64_MAX - out->out_relations[edges[index].source] <
            edges[index].multiplicity ||
        UINT64_MAX - out->in_relations[edges[index].target] <
            edges[index].multiplicity) {
      status = ARCHBIRD_LIMIT_EXCEEDED;
      goto cleanup;
    }
    out->out_neighbors[edges[index].source]++;
    out->in_neighbors[edges[index].target]++;
    out->out_relations[edges[index].source] += edges[index].multiplicity;
    out->in_relations[edges[index].target] += edges[index].multiplicity;
  }
  status = graph_csr(engine, edges, edge_count, node_count, 0, &forward_offsets,
                     &forward);
  if (status == ARCHBIRD_OK)
    status = graph_csr(engine, edges, edge_count, node_count, 1,
                       &reverse_offsets, &reverse);
  if (status == ARCHBIRD_OK && node_count) {
    stack = (size_t *)ab_calloc(engine, node_count, sizeof(*stack));
    cursor = (size_t *)ab_calloc(engine, node_count, sizeof(*cursor));
    order = (size_t *)ab_calloc(engine, node_count, sizeof(*order));
    component = (size_t *)ab_calloc(engine, node_count, sizeof(*component));
    component_sizes =
        (size_t *)ab_calloc(engine, node_count, sizeof(*component_sizes));
    visited = (unsigned char *)ab_calloc(engine, node_count, 1);
    component_cyclic = (unsigned char *)ab_calloc(engine, node_count, 1);
    if (!stack || !cursor || !order || !component || !component_sizes ||
        !visited || !component_cyclic)
      status = ARCHBIRD_OUT_OF_MEMORY;
  }
  for (root = 0; status == ARCHBIRD_OK && root < node_count; root++) {
    size_t depth = 0;
    if (visited[root])
      continue;
    stack[depth++] = root;
    visited[root] = 1;
    while (depth) {
      size_t node = stack[depth - 1];
      size_t degree = forward_offsets[node + 1] - forward_offsets[node];
      if (cursor[node] < degree) {
        size_t target = forward[forward_offsets[node] + cursor[node]++];
        if (!visited[target]) {
          visited[target] = 1;
          stack[depth++] = target;
        }
      } else {
        depth--;
        order[order_count++] = node;
      }
    }
  }
  if (status == ARCHBIRD_OK && order_count != node_count)
    status = ARCHBIRD_INVALID_SCHEMA;
  if (status == ARCHBIRD_OK && node_count)
    memset(component, 0xff, node_count * sizeof(*component));
  for (index = order_count; status == ARCHBIRD_OK && index; index--) {
    size_t depth = 0;
    size_t seed = order[index - 1];
    if (component[seed] != SIZE_MAX)
      continue;
    component[seed] = component_count;
    stack[depth++] = seed;
    while (depth) {
      size_t node = stack[--depth];
      size_t edge;
      component_sizes[component_count]++;
      for (edge = reverse_offsets[node]; edge < reverse_offsets[node + 1];
           edge++) {
        size_t source = reverse[edge];
        if (component[source] == SIZE_MAX) {
          component[source] = component_count;
          stack[depth++] = source;
        }
      }
    }
    component_count++;
  }
  for (index = 0; status == ARCHBIRD_OK && index < edge_count; index++)
    if (edges[index].source == edges[index].target)
      component_cyclic[component[edges[index].source]] = 1;
  for (index = 0; status == ARCHBIRD_OK && index < component_count; index++)
    if (component_sizes[index] > 1)
      component_cyclic[index] = 1;
  for (index = 0; status == ARCHBIRD_OK && index < node_count; index++) {
    out->scc_size[index] = (uint64_t)component_sizes[component[index]];
    out->cyclic[index] = component_cyclic[component[index]];
  }
cleanup:
  ab_free(engine, component_cyclic);
  ab_free(engine, visited);
  ab_free(engine, component_sizes);
  ab_free(engine, component);
  ab_free(engine, order);
  ab_free(engine, cursor);
  ab_free(engine, stack);
  ab_free(engine, reverse);
  ab_free(engine, reverse_offsets);
  ab_free(engine, forward);
  ab_free(engine, forward_offsets);
  if (status != ARCHBIRD_OK)
    ab_graph_metrics_free(engine, out);
  return status;
}
