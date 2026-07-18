#include <archbird/archbird.h>

#include "json_value.h"
#include "render_internal.h"
#include "sha256.h"
#include "verify_checks.h"
#include "verify_internal.h"
#include "verify_runtime.h"

#include <string.h>

static ArchbirdStatus render_nullable_path(AbBuffer *buffer,
                                           const AbValue *value) {
  return value ? ab_verify_render_normalized_path(buffer, value)
               : ab_buffer_literal(buffer, "null");
}

static int source_kind(const AbValue *kind) {
  return ab_verify_string_is(kind, "python_enum") ||
         ab_verify_string_is(kind, "python_set") ||
         ab_verify_string_is(kind, "c_enum") ||
         ab_verify_string_is(kind, "c_designated_initializer") ||
         ab_verify_string_is(kind, "c_macro_set");
}

static int source_extractor_covers(const AbVerifySuiteView *suite,
                                   const AbString *project,
                                   const AbString *path) {
  size_t index;
  for (index = 0; index < suite->extractors->as.object.count; index++) {
    const AbValue *row = &suite->extractors->as.object.fields[index].value;
    const AbValue *kind = ab_value_member(row, "kind");
    const AbValue *candidate_project = ab_value_member(row, "project");
    const AbValue *candidate_path = ab_value_member(row, "path");
    if (source_kind(kind) && candidate_project && candidate_path &&
        ab_string_equal(&candidate_project->as.text, project) &&
        ab_string_equal(&candidate_path->as.text, path))
      return 1;
  }
  return 0;
}

static ArchbirdStatus suite_digest(AbVerifySuiteView *suite) {
  AbBuffer canonical;
  uint8_t digest[32];
  ArchbirdStatus status;
  ab_buffer_init(&canonical, suite->engine);
  status = ab_value_render(&canonical, suite->root);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(canonical.data, canonical.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, suite->sha256);
  ab_buffer_free(&canonical);
  return status;
}

static ArchbirdStatus render_projects(AbBuffer *buffer,
                                      const AbVerifySuiteView *suite) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0;
       status == ARCHBIRD_OK && index < suite->projects->as.object.count;
       index++) {
    const AbObjectField *field = &suite->projects->as.object.fields[index];
    const AbValue *config = ab_value_member(&field->value, "config");
    const AbValue *map = ab_value_member(&field->value, "map");
    const AbValue *root = ab_value_member(&field->value, "root");
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"config\":");
    if (status == ARCHBIRD_OK)
      status = render_nullable_path(buffer, config);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"map\":");
    if (status == ARCHBIRD_OK)
      status = render_nullable_path(buffer, map);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"name\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, field->name.data, field->name.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"root\":");
    if (status == ARCHBIRD_OK)
      status = render_nullable_path(buffer, root);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_sources(AbBuffer *buffer,
                                     const AbVerifySuiteView *suite) {
  size_t index;
  size_t emitted = 0;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0;
       status == ARCHBIRD_OK && index < suite->extractors->as.object.count;
       index++) {
    const AbObjectField *field = &suite->extractors->as.object.fields[index];
    const AbValue *kind = ab_value_member(&field->value, "kind");
    const AbValue *project;
    const AbValue *path;
    const char *provider;
    if (!source_kind(kind))
      continue;
    project = ab_value_member(&field->value, "project");
    path = ab_value_member(&field->value, "path");
    provider = (ab_verify_string_is(kind, "python_enum") ||
                ab_verify_string_is(kind, "python_set"))
                   ? "python-ast"
                   : "native-c";
    if (emitted++)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"extractor\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, field->name.data, field->name.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"kind\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, kind);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"path\":");
    if (status == ARCHBIRD_OK)
      status = ab_verify_render_normalized_path(buffer, path);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"project\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, project);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"provider\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, provider, strlen(provider));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  for (index = 0;
       status == ARCHBIRD_OK && index < suite->projects->as.object.count;
       index++) {
    const AbObjectField *project = &suite->projects->as.object.fields[index];
    const AbValue *lock = ab_value_member(&project->value, "source_lock");
    size_t lock_index;
    if (!lock)
      continue;
    for (lock_index = 0;
         status == ARCHBIRD_OK && lock_index < lock->as.object.count;
         lock_index++) {
      const AbString *path = &lock->as.object.fields[lock_index].name;
      if (source_extractor_covers(suite, &project->name, path))
        continue;
      if (emitted++)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(
            buffer, "{\"extractor\":\"\",\"kind\":\"source_lock\",\"path\":");
      if (status == ARCHBIRD_OK)
        status = ab_verify_render_normalized_path(
            buffer, &(AbValue){.kind = AB_VALUE_STRING, .as.text = *path});
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"project\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_json_string(buffer, project->name.data,
                                       project->name.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"provider\":\"content-lock\"}");
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_attestations(AbBuffer *buffer,
                                          const AbVerifySuiteView *suite) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  if (!suite->attestations)
    return ab_buffer_literal(buffer, "]");
  for (index = 0;
       status == ARCHBIRD_OK && index < suite->attestations->as.object.count;
       index++) {
    const AbObjectField *field = &suite->attestations->as.object.fields[index];
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"name\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, field->name.data, field->name.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"path\":");
    if (status == ARCHBIRD_OK)
      status = ab_verify_render_normalized_path(
          buffer, ab_value_member(&field->value, "path"));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"project\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_value_render(buffer, ab_value_member(&field->value, "project"));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_plan(AbBuffer *buffer,
                                  const AbVerifySuiteView *suite) {
  ArchbirdStatus status = ab_buffer_literal(
      buffer, "{\"artifact\":\"verification-plan\",\"attestations\":");
  if (status == ARCHBIRD_OK)
    status = render_attestations(buffer, suite);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"projects\":");
  if (status == ARCHBIRD_OK)
    status = render_projects(buffer, suite);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"schema_version\":1,\"sources\":");
  if (status == ARCHBIRD_OK)
    status = render_sources(buffer, suite);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"suite\":{\"candidate\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, suite->candidate ? "true" : "false");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"name\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, suite->name);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, suite->sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}}");
  return status;
}

