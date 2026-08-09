#include "archbird/archbird.h"

#include "base/render_internal.h"
#include "plan/plan_internal.h"
#include "plan/plan_report.h"

ArchbirdStatus archbird_plan_validate(ArchbirdEngine *engine,
                                      const uint8_t *plan_json,
                                      size_t plan_length) {
  AbPlan plan = {0};
  ArchbirdStatus status;
  if (!engine || !plan_json || !plan_length)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_plan_load(engine, plan_json, plan_length, &plan);
  ab_plan_free(engine, &plan);
  return status;
}

ArchbirdStatus archbird_plan_render_markdown(ArchbirdEngine *engine,
                                             const uint8_t *plan_json,
                                             size_t plan_length,
                                             ArchbirdWriteFn write_fn,
                                             void *user_data) {
  AbPlan plan = {0};
  AbBuffer report;
  ArchbirdStatus status;
  if (!engine || !plan_json || !plan_length || !write_fn)
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&report, engine);
  status = ab_plan_load(engine, plan_json, plan_length, &plan);
  if (status == ARCHBIRD_OK)
    status = ab_plan_report_markdown(engine, &plan, &report);
  if (status == ARCHBIRD_OK && write_fn(user_data, report.data, report.length))
    status =
        archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                           "Plan report callback failed");
  ab_buffer_free(&report);
  ab_plan_free(engine, &plan);
  return status;
}
