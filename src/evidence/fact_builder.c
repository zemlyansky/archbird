#include "fact_builder.h"

#include "archbird_internal.h"
#include "sha256.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ArchbirdStatus copy_literal(ArchbirdEngine *engine, AbString *out,
                                   const char *value) {
  return ab_string_copy(engine, out, value, strlen(value));
}

static void hash_u64(ArchbirdSha256Context *context, uint64_t value) {
  uint8_t bytes[8];
  size_t index;
  for (index = 0; index < sizeof(bytes); index++)
    bytes[sizeof(bytes) - index - 1] = (uint8_t)(value >> (index * 8));
  (void)archbird_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_bytes(ArchbirdSha256Context *context, const void *bytes,
                       size_t length) {
  hash_u64(context, length);
  (void)archbird_sha256_update(context, bytes, length);
}

static ArchbirdStatus fact_make_id(ArchbirdEngine *engine, AbFact *fact) {
  ArchbirdSha256Context context;
  uint8_t digest[32];
  char hex[65];
  char id[67];
  archbird_sha256_init(&context);
  hash_bytes(&context, fact->project.data, fact->project.length);
  hash_bytes(&context, fact->path.data, fact->path.length);
  hash_bytes(&context, fact->domain.data, fact->domain.length);
  hash_bytes(&context, fact->kind.data, fact->kind.length);
  hash_u64(&context, fact->span_start);
  hash_u64(&context, fact->span_end);
  hash_bytes(&context, fact->key.data, fact->key.length);
  archbird_sha256_final(&context, digest);
  archbird_sha256_hex(digest, hex);
  id[0] = 'f';
  id[1] = ':';
  memcpy(id + 2, hex, 64);
  id[66] = '\0';
  return ab_string_copy(engine, &fact->id, id, 66);
}

void ab_bundle_builder_abort(AbBundleBuilder *builder) {
  if (!builder)
    return;
  ab_provider_bundle_free(builder->engine, &builder->bundle);
  memset(builder, 0, sizeof(*builder));
}

static ArchbirdStatus
builder_init(AbBundleBuilder *builder, ArchbirdEngine *engine,
             const AbSourceManifest *manifest, const AbManifestFile *file,
             const char *scope, const uint8_t source_manifest_sha256[32],
             int bind_source, const char *producer_name,
             const char *producer_version,
             const uint8_t implementation_sha256[32],
             const uint8_t configuration_sha256[32]) {
  ArchbirdStatus status;
  if (!builder || !engine || !manifest || !scope ||
      (!file && strcmp(scope, "file") == 0) ||
      (!bind_source && !source_manifest_sha256) || !producer_name ||
      !producer_version || !implementation_sha256 || !configuration_sha256)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(builder, 0, sizeof(*builder));
  builder->engine = engine;
#define COPY(expression)                                                       \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK) {                                               \
      ab_bundle_builder_abort(builder);                                        \
      return status;                                                           \
    }                                                                          \
  } while (0)
  COPY(copy_literal(engine, &builder->bundle.subject.scope, scope));
  COPY(ab_string_copy(engine, &builder->bundle.subject.project,
                      manifest->project.data, manifest->project.length));
  builder->bundle.subject.has_project = 1;
  if (file) {
    COPY(ab_string_copy(engine, &builder->bundle.subject.path, file->path.data,
                        file->path.length));
    builder->bundle.subject.has_path = 1;
  }
  COPY(copy_literal(engine, &builder->bundle.producer.name, producer_name));
  COPY(copy_literal(engine, &builder->bundle.producer.version,
                    producer_version));
  memcpy(builder->bundle.producer.implementation_sha256, implementation_sha256,
         32);
  memcpy(builder->bundle.producer.configuration_sha256, configuration_sha256,
         32);
  builder->bundle.producer.has_configuration_sha256 = 1;
  builder->bundle.inputs =
      (AbProviderInput *)ab_calloc(engine, 1, sizeof(*builder->bundle.inputs));
  if (!builder->bundle.inputs) {
    ab_bundle_builder_abort(builder);
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory creating provider input");
  }
  builder->bundle.input_count = 1;
  COPY(ab_string_copy(engine, &builder->bundle.inputs[0].project,
                      manifest->project.data, manifest->project.length));
  if (bind_source) {
    COPY(ab_string_copy(engine, &builder->bundle.inputs[0].path,
                        file->path.data, file->path.length));
    builder->bundle.inputs[0].has_path = 1;
    memcpy(builder->bundle.inputs[0].source_sha256, file->sha256, 32);
    builder->bundle.inputs[0].has_source_sha256 = 1;
  } else {
    memcpy(builder->bundle.inputs[0].source_manifest_sha256,
           source_manifest_sha256, 32);
    builder->bundle.inputs[0].has_source_manifest_sha256 = 1;
  }
