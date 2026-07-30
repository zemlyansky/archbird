#ifndef ARCHBIRD_VERIFICATION_ARTIFACT_H
#define ARCHBIRD_VERIFICATION_ARTIFACT_H

/* Strict canonical Verification decoding shared by artifact consumers. */
#include "json_value.h"
#include "projection_model.h"

typedef struct AbVerificationArtifact {
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
} AbVerificationArtifact;

ArchbirdStatus ab_verification_artifact_load(ArchbirdEngine *engine,
                                             const uint8_t *json,
                                             size_t json_length,
                                             AbVerificationArtifact *out);
void ab_verification_artifact_free(AbVerificationArtifact *artifact);
const AbValue *
ab_verification_artifact_constraint(const AbVerificationArtifact *artifact,
                                    const AbString *id);
const AbValue *
ab_verification_artifact_finding(const AbVerificationArtifact *artifact,
                                 const AbString *fingerprint,
                                 const AbValue **out_check);
const AbValue *
ab_verification_artifact_fact_value(const AbVerificationArtifact *artifact,
                                    const AbString *name,
                                    const AbProjectionData **out_fact);
const AbValue *ab_verification_artifact_operand_definition(
    const AbVerificationArtifact *artifact, const AbString *name);
const AbValue *
ab_verification_artifact_mapping(const AbVerificationArtifact *artifact,
                                 const AbString *name);
const AbValue *
ab_verification_artifact_observation(const AbVerificationArtifact *artifact,
                                     const AbString *name);
ArchbirdStatus ab_verification_finding_validate(ArchbirdEngine *engine,
                                                const AbValue *row);

#endif
