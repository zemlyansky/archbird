#include "evidence/project/provider_merge.h"

#include "evidence/project/provider_store.h"

#include <stdlib.h>
#include <string.h>

static ArchbirdStatus merged_fact_add_contributor(ArchbirdEngine *engine,
                                                  AbMergedFact *merged,
                                                  size_t capacity,
                                                  AbFactReference contributor) {
  if (merged->contributors == &merged->primary_contributor) {
    AbFactReference *contributors =
        (AbFactReference *)ab_calloc(engine, capacity, sizeof(*contributors));
    if (!contributors)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing provider witnesses");
    contributors[0] = merged->primary_contributor;
    merged->contributors = contributors;
  }
  merged->contributors[merged->contributor_count++] = contributor;
  return ARCHBIRD_OK;
}

void ab_project_merge_result_destroy(ArchbirdEngine *engine,
                                     AbProjectMergeResult *result) {
  size_t index;
  if (!result)
    return;
  for (index = 0; result->facts && index < result->fact_count; index++) {
    if (result->facts[index].value == &result->facts[index].fact)
      ab_fact_free(engine, &result->facts[index].fact);
    if (result->facts[index].contributors !=
        &result->facts[index].primary_contributor)
      ab_free(engine, result->facts[index].contributors);
  }
  ab_free(engine, result->facts);
  ab_free(engine, result->facts_by_path);
  ab_free(engine, result->conflicts);
  ab_free(engine, result->variations);
  memset(result, 0, sizeof(*result));
}

int ab_project_providers_finalized(const ArchbirdProject *project) {
  return project ? project->merge.finalized : 0;
}

size_t ab_project_merged_fact_count(const ArchbirdProject *project) {
  return project ? project->merge.fact_count : 0;
}

const AbFact *ab_project_merged_fact(const ArchbirdProject *project,
                                     size_t index) {
  if (!project || index >= project->merge.fact_count)
    return NULL;
  return project->merge.facts[index].value;
}

const AbProviderBundle *
ab_project_merged_fact_provider(const ArchbirdProject *project, size_t index) {
  size_t provider_index;
  if (!project || index >= project->merge.fact_count)
    return NULL;
  provider_index = project->merge.facts[index].provider_index;
  return provider_index < project->providers.count
             ? &project->providers.bundles[provider_index]
             : NULL;
}

static int fact_path_index_compare(const void *left_raw,
                                   const void *right_raw) {
  const AbFact *left = (*(AbMergedFact *const *)left_raw)->value;
  const AbFact *right = (*(AbMergedFact *const *)right_raw)->value;
  int compared = ab_string_compare(&left->path, &right->path);
  if (compared != 0)
    return compared;
  compared = ab_string_compare(&left->domain, &right->domain);
  if (compared != 0)
    return compared;
  if (left->span_start != right->span_start)
    return left->span_start < right->span_start ? -1 : 1;
  if (left->span_end != right->span_end)
    return left->span_end < right->span_end ? -1 : 1;
  compared = ab_string_compare(&left->kind, &right->kind);
  if (compared != 0)
    return compared;
  return ab_string_compare(&left->id, &right->id);
}

static int fact_path_domain_compare(const AbFact *fact, const AbString *path,
                                    const char *domain, size_t domain_length) {
  AbString wanted_domain = {(char *)domain, domain_length};
  int compared = ab_string_compare(&fact->path, path);
  if (compared != 0)
    return compared;
  return ab_string_compare(&fact->domain, &wanted_domain);
}

void ab_project_merged_fact_range(const ArchbirdProject *project,
                                  const AbString *path, const char *domain,
                                  size_t *out_start, size_t *out_end) {
  size_t domain_length = strlen(domain);
  size_t low = 0;
  size_t high = project ? project->merge.fact_count : 0;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (fact_path_domain_compare(project->merge.facts_by_path[middle]->value,
                                 path, domain, domain_length) < 0)
      low = middle + 1;
    else
      high = middle;
  }
  *out_start = low;
  high = project ? project->merge.fact_count : 0;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (fact_path_domain_compare(project->merge.facts_by_path[middle]->value,
                                 path, domain, domain_length) <= 0)
      low = middle + 1;
    else
      high = middle;
  }
  *out_end = low;
}

