#include "evidence_render.h"

#include "json_number.h"
#include "sha256.h"

#include <string.h>

#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    ArchbirdStatus render_status__ = (expression);                             \
    if (render_status__ != ARCHBIRD_OK)                                        \
      return render_status__;                                                  \
  } while (0)

static ArchbirdStatus render_string(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value->data, value->length);
}

static ArchbirdStatus render_sha(AbBuffer *buffer, const uint8_t digest[32]) {
  char hex[65];
  archbird_sha256_hex(digest, hex);
  return ab_buffer_json_string(buffer, hex, 64);
}

static ArchbirdStatus render_string_array(AbBuffer *buffer,
                                          const AbStringArray *array) {
  size_t index;
  RENDER_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < array->count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(buffer, ","));
    RENDER_TRY(render_string(buffer, &array->items[index]));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_value(AbBuffer *buffer, const AbValue *value) {
  size_t index;
  switch (value->kind) {
  case AB_VALUE_NULL:
    return ab_buffer_literal(buffer, "null");
  case AB_VALUE_BOOL:
    return ab_buffer_literal(buffer, value->as.boolean ? "true" : "false");
  case AB_VALUE_INTEGER:
    return ab_buffer_append(buffer, value->as.text.data, value->as.text.length);
  case AB_VALUE_REAL: {
    char number[AB_JSON_REAL_BUFFER_SIZE];
    size_t length;
    ArchbirdStatus status =
        ab_json_real_format(buffer->engine, value->as.real, number, &length);
    return status == ARCHBIRD_OK ? ab_buffer_append(buffer, number, length)
                                 : status;
  }
  case AB_VALUE_STRING:
    return render_string(buffer, &value->as.text);
  case AB_VALUE_ARRAY:
    RENDER_TRY(ab_buffer_literal(buffer, "["));
    for (index = 0; index < value->as.array.count; index++) {
      if (index)
        RENDER_TRY(ab_buffer_literal(buffer, ","));
      RENDER_TRY(render_value(buffer, &value->as.array.items[index]));
    }
    return ab_buffer_literal(buffer, "]");
  case AB_VALUE_OBJECT:
    RENDER_TRY(ab_buffer_literal(buffer, "{"));
    for (index = 0; index < value->as.object.count; index++) {
      if (index)
        RENDER_TRY(ab_buffer_literal(buffer, ","));
      RENDER_TRY(render_string(buffer, &value->as.object.fields[index].name));
      RENDER_TRY(ab_buffer_literal(buffer, ":"));
      RENDER_TRY(render_value(buffer, &value->as.object.fields[index].value));
    }
    return ab_buffer_literal(buffer, "}");
  }
  return archbird_error_set(buffer->engine, ARCHBIRD_INVALID_SCHEMA,
                            ARCHBIRD_NO_OFFSET,
                            "unknown typed evidence value kind");
}

