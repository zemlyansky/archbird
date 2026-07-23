#ifndef ARCHBIRD_ACT_INTERNAL_H
#define ARCHBIRD_ACT_INTERNAL_H

#include "archbird_internal.h"
#include "json_value.h"
#include "sha256.h"
#include "verify_checks.h"

typedef struct AbActVerification {
  ArchbirdEngine *engine;
  AbValue root;
  char sha256[65];
  const AbValue *tool;
  const AbValue *policy;
  const AbValue *evaluations;
  const AbValue *evaluation;
  const AbValue *mappings;
  const AbValue *observations;
  const AbValue *operand_definitions;
  const AbValue *operands;
  const AbValue *constraints;
  AbProjectionData *decoded_facts;
  size_t fact_count;
} AbActVerification;

typedef struct AbActEvidenceList {
  ArchbirdEngine *engine;
  AbProjectionEvidence *items;
  size_t count;
  size_t capacity;
} AbActEvidenceList;

typedef struct AbActCoverage {
  const char *classification;
  AbString domain;
  AbString provider;
  const char *unknown;
} AbActCoverage;

typedef struct AbActSliceEntry {
  const char *kind;
  AbString name;
  char sha256[65];
} AbActSliceEntry;

typedef struct AbActProjection {
  AbString name;
  AbString source;
  const AbValue *aliases;
} AbActProjection;

typedef struct AbActCandidate {
  AbString id;
  const char *kind;
  AbString project;
  AbString path;
  AbString reason;
  AbActEvidenceList evidence;
} AbActCandidate;

typedef struct AbActUnknown {
  AbString id;
  const char *code;
  AbString scope;
  const char *message;
  AbActEvidenceList evidence;
} AbActUnknown;

typedef struct AbActPreserved {
  const AbValue *constraint;
  char sha256[65];
} AbActPreserved;

typedef struct AbActPostcondition {
  const char *id;
  const char *strength;
  const char *assertion;
  AbString constraint_id;
  AbString rationale;
  AbString expected;
  AbString actual;
  const AbValue *owner;
  const AbValue *requirements;
  const AbValue *tags;
  const AbValue *minimum;
  const AbValue *maximum;
  const AbValue *exact;
  const AbValue *required_routes;
  AbActEvidenceList evidence;
} AbActPostcondition;

typedef struct AbActProposalData {
  AbActVerification *verification;
  const AbValue *origin_constraint;
  const AbValue *origin_finding;
  char origin_constraint_sha256[65];
  char origin_finding_sha256[65];
  AbString fingerprint;
  AbString actual_name;
  AbActCoverage coverage;
  AbActSliceEntry *slice;
  size_t slice_count;
  char slice_sha256[65];
  AbProjectionData literal_fact;
  int has_literal_fact;
  AbActProjection projection;
  int has_projection;
  AbActPostcondition postcondition;
  int has_postcondition;
  AbActPreserved *preserved;
  size_t preserved_count;
  AbActCandidate *candidates;
  size_t candidate_count;
  AbActUnknown *unknowns;
  size_t unknown_count;
  AbActEvidenceList evidence;
} AbActProposalData;

typedef struct AbActProposalView {
  ArchbirdEngine *engine;
  AbValue root;
  char sha256[65];
  const AbValue *tool;
  const AbValue *source;
  const AbValue *origin;
  const AbValue *mutable_sources;
  const AbValue *evidence_slice;
  const AbValue *facts;
  const AbValue *projections;
  const AbValue *postconditions;
  const AbValue *preserved;
  const AbValue *candidates;
  const AbValue *unknowns;
  const AbValue *evidence;
} AbActProposalView;

typedef struct AbActContractView {
  ArchbirdEngine *engine;
  AbValue root;
  char sha256[65];
  const AbValue *tool;
  const AbValue *origin;
  const AbValue *postconditions;
  const AbValue *preserved;
  const AbValue *selected_candidates;
  const AbValue *acknowledged_unknowns;
} AbActContractView;

typedef struct AbActResultView {
  ArchbirdEngine *engine;
  AbValue root;
  char sha256[65];
  const AbValue *tool;
  const AbValue *outcomes;
  const AbValue *diagnostics;
} AbActResultView;

typedef struct AbActOutcome {
  AbString id;
  const char *kind;
  const char *status;
  const char *message;
  AbActEvidenceList evidence;
} AbActOutcome;

typedef struct AbActStringList {
  ArchbirdEngine *engine;
  AbString *items;
  size_t count;
  size_t capacity;
} AbActStringList;