static int fact_path_domain_span_compare(const AbFact *fact,
                                         const AbString *path,
                                         const char *domain,
                                         size_t domain_length,
                                         size_t span_start, size_t span_end) {
  int compared = fact_path_domain_compare(fact, path, domain, domain_length);
  if (compared != 0)
    return compared;
  if (fact->span_start != span_start)
    return fact->span_start < span_start ? -1 : 1;
  if (fact->span_end != span_end)
    return fact->span_end < span_end ? -1 : 1;
  return 0;
}

void ab_project_merged_fact_span_range(const ArchbirdProject *project,
                                       const AbString *path, const char *domain,
                                       size_t span_start, size_t span_end,
                                       size_t *out_start, size_t *out_end) {
  size_t domain_length = strlen(domain);
  size_t low = 0;
  size_t high = project ? project->merge.fact_count : 0;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (fact_path_domain_span_compare(
            project->merge.facts_by_path[middle]->value, path, domain,
            domain_length, span_start, span_end) < 0)
      low = middle + 1;
    else
      high = middle;
  }
  *out_start = low;
  high = project ? project->merge.fact_count : 0;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (fact_path_domain_span_compare(
            project->merge.facts_by_path[middle]->value, path, domain,
            domain_length, span_start, span_end) <= 0)
      low = middle + 1;
    else
      high = middle;
  }
  *out_end = low;
}

const AbFact *ab_project_merged_fact_by_path(const ArchbirdProject *project,
                                             size_t index) {
  if (!project || index >= project->merge.fact_count)
    return NULL;
  return project->merge.facts_by_path[index]->value;
}

const AbProviderBundle *
ab_project_merged_fact_provider_by_path(const ArchbirdProject *project,
                                        size_t index) {
  size_t provider_index;
  if (!project || index >= project->merge.fact_count)
    return NULL;
  provider_index = project->merge.facts_by_path[index]->provider_index;
  return provider_index < project->providers.count
             ? &project->providers.bundles[provider_index]
             : NULL;
}

static int domain_compare(const void *left_raw, const void *right_raw) {
  const AbDomainSelection *left = (const AbDomainSelection *)left_raw;
  const AbDomainSelection *right = (const AbDomainSelection *)right_raw;
  const AbString *left_fields[] = {
      &left->subject->scope,
      &left->subject->project,
      &left->subject->path,
      &left->subject->name,
  };
  const AbString *right_fields[] = {
      &right->subject->scope,
      &right->subject->project,
      &right->subject->path,
      &right->subject->name,
  };
  size_t index;
  for (index = 0; index < sizeof(left_fields) / sizeof(left_fields[0]);
       index++) {
    int compared = ab_string_compare(left_fields[index], right_fields[index]);
    if (compared != 0)
      return compared;
  }
  return ab_string_compare(left->name, right->name);
}

static AbDomainSelection *find_domain(AbDomainSelection *domains,
                                      size_t domain_count,
                                      const AbSubject *subject,
                                      const AbString *name) {
  size_t low = 0;
  size_t high = domain_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    AbDomainSelection wanted;
    int compared;
    memset(&wanted, 0, sizeof(wanted));
    wanted.subject = subject;
    wanted.name = name;
    compared = domain_compare(&domains[middle], &wanted);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &domains[middle];
  }
  return NULL;
}

static int fact_correlation_compare(const AbFact *left, const AbFact *right) {
  const AbString *left_fields[] = {&left->project, &left->path, &left->domain,
                                   &left->kind};
  const AbString *right_fields[] = {&right->project, &right->path,
                                    &right->domain, &right->kind};
  size_t index;
  for (index = 0; index < sizeof(left_fields) / sizeof(left_fields[0]);
       index++) {
    int compared = ab_string_compare(left_fields[index], right_fields[index]);
    if (compared != 0)
      return compared;
  }
  if (left->span_start != right->span_start)
    return left->span_start < right->span_start ? -1 : 1;
  if (left->span_end != right->span_end)
    return left->span_end < right->span_end ? -1 : 1;
  /* Providers must opt into span correlation. This keeps occurrence-shaped
     evidence composable while preventing compact relations and synthetic
     summaries that share a representative span from collapsing. */
  if (left->correlate_by_span != right->correlate_by_span)
    return left->correlate_by_span ? 1 : -1;
  if (left->correlate_by_span && left->span_start < left->span_end)
    return 0;
  return ab_string_compare(&left->key, &right->key);
}

