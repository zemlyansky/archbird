#include "evidence/project/project_model.h"

#include "base/render_internal.h"
#include "base/sha256.h"
#include "evidence/evidence_render.h"
#include "evidence/project/provider_merge.h"

#include <stdlib.h>
#include <string.h>

static int provider_has_selection(const AbProviderBundle *provider,
                                  const AbSubject *subject,
                                  const AbString *domain) {
  size_t index;
  if (!ab_project_subject_equal(&provider->subject, subject))
    return 0;
  for (index = 0; index < provider->capability_count; index++) {
    int compared =
        ab_string_compare(&provider->capabilities[index].domain, domain);
    if (compared == 0)
      return 1;
    if (compared > 0)
      return 0;
  }
  return 0;
}

static const char *provider_mode_name(ArchbirdProviderMode mode) {
  if (mode == ARCHBIRD_PROVIDER_PRIMARY)
    return "primary";
  if (mode == ARCHBIRD_PROVIDER_AUGMENT)
    return "augment";
  return "audit";
}

static ArchbirdStatus json_string(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value->data, value->length);
}

static ArchbirdStatus json_sha(AbBuffer *buffer, const char *hex) {
  return ab_buffer_json_string(buffer, hex, 64);
}

static ArchbirdStatus render_subject(AbBuffer *buffer,
                                     const AbSubject *subject) {
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      return status;                                                           \
  } while (0)
  RENDER_TRY(ab_buffer_literal(buffer, "{"));
  if (subject->has_name) {
    RENDER_TRY(ab_buffer_literal(buffer, "\"name\":"));
    RENDER_TRY(json_string(buffer, &subject->name));
    if (subject->has_project)
      RENDER_TRY(ab_buffer_literal(buffer, ","));
  }
  if (subject->has_path) {
    RENDER_TRY(ab_buffer_literal(buffer, "\"path\":"));
    RENDER_TRY(json_string(buffer, &subject->path));
    if (subject->has_project)
      RENDER_TRY(ab_buffer_literal(buffer, ","));
  }
  if (subject->has_project) {
    RENDER_TRY(ab_buffer_literal(buffer, "\"project\":"));
    RENDER_TRY(json_string(buffer, &subject->project));
    RENDER_TRY(ab_buffer_literal(buffer, ","));
  }
  RENDER_TRY(ab_buffer_literal(buffer, "\"scope\":"));
  RENDER_TRY(json_string(buffer, &subject->scope));
  RENDER_TRY(ab_buffer_literal(buffer, "}"));
#undef RENDER_TRY
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_provider(AbBuffer *buffer,
                                      const AbProviderBundle *provider) {
  size_t index;
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      return status;                                                           \
  } while (0)
  RENDER_TRY(ab_buffer_literal(buffer, "{\"domains\":["));
  for (index = 0; index < provider->capability_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(buffer, ","));
    RENDER_TRY(json_string(buffer, &provider->capabilities[index].domain));
  }
  RENDER_TRY(ab_buffer_literal(buffer, "],\"facts\":"));
  RENDER_TRY(ab_buffer_u64(buffer, provider->fact_count));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"mode\":"));
  RENDER_TRY(ab_buffer_json_string(buffer, provider_mode_name(provider->mode),
                                   strlen(provider_mode_name(provider->mode))));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"producer\":{"));
  RENDER_TRY(ab_buffer_literal(buffer, "\"configuration_sha256\":"));
  {
    char hex[65];
    archbird_sha256_hex(provider->producer.configuration_sha256, hex);
    RENDER_TRY(json_sha(buffer, hex));
  }
  RENDER_TRY(ab_buffer_literal(buffer, ",\"implementation_sha256\":"));
  {
    char hex[65];
    archbird_sha256_hex(provider->producer.implementation_sha256, hex);
    RENDER_TRY(json_sha(buffer, hex));
  }
  RENDER_TRY(ab_buffer_literal(buffer, ",\"name\":"));
  RENDER_TRY(json_string(buffer, &provider->producer.name));
  if (provider->producer.has_runtime) {
    RENDER_TRY(ab_buffer_literal(buffer, ",\"runtime\":"));
    RENDER_TRY(json_string(buffer, &provider->producer.runtime));
  }
  RENDER_TRY(ab_buffer_literal(buffer, ",\"version\":"));
  RENDER_TRY(json_string(buffer, &provider->producer.version));
  RENDER_TRY(ab_buffer_literal(buffer, "},\"sha256\":"));
  RENDER_TRY(json_sha(buffer, provider->sha256_hex));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"subject\":"));
  RENDER_TRY(render_subject(buffer, &provider->subject));
  RENDER_TRY(ab_buffer_literal(buffer, "}"));