#undef COPY
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_bundle_builder_init_file(
    AbBundleBuilder *builder, ArchbirdEngine *engine,
    const AbSourceManifest *manifest, const AbManifestFile *file,
    const char *producer_name, const char *producer_version,
    const uint8_t implementation_sha256[32],
    const uint8_t configuration_sha256[32]) {
  return builder_init(builder, engine, manifest, file, "file", NULL, 1,
                      producer_name, producer_version, implementation_sha256,
                      configuration_sha256);
}

ArchbirdStatus ab_bundle_builder_init_file_manifest(
    AbBundleBuilder *builder, ArchbirdEngine *engine,
    const AbSourceManifest *manifest, const AbManifestFile *file,
    const uint8_t source_manifest_sha256[32], const char *producer_name,
    const char *producer_version, const uint8_t implementation_sha256[32],
    const uint8_t configuration_sha256[32]) {
  return builder_init(builder, engine, manifest, file, "file",
                      source_manifest_sha256, 0, producer_name,
                      producer_version, implementation_sha256,
                      configuration_sha256);
}

ArchbirdStatus ab_bundle_builder_init_file_sources(
    AbBundleBuilder *builder, ArchbirdEngine *engine,
    const AbSourceManifest *manifest, const AbManifestFile *file,
    const AbManifestFile *const *inputs, size_t input_count,
    const char *producer_name, const char *producer_version,
    const uint8_t implementation_sha256[32],
    const uint8_t configuration_sha256[32]) {
  AbProviderInput *expanded;
  size_t index;
  ArchbirdStatus status;
  if (!inputs || !input_count)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_bundle_builder_init_file(
      builder, engine, manifest, file, producer_name, producer_version,
      implementation_sha256, configuration_sha256);
  if (status != ARCHBIRD_OK)
    return status;
  ab_string_free(engine, &builder->bundle.inputs[0].project);
  ab_string_free(engine, &builder->bundle.inputs[0].path);
  ab_free(engine, builder->bundle.inputs);
  builder->bundle.inputs = NULL;
  builder->bundle.input_count = 0;
  if (input_count > engine->options.max_values ||
      input_count > SIZE_MAX / sizeof(*expanded)) {
    ab_bundle_builder_abort(builder);
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "provider source-input limit exceeded");
  }
  expanded =
      (AbProviderInput *)ab_calloc(engine, input_count, sizeof(*expanded));
  if (!expanded) {
    ab_bundle_builder_abort(builder);
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory storing provider source inputs");
  }
  builder->bundle.inputs = expanded;
  builder->bundle.input_count = input_count;
  for (index = 0; index < input_count; index++) {
    const AbManifestFile *input = inputs[index];
    if (!input || (index && ab_string_compare(&inputs[index - 1]->path,
                                              &input->path) >= 0)) {
      ab_bundle_builder_abort(builder);
      return archbird_error_set(
          engine, ARCHBIRD_INVALID_ARGUMENT, ARCHBIRD_NO_OFFSET,
          "provider source inputs must be unique and sorted");
    }
    status = ab_string_copy(engine, &expanded[index].project,
                            manifest->project.data, manifest->project.length);
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(engine, &expanded[index].path, input->path.data,
                              input->path.length);
    if (status != ARCHBIRD_OK) {
      ab_bundle_builder_abort(builder);
      return status;
    }
    expanded[index].has_path = 1;
    memcpy(expanded[index].source_sha256, input->sha256, 32);
    expanded[index].has_source_sha256 = 1;
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_bundle_builder_init_project(
    AbBundleBuilder *builder, ArchbirdEngine *engine,
    const AbSourceManifest *manifest, const uint8_t source_manifest_sha256[32],
    const char *producer_name, const char *producer_version,
    const uint8_t implementation_sha256[32],
    const uint8_t configuration_sha256[32]) {
  return builder_init(builder, engine, manifest, NULL, "project",
                      source_manifest_sha256, 0, producer_name,
                      producer_version, implementation_sha256,
                      configuration_sha256);
}

ArchbirdStatus ab_bundle_builder_set_runtime(AbBundleBuilder *builder,
                                             const char *runtime) {
  ArchbirdStatus status;
  if (!builder || !builder->engine || !runtime || !runtime[0])
    return ARCHBIRD_INVALID_ARGUMENT;
  if (builder->bundle.producer.runtime.data)
    return archbird_error_set(builder->engine, ARCHBIRD_CONFLICT,
                              ARCHBIRD_NO_OFFSET,
                              "provider runtime was already supplied");
  status =
      copy_literal(builder->engine, &builder->bundle.producer.runtime, runtime);
  if (status == ARCHBIRD_OK)
    builder->bundle.producer.has_runtime = 1;
  return status;
}

ArchbirdStatus ab_bundle_builder_add_capability(AbBundleBuilder *builder,
                                                const char *domain,
                                                const char *coverage,
                                                const char *claim,
                                                const char *boundary) {
  AbCapability *resized;
  AbCapability *capability;
  ArchbirdStatus status;
  if (!builder || !builder->engine || !domain || !coverage || !claim)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (builder->bundle.capability_count == builder->capability_capacity) {
    size_t capacity =
        builder->capability_capacity ? builder->capability_capacity * 2 : 8;
    resized = (AbCapability *)ab_realloc(builder->engine,
                                         builder->bundle.capabilities,
                                         capacity * sizeof(*resized));
    if (!resized)
      return archbird_error_set(builder->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory adding provider capability");
    builder->bundle.capabilities = resized;
    builder->capability_capacity = capacity;
  }
  capability =
      &builder->bundle.capabilities[builder->bundle.capability_count++];
  memset(capability, 0, sizeof(*capability));
  status = copy_literal(builder->engine, &capability->domain, domain);
  if (status == ARCHBIRD_OK)
    status = copy_literal(builder->engine, &capability->coverage, coverage);
  capability->claims.items =
      (AbString *)ab_calloc(builder->engine, 1, sizeof(AbString));
  capability->claims.count = 1;
  if (status == ARCHBIRD_OK && !capability->claims.items)
    status = archbird_error_set(builder->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory adding extraction claim");
  if (status == ARCHBIRD_OK)
    status = copy_literal(builder->engine, &capability->claims.items[0], claim);
  if (status == ARCHBIRD_OK && boundary) {
    status = copy_literal(builder->engine, &capability->boundary, boundary);
    if (status == ARCHBIRD_OK)
      capability->has_boundary = 1;
  }
  return status;
}

ArchbirdStatus ab_bundle_builder_add_fact_at(
    AbBundleBuilder *builder, const AbManifestFile *file, const char *domain,
    const char *kind, const char *claim, size_t start, size_t end,
    const uint8_t *key, size_t key_length, const uint8_t *name,
    size_t name_length, AbFact **out_fact) {
  AbFact *resized;
  AbFact *fact;
  ArchbirdStatus status;
  if (!builder || !builder->engine || !file || !domain || !kind || !claim ||
      (!key && key_length) || (!name && name_length) || !out_fact)
    return ARCHBIRD_INVALID_ARGUMENT;
  *out_fact = NULL;
  if (builder->bundle.fact_count >= builder->engine->options.max_facts)
    return archbird_error_set(builder->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "provider fact limit exceeded");
  if (builder->bundle.fact_count == builder->fact_capacity) {
    size_t capacity = builder->fact_capacity ? builder->fact_capacity * 2 : 64;
    if (capacity > builder->engine->options.max_facts)
      capacity = builder->engine->options.max_facts;
    resized = (AbFact *)ab_realloc(builder->engine, builder->bundle.facts,
                                   capacity * sizeof(*builder->bundle.facts));
    if (!resized)
      return archbird_error_set(builder->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory adding provider fact");
    builder->bundle.facts = resized;
    builder->fact_capacity = capacity;
  }
  fact = &builder->bundle.facts[builder->bundle.fact_count++];
  memset(fact, 0, sizeof(*fact));
#define COPY(expression)                                                       \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      return status;                                                           \
  } while (0)
  COPY(copy_literal(builder->engine, &fact->domain, domain));
  COPY(copy_literal(builder->engine, &fact->kind, kind));
  COPY(copy_literal(builder->engine, &fact->claim, claim));
  COPY(ab_string_copy(builder->engine, &fact->project,
                      builder->bundle.subject.project.data,
                      builder->bundle.subject.project.length));
  COPY(ab_string_copy(builder->engine, &fact->path, file->path.data,
                      file->path.length));
  fact->span_start = start;
  fact->span_end = end;
  fact->correlate_by_span = start < end;
  COPY(ab_string_copy(builder->engine, &fact->key, (const char *)key,
                      key_length));
  if (name) {
    COPY(ab_string_copy(builder->engine, &fact->name, (const char *)name,
                        name_length));
    fact->has_name = 1;
  }
  COPY(fact_make_id(builder->engine, fact));
#undef COPY
  *out_fact = fact;
  return ARCHBIRD_OK;
}

ArchbirdStatus
ab_bundle_builder_add_fact(AbBundleBuilder *builder, const char *domain,
                           const char *kind, const char *claim, size_t start,
                           size_t end, const uint8_t *key, size_t key_length,
                           const uint8_t *name, size_t name_length,
                           AbFact **out_fact) {
  AbManifestFile file;
  if (!builder || !builder->bundle.subject.has_path)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(&file, 0, sizeof(file));
  file.path = builder->bundle.subject.path;
  return ab_bundle_builder_add_fact_at(builder, &file, domain, kind, claim,
                                       start, end, key, key_length, name,
                                       name_length, out_fact);
}

void ab_fact_set_keyed_correlation(AbFact *fact) {
  if (fact)
    fact->correlate_by_span = 0;
}

ArchbirdStatus ab_fact_set_resolution(ArchbirdEngine *engine, AbFact *fact,
                                      const char *state,
                                      const AbString *targets,
                                      size_t target_count, const char *reason) {
  size_t index;
  ArchbirdStatus status;
  if (!engine || !fact || !state || (target_count && !targets) ||
      fact->has_resolution)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = copy_literal(engine, &fact->resolution.state, state);
  if (status != ARCHBIRD_OK)
    return status;
  if (target_count) {
    fact->resolution.targets.items = (AbString *)ab_calloc(
        engine, target_count, sizeof(*fact->resolution.targets.items));
    if (!fact->resolution.targets.items)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory adding fact resolution");
    fact->resolution.targets.count = target_count;
    for (index = 0; index < target_count; index++) {
      status = ab_string_copy(engine, &fact->resolution.targets.items[index],
                              targets[index].data, targets[index].length);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  if (reason) {
    status = copy_literal(engine, &fact->resolution.reason, reason);
    if (status != ARCHBIRD_OK)
      return status;
    fact->resolution.has_reason = 1;
  }
  fact->has_resolution = 1;
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_attribute(ArchbirdEngine *engine, AbFact *fact,
                                    const char *name,
                                    AbObjectField **out_field) {
  AbObjectField *resized;
  AbObjectField *field = NULL;
  resized = (AbObjectField *)ab_realloc(engine, fact->attributes,
                                        (fact->attribute_count + 1) *
                                            sizeof(*fact->attributes));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory adding fact attribute");
  fact->attributes = resized;
  field = &fact->attributes[fact->attribute_count++];
  memset(field, 0, sizeof(*field));
  *out_field = field;
  return copy_literal(engine, &field->name, name);
}

ArchbirdStatus ab_fact_add_string_attribute(ArchbirdEngine *engine,
                                            AbFact *fact, const char *name,
                                            const uint8_t *value,
                                            size_t value_length) {
  AbObjectField *field = NULL;
  ArchbirdStatus status;
  if (!engine || !fact || !name || (!value && value_length))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = add_attribute(engine, fact, name, &field);
  if (status != ARCHBIRD_OK)
    return status;
  field->value.kind = AB_VALUE_STRING;
  return ab_string_copy(engine, &field->value.as.text, (const char *)value,
                        value_length);
}

ArchbirdStatus ab_fact_add_u64_attribute(ArchbirdEngine *engine, AbFact *fact,
                                         const char *name, uint64_t value) {
  AbObjectField *field = NULL;
  char bytes[32];
  int length;
  ArchbirdStatus status;
  if (!engine || !fact || !name)
    return ARCHBIRD_INVALID_ARGUMENT;
  length = snprintf(bytes, sizeof(bytes), "%" PRIu64, value);
  if (length < 0 || (size_t)length >= sizeof(bytes))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "failed to encode fact integer");
  status = add_attribute(engine, fact, name, &field);
  if (status != ARCHBIRD_OK)
    return status;
  field->value.kind = AB_VALUE_INTEGER;
  return ab_string_copy(engine, &field->value.as.text, bytes, (size_t)length);
}

ArchbirdStatus
ab_bundle_builder_add_diagnostic(AbBundleBuilder *builder, const char *severity,
                                 const char *code, const char *message,
                                 size_t start, size_t end, int has_span) {
  AbDiagnostic *resized;
  AbDiagnostic *diagnostic;
  ArchbirdStatus status;
  if (!builder || !builder->engine || !severity || !code || !message ||
      !severity[0] || !code[0] || !message[0] || (has_span && end < start))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (builder->bundle.diagnostic_count >= builder->engine->options.max_values)
    return archbird_error_set(builder->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "provider diagnostic limit exceeded");
  resized = (AbDiagnostic *)ab_realloc(
      builder->engine, builder->bundle.diagnostics,
      (builder->bundle.diagnostic_count + 1) * sizeof(*resized));
  if (!resized)
    return archbird_error_set(builder->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory adding provider diagnostic");
  builder->bundle.diagnostics = resized;
  diagnostic = &builder->bundle.diagnostics[builder->bundle.diagnostic_count++];
  memset(diagnostic, 0, sizeof(*diagnostic));
  status = copy_literal(builder->engine, &diagnostic->severity, severity);
  if (status == ARCHBIRD_OK)
    status = copy_literal(builder->engine, &diagnostic->code, code);
  if (status == ARCHBIRD_OK)
    status = copy_literal(builder->engine, &diagnostic->message, message);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(builder->engine, &diagnostic->project,
                            builder->bundle.subject.project.data,
                            builder->bundle.subject.project.length);
  if (status == ARCHBIRD_OK)
    diagnostic->has_project = 1;
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(builder->engine, &diagnostic->path,
                            builder->bundle.subject.path.data,
                            builder->bundle.subject.path.length);
  if (status == ARCHBIRD_OK)
    diagnostic->has_path = 1;
  if (status == ARCHBIRD_OK && has_span) {
    diagnostic->span_start = start;
    diagnostic->span_end = end;
    diagnostic->has_span = 1;
  }
  return status;
}

static int capability_compare(const void *left_raw, const void *right_raw) {
  const AbCapability *left = (const AbCapability *)left_raw;
  const AbCapability *right = (const AbCapability *)right_raw;
  return ab_string_compare(&left->domain, &right->domain);
}

static int fact_compare(const void *left_raw, const void *right_raw) {
  const AbFact *left = (const AbFact *)left_raw;
  const AbFact *right = (const AbFact *)right_raw;
  return ab_string_compare(&left->id, &right->id);
}

static int attribute_compare(const void *left_raw, const void *right_raw) {
  const AbObjectField *left = (const AbObjectField *)left_raw;
  const AbObjectField *right = (const AbObjectField *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static int diagnostic_compare(const void *left_raw, const void *right_raw) {
  const AbDiagnostic *left = (const AbDiagnostic *)left_raw;
  const AbDiagnostic *right = (const AbDiagnostic *)right_raw;
  const AbString *left_strings[] = {&left->code, &left->project, &left->path};
  const AbString *right_strings[] = {&right->code, &right->project,
                                     &right->path};
  size_t index;
  int compared;
  for (index = 0; index < sizeof(left_strings) / sizeof(left_strings[0]);
       index++) {
    compared = ab_string_compare(left_strings[index], right_strings[index]);
    if (compared != 0)
      return compared;
  }
  if (left->span_start != right->span_start)
    return left->span_start < right->span_start ? -1 : 1;
  if (left->span_end != right->span_end)
    return left->span_end < right->span_end ? -1 : 1;
  compared = ab_string_compare(&left->severity, &right->severity);
  if (compared != 0)
    return compared;
  return ab_string_compare(&left->message, &right->message);
}

ArchbirdStatus ab_bundle_builder_finish(AbBundleBuilder *builder,
                                        AbProviderBundle *out_bundle) {
  size_t index;
  size_t write_index;
  if (!builder || !builder->engine || !out_bundle)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (builder->bundle.capability_count > 1)
    qsort(builder->bundle.capabilities, builder->bundle.capability_count,
          sizeof(*builder->bundle.capabilities), capability_compare);
  for (index = 0; index < builder->bundle.fact_count; index++) {
    AbFact *fact = &builder->bundle.facts[index];
    if (fact->attribute_count > 1)
      qsort(fact->attributes, fact->attribute_count, sizeof(*fact->attributes),
            attribute_compare);
  }
  if (builder->bundle.fact_count > 1)
    qsort(builder->bundle.facts, builder->bundle.fact_count,
          sizeof(*builder->bundle.facts), fact_compare);
  if (builder->bundle.diagnostic_count > 1)
    qsort(builder->bundle.diagnostics, builder->bundle.diagnostic_count,
          sizeof(*builder->bundle.diagnostics), diagnostic_compare);
  write_index = 0;
  for (index = 0; index < builder->bundle.fact_count; index++) {
    AbFact *fact = &builder->bundle.facts[index];
    if (write_index > 0 &&
        ab_string_equal(&builder->bundle.facts[write_index - 1].id,
                        &fact->id)) {
      if (!ab_fact_equal(&builder->bundle.facts[write_index - 1], fact))
        return archbird_error_set(
            builder->engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
            "built-in provider emitted conflicting facts with one identity");
      ab_fact_free(builder->engine, fact);
      continue;
    }
    if (write_index != index) {
      builder->bundle.facts[write_index] = *fact;
      memset(fact, 0, sizeof(*fact));
    }
    write_index++;
  }
  builder->bundle.fact_count = write_index;
  *out_bundle = builder->bundle;
  memset(builder, 0, sizeof(*builder));
  return ARCHBIRD_OK;
}