static int fact_claim_rank(const AbString *claim) {
  static const char semantic[] = "semantic-";
  if (claim->length >= sizeof(semantic) - 1 &&
      memcmp(claim->data, semantic, sizeof(semantic) - 1) == 0)
    return 3;
  if (ab_project_string_equals_literal(claim, "syntax-structure"))
    return 2;
  if (ab_project_string_equals_literal(claim, "lexical-occurrence"))
    return 1;
  return 0;
}

static int fact_witness_compare(const AbFactReference *left,
                                const AbFactReference *right,
                                const AbProviderBundle *providers) {
  const AbProviderBundle *left_provider = &providers[left->provider_index];
  const AbProviderBundle *right_provider = &providers[right->provider_index];
  int left_rank = fact_claim_rank(&left->fact->claim);
  int right_rank = fact_claim_rank(&right->fact->claim);
  int compared;
  if (left_rank != right_rank)
    return left_rank > right_rank ? -1 : 1;
  compared = ab_string_compare(&left->fact->claim, &right->fact->claim);
  if (compared != 0)
    return compared;
  compared = ab_string_compare(&left_provider->producer.name,
                               &right_provider->producer.name);
  if (compared != 0)
    return compared;
  compared = ab_string_compare(&left_provider->producer.version,
                               &right_provider->producer.version);
  if (compared != 0)
    return compared;
  compared = memcmp(left_provider->producer.configuration_sha256,
                    right_provider->producer.configuration_sha256, 32);
  if (compared != 0)
    return compared;
  compared = memcmp(left_provider->producer.implementation_sha256,
                    right_provider->producer.implementation_sha256, 32);
  if (compared != 0)
    return compared;
  compared = memcmp(left_provider->sha256, right_provider->sha256, 32);
  if (compared != 0)
    return compared;
  return ab_string_compare(&left->fact->id, &right->fact->id);
}

static int fact_reference_compare(const AbFactReference *left,
                                  const AbFactReference *right,
                                  const AbProviderBundle *providers) {
  int compared = domain_compare(
      &(AbDomainSelection){.subject = &providers[left->provider_index].subject,
                           .name = &left->fact->domain},
      &(AbDomainSelection){.subject = &providers[right->provider_index].subject,
                           .name = &right->fact->domain});
  ArchbirdProviderMode left_mode;
  ArchbirdProviderMode right_mode;
  if (compared != 0)
    return compared;
  compared = fact_correlation_compare(left->fact, right->fact);
  if (compared != 0)
    return compared;
  left_mode = providers[left->provider_index].mode;
  right_mode = providers[right->provider_index].mode;
  if (left_mode != right_mode)
    return (left_mode > right_mode) - (left_mode < right_mode);
  return fact_witness_compare(left, right, providers);
}

static ArchbirdStatus sort_fact_references(ArchbirdEngine *engine,
                                           const AbProjectProviderStore *store,
                                           AbFactReference *facts,
                                           size_t count) {
  AbFactReference *scratch;
  AbFactReference *source = facts;
  AbFactReference *target;
  size_t width;
  int swapped = 0;
  if (count < 2)
    return ARCHBIRD_OK;
  scratch = (AbFactReference *)ab_malloc(engine, count * sizeof(*scratch));
  if (!scratch)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory sorting provider facts");
  target = scratch;
  for (width = 1; width < count;) {
    size_t start;
    for (start = 0; start < count; start += width * 2) {
      size_t left = start;
      size_t left_end = start + width < count ? start + width : count;
      size_t right = left_end;
      size_t right_end = right + width < count ? right + width : count;
      size_t output = start;
      while (left < left_end && right < right_end) {
        if (fact_reference_compare(&source[left], &source[right],
                                   store->bundles) <= 0)
          target[output++] = source[left++];
        else
          target[output++] = source[right++];
      }
      while (left < left_end)
        target[output++] = source[left++];
      while (right < right_end)
        target[output++] = source[right++];
    }
    {
      AbFactReference *temporary = source;
      source = target;
      target = temporary;
    }
    swapped = !swapped;
    if (width > count / 2)
      break;
    width *= 2;
  }
  if (swapped)
    memcpy(facts, source, count * sizeof(*facts));
  ab_free(engine, scratch);
  return ARCHBIRD_OK;
}

static int string_arrays_equal(const AbStringArray *left,
                               const AbStringArray *right) {
  size_t index;
  if (left->count != right->count)
    return 0;
  for (index = 0; index < left->count; index++) {
    if (!ab_string_equal(&left->items[index], &right->items[index]))
      return 0;
  }
  return 1;
}

