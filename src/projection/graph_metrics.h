#ifndef ARCHBIRD_GRAPH_METRICS_H
#define ARCHBIRD_GRAPH_METRICS_H

#include "base/archbird_internal.h"

typedef struct AbGraphMetricsEdge {
  size_t source;
  size_t target;
  uint64_t multiplicity;
} AbGraphMetricsEdge;

typedef struct AbGraphMetricsResult {
  uint64_t *out_neighbors;
  uint64_t *in_neighbors;
  uint64_t *out_relations;
  uint64_t *in_relations;
  uint64_t *scc_size;
  unsigned char *cyclic;
  size_t node_count;
} AbGraphMetricsResult;

/* Edges are unique directed identity pairs with positive multiplicity. */
ArchbirdStatus ab_graph_metrics_evaluate(ArchbirdEngine *engine,
                                         size_t node_count,
                                         const AbGraphMetricsEdge *edges,
                                         size_t edge_count,
                                         AbGraphMetricsResult *out);
void ab_graph_metrics_free(ArchbirdEngine *engine,
                           AbGraphMetricsResult *result);

#endif