#undef RENDER_TRY
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_domain(AbBuffer *buffer,
                                    const ArchbirdProject *project,
                                    const AbDomainSelection *domain) {
  size_t index;
  int first;
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      return status;                                                           \
  } while (0)
  RENDER_TRY(ab_buffer_literal(buffer, "{\"audits\":["));
  first = 1;
  for (index = 0; index < project->providers.count; index++) {
    const AbProviderBundle *provider = &project->providers.bundles[index];
    if (provider->mode != ARCHBIRD_PROVIDER_AUDIT ||
        !provider_has_selection(provider, domain->subject, domain->name))
      continue;
    if (!first)
      RENDER_TRY(ab_buffer_literal(buffer, ","));
    RENDER_TRY(json_sha(buffer, provider->sha256_hex));
    first = 0;
  }
  RENDER_TRY(ab_buffer_literal(buffer, "],\"augments\":["));
  first = 1;
  for (index = 0; index < project->providers.count; index++) {
    const AbProviderBundle *provider = &project->providers.bundles[index];
    if (provider->mode != ARCHBIRD_PROVIDER_AUGMENT ||
        !provider_has_selection(provider, domain->subject, domain->name))
      continue;
    if (!first)
      RENDER_TRY(ab_buffer_literal(buffer, ","));
    RENDER_TRY(json_sha(buffer, provider->sha256_hex));
    first = 0;
  }
  RENDER_TRY(ab_buffer_literal(buffer, "],\"domain\":"));
  RENDER_TRY(json_string(buffer, domain->name));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"primary\":"));
  if (domain->primary_count == 1)
    RENDER_TRY(json_sha(
        buffer,
        project->providers.bundles[domain->primary_provider].sha256_hex));
  else
    RENDER_TRY(ab_buffer_literal(buffer, "null"));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"primary_count\":"));
  RENDER_TRY(ab_buffer_u64(buffer, domain->primary_count));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"subject\":"));
  RENDER_TRY(render_subject(buffer, domain->subject));
  RENDER_TRY(ab_buffer_literal(buffer, "}"));
#undef RENDER_TRY
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_conflict_fact(AbBuffer *buffer,
                                           const AbFact *fact) {
  size_t index;
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      return status;                                                           \
  } while (0)
  if (!fact)
    return ab_buffer_literal(buffer, "null");
  RENDER_TRY(ab_buffer_literal(buffer, "{"));
  if (fact->attribute_count) {
    RENDER_TRY(ab_buffer_literal(buffer, "\"attributes\":{"));
    for (index = 0; index < fact->attribute_count; index++) {
      if (index)
        RENDER_TRY(ab_buffer_literal(buffer, ","));
      RENDER_TRY(json_string(buffer, &fact->attributes[index].name));
      RENDER_TRY(ab_buffer_literal(buffer, ":"));
      RENDER_TRY(ab_value_render(buffer, &fact->attributes[index].value));
    }
    RENDER_TRY(ab_buffer_literal(buffer, "},"));
  }
  RENDER_TRY(ab_buffer_literal(buffer, "\"claim\":"));
  RENDER_TRY(json_string(buffer, &fact->claim));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"correlation\":\""));
  RENDER_TRY(
      ab_buffer_literal(buffer, fact->correlate_by_span ? "span" : "key"));
  RENDER_TRY(ab_buffer_literal(buffer, "\",\"id\":"));
  RENDER_TRY(json_string(buffer, &fact->id));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"key\":"));
  RENDER_TRY(json_string(buffer, &fact->key));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"kind\":"));
  RENDER_TRY(json_string(buffer, &fact->kind));
  if (fact->has_name) {
    RENDER_TRY(ab_buffer_literal(buffer, ",\"name\":"));
    RENDER_TRY(json_string(buffer, &fact->name));
  }
  RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
  RENDER_TRY(json_string(buffer, &fact->path));
  if (fact->has_resolution) {
    RENDER_TRY(ab_buffer_literal(buffer, ",\"resolution\":{"));
    if (fact->resolution.has_reason) {
      RENDER_TRY(ab_buffer_literal(buffer, "\"reason\":"));
      RENDER_TRY(json_string(buffer, &fact->resolution.reason));
      RENDER_TRY(ab_buffer_literal(buffer, ","));
    }
    RENDER_TRY(ab_buffer_literal(buffer, "\"state\":"));
    RENDER_TRY(json_string(buffer, &fact->resolution.state));
    RENDER_TRY(ab_buffer_literal(buffer, ",\"targets\":["));
    for (index = 0; index < fact->resolution.targets.count; index++) {
      if (index)
        RENDER_TRY(ab_buffer_literal(buffer, ","));
      RENDER_TRY(json_string(buffer, &fact->resolution.targets.items[index]));
    }
    RENDER_TRY(ab_buffer_literal(buffer, "]}"));
  }
  RENDER_TRY(ab_buffer_literal(buffer, ",\"span\":{\"end\":"));
  RENDER_TRY(ab_buffer_u64(buffer, fact->span_end));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"start\":"));
  RENDER_TRY(ab_buffer_u64(buffer, fact->span_start));
  RENDER_TRY(ab_buffer_literal(buffer, "}}"));
