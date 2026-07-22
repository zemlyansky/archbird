#ifndef ARCHBIRD_VERIFY_RUNTIME_H
#define ARCHBIRD_VERIFY_RUNTIME_H

#include "verify_model.h"

typedef struct AbVerifyDiagnostic {
  AbString severity;
  AbString code;
  AbString message;
  AbString path;
} AbVerifyDiagnostic;

typedef struct AbVerifyCoverageRegression {
  AbString check;
  AbStringArray values;
} AbVerifyCoverageRegression;

typedef struct AbVerifyBaselineState {
  int enabled;
  char sha256[65];
  AbString owner;
  AbString rationale;
  size_t active_count;
  size_t resolved_count;
  AbVerifyCoverageRegression *coverage_regressions;
  size_t coverage_regression_count;
} AbVerifyBaselineState;

typedef struct AbVerifyObservationEvidence {
  AbString role;
  AbString path;
  AbString sha256;
} AbVerifyObservationEvidence;

typedef struct AbVerifyEqualityPolicy {
  const AbValue *source;
  const AbString *kind;
  double atol;
  double rtol;
  uint64_t max_ulp;
  int has_max_ulp;
  int nan_equal;
  int signed_zero_equal;
} AbVerifyEqualityPolicy;

typedef struct AbVerifyObservationView {
  const AbString *route;
  const AbString *phase;
  const AbString *kind;
  const AbString *type_name;
  const AbValue *value;
} AbVerifyObservationView;

typedef struct AbVerifyObservationCase {
  const AbString *id;
  const AbValue *requirements;
  const AbValue *input;
  const AbValue *required_capabilities;
  const AbValue *requires_parameters;
  AbVerifyEqualityPolicy comparison;
  AbVerifyObservationView *observations;
  size_t observation_count;
} AbVerifyObservationCase;

typedef struct AbVerifyObservationDocument {
  const AbValue *root;
  const AbString *id;
  const AbString *project;
  const AbString *revision;
  const AbString *map_input_sha256;
  AbVerifyObservationEvidence *evidence;
  size_t evidence_count;
  const AbString *evidence_slice_sha256;
  const AbString *profile;
  const AbValue *capabilities;
  const AbValue *parameters;
  AbVerifyObservationCase *cases;
  size_t case_count;
  char sha256[65];
} AbVerifyObservationDocument;

typedef struct AbVerifyObservationState {
  AbString name;
  AbString project;
  AbString state;
  AbString message;
  int whole_map_matches;
  int has_data;
  AbVerifyObservationDocument data;
  AbVerifyEvidence *witnesses;
  size_t witness_count;
  size_t witness_capacity;
} AbVerifyObservationState;

typedef struct AbVerificationContext {
  ArchbirdEngine *engine;
  const AbValue *project_configuration;
  const AbValue *project;
  const AbValue *description;
  const AbValue *operand_definitions;
  const AbValue *mappings;
  const AbValue *constraint_plans;
  const AbValue *constraint_policy;
  const AbValue *policy_date;
  const AbValue *baseline_input;
  const AbValue *additional_maps;
  const AbValue *current_map;
  const AbValue *current_resolution;
  char constraint_policy_sha256[65];
  AbVerifyFactSet *facts;
  size_t fact_count;
  struct AbVerifyCheckResult *checks;
  size_t check_count;
  AbVerifyDiagnostic *diagnostics;
  size_t diagnostic_count;
  size_t diagnostic_capacity;
  AbVerifyBaselineState baseline;
  AbVerifyObservationState *observations;
  size_t observation_count;
} AbVerificationContext;
ArchbirdStatus ab_projection_extract_map(ArchbirdEngine *engine,
                                         const AbValue *map,
                                         const AbValue *resolution,
                                         const AbObjectField *projection,
                                         AbVerifyFactSet *out);
ArchbirdStatus ab_projection_extract_literal(ArchbirdEngine *engine,
                                             const AbObjectField *operand,
                                             AbVerifyFactSet *out);
/* Decode one canonical verification-result fact and verify its content hash. */
ArchbirdStatus ab_verify_fact_decode_artifact(ArchbirdEngine *engine,
                                              const AbValue *value,
                                              AbVerifyFactSet *out);
ArchbirdStatus ab_verify_evidence_decode_artifact(ArchbirdEngine *engine,
                                                  const AbValue *value,
                                                  AbVerifyEvidence *out);
ArchbirdStatus ab_constraints_render_summary(AbVerificationContext *context,
                                             AbBuffer *buffer);
ArchbirdStatus ab_verify_render_diagnostics(AbVerificationContext *context,
                                            AbBuffer *buffer);
ArchbirdStatus ab_constraints_render_baseline(
    AbVerificationContext *context, const char *owner, size_t owner_length,
    const char *rationale, size_t rationale_length, AbBuffer *buffer);
ArchbirdStatus ab_verify_add_diagnostic(AbVerificationContext *context,
                                        const char *severity, const char *code,
                                        const char *message,
                                        size_t message_length, const char *path,
                                        size_t path_length);
ArchbirdStatus
ab_verify_collect_project_diagnostics(AbVerificationContext *context);
void ab_verify_diagnostics_finish(AbVerificationContext *context);
void ab_verify_diagnostics_free(AbVerificationContext *context);
ArchbirdStatus ab_constraints_apply_baseline(AbVerificationContext *context);
void ab_verify_baseline_free(ArchbirdEngine *engine,
                             AbVerifyBaselineState *baseline);
ArchbirdStatus ab_constraints_observations_load(AbVerificationContext *context,
                                                const AbValue *observations);
void ab_constraints_observations_free(AbVerificationContext *context);
ArchbirdStatus
ab_constraints_observations_render(AbVerificationContext *context,
                                   AbBuffer *buffer);
const AbVerifyObservationState *
ab_constraints_observation_find(const AbVerificationContext *context,
                                const AbString *name);
int ab_constraints_observation_case_applicable(
    const AbVerifyObservationCase *case_view,
    const AbVerifyObservationDocument *observation);
int ab_constraints_observation_values_equal(
    const AbVerifyObservationView *expected,
    const AbVerifyObservationView *actual,
    const AbVerifyEqualityPolicy *policy);
void ab_verification_context_free(AbVerificationContext *context);

ArchbirdStatus ab_verify_item_init(ArchbirdEngine *engine,
                                   AbVerifyFactItem *item, const AbString *key,
                                   const AbString *label, const AbValue *value);
ArchbirdStatus ab_verify_item_add_evidence(ArchbirdEngine *engine,
                                           AbVerifyFactItem *item,
                                           const AbVerifyEvidence *source);
ArchbirdStatus ab_verify_item_set_state(ArchbirdEngine *engine,
                                        AbVerifyFactItem *item,
                                        const char *state, const char *message);
ArchbirdStatus ab_verify_normalized_name(ArchbirdEngine *engine,
                                         const AbValue *spec,
                                         const AbString *raw, AbString *out,
                                         int *selected);

#endif
