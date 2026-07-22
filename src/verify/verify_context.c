#include "verify_runtime.h"

#include "verify_checks.h"

void ab_verification_context_free(AbVerificationContext *context) {
  size_t index;
  if (!context)
    return;
  for (index = 0; context->facts && index < context->fact_count; index++)
    ab_projection_data_free(context->engine, &context->facts[index]);
  ab_free(context->engine, context->facts);
  context->facts = NULL;
  context->fact_count = 0;
  for (index = 0; context->checks && index < context->check_count; index++)
    ab_verify_check_result_free(context->engine, &context->checks[index]);
  ab_free(context->engine, context->checks);
  context->checks = NULL;
  context->check_count = 0;
  ab_verify_diagnostics_free(context);
  ab_verify_baseline_free(context->engine, &context->baseline);
  ab_constraints_observations_free(context);
}
