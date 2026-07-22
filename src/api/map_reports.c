#include <archbird/archbird.h>

#include "act_internal.h"
#include "archbird_internal.h"
#include "json_value.h"
#include "map_reports.h"
#include "query_internal.h"
#include "render_internal.h"

#include <string.h>

static ArchbirdStatus map_report_write(ArchbirdEngine *engine, AbBuffer *buffer,
                                       ArchbirdWriteFn write_fn,
                                       void *user_data) {
  if (write_fn(user_data, buffer->data, buffer->length) != 0)
    return archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                              "map report callback failed");
  return ARCHBIRD_OK;
}

ArchbirdStatus
archbird_map_render_markdown(ArchbirdEngine *engine, const uint8_t *map_json,
                             size_t map_length, int full, size_t max_chars,
                             ArchbirdWriteFn write_fn, void *user_data) {
  AbValue map = {0};
  AbBuffer report;
  ArchbirdStatus status;
  if (!engine || (!map_json && map_length) || !write_fn ||
      (full != 0 && full != 1))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_build_identity_validate(engine);
  if (status != ARCHBIRD_OK)
    return status;
  ab_buffer_init(&report, engine);
  status = ab_json_value_decode(engine, map_json, map_length, &map);
  if (status == ARCHBIRD_OK)
    status = ab_map_report_markdown(engine, &map, full, max_chars, &report);
  if (status == ARCHBIRD_OK)
    status = map_report_write(engine, &report, write_fn, user_data);
  ab_buffer_free(&report);
  ab_value_free(engine, &map);
  return status;
}

ArchbirdStatus archbird_map_render_markdown_view(
    ArchbirdEngine *engine, const uint8_t *map_json, size_t map_length,
    ArchbirdMapView view, ArchbirdReportDetail detail, size_t max_chars,
    ArchbirdWriteFn write_fn, void *user_data) {
  AbValue map = {0};
  AbBuffer report;
  ArchbirdStatus status;
  if (!engine || (!map_json && map_length) || !write_fn ||
      view < ARCHBIRD_MAP_VIEW_OVERVIEW || view > ARCHBIRD_MAP_VIEW_AUDIT ||
      detail < ARCHBIRD_REPORT_DETAIL_COMPACT ||
      detail > ARCHBIRD_REPORT_DETAIL_FULL)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_build_identity_validate(engine);
  if (status != ARCHBIRD_OK)
    return status;
  ab_buffer_init(&report, engine);
  status = ab_json_value_decode(engine, map_json, map_length, &map);
  if (status == ARCHBIRD_OK)
    status = ab_map_report_markdown_view(engine, &map, view, detail, max_chars,
                                         &report);
  if (status == ARCHBIRD_OK)
    status = map_report_write(engine, &report, write_fn, user_data);
  ab_buffer_free(&report);
  ab_value_free(engine, &map);
  return status;
}

ArchbirdStatus
archbird_map_query_markdown(ArchbirdEngine *engine, const uint8_t *map_json,
                            size_t map_length, const uint8_t *resolution_json,
                            size_t resolution_length, const uint8_t *query_json,
                            size_t query_length, size_t max_chars,
                            ArchbirdWriteFn write_fn, void *user_data) {
  return archbird_map_query_markdown_view(
      engine, map_json, map_length, resolution_json, resolution_length,
      query_json, query_length, ARCHBIRD_QUERY_VIEW_FOCUSED,
      ARCHBIRD_REPORT_DETAIL_STANDARD, max_chars, write_fn, user_data);
}

ArchbirdStatus archbird_map_query_markdown_view(
    ArchbirdEngine *engine, const uint8_t *map_json, size_t map_length,
    const uint8_t *resolution_json, size_t resolution_length,
    const uint8_t *query_json, size_t query_length, ArchbirdQueryView view,
    ArchbirdReportDetail detail, size_t max_chars, ArchbirdWriteFn write_fn,
    void *user_data) {
  return archbird_map_query_markdown_view_with_verification(
      engine, map_json, map_length, resolution_json, resolution_length,
      query_json, query_length, NULL, 0, view, detail, max_chars, write_fn,
      user_data);
}

ArchbirdStatus archbird_map_query_markdown_view_with_verification(
    ArchbirdEngine *engine, const uint8_t *map_json, size_t map_length,
    const uint8_t *resolution_json, size_t resolution_length,
    const uint8_t *query_json, size_t query_length,
    const uint8_t *verification_json, size_t verification_length,
    ArchbirdQueryView view, ArchbirdReportDetail detail, size_t max_chars,
    ArchbirdWriteFn write_fn, void *user_data) {
  AbValue map = {0};
  AbValue resolution = {0};
  AbValue request = {0};
  AbValue query = {0};
  AbActVerification verification = {0};
  AbBuffer query_json_buffer;
  AbBuffer report;
  ArchbirdStatus status;
  if (!engine || (!map_json && map_length) ||
      (!resolution_json && resolution_length) ||
      (!query_json && query_length) ||
      (!verification_json && verification_length) || !write_fn ||
      view < ARCHBIRD_QUERY_VIEW_FOCUSED ||
      view > ARCHBIRD_QUERY_VIEW_CHANGES ||
      detail < ARCHBIRD_REPORT_DETAIL_COMPACT ||
      detail > ARCHBIRD_REPORT_DETAIL_FULL)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_build_identity_validate(engine);
  if (status != ARCHBIRD_OK)
    return status;
  ab_buffer_init(&query_json_buffer, engine);
  ab_buffer_init(&report, engine);
  status = ab_json_value_decode(engine, map_json, map_length, &map);
  if (status == ARCHBIRD_OK && resolution_length)
    status = ab_json_value_decode(engine, resolution_json, resolution_length,
                                  &resolution);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, query_json, query_length, &request);
  if (status == ARCHBIRD_OK)
    status = ab_query_execute_value(engine, &map,
                                    resolution_length ? &resolution : NULL,
                                    &request, &query_json_buffer);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, query_json_buffer.data,
                                  query_json_buffer.length, &query);
  if (status == ARCHBIRD_OK && verification_length)
    status = ab_act_verification_load(engine, verification_json,
                                      verification_length, &verification);
  if (status == ARCHBIRD_OK)
    status = ab_query_report_markdown_view_with_verification(
        engine, &map, &query, verification_length ? &verification.root : NULL,
        view, detail, max_chars, &report);
  if (status == ARCHBIRD_OK)
    status = map_report_write(engine, &report, write_fn, user_data);
  ab_value_free(engine, &query);
  ab_value_free(engine, &request);
  ab_act_verification_free(&verification);
  ab_value_free(engine, &resolution);
  ab_value_free(engine, &map);
  ab_buffer_free(&report);
  ab_buffer_free(&query_json_buffer);
  return status;
}