static int attributes_relation(const AbFact *left, const AbFact *right,
                               int *enriched, int *varied) {
  size_t left_index = 0;
  size_t right_index = 0;
  while (left_index < left->attribute_count &&
         right_index < right->attribute_count) {
    const AbObjectField *left_field = &left->attributes[left_index];
    const AbObjectField *right_field = &right->attributes[right_index];
    int compared = ab_string_compare(&left_field->name, &right_field->name);
    if (compared < 0) {
      *enriched = 1;
      left_index++;
    } else if (compared > 0) {
      *enriched = 1;
      right_index++;
    } else {
      if (!ab_value_equal(&left_field->value, &right_field->value)) {
        if (!ab_fact_attribute_is_presentation(&left_field->name))
          return 0;
        *varied = 1;
      }
      left_index++;
      right_index++;
    }
  }
  if (left_index < left->attribute_count ||
      right_index < right->attribute_count)
    *enriched = 1;
  return 1;
}

static int fact_relation(const AbFact *left, const AbFact *right, int *varied) {
  int enriched = 0;
  *varied = 0;
  if (!ab_fact_names_compatible(left, right))
    return -1;
  if (!ab_fact_declaration_extents_compatible(left, right))
    return -1;
  if (left->has_name != right->has_name ||
      (left->has_name && !ab_string_equal(&left->name, &right->name)))
    enriched = 1;
  if (!attributes_relation(left, right, &enriched, varied))
    return -1;
  if (left->has_resolution && right->has_resolution) {
    if (!ab_string_equal(&left->resolution.state, &right->resolution.state) ||
        !string_arrays_equal(&left->resolution.targets,
                             &right->resolution.targets))
      return -1;
    if (left->resolution.has_reason && right->resolution.has_reason &&
        !ab_string_equal(&left->resolution.reason, &right->resolution.reason))
      return -1;
    if (left->resolution.has_reason != right->resolution.has_reason)
      enriched = 1;
  } else if (left->has_resolution != right->has_resolution) {
    enriched = 1;
  }
  if (!ab_string_equal(&left->claim, &right->claim))
    enriched = 1;
  return enriched;
}

ArchbirdStatus ab_provider_collect_domains(ArchbirdEngine *engine,
                                           const AbProjectProviderStore *store,
                                           AbDomainSelection **out_domains,
                                           size_t *out_count) {
  AbDomainSelection *domains = NULL;
  size_t domain_count = 0;
  size_t capacity = 0;
  size_t provider_index;
  *out_domains = NULL;
  *out_count = 0;
  for (provider_index = 0; provider_index < store->count; provider_index++) {
    const AbProviderBundle *provider = &store->bundles[provider_index];
    size_t capability_index;
    for (capability_index = 0; capability_index < provider->capability_count;
         capability_index++) {
      AbDomainSelection *domain;
      if (domain_count == capacity) {
        size_t next = capacity ? capacity * 2 : 8;
        AbDomainSelection *resized;
        if (next < capacity || next > SIZE_MAX / sizeof(*domains)) {
          ab_free(engine, domains);
          return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                    ARCHBIRD_NO_OFFSET,
                                    "provider domain limit exceeded");
        }
        resized = (AbDomainSelection *)ab_realloc(engine, domains,
                                                  next * sizeof(*domains));
        if (!resized) {
          ab_free(engine, domains);
          return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory selecting domains");
        }
        domains = resized;
        capacity = next;
      }
      domain = &domains[domain_count++];
      memset(domain, 0, sizeof(*domain));
      domain->subject = &provider->subject;
      domain->name = &provider->capabilities[capability_index].domain;
      domain->primary_provider = SIZE_MAX;
      if (provider->mode == ARCHBIRD_PROVIDER_PRIMARY) {
        domain->primary_count++;
        domain->primary_provider = provider_index;
        domain->primary_complete = ab_project_string_equals_literal(
            &provider->capabilities[capability_index].coverage, "complete");
      } else if (provider->mode == ARCHBIRD_PROVIDER_AUGMENT) {
        domain->augment_count++;
      } else {
        domain->audit_count++;
      }
    }
  }
  if (domain_count > 1) {
    size_t read_index;
    size_t write_index = 0;
    qsort(domains, domain_count, sizeof(*domains), domain_compare);
    for (read_index = 0; read_index < domain_count; read_index++) {
      AbDomainSelection *current = &domains[read_index];
      if (write_index > 0 &&
          domain_compare(&domains[write_index - 1], current) == 0) {
        AbDomainSelection *selected = &domains[write_index - 1];
        selected->primary_count += current->primary_count;
        selected->augment_count += current->augment_count;
        selected->audit_count += current->audit_count;
        if (current->primary_count) {
          selected->primary_provider = current->primary_provider;
          selected->primary_complete = current->primary_complete;
        }
      } else {
        if (write_index != read_index)
          domains[write_index] = *current;
        write_index++;
      }
    }
    domain_count = write_index;
  }
  if (domain_count > store->max_facts) {
    ab_free(engine, domains);
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "provider domain limit exceeded");
  }
  *out_domains = domains;
  *out_count = domain_count;
  return ARCHBIRD_OK;
}