static ArchbirdStatus render_subject(AbBuffer *buffer,
                                     const AbSubject *subject) {
  int has_previous = 0;
  RENDER_TRY(ab_buffer_literal(buffer, "{"));
  if (subject->has_name) {
    RENDER_TRY(ab_buffer_literal(buffer, "\"name\":"));
    RENDER_TRY(render_string(buffer, &subject->name));
    has_previous = 1;
  }
  if (subject->has_path) {
    if (has_previous)
      RENDER_TRY(ab_buffer_literal(buffer, ","));
    RENDER_TRY(ab_buffer_literal(buffer, "\"path\":"));
    RENDER_TRY(render_string(buffer, &subject->path));
    has_previous = 1;
  }
  if (subject->has_project) {
    if (has_previous)
      RENDER_TRY(ab_buffer_literal(buffer, ","));
    RENDER_TRY(ab_buffer_literal(buffer, "\"project\":"));
    RENDER_TRY(render_string(buffer, &subject->project));
    has_previous = 1;
  }
  if (has_previous)
    RENDER_TRY(ab_buffer_literal(buffer, ","));
  RENDER_TRY(ab_buffer_literal(buffer, "\"scope\":"));
  RENDER_TRY(render_string(buffer, &subject->scope));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_producer(AbBuffer *buffer,
                                      const AbProducer *producer) {
  RENDER_TRY(ab_buffer_literal(buffer, "{"));
  if (producer->has_configuration_sha256) {
    RENDER_TRY(ab_buffer_literal(buffer, "\"configuration_sha256\":"));
    RENDER_TRY(render_sha(buffer, producer->configuration_sha256));
    RENDER_TRY(ab_buffer_literal(buffer, ","));
  }
  RENDER_TRY(ab_buffer_literal(buffer, "\"implementation_sha256\":"));
  RENDER_TRY(render_sha(buffer, producer->implementation_sha256));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"name\":"));
  RENDER_TRY(render_string(buffer, &producer->name));
  if (producer->has_runtime) {
    RENDER_TRY(ab_buffer_literal(buffer, ",\"runtime\":"));
    RENDER_TRY(render_string(buffer, &producer->runtime));
  }
  RENDER_TRY(ab_buffer_literal(buffer, ",\"version\":"));
  RENDER_TRY(render_string(buffer, &producer->version));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_capability(AbBuffer *buffer,
                                        const AbCapability *capability) {
  RENDER_TRY(ab_buffer_literal(buffer, "{"));
  if (capability->has_boundary) {
    RENDER_TRY(ab_buffer_literal(buffer, "\"boundary\":"));
    RENDER_TRY(render_string(buffer, &capability->boundary));
    RENDER_TRY(ab_buffer_literal(buffer, ","));
  }
  RENDER_TRY(ab_buffer_literal(buffer, "\"claims\":"));
  RENDER_TRY(render_string_array(buffer, &capability->claims));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"coverage\":"));
  RENDER_TRY(render_string(buffer, &capability->coverage));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"domain\":"));
  RENDER_TRY(render_string(buffer, &capability->domain));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_span(AbBuffer *buffer, size_t start, size_t end) {
  RENDER_TRY(ab_buffer_literal(buffer, "{\"end\":"));
  RENDER_TRY(ab_buffer_u64(buffer, end));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"start\":"));
  RENDER_TRY(ab_buffer_u64(buffer, start));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_fact(AbBuffer *buffer, const AbFact *fact) {
  RENDER_TRY(ab_buffer_literal(buffer, "{"));
  if (fact->attribute_count) {
    size_t index;
    RENDER_TRY(ab_buffer_literal(buffer, "\"attributes\":{"));
    for (index = 0; index < fact->attribute_count; index++) {
      if (index)
        RENDER_TRY(ab_buffer_literal(buffer, ","));
      RENDER_TRY(render_string(buffer, &fact->attributes[index].name));
      RENDER_TRY(ab_buffer_literal(buffer, ":"));
      RENDER_TRY(render_value(buffer, &fact->attributes[index].value));
    }
    RENDER_TRY(ab_buffer_literal(buffer, "},"));
  }
  RENDER_TRY(ab_buffer_literal(buffer, "\"claim\":"));
  RENDER_TRY(render_string(buffer, &fact->claim));
  if (fact->correlate_by_span)
    RENDER_TRY(ab_buffer_literal(buffer, ",\"correlation\":\"span\""));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"domain\":"));
  RENDER_TRY(render_string(buffer, &fact->domain));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"id\":"));
  RENDER_TRY(render_string(buffer, &fact->id));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"key\":"));
  RENDER_TRY(render_string(buffer, &fact->key));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"kind\":"));
  RENDER_TRY(render_string(buffer, &fact->kind));
  if (fact->has_name) {
    RENDER_TRY(ab_buffer_literal(buffer, ",\"name\":"));
    RENDER_TRY(render_string(buffer, &fact->name));
  }
  RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
  RENDER_TRY(render_string(buffer, &fact->path));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"project\":"));
  RENDER_TRY(render_string(buffer, &fact->project));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"span\":"));
  RENDER_TRY(render_span(buffer, fact->span_start, fact->span_end));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_diagnostic(AbBuffer *buffer,
                                        const AbDiagnostic *diagnostic) {
  RENDER_TRY(ab_buffer_literal(buffer, "{\"code\":"));
  RENDER_TRY(render_string(buffer, &diagnostic->code));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"message\":"));
  RENDER_TRY(render_string(buffer, &diagnostic->message));
  if (diagnostic->has_path) {
    RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
    RENDER_TRY(render_string(buffer, &diagnostic->path));
  }
  if (diagnostic->has_project) {
    RENDER_TRY(ab_buffer_literal(buffer, ",\"project\":"));
    RENDER_TRY(render_string(buffer, &diagnostic->project));
  }
  RENDER_TRY(ab_buffer_literal(buffer, ",\"severity\":"));
  RENDER_TRY(render_string(buffer, &diagnostic->severity));
  if (diagnostic->has_span) {
    RENDER_TRY(ab_buffer_literal(buffer, ",\"span\":"));
    RENDER_TRY(
        render_span(buffer, diagnostic->span_start, diagnostic->span_end));
  }
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_resolution(AbBuffer *buffer, const AbFact *fact) {
  RENDER_TRY(ab_buffer_literal(buffer, "{\"fact_id\":"));
  RENDER_TRY(render_string(buffer, &fact->id));
  if (fact->resolution.has_reason) {
    RENDER_TRY(ab_buffer_literal(buffer, ",\"reason\":"));
    RENDER_TRY(render_string(buffer, &fact->resolution.reason));
  }
  RENDER_TRY(ab_buffer_literal(buffer, ",\"state\":"));
  RENDER_TRY(render_string(buffer, &fact->resolution.state));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"targets\":"));
  RENDER_TRY(render_string_array(buffer, &fact->resolution.targets));
  return ab_buffer_literal(buffer, "}");
}