typedef struct AbActResultData {
  ArchbirdEngine *engine;
  const char *freshness;
  const char *status;
  char proposal_sha256[65];
  char contract_sha256[65];
  char before_sha256[65];
  char after_sha256[65];
  AbActOutcome *outcomes;
  size_t outcome_count;
  AbActStringList diagnostics;
  char sha256[65];
} AbActResultData;

ArchbirdStatus ab_act_value_digest(ArchbirdEngine *engine, const AbValue *value,
                                   char output[65]);
ArchbirdStatus ab_act_value_digest_without_field(ArchbirdEngine *engine,
                                                 const AbValue *value,
                                                 const char *field_name,
                                                 char output[65]);
int ab_act_lowercase_sha256(const AbValue *value);
int ab_act_identifier(const AbValue *value);
int ab_act_string_values_equal(const AbValue *left, const AbValue *right);
int ab_act_object_fields_allowed(const AbValue *object,
                                 const char *const *allowed, size_t count);
ArchbirdStatus ab_act_validate_finding(ArchbirdEngine *engine,
                                       const AbValue *row);

ArchbirdStatus ab_act_verification_load(ArchbirdEngine *engine,
                                        const uint8_t *json, size_t json_length,
                                        AbActVerification *out);
void ab_act_verification_free(AbActVerification *artifact);
const AbValue *ab_act_verification_constraint(const AbActVerification *artifact,
                                              const AbString *id);
const AbValue *ab_act_verification_finding(const AbActVerification *artifact,
                                           const AbString *fingerprint,
                                           const AbValue **out_check);
const AbValue *
ab_act_verification_fact_value(const AbActVerification *artifact,
                               const AbString *name,
                               const AbProjectionData **out_fact);
const AbValue *
ab_act_verification_operand_definition(const AbActVerification *artifact,
                                       const AbString *name);
const AbValue *ab_act_verification_mapping(const AbActVerification *artifact,
                                           const AbString *name);
const AbValue *
ab_act_verification_observation(const AbActVerification *artifact,
                                const AbString *name);

ArchbirdStatus ab_act_evidence_list_add(ArchbirdEngine *engine,
                                        AbActEvidenceList *list,
                                        const AbProjectionEvidence *evidence);
ArchbirdStatus ab_act_evidence_list_add_array(ArchbirdEngine *engine,
                                              AbActEvidenceList *list,
                                              const AbValue *rows);
void ab_act_evidence_list_finish(AbActEvidenceList *list);
void ab_act_evidence_list_free(AbActEvidenceList *list);
ArchbirdStatus ab_act_evidence_list_render(AbBuffer *buffer,
                                           const AbActEvidenceList *list);

ArchbirdStatus ab_act_proposal_compile(ArchbirdEngine *engine,
                                       AbActVerification *verification,
                                       const char *fingerprint,
                                       size_t fingerprint_length,
                                       AbActProposalData *out);
void ab_act_proposal_data_free(AbActProposalData *proposal);
ArchbirdStatus ab_act_proposal_render_json(AbBuffer *buffer,
                                           const AbActProposalData *proposal);
ArchbirdStatus ab_act_project_fact(ArchbirdEngine *engine,
                                   const AbProjectionData *source,
                                   const AbString *name, const AbValue *aliases,
                                   const char *selection, const AbValue *keys,
                                   AbProjectionData *out);
ArchbirdStatus ab_act_proposal_load(ArchbirdEngine *engine, const uint8_t *json,
                                    size_t json_length, AbActProposalView *out);
void ab_act_proposal_view_free(AbActProposalView *proposal);
ArchbirdStatus ab_act_contract_load(ArchbirdEngine *engine, const uint8_t *json,
                                    size_t json_length, AbActContractView *out);
void ab_act_contract_view_free(AbActContractView *contract);
ArchbirdStatus ab_act_result_load(ArchbirdEngine *engine, const uint8_t *json,
                                  size_t json_length, AbActResultView *out);
void ab_act_result_view_free(AbActResultView *result);
unsigned ab_act_result_status_bit(const char *status, size_t status_length);
const char *ab_act_result_status_reduce(const char *freshness,
                                        size_t freshness_length,
                                        unsigned outcome_statuses);
ArchbirdStatus ab_act_contract_create_json(ArchbirdEngine *engine,
                                           const AbActProposalView *proposal,
                                           const uint8_t *review_json,
                                           size_t review_length, AbBuffer *out);
ArchbirdStatus ab_act_result_verify(ArchbirdEngine *engine,
                                    const AbActProposalView *proposal,
                                    const AbActContractView *contract,
                                    const AbActVerification *before,
                                    const AbActVerification *after,
                                    AbActResultData *out);
void ab_act_result_data_free(AbActResultData *result);
ArchbirdStatus ab_act_result_render_json(AbBuffer *buffer,
                                         AbActResultData *result);
#endif