#undef RENDER_TRY
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_conflict(AbBuffer *buffer,
                                      const ArchbirdProject *project,
                                      const AbMergeConflict *conflict) {
  const AbFact *witness =
      conflict->left_fact ? conflict->left_fact : conflict->right_fact;
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      return status;                                                           \
  } while (0)
  RENDER_TRY(ab_buffer_literal(buffer, "{\"domain\":"));
  RENDER_TRY(json_string(buffer, conflict->domain));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"left_provider\":"));
  if (conflict->left_provider == SIZE_MAX)
    RENDER_TRY(ab_buffer_literal(buffer, "null"));
  else
    RENDER_TRY(json_sha(
        buffer,
        project->providers.bundles[conflict->left_provider].sha256_hex));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"left_fact\":"));
  RENDER_TRY(render_conflict_fact(buffer, conflict->left_fact));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"reason\":"));
  RENDER_TRY(ab_buffer_json_string(buffer, conflict->reason,
                                   strlen(conflict->reason)));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"right_provider\":"));
  if (conflict->right_provider == SIZE_MAX)
    RENDER_TRY(ab_buffer_literal(buffer, "null"));
  else
    RENDER_TRY(json_sha(
        buffer,
        project->providers.bundles[conflict->right_provider].sha256_hex));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"right_fact\":"));
  RENDER_TRY(render_conflict_fact(buffer, conflict->right_fact));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"subject\":"));
  RENDER_TRY(render_subject(buffer, conflict->subject));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"witness\":"));
  if (!witness) {
    RENDER_TRY(ab_buffer_literal(buffer, "null"));
  } else {
    RENDER_TRY(ab_buffer_literal(buffer, "{\"end\":"));
    RENDER_TRY(ab_buffer_u64(buffer, witness->span_end));
    RENDER_TRY(ab_buffer_literal(buffer, ",\"key\":"));
    RENDER_TRY(json_string(buffer, &witness->key));
    RENDER_TRY(ab_buffer_literal(buffer, ",\"kind\":"));
    RENDER_TRY(json_string(buffer, &witness->kind));
    RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
    RENDER_TRY(json_string(buffer, &witness->path));
    RENDER_TRY(ab_buffer_literal(buffer, ",\"start\":"));
    RENDER_TRY(ab_buffer_u64(buffer, witness->span_start));
    RENDER_TRY(ab_buffer_literal(buffer, "}"));
  }
  RENDER_TRY(ab_buffer_literal(buffer, "}"));