ArchbirdStatus ab_provider_bundle_render_compact(ArchbirdEngine *engine,
                                                 const AbProviderBundle *bundle,
                                                 AbBuffer *out) {
  size_t index;
  int first;
  if (!engine || !bundle || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(out, engine);
  RENDER_TRY(ab_buffer_literal(
      out, "{\"artifact\":\"archbird-provider-facts\",\"capabilities\":["));
  for (index = 0; index < bundle->capability_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(out, ","));
    RENDER_TRY(render_capability(out, &bundle->capabilities[index]));
  }
  RENDER_TRY(ab_buffer_literal(out, "],\"diagnostics\":["));
  for (index = 0; index < bundle->diagnostic_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(out, ","));
    RENDER_TRY(render_diagnostic(out, &bundle->diagnostics[index]));
  }
  RENDER_TRY(ab_buffer_literal(out, "],\"facts\":["));
  for (index = 0; index < bundle->fact_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(out, ","));
    RENDER_TRY(render_fact(out, &bundle->facts[index]));
  }
  RENDER_TRY(ab_buffer_literal(out, "],\"inputs\":["));
  for (index = 0; index < bundle->input_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(out, ","));
    RENDER_TRY(ab_buffer_literal(out, "{\"project\":"));
    RENDER_TRY(render_string(out, &bundle->inputs[index].project));
    if (bundle->inputs[index].has_source_manifest_sha256) {
      RENDER_TRY(ab_buffer_literal(out, ",\"source_manifest_sha256\":"));
      RENDER_TRY(render_sha(out, bundle->inputs[index].source_manifest_sha256));
    } else {
      RENDER_TRY(ab_buffer_literal(out, ",\"path\":"));
      RENDER_TRY(render_string(out, &bundle->inputs[index].path));
      RENDER_TRY(ab_buffer_literal(out, ",\"source_sha256\":"));
      RENDER_TRY(render_sha(out, bundle->inputs[index].source_sha256));
    }
    RENDER_TRY(ab_buffer_literal(out, "}"));
  }
  RENDER_TRY(ab_buffer_literal(out, "],\"producer\":"));
  RENDER_TRY(render_producer(out, &bundle->producer));
  RENDER_TRY(ab_buffer_literal(out, ",\"provenance\":\"derived\","
                                    "\"resolutions\":["));
  first = 1;
  for (index = 0; index < bundle->fact_count; index++) {
    if (!bundle->facts[index].has_resolution)
      continue;
    if (!first)
      RENDER_TRY(ab_buffer_literal(out, ","));
    RENDER_TRY(render_resolution(out, &bundle->facts[index]));
    first = 0;
  }
  RENDER_TRY(ab_buffer_literal(out, "],\"schema_version\":1,\"subject\":"));
  RENDER_TRY(render_subject(out, &bundle->subject));
  return ab_buffer_literal(out, "}");
}

ArchbirdStatus ab_provider_bundle_digest(ArchbirdEngine *engine,
                                         AbProviderBundle *bundle) {
  AbBuffer buffer = {0};
  ArchbirdStatus status =
      ab_provider_bundle_render_compact(engine, bundle, &buffer);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(buffer.data, buffer.length, bundle->sha256);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(bundle->sha256, bundle->sha256_hex);
  ab_buffer_free(&buffer);
  return status;
}

#undef RENDER_TRY
