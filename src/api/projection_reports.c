#include <archbird/archbird.h>

#include "base/archbird_internal.h"
#include "base/json_value.h"
#include "base/render_internal.h"
#include "interchange/reports/projection_reports.h"
#include "projection/projection_internal.h"

#include <string.h>

static int stable_id(const AbString *value) {
  size_t index;
  if (!value || !value->length)
    return 0;
  for (index = 0; index < value->length; index++) {
    unsigned char byte = (unsigned char)value->data[index];
    if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
          (byte >= '0' && byte <= '9') ||
          (index &&
           (byte == '_' || byte == '.' || byte == ':' || byte == '-'))))
      return 0;
  }
  return 1;
}

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "projection: %s", message);
}

ArchbirdStatus archbird_projection_render_markdown(
    ArchbirdEngine *engine, const uint8_t *map_json, size_t map_length,
    const uint8_t *resolution_json, size_t resolution_length,
    const uint8_t *projection_json, size_t projection_length,
    ArchbirdReportDetail detail, size_t max_chars, ArchbirdWriteFn write_fn,
    void *user_data) {
  AbValue map = {0};
  AbValue resolution = {0};
  AbValue definition = {0};
  AbProjectionPlan plan = {0};
  AbProjectionResult result = {0};
  AbBuffer report;
  const AbValue *id_value;
  const AbString *id = NULL;
  ArchbirdStatus status;
  if (!engine || !map_json || !map_length ||
      (!resolution_json && resolution_length) || !projection_json ||
      !projection_length || !write_fn ||
      detail < ARCHBIRD_REPORT_DETAIL_COMPACT ||
      detail > ARCHBIRD_REPORT_DETAIL_FULL)
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&report, engine);
  status = ab_json_value_decode(engine, map_json, map_length, &map);
  if (status == ARCHBIRD_OK && resolution_length)
    status = ab_json_value_decode(engine, resolution_json, resolution_length,
                                  &resolution);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, projection_json, projection_length,
                                  &definition);
  if (status == ARCHBIRD_OK && definition.kind != AB_VALUE_OBJECT)
    status = invalid(engine, "definition must be an object");
  id_value = status == ARCHBIRD_OK ? ab_value_member(&definition, "id") : NULL;
  if (id_value && id_value->kind == AB_VALUE_STRING)
    id = &id_value->as.text;
  if (status == ARCHBIRD_OK && (!id || !stable_id(id)))
    status = invalid(engine, "id must be a stable non-empty identifier");
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_compile(engine, &definition, id, &plan);
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_evaluate(
        engine, &plan, &map, resolution_length ? &resolution : NULL, &result);
  if (status == ARCHBIRD_OK)
    status = ab_projection_report_markdown(engine, &plan, &result, detail,
                                           max_chars, &report);
  if (status == ARCHBIRD_OK && write_fn(user_data, report.data, report.length))
    status =
        archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                           "projection report callback failed");
  ab_buffer_free(&report);
  ab_projection_result_free(engine, &result);
  ab_projection_plan_free(engine, &plan);
  ab_value_free(engine, &definition);
  ab_value_free(engine, &resolution);
  ab_value_free(engine, &map);
  return status;
}
