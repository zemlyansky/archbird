#ifndef ARCHBIRD_PROJECT_MODEL_H
#define ARCHBIRD_PROJECT_MODEL_H

#include "base/archbird_internal.h"
#include "base/json_value.h"
#include "base/model.h"
#include "evidence/config.h"
#include "evidence/evidence.h"

#include <string.h>

typedef struct AbSourceState {
  uint8_t *bytes;
  int supplied;
} AbSourceState;

typedef struct AbProjectSourceStore {
  AbSourceManifest manifest;
  AbSourceState *entries;
  size_t supplied_count;
  size_t supplied_bytes;
  uint8_t manifest_sha256[32];
  char manifest_sha256_hex[65];
  char map_input_sha256_hex[65];
} AbProjectSourceStore;

typedef struct AbProjectProviderStore {
  AbProviderBundle *bundles;
  size_t count;
  size_t capacity;
  uint8_t *digest_index;
  uint8_t *digest_occupied;
  size_t digest_capacity;
  size_t fact_count;
  size_t max_bundles;
  size_t max_facts;
} AbProjectProviderStore;

typedef struct AbFactReference {
  size_t provider_index;
  const AbFact *fact;
} AbFactReference;

typedef struct AbMergedFact {
  size_t provider_index;
  AbFact fact;
  const AbFact *value;
  AbFactReference primary_contributor;
  AbFactReference *contributors;
  size_t contributor_count;
} AbMergedFact;

typedef struct AbMergeConflict {
  const char *reason;
  const AbSubject *subject;
  const AbString *domain;
  size_t left_provider;
  const AbFact *left_fact;
  size_t right_provider;
  const AbFact *right_fact;
} AbMergeConflict;

typedef struct AbMergeVariation {
  const AbSubject *subject;
  const AbString *domain;
  size_t canonical_provider;
  const AbFact *canonical_fact;
  const AbObjectField *canonical_attribute;
  size_t alternate_provider;
  const AbFact *alternate_fact;
  const AbObjectField *alternate_attribute;
} AbMergeVariation;

typedef struct AbProjectMergeResult {
  AbMergedFact *facts;
  size_t fact_count;
  AbMergedFact **facts_by_path;
  AbMergeConflict *conflicts;
  size_t conflict_count;
  size_t conflict_capacity;
  AbMergeVariation *variations;
  size_t variation_count;
  size_t variation_capacity;
  ArchbirdMergeSummary summary;
  int finalized;
} AbProjectMergeResult;

typedef struct AbProjectObservationStore {
  AbValue *documents;
  size_t count;
  size_t capacity;
} AbProjectObservationStore;

typedef struct AbDomainSelection {
  const AbSubject *subject;
  const AbString *name;
  size_t primary_count;
  size_t primary_provider;
  int primary_complete;
  size_t augment_count;
  size_t audit_count;
} AbDomainSelection;

struct ArchbirdProject {
  ArchbirdEngine *engine;
  AbProjectSourceStore sources;
  AbProjectProviderStore providers;
  AbProjectMergeResult merge;
  AbProjectObservationStore observations;
  AbMapConfig *config;
};

static inline int ab_project_string_equals_literal(const AbString *value,
                                                   const char *literal) {
  size_t length = strlen(literal);
  return value->length == length &&
         (length == 0 || memcmp(value->data, literal, length) == 0);
}

static inline int ab_project_subject_equal(const AbSubject *left,
                                           const AbSubject *right) {
  return ab_string_equal(&left->scope, &right->scope) &&
         left->has_project == right->has_project &&
         (!left->has_project ||
          ab_string_equal(&left->project, &right->project)) &&
         left->has_path == right->has_path &&
         (!left->has_path || ab_string_equal(&left->path, &right->path)) &&
         left->has_name == right->has_name &&
         (!left->has_name || ab_string_equal(&left->name, &right->name));
}

#endif