static ArchbirdStatus
collect_fact_references(ArchbirdEngine *engine,
                        const AbProjectProviderStore *store,
                        AbFactReference **out_facts) {
  AbFactReference *facts;
  size_t count = 0;
  size_t provider_index;
  *out_facts = NULL;
  if (!store->fact_count)
    return ARCHBIRD_OK;
  facts =
      (AbFactReference *)ab_malloc(engine, store->fact_count * sizeof(*facts));
  if (!facts)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory indexing provider facts");
  for (provider_index = 0; provider_index < store->count; provider_index++) {
    size_t fact_index;
    for (fact_index = 0; fact_index < store->bundles[provider_index].fact_count;
         fact_index++) {
      facts[count].provider_index = provider_index;
      facts[count].fact = &store->bundles[provider_index].facts[fact_index];
      count++;
    }
  }
  {
    ArchbirdStatus status = sort_fact_references(engine, store, facts, count);
    if (status != ARCHBIRD_OK) {
      ab_free(engine, facts);
      return status;
    }
  }
  *out_facts = facts;
  return ARCHBIRD_OK;
}

static void record_conflict(AbProjectMergeResult *result, const char *reason,
                            const AbSubject *subject, const AbString *domain,
                            size_t left_provider, const AbFact *left_fact,
                            size_t right_provider, const AbFact *right_fact) {
  AbMergeConflict *conflict;
  if (result->conflict_count >= result->conflict_capacity)
    return;
  conflict = &result->conflicts[result->conflict_count++];
  conflict->reason = reason;
  conflict->subject = subject;
  conflict->domain = domain;
  conflict->left_provider = left_provider;
  conflict->left_fact = left_fact;
  conflict->right_provider = right_provider;
  conflict->right_fact = right_fact;
}

static ArchbirdStatus
provider_merge_conflict_error(ArchbirdEngine *engine,
                              const ArchbirdProject *project) {
  const AbMergeConflict *conflict;
  const AbFact *fact;
  const AbString *left_name;
  const AbString *right_name;
  if (!project->merge.conflict_count)
    return ARCHBIRD_OK;
  conflict = &project->merge.conflicts[0];
  fact = conflict->left_fact ? conflict->left_fact : conflict->right_fact;
  if (!fact || conflict->left_provider >= project->providers.count ||
      conflict->right_provider >= project->providers.count)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "provider merge contains %zu conflict(s); first "
                              "reason: %s",
                              project->merge.conflict_count, conflict->reason);
  left_name =
      &project->providers.bundles[conflict->left_provider].producer.name;
  right_name =
      &project->providers.bundles[conflict->right_provider].producer.name;
  return archbird_error_set(
      engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
      "provider merge contains %zu conflict(s); first %s at %.*s:%zu..%zu "
      "between %.*s and %.*s",
      project->merge.conflict_count, conflict->reason, (int)fact->path.length,
      fact->path.data, fact->span_start, fact->span_end, (int)left_name->length,
      left_name->data, (int)right_name->length, right_name->data);
}

