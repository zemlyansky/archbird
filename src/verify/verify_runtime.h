#ifndef ARCHBIRD_VERIFY_RUNTIME_H
#define ARCHBIRD_VERIFY_RUNTIME_H

#include "verify_model.h"

typedef struct AbVerifyInputView {
  ArchbirdEngine *engine;
  const AbValue *root;
  const AbValue *suite_path;
  const AbValue *projects;
  const AbValue *provided_facts;
  const AbValue *attestations;
  const AbValue *baseline;
} AbVerifyInputView;

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

typedef struct AbVerifyAttestationEvidenceView {
  AbString role;
  AbString path;
  AbString sha256;
} AbVerifyAttestationEvidenceView;

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

typedef struct AbVerifyAttestationCaseView {
  const AbString *id;
  const AbValue *requirements;
  const AbValue *input;
  const AbValue *required_capabilities;
  const AbValue *requires_parameters;
  AbVerifyEqualityPolicy comparison;
  AbVerifyObservationView *observations;
  size_t observation_count;
} AbVerifyAttestationCaseView;

typedef struct AbVerifyAttestationDataView {
  const AbValue *root;
  const AbString *suite;
  const AbString *project;
  const AbString *revision;
  const AbString *map_input_sha256;
  AbVerifyAttestationEvidenceView *evidence;
  size_t evidence_count;
  const AbString *evidence_slice_sha256;
  const AbString *profile;
  const AbValue *capabilities;
  const AbValue *parameters;
  AbVerifyAttestationCaseView *cases;
  size_t case_count;
  char sha256[65];
} AbVerifyAttestationDataView;

typedef struct AbVerifyAttestationState {
  AbString name;
  AbString project;
  AbString state;
  AbString message;
  int whole_map_matches;
  int has_data;
  AbVerifyAttestationDataView data;
  AbVerifyEvidence *witnesses;
  size_t witness_count;
  size_t witness_capacity;
} AbVerifyAttestationState;

typedef struct AbVerificationContext {
  ArchbirdEngine *engine;
  AbVerifySuiteView suite;
  AbVerifyInputView input;
  AbVerifyFactSet *facts;
  size_t fact_count;
  struct AbVerifyCheckResult *checks;
  size_t check_count;
  AbVerifyDiagnostic *diagnostics;
  size_t diagnostic_count;
  size_t diagnostic_capacity;
  AbVerifyBaselineState baseline;
  AbVerifyAttestationState *attestations;
  size_t attestation_count;
} AbVerificationContext;

typedef enum AbVerifySourceLockState {
  AB_VERIFY_SOURCE_LOCK_NOT_DECLARED = 0,
  AB_VERIFY_SOURCE_LOCK_CURRENT = 1,
  AB_VERIFY_SOURCE_LOCK_MISMATCH = 2,
  AB_VERIFY_SOURCE_LOCK_UNAVAILABLE = 3
} AbVerifySourceLockState;

AbVerifySourceLockState
ab_verify_source_lock_state(const AbVerificationContext *context,
                            const AbString *project);
const char *ab_verify_source_lock_state_name(AbVerifySourceLockState state);
int ab_verify_source_lock_observed_sha256(const AbVerificationContext *context,
                                          const AbString *project,
                                          const AbString *path,
                                          char output[65]);

ArchbirdStatus ab_verify_input_validate(ArchbirdEngine *engine,
                                        const AbVerifySuiteView *suite,
                                        const AbValue *root,
                                        AbVerifyInputView *out);

const AbValue *ab_verify_input_project(const AbVerifyInputView *input,
                                       const AbString *name);
const AbValue *ab_verify_input_source(const AbValue *project,
                                      const AbString *path);
const AbValue *ab_verify_input_provided_fact(const AbVerifyInputView *input,
                                             const AbString *name);
const AbValue *ab_verify_input_attestation(const AbVerifyInputView *input,
                                           const AbString *name);

ArchbirdStatus ab_verify_extract_all(AbVerificationContext *context);
/* Decode one canonical verification-result fact and verify its content hash. */
ArchbirdStatus ab_verify_fact_decode_artifact(ArchbirdEngine *engine,
                                              const AbValue *value,
                                              AbVerifyFactSet *out);
ArchbirdStatus ab_verify_evidence_decode_artifact(ArchbirdEngine *engine,
                                                  const AbValue *value,
                                                  AbVerifyEvidence *out);
ArchbirdStatus ab_verify_extract_c(AbVerificationContext *context,
                                   const AbObjectField *extractor,
                                   AbVerifyFactSet *fact);
ArchbirdStatus ab_verify_render_result(AbVerificationContext *context,
                                       AbBuffer *buffer);
ArchbirdStatus ab_verification_context_analyze(
    ArchbirdEngine *engine, const uint8_t *suite_json, size_t suite_length,
    const uint8_t *verification_input_json, size_t verification_input_length,
    AbValue *suite_document, AbValue *input_document,
    AbVerificationContext *context);
ArchbirdStatus ab_verification_context_prepare(
    ArchbirdEngine *engine, const uint8_t *suite_json, size_t suite_length,
    const uint8_t *verification_input_json, size_t verification_input_length,
    AbValue *suite_document, AbValue *input_document,
    AbVerificationContext *context);
ArchbirdStatus ab_verification_context_evaluate(AbVerificationContext *context);
ArchbirdStatus ab_verify_add_diagnostic(AbVerificationContext *context,
                                        const char *severity, const char *code,
                                        const char *message,
                                        size_t message_length, const char *path,
                                        size_t path_length);
ArchbirdStatus
ab_verify_collect_project_diagnostics(AbVerificationContext *context);
void ab_verify_diagnostics_finish(AbVerificationContext *context);
void ab_verify_diagnostics_free(AbVerificationContext *context);
ArchbirdStatus ab_verify_apply_baseline(AbVerificationContext *context);
void ab_verify_baseline_free(ArchbirdEngine *engine,
                             AbVerifyBaselineState *baseline);
ArchbirdStatus ab_verify_attestations_load(AbVerificationContext *context);
void ab_verify_attestations_free(AbVerificationContext *context);
ArchbirdStatus ab_verify_attestations_render(AbVerificationContext *context,
                                             AbBuffer *buffer);
const AbVerifyAttestationState *
ab_verify_attestation_find(const AbVerificationContext *context,
                           const AbString *name);
int ab_verify_attestation_case_applicable(
    const AbVerifyAttestationCaseView *case_view,
    const AbVerifyAttestationDataView *attestation);
int ab_verify_attestation_observations_equal(
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