#undef RENDER_TRY
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_occurrence(AbBuffer *buffer,
                                        const ArchbirdProject *project,
                                        const AbMergedFact *merged) {
  const AbFact *fact = merged->value;
  size_t index;
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      return status;                                                           \
  } while (0)
  RENDER_TRY(ab_buffer_literal(buffer, "{\"canonical\":{"));
  RENDER_TRY(ab_buffer_literal(buffer, "\"claim\":"));
  RENDER_TRY(json_string(buffer, &fact->claim));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"fact_id\":"));
  RENDER_TRY(json_string(buffer, &fact->id));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"provider\":"));
  RENDER_TRY(json_sha(
      buffer, project->providers.bundles[merged->provider_index].sha256_hex));
  RENDER_TRY(ab_buffer_literal(buffer, "},\"contributors\":["));
  for (index = 0; index < merged->contributor_count; index++) {
    const AbFactReference *contributor = &merged->contributors[index];
    if (index)
      RENDER_TRY(ab_buffer_literal(buffer, ","));
    RENDER_TRY(ab_buffer_literal(buffer, "{\"claim\":"));
    RENDER_TRY(json_string(buffer, &contributor->fact->claim));
    RENDER_TRY(ab_buffer_literal(buffer, ",\"fact_id\":"));
    RENDER_TRY(json_string(buffer, &contributor->fact->id));
    RENDER_TRY(ab_buffer_literal(buffer, ",\"provider\":"));
    RENDER_TRY(json_sha(
        buffer,
        project->providers.bundles[contributor->provider_index].sha256_hex));
    RENDER_TRY(ab_buffer_literal(buffer, "}"));
  }
  RENDER_TRY(ab_buffer_literal(buffer, "],\"domain\":"));
  RENDER_TRY(json_string(buffer, &fact->domain));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"end\":"));
  RENDER_TRY(ab_buffer_u64(buffer, fact->span_end));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"key\":"));
  RENDER_TRY(json_string(buffer, &fact->key));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"kind\":"));
  RENDER_TRY(json_string(buffer, &fact->kind));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
  RENDER_TRY(json_string(buffer, &fact->path));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"start\":"));
  RENDER_TRY(ab_buffer_u64(buffer, fact->span_start));
  RENDER_TRY(ab_buffer_literal(buffer, "}"));
#undef RENDER_TRY
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_variation(AbBuffer *buffer,
                                       const ArchbirdProject *project,
                                       const AbMergeVariation *variation) {
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      return status;                                                           \
  } while (0)
  RENDER_TRY(ab_buffer_literal(buffer, "{\"alternate_provider\":"));
  RENDER_TRY(json_sha(
      buffer,
      project->providers.bundles[variation->alternate_provider].sha256_hex));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"alternate_value\":"));
  RENDER_TRY(ab_value_render(buffer, &variation->alternate_attribute->value));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"attribute\":"));
  RENDER_TRY(json_string(buffer, &variation->canonical_attribute->name));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"canonical_provider\":"));
  RENDER_TRY(json_sha(
      buffer,
      project->providers.bundles[variation->canonical_provider].sha256_hex));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"canonical_value\":"));
  RENDER_TRY(ab_value_render(buffer, &variation->canonical_attribute->value));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"domain\":"));
  RENDER_TRY(json_string(buffer, variation->domain));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"subject\":"));
  RENDER_TRY(render_subject(buffer, variation->subject));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"witness\":{\"end\":"));
  RENDER_TRY(ab_buffer_u64(buffer, variation->canonical_fact->span_end));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"key\":"));
  RENDER_TRY(json_string(buffer, &variation->canonical_fact->key));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"kind\":"));
  RENDER_TRY(json_string(buffer, &variation->canonical_fact->kind));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
  RENDER_TRY(json_string(buffer, &variation->canonical_fact->path));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"start\":"));
  RENDER_TRY(ab_buffer_u64(buffer, variation->canonical_fact->span_start));
  RENDER_TRY(ab_buffer_literal(buffer, "}}"));
#undef RENDER_TRY
  return ARCHBIRD_OK;
}