ArchbirdStatus
archbird_verification_plan(ArchbirdEngine *engine, const uint8_t *suite_json,
                           size_t suite_length, uint32_t json_flags,
                           ArchbirdWriteFn write_fn, void *user_data) {
  AbValue document = {0};
  AbVerifySuiteView suite = {0};
  AbBuffer buffer;
  ArchbirdStatus status;
  if (!engine || (!suite_json && suite_length) || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&buffer, engine);
  status = ab_json_value_decode(engine, suite_json, suite_length, &document);
  if (status == ARCHBIRD_OK)
    status = ab_verify_suite_validate(engine, &document, &suite);
  if (status == ARCHBIRD_OK)
    status = suite_digest(&suite);
  if (status == ARCHBIRD_OK)
    status = render_plan(&buffer, &suite);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&buffer);
  ab_value_free(engine, &document);
  return status;
}

ArchbirdStatus ab_verification_context_analyze(
    ArchbirdEngine *engine, const uint8_t *suite_json, size_t suite_length,
    const uint8_t *verification_input_json, size_t verification_input_length,
    AbValue *suite_document, AbValue *input_document,
    AbVerificationContext *context) {
  ArchbirdStatus status;
  if (!engine || (!suite_json && suite_length) ||
      (!verification_input_json && verification_input_length) ||
      !suite_document || !input_document || !context)
    return ARCHBIRD_INVALID_ARGUMENT;
  context->engine = engine;
  status =
      ab_json_value_decode(engine, suite_json, suite_length, suite_document);
  if (status == ARCHBIRD_OK)
    status = ab_verify_suite_validate(engine, suite_document, &context->suite);
  if (status == ARCHBIRD_OK)
    status = suite_digest(&context->suite);
  if (status == ARCHBIRD_OK && context->suite.candidate)
    status =
        archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                           "verification suite is an unreviewed candidate");
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, verification_input_json,
                                  verification_input_length, input_document);
  if (status == ARCHBIRD_OK)
    status = ab_verify_input_validate(engine, &context->suite, input_document,
                                      &context->input);
  if (status == ARCHBIRD_OK)
    status = ab_verify_collect_project_diagnostics(context);
  if (status == ARCHBIRD_OK)
    status = ab_verify_attestations_load(context);
  if (status == ARCHBIRD_OK)
    status = ab_verify_extract_all(context);
  if (status == ARCHBIRD_OK)
    status = ab_verify_evaluate_checks(context);
  if (status == ARCHBIRD_OK)
    status = ab_verify_apply_waivers(context);
  if (status == ARCHBIRD_OK)
    status = ab_verify_apply_baseline(context);
  if (status == ARCHBIRD_OK)
    ab_verify_diagnostics_finish(context);
  return status;
}

ArchbirdStatus archbird_verification_analyze(
    ArchbirdEngine *engine, const uint8_t *suite_json, size_t suite_length,
    const uint8_t *verification_input_json, size_t verification_input_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data) {
  AbValue suite_document = {0};
  AbValue input_document = {0};
  AbVerificationContext context = {0};
  AbBuffer rendered;
  ArchbirdStatus status;
  if (!engine || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&rendered, engine);
  status = ab_verification_context_analyze(
      engine, suite_json, suite_length, verification_input_json,
      verification_input_length, &suite_document, &input_document, &context);
  if (status == ARCHBIRD_OK)
    status = ab_verify_render_result(&context, &rendered);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, rendered.data, rendered.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&rendered);
  ab_verification_context_free(&context);
  ab_value_free(engine, &input_document);
  ab_value_free(engine, &suite_document);
  return status;
}