static ArchbirdStatus
ensure_merge_variation_capacity(ArchbirdEngine *engine,
                                AbProjectMergeResult *result) {
  AbMergeVariation *resized;
  size_t next;
  if (result->variation_count < result->variation_capacity)
    return ARCHBIRD_OK;
  if (result->variation_capacity >= engine->options.max_values)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "provider variation ledger is too large");
  next =
      result->variation_capacity ? result->variation_capacity * 2 : (size_t)16;
  if (next < result->variation_capacity || next > engine->options.max_values)
    next = engine->options.max_values;
  if (next > SIZE_MAX / sizeof(*resized))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "provider variation ledger is too large");
  resized = (AbMergeVariation *)ab_realloc(engine, result->variations,
                                           next * sizeof(*resized));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory storing provider variations");
  result->variations = resized;
  result->variation_capacity = next;
  return ARCHBIRD_OK;
}

static ArchbirdStatus
record_variations(ArchbirdEngine *engine, AbProjectMergeResult *result,
                  const AbSubject *subject, const AbString *domain,
                  size_t canonical_provider, const AbFact *canonical_fact,
                  size_t alternate_provider, const AbFact *alternate_fact,
                  int extent_attributes) {
  size_t canonical_index = 0;
  size_t alternate_index = 0;
  while (canonical_index < canonical_fact->attribute_count &&
         alternate_index < alternate_fact->attribute_count) {
    const AbObjectField *canonical =
        &canonical_fact->attributes[canonical_index];
    const AbObjectField *alternate =
        &alternate_fact->attributes[alternate_index];
    int compared = ab_string_compare(&canonical->name, &alternate->name);
    if (compared < 0) {
      canonical_index++;
    } else if (compared > 0) {
      alternate_index++;
    } else {
      if (ab_fact_attribute_is_presentation(&canonical->name) &&
          ab_fact_attribute_is_declaration_extent(&canonical->name) ==
              extent_attributes &&
          (!extent_attributes || !ab_project_string_equals_literal(
                                     &canonical->name, "extent_fidelity")) &&
          !ab_value_equal(&canonical->value, &alternate->value)) {
        AbMergeVariation *variation;
        ArchbirdStatus status = ensure_merge_variation_capacity(engine, result);
        if (status != ARCHBIRD_OK)
          return status;
        variation = &result->variations[result->variation_count++];
        variation->subject = subject;
        variation->domain = domain;
        variation->canonical_provider = canonical_provider;
        variation->canonical_fact = canonical_fact;
        variation->canonical_attribute = canonical;
        variation->alternate_provider = alternate_provider;
        variation->alternate_fact = alternate_fact;
        variation->alternate_attribute = alternate;
      }
      canonical_index++;
      alternate_index++;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus
build_provider_merge_result(ArchbirdEngine *engine,
                            const AbProjectProviderStore *store,
                            AbProjectMergeResult *result) {
  AbDomainSelection *domains = NULL;
  size_t domain_count = 0;
  AbFactReference *facts = NULL;
  size_t cursor = 0;
  size_t domain_index;
  ArchbirdStatus status;
  status = ab_provider_collect_domains(engine, store, &domains, &domain_count);
  if (status == ARCHBIRD_OK)
    status = collect_fact_references(engine, store, &facts);
  if (status != ARCHBIRD_OK) {
    ab_free(engine, domains);
    ab_free(engine, facts);
    return status;
  }
  memset(&result->summary, 0, sizeof(result->summary));
  result->summary.struct_size = sizeof(result->summary);
  result->summary.providers = store->count;
  result->summary.selections = domain_count;
  if (domain_count > SIZE_MAX - store->fact_count) {
    ab_free(engine, domains);
    ab_free(engine, facts);
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "provider conflict ledger is too large");
  }
  result->conflict_capacity = domain_count + store->fact_count;
  if (result->conflict_capacity) {
    result->conflicts = (AbMergeConflict *)ab_calloc(
        engine, result->conflict_capacity, sizeof(*result->conflicts));
    if (!result->conflicts) {
      ab_free(engine, domains);
      ab_free(engine, facts);
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing provider conflicts");
    }
  }
  for (domain_index = 0; domain_index < domain_count; domain_index++) {
    if (domains[domain_index].primary_count > 1)
      record_conflict(result, "primary-cardinality",
                      domains[domain_index].subject, domains[domain_index].name,
                      SIZE_MAX, NULL, SIZE_MAX, NULL);
  }
  if (store->fact_count) {
    result->facts = (AbMergedFact *)ab_calloc(engine, store->fact_count,
                                              sizeof(*result->facts));
    if (!result->facts) {
      ab_free(engine, domains);
      ab_free(engine, facts);
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing merged facts");
    }
  }
  while (cursor < store->fact_count) {
    size_t end = cursor + 1;
    size_t selected = SIZE_MAX;
    size_t index;
    const AbProviderBundle *group_provider =
        &store->bundles[facts[cursor].provider_index];
    AbDomainSelection *domain =
        find_domain(domains, domain_count, &group_provider->subject,
                    &facts[cursor].fact->domain);
    while (end < store->fact_count &&
           ab_project_subject_equal(
               &group_provider->subject,
               &store->bundles[facts[end].provider_index].subject) &&
           fact_correlation_compare(facts[cursor].fact, facts[end].fact) == 0)
      end++;
    if (!domain || domain->primary_count > 1) {
      cursor = end;
      continue;
    }
    for (index = cursor; index < end; index++) {
      if (store->bundles[facts[index].provider_index].mode ==
          ARCHBIRD_PROVIDER_PRIMARY) {
        if (selected == SIZE_MAX)
          selected = index;
        else
          record_conflict(result, "duplicate-primary-occurrence",
                          domain->subject, domain->name,
                          facts[selected].provider_index, facts[selected].fact,
                          facts[index].provider_index, facts[index].fact);
      }
    }
    /* A complete primary capability owns its closed inventory. Bounded and
       partial primaries do not prove absence, so independently anchored
       augment facts remain part of the common view. */
    if (selected == SIZE_MAX &&
        (domain->primary_count == 0 || !domain->primary_complete)) {
      for (index = cursor; index < end; index++) {
        if (store->bundles[facts[index].provider_index].mode ==
            ARCHBIRD_PROVIDER_AUGMENT) {
          selected = index;
          break;
        }
      }
    }
    if (selected != SIZE_MAX) {
      size_t extent_selected = SIZE_MAX;
      int extent_rank = ab_fact_declaration_extent_rank(facts[selected].fact);
      AbMergedFact *merged = &result->facts[result->fact_count++];
      if (extent_rank > 0)
        extent_selected = selected;
      merged->provider_index = facts[selected].provider_index;
      merged->value = facts[selected].fact;
      if (end - cursor > SIZE_MAX / sizeof(*merged->contributors)) {
        status = archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                    ARCHBIRD_NO_OFFSET,
                                    "provider witness set is too large");
        goto merge_failed;
      }
      merged->primary_contributor = facts[selected];
      merged->contributors = &merged->primary_contributor;
      merged->contributor_count = 1;
      result->summary.contributed++;
      for (index = cursor; index < end; index++) {
        ArchbirdProviderMode mode =
            store->bundles[facts[index].provider_index].mode;
        int relation;
        int varied;
        if (index == selected || mode == ARCHBIRD_PROVIDER_PRIMARY)
          continue;
        relation =
            fact_relation(facts[selected].fact, facts[index].fact, &varied);
        if (varied) {
          status = record_variations(
              engine, result, domain->subject, domain->name,
              facts[selected].provider_index, facts[selected].fact,
              facts[index].provider_index, facts[index].fact, 0);
          if (status != ARCHBIRD_OK)
            goto merge_failed;
        }
        if (mode == ARCHBIRD_PROVIDER_AUDIT) {
          if (relation < 0)
            result->summary.audit_differences++;
          else
            result->summary.audit_matches++;
        } else if (relation < 0) {
          record_conflict(result, "augment-mismatch", domain->subject,
                          domain->name, facts[selected].provider_index,
                          facts[selected].fact, facts[index].provider_index,
                          facts[index].fact);
        } else if (relation > 0) {
          int candidate_extent_rank =
              ab_fact_declaration_extent_rank(facts[index].fact);
          if (candidate_extent_rank > extent_rank) {
            extent_rank = candidate_extent_rank;
            extent_selected = index;
          }
          if (merged->value != &merged->fact) {
            status = ab_fact_copy(engine, &merged->fact, merged->value);
            if (status != ARCHBIRD_OK)
              goto merge_failed;
            merged->value = &merged->fact;
          }
          status = ab_fact_merge_compatible(engine, &merged->fact,
                                            facts[index].fact);
          if (status != ARCHBIRD_OK)
            goto merge_failed;
          status = merged_fact_add_contributor(engine, merged, end - cursor,
                                               facts[index]);
          if (status != ARCHBIRD_OK)
            goto merge_failed;
          result->summary.enriched++;
        } else {
          int candidate_extent_rank =
              ab_fact_declaration_extent_rank(facts[index].fact);
          if (candidate_extent_rank > extent_rank) {
            extent_rank = candidate_extent_rank;
            extent_selected = index;
          }
          status = merged_fact_add_contributor(engine, merged, end - cursor,
                                               facts[index]);
          if (status != ARCHBIRD_OK)
            goto merge_failed;
          result->summary.deduplicated++;
        }
      }
      if (extent_selected != SIZE_MAX) {
        for (index = cursor; index < end; index++) {
          if (index == extent_selected)
            continue;
          status = record_variations(
              engine, result, domain->subject, domain->name,
              facts[extent_selected].provider_index,
              facts[extent_selected].fact, facts[index].provider_index,
              facts[index].fact, 1);
          if (status != ARCHBIRD_OK)
            goto merge_failed;
        }
        if (extent_selected != selected) {
          if (merged->value != &merged->fact) {
            status = ab_fact_copy(engine, &merged->fact, merged->value);
            if (status != ARCHBIRD_OK)
              goto merge_failed;
            merged->value = &merged->fact;
          }
          status = ab_fact_adopt_declaration_extent(
              engine, &merged->fact, facts[extent_selected].fact);
          if (status != ARCHBIRD_OK)
            goto merge_failed;
        }
      }
    } else {
      for (index = cursor; index < end; index++) {
        if (store->bundles[facts[index].provider_index].mode ==
            ARCHBIRD_PROVIDER_AUDIT)
          result->summary.audit_differences++;
      }
    }
    cursor = end;
  }
  result->summary.selected_facts = result->fact_count;
  result->summary.conflicts = result->conflict_count;
  result->summary.variations = result->variation_count;
  if (result->fact_count) {
    size_t fact_index;
    result->facts_by_path = (AbMergedFact **)ab_malloc(
        engine, result->fact_count * sizeof(*result->facts_by_path));
    if (!result->facts_by_path) {
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory indexing merged facts by path");
      goto merge_failed;
    }
    for (fact_index = 0; fact_index < result->fact_count; fact_index++)
      result->facts_by_path[fact_index] = &result->facts[fact_index];
    if (result->fact_count > 1)
      qsort(result->facts_by_path, result->fact_count,
            sizeof(*result->facts_by_path), fact_path_index_compare);
  }
  result->finalized = 1;
  ab_free(engine, domains);
  ab_free(engine, facts);
  return ARCHBIRD_OK;

merge_failed:
  ab_project_merge_result_destroy(engine, result);
  ab_free(engine, domains);
  ab_free(engine, facts);
  return status;
}

ArchbirdStatus ab_provider_merge(ArchbirdEngine *engine,
                                 const AbProjectProviderStore *providers,
                                 AbProjectMergeResult *out_result) {
  AbProjectMergeResult result;
  ArchbirdStatus status;
  if (!engine || !providers || !out_result)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(&result, 0, sizeof(result));
  status = build_provider_merge_result(engine, providers, &result);
  if (status != ARCHBIRD_OK) {
    ab_project_merge_result_destroy(engine, &result);
    return status;
  }
  *out_result = result;
  return ARCHBIRD_OK;
}

ArchbirdStatus archbird_project_finalize_providers(ArchbirdEngine *engine,
                                                   ArchbirdProject *project) {
  AbProjectMergeResult result;
  ArchbirdStatus status;
  if (!engine || !project)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (project->merge.finalized)
    return project->merge.summary.conflicts
               ? provider_merge_conflict_error(engine, project)
               : ARCHBIRD_OK;
  if (project->sources.supplied_count != project->sources.manifest.file_count)
    return archbird_error_set(
        engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
        "source project must be complete before provider merge");
  ab_project_provider_store_canonicalize(&project->providers);
  status = ab_provider_merge(engine, &project->providers, &result);
  if (status != ARCHBIRD_OK)
    return status;
  project->merge = result;
  if (project->merge.summary.conflicts)
    return provider_merge_conflict_error(engine, project);
  return ARCHBIRD_OK;
}

ArchbirdStatus
archbird_project_merge_summary(const ArchbirdProject *project,
                               ArchbirdMergeSummary *out_summary) {
  if (!project || !out_summary ||
      out_summary->struct_size != sizeof(*out_summary))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!project->merge.finalized)
    return ARCHBIRD_CONFLICT;
  *out_summary = project->merge.summary;
  return ARCHBIRD_OK;
}