ArchbirdStatus archbird_project_render_merge_ledger(
    ArchbirdEngine *engine, const ArchbirdProject *project, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data) {
  AbDomainSelection *domains = NULL;
  size_t domain_count = 0;
  AbBuffer buffer;
  size_t index;
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      goto done;                                                               \
  } while (0)
  if (!engine || !project || !write_fn || !project->merge.finalized ||
      !ab_json_flags_valid(json_flags))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_provider_collect_domains(engine, &project->providers, &domains,
                                       &domain_count);
  if (status != ARCHBIRD_OK)
    return status;
  ab_buffer_init(&buffer, engine);
  RENDER_TRY(ab_buffer_literal(
      &buffer, "{\"artifact\":\"archbird-provider-merge\",\"conflicts\":["));
  for (index = 0; index < project->merge.conflict_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(&buffer, ","));
    RENDER_TRY(
        render_conflict(&buffer, project, &project->merge.conflicts[index]));
  }
  RENDER_TRY(ab_buffer_literal(&buffer, "],\"occurrences\":["));
  for (index = 0; index < project->merge.fact_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(&buffer, ","));
    RENDER_TRY(
        render_occurrence(&buffer, project, &project->merge.facts[index]));
  }
  RENDER_TRY(ab_buffer_literal(&buffer, "],\"selections\":["));
  for (index = 0; index < domain_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(&buffer, ","));
    RENDER_TRY(render_domain(&buffer, project, &domains[index]));
  }
  RENDER_TRY(ab_buffer_literal(&buffer, "],\"project\":"));
  RENDER_TRY(json_string(&buffer, &project->sources.manifest.project));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"providers\":["));
  for (index = 0; index < project->providers.count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(&buffer, ","));
    RENDER_TRY(render_provider(&buffer, &project->providers.bundles[index]));
  }
  RENDER_TRY(ab_buffer_literal(
      &buffer, "],\"schema_version\":5,\"source_manifest_sha256\":"));
  RENDER_TRY(json_sha(&buffer, project->sources.manifest_sha256_hex));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"summary\":{"));
  RENDER_TRY(ab_buffer_literal(&buffer, "\"audit_differences\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge.summary.audit_differences));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"audit_matches\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge.summary.audit_matches));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"conflicts\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge.summary.conflicts));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"contributed\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge.summary.contributed));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"deduplicated\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge.summary.deduplicated));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"selections\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge.summary.selections));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"enriched\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge.summary.enriched));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"providers\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge.summary.providers));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"selected_facts\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge.summary.selected_facts));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"variations\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge.summary.variations));
  RENDER_TRY(ab_buffer_literal(&buffer, "},\"variations\":["));
  for (index = 0; index < project->merge.variation_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(&buffer, ","));
    RENDER_TRY(
        render_variation(&buffer, project, &project->merge.variations[index]));
  }
  RENDER_TRY(ab_buffer_literal(&buffer, "]}"));
  status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                      json_flags, write_fn, user_data);
done:
  ab_buffer_free(&buffer);
  ab_free(engine, domains);
#undef RENDER_TRY
  return status;
}

static int provider_has_conflict(const ArchbirdProject *project,
                                 size_t provider_index) {
  size_t index;
  for (index = 0; index < project->merge.conflict_count; index++) {
    const AbMergeConflict *conflict = &project->merge.conflicts[index];
    if (conflict->left_provider == provider_index ||
        conflict->right_provider == provider_index)
      return 1;
    if ((conflict->left_provider == SIZE_MAX ||
         conflict->right_provider == SIZE_MAX) &&
        project->providers.bundles[provider_index].mode ==
            ARCHBIRD_PROVIDER_PRIMARY &&
        provider_has_selection(&project->providers.bundles[provider_index],
                               conflict->subject, conflict->domain))
      return 1;
  }
  return 0;
}

ArchbirdStatus archbird_project_render_merge_conflicts(
    ArchbirdEngine *engine, const ArchbirdProject *project, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data) {
  AbBuffer buffer;
  size_t index;
  size_t provider_count = 0;
  int first;
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      goto done;                                                               \
  } while (0)
  if (!engine || !project || !write_fn || !project->merge.finalized ||
      !ab_json_flags_valid(json_flags))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&buffer, engine);
  RENDER_TRY(ab_buffer_literal(
      &buffer,
      "{\"artifact\":\"archbird-provider-merge-conflicts\",\"conflicts\":["));
  for (index = 0; index < project->merge.conflict_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(&buffer, ","));
    RENDER_TRY(
        render_conflict(&buffer, project, &project->merge.conflicts[index]));
  }
  RENDER_TRY(ab_buffer_literal(&buffer, "],\"project\":"));
  RENDER_TRY(json_string(&buffer, &project->sources.manifest.project));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"providers\":["));
  first = 1;
  for (index = 0; index < project->providers.count; index++) {
    if (!provider_has_conflict(project, index))
      continue;
    if (!first)
      RENDER_TRY(ab_buffer_literal(&buffer, ","));
    RENDER_TRY(render_provider(&buffer, &project->providers.bundles[index]));
    first = 0;
    provider_count++;
  }
  RENDER_TRY(ab_buffer_literal(
      &buffer, "],\"schema_version\":1,\"source_manifest_sha256\":"));
  RENDER_TRY(json_sha(&buffer, project->sources.manifest_sha256_hex));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"summary\":{\"conflicts\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge.summary.conflicts));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"providers_in_conflicts\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, provider_count));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"providers_total\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge.summary.providers));
  RENDER_TRY(ab_buffer_literal(&buffer, "}}"));
  status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                      json_flags, write_fn, user_data);
done:
  ab_buffer_free(&buffer);
#undef RENDER_TRY
  return status;
}
