#include "okf_internal.h"

ArchbirdStatus
archbird_okf_analyze(ArchbirdEngine *engine, const uint8_t *source_bundle_json,
                     size_t source_bundle_length, const uint8_t *query_json,
                     size_t query_length, ArchbirdOkfFormat format,
                     int include_body, uint32_t json_flags,
                     ArchbirdWriteFn write_fn, void *user_data) {
  AbOkfIndex index = {0};
  AbOkfQuery query = {0};
  AbBuffer rendered;
  ArchbirdStatus status;
  int has_query = query_json || query_length;
  if (!engine || (!source_bundle_json && source_bundle_length) ||
      (!query_json && query_length) || !write_fn ||
      (format != ARCHBIRD_OKF_JSON && format != ARCHBIRD_OKF_MARKDOWN) ||
      (include_body != 0 && include_body != 1) ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&rendered, engine);
  status = ab_okf_index_load(engine, source_bundle_json, source_bundle_length,
                             &index);
  if (status == ARCHBIRD_OK && has_query)
    status =
        ab_okf_query_load(engine, query_json, query_length, &index, &query);
  if (status == ARCHBIRD_OK && format == ARCHBIRD_OKF_JSON)
    status = ab_okf_render_json(&index, has_query ? &query : NULL, include_body,
                                &rendered);
  else if (status == ARCHBIRD_OK)
    status =
        ab_okf_render_markdown(&index, has_query ? &query : NULL, &rendered);
  if (status == ARCHBIRD_OK && format == ARCHBIRD_OKF_JSON)
    status = archbird_json_canonicalize(engine, rendered.data, rendered.length,
                                        json_flags, write_fn, user_data);
  else if (status == ARCHBIRD_OK &&
           write_fn(user_data, rendered.data, rendered.length) != 0)
    status =
        archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                           "OKF report callback failed");
  ab_buffer_free(&rendered);
  ab_okf_query_free(&query);
  ab_okf_index_free(&index);
  return status;
}
