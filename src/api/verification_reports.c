#include <archbird/archbird.h>

#include "json_value.h"
#include "render_internal.h"
#include "verify_reports.h"
#include "verify_runtime.h"

ArchbirdStatus archbird_verification_analyze_report(
    ArchbirdEngine *engine, const uint8_t *suite_json, size_t suite_length,
    const uint8_t *verification_input_json, size_t verification_input_length,
    ArchbirdVerificationFormat format, size_t max_findings, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data) {
  AbValue suite_document = {0};
  AbValue input_document = {0};
  AbVerificationContext context = {0};
  AbBuffer rendered;
  ArchbirdStatus status;
  if (!engine)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (format == ARCHBIRD_VERIFICATION_JSON)
    return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                              ARCHBIRD_NO_OFFSET,
                              "verification report format must not be JSON");
  if (!write_fn || format < ARCHBIRD_VERIFICATION_MARKDOWN ||
      format > ARCHBIRD_VERIFICATION_JUNIT ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_build_identity_validate(engine);
  if (status != ARCHBIRD_OK)
    return status;
  ab_buffer_init(&rendered, engine);
  status = ab_verification_context_analyze(
      engine, suite_json, suite_length, verification_input_json,
      verification_input_length, &suite_document, &input_document, &context);
  if (status == ARCHBIRD_OK && format == ARCHBIRD_VERIFICATION_MARKDOWN)
    status = ab_verify_render_markdown(&context, &rendered, max_findings);
  else if (status == ARCHBIRD_OK && format == ARCHBIRD_VERIFICATION_SARIF)
    status = ab_verify_render_sarif(&context, &rendered);
  else if (status == ARCHBIRD_OK)
    status = ab_verify_render_junit(&context, &rendered);
  if (status == ARCHBIRD_OK && format == ARCHBIRD_VERIFICATION_SARIF)
    status = archbird_json_canonicalize(engine, rendered.data, rendered.length,
                                        json_flags, write_fn, user_data);
  else if (status == ARCHBIRD_OK &&
           write_fn(user_data, rendered.data, rendered.length) != 0)
    status =
        archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                           "verification report callback failed");
  ab_buffer_free(&rendered);
  ab_verification_context_free(&context);
  ab_value_free(engine, &input_document);
  ab_value_free(engine, &suite_document);
  return status;
}
