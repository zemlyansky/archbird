#include "map_internal.h"

#include "json_value.h"
#include "sha256.h"

#include <string.h>

#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

static int string_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static int map_fact_domain(const AbString *domain) {
  return string_is(domain, "constant-memberships") ||
         string_is(domain, "constant-values") ||
         string_is(domain, "macro-invocations");
}

static ArchbirdStatus render_string(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value->data, value->length);
}

static ArchbirdStatus render_attributes(AbBuffer *buffer, const AbFact *fact) {
  size_t index;
  RENDER_TRY(ab_buffer_literal(buffer, "{"));
  for (index = 0; index < fact->attribute_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(buffer, ","));
    RENDER_TRY(render_string(buffer, &fact->attributes[index].name));
    RENDER_TRY(ab_buffer_literal(buffer, ":"));
    RENDER_TRY(ab_value_render(buffer, &fact->attributes[index].value));
  }
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_fact(AbBuffer *buffer, const AbFact *fact,
                                  const AbProviderBundle *provider) {
  char implementation_sha256[65];
  archbird_sha256_hex(provider->producer.implementation_sha256,
                      implementation_sha256);
  RENDER_TRY(ab_buffer_literal(buffer, "{\"attributes\":"));
  RENDER_TRY(render_attributes(buffer, fact));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"claim\":"));
  RENDER_TRY(render_string(buffer, &fact->claim));
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
  RENDER_TRY(
      ab_buffer_literal(buffer, ",\"provider\":{\"implementation_sha256\":"));
  RENDER_TRY(ab_buffer_json_string(buffer, implementation_sha256, 64));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"name\":"));
  RENDER_TRY(render_string(buffer, &provider->producer.name));
  RENDER_TRY(ab_buffer_literal(buffer, "},\"span\":{\"end\":"));
  RENDER_TRY(ab_buffer_u64(buffer, fact->span_end));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"start\":"));
  RENDER_TRY(ab_buffer_u64(buffer, fact->span_start));
  return ab_buffer_literal(buffer, "}}");
}

ArchbirdStatus ab_map_render_facts(AbBuffer *buffer,
                                   const ArchbirdProject *project) {
  size_t index;
  size_t rendered = 0;
  RENDER_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < ab_project_merged_fact_count(project); index++) {
    const AbFact *fact = ab_project_merged_fact(project, index);
    const AbProviderBundle *provider;
    if (!map_fact_domain(&fact->domain))
      continue;
    provider = ab_project_merged_fact_provider(project, index);
    if (!provider)
      return archbird_error_set(buffer->engine, ARCHBIRD_CONFLICT,
                                ARCHBIRD_NO_OFFSET,
                                "Map fact has no canonical provider");
    if (rendered++)
      RENDER_TRY(ab_buffer_literal(buffer, ","));
    RENDER_TRY(render_fact(buffer, fact, provider));
  }
  return ab_buffer_literal(buffer, "]");
}

#undef RENDER_TRY
