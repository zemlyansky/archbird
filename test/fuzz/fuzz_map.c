#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  ArchbirdEngine *engine = fuzz_engine();
  if (!engine)
    return 0;
#ifdef ARCHBIRD_FUZZ_QUERY_INPUT
  (void)archbird_map_query(engine, fuzz_map_json, sizeof(fuzz_map_json) - 1,
                           data, size, 0, fuzz_discard, NULL);
  (void)archbird_map_query_markdown(engine, fuzz_map_json,
                                    sizeof(fuzz_map_json) - 1, data, size, 0,
                                    fuzz_discard, NULL);
  (void)archbird_map_query_markdown_view(
      engine, fuzz_map_json, sizeof(fuzz_map_json) - 1, data, size,
      ARCHBIRD_QUERY_VIEW_CHANGES, ARCHBIRD_REPORT_DETAIL_STANDARD, 0,
      fuzz_discard, NULL);
#else
  ArchbirdGraphOptions options;
  size_t format;
  size_t view;
  (void)archbird_map_query(engine, data, size, fuzz_query_json,
                           sizeof(fuzz_query_json) - 1, 0, fuzz_discard, NULL);
  (void)archbird_map_diff(engine, fuzz_map_json, sizeof(fuzz_map_json) - 1,
                          data, size, 0, fuzz_discard, NULL);
  (void)archbird_map_diff(engine, data, size, fuzz_map_json,
                          sizeof(fuzz_map_json) - 1, 0, fuzz_discard, NULL);
  (void)archbird_map_freshness(engine, fuzz_map_json, sizeof(fuzz_map_json) - 1,
                               data, size, 0, fuzz_discard, NULL);
  (void)archbird_map_freshness(engine, data, size, fuzz_map_json,
                               sizeof(fuzz_map_json) - 1, 0, fuzz_discard,
                               NULL);
  (void)archbird_map_render_markdown(engine, data, size, 0, 0, fuzz_discard,
                                     NULL);
  (void)archbird_map_render_markdown(engine, data, size, 1, 0, fuzz_discard,
                                     NULL);
  (void)archbird_map_query_markdown(engine, data, size, fuzz_query_json,
                                    sizeof(fuzz_query_json) - 1, 4096,
                                    fuzz_discard, NULL);
  (void)archbird_map_query_markdown_view(
      engine, data, size, fuzz_query_json, sizeof(fuzz_query_json) - 1,
      ARCHBIRD_QUERY_VIEW_CHANGES, ARCHBIRD_REPORT_DETAIL_COMPACT, 4096,
      fuzz_discard, NULL);
  for (format = ARCHBIRD_GRAPH_GRAPHML; format <= ARCHBIRD_GRAPH_MERMAID;
       format++) {
    for (view = ARCHBIRD_GRAPH_COMPONENTS; view <= ARCHBIRD_GRAPH_FILES;
         view++) {
      archbird_graph_options_init(&options);
      options.format = (ArchbirdGraphFormat)format;
      options.view = (ArchbirdGraphView)view;
      options.direction = ARCHBIRD_GRAPH_LR;
      (void)archbird_map_export_graph(engine, data, size, &options,
                                      fuzz_discard, NULL);
    }
  }
  (void)archbird_okf_publish(engine, data, size, NULL, 0, NULL, 0, NULL, 0,
                             NULL, 0, NULL, 0, 0, fuzz_discard, NULL);
#endif
  archbird_engine_destroy(engine);
  return 0;
}
