#ifndef ARCHBIRD_OKF_PUBLISH_INTERNAL_H
#define ARCHBIRD_OKF_PUBLISH_INTERNAL_H

#include "act_internal.h"
#include "json_value.h"
#include "render_internal.h"
#include "sha256.h"

typedef struct AbOkfPubSource {
  const char *artifact;
  const AbValue *root;
  const AbValue *tool;
  uint64_t schema_version;
  AbString evidence_sha256;
  char file_sha256[65];
} AbOkfPubSource;

typedef struct AbOkfPubRelation {
  AbString json;
} AbOkfPubRelation;

typedef struct AbOkfPubRelationList {
  AbOkfPubRelation *items;
  size_t count;
  size_t capacity;
} AbOkfPubRelationList;

typedef struct AbOkfPubField {
  const char *name;
  AbString json;
} AbOkfPubField;

typedef struct AbOkfPubConcept {
  AbString path;
  AbString type_name;
  AbString title;
  AbString sort_key;
  AbString description;
  AbString text;
} AbOkfPubConcept;

typedef struct AbOkfPubFile {
  AbString path;
  AbString text;
  char sha256[65];
} AbOkfPubFile;

typedef struct AbOkfPubNormalization {
  AbValue root;
  const AbValue *rows;
} AbOkfPubNormalization;

typedef struct AbOkfPublication {
  ArchbirdEngine *engine;
  AbValue map;
  AbOkfPubNormalization normalization;
  AbOkfPubSource map_source;
  AbActVerification verification;
  AbOkfPubSource verification_source;
  int has_verification;
  AbActProposalView proposal;
  AbOkfPubSource proposal_source;
  int has_proposal;
  AbActContractView contract;
  AbOkfPubSource contract_source;
  int has_contract;
  AbValue result;
  AbOkfPubSource result_source;
  int has_result;
  AbOkfPubConcept *concepts;
  size_t concept_count;
  size_t concept_capacity;
  AbOkfPubFile *files;
  size_t file_count;
  size_t file_capacity;
  AbString project;
  AbString content_sha256;
  AbString aggregate_sha256;
} AbOkfPublication;

typedef struct AbOkfConceptSpec {
  const AbOkfPubSource *source;
  const char *path;
  const char *type_name;
  const AbString *title;
  const AbString *sort_key;
  const AbString *description;
  const char *provenance;
  const char *entity_kind;
  const AbString *entity_id;
  const AbString *tags;
  size_t tag_count;
  const AbOkfPubRelationList *relations;
  const AbOkfPubField *extra;
  size_t extra_count;
  const AbBuffer *body;
} AbOkfConceptSpec;

ArchbirdStatus ab_okf_pub_error(AbOkfPublication *pub, const char *message);
const AbValue *ab_okf_pub_member(const AbValue *object, const char *name,
                                 AbValueKind kind);
const AbString *ab_okf_pub_text(const AbValue *object, const char *name);
const AbValue *ab_okf_pub_array(const AbValue *object, const char *name);
int ab_okf_pub_u64(const AbValue *object, const char *name, uint64_t *out);

ArchbirdStatus ab_okf_pub_copy(AbOkfPublication *pub, AbString *out,
                               const char *data, size_t length);
ArchbirdStatus ab_okf_pub_literal(AbOkfPublication *pub, AbString *out,
                                  const char *value);
ArchbirdStatus ab_okf_pub_buffer_string(AbOkfPublication *pub,
                                        const AbBuffer *buffer, AbString *out);
ArchbirdStatus ab_okf_pub_sha256(const uint8_t *data, size_t length,
                                 char output[65]);
ArchbirdStatus ab_okf_pub_value_digest(AbOkfPublication *pub,
                                       const AbValue *value, char output[65]);

ArchbirdStatus ab_okf_pub_one_line(AbOkfPublication *pub, const AbString *value,
                                   AbString *out);
ArchbirdStatus ab_okf_pub_body(AbOkfPublication *pub, const AbBuffer *value,
                               AbString *out);
ArchbirdStatus ab_okf_pub_plain(AbBuffer *buffer, const AbString *value);
ArchbirdStatus ab_okf_pub_code(AbBuffer *buffer, const AbString *value);
ArchbirdStatus ab_okf_pub_json_code(AbBuffer *buffer, const AbValue *value);
ArchbirdStatus ab_okf_pub_url(AbBuffer *buffer, const AbString *value);
ArchbirdStatus ab_okf_pub_slug(AbOkfPublication *pub, const AbString *value,
                               AbString *out);
ArchbirdStatus ab_okf_pub_sort_key(AbOkfPublication *pub, const AbString *value,
                                   AbString *out);

ArchbirdStatus ab_okf_pub_relative_link(AbBuffer *buffer,
                                        const char *source_path,
                                        const char *target_path,
                                        const AbString *label);
ArchbirdStatus ab_okf_pub_external_link(AbBuffer *buffer,
                                        const AbString *target,
                                        const AbString *label);

void ab_okf_pub_relations_free(AbOkfPublication *pub,
                               AbOkfPubRelationList *relations);
ArchbirdStatus ab_okf_pub_relation(AbOkfPublication *pub,
                                   AbOkfPubRelationList *relations,
                                   const AbOkfPubField *fields,
                                   size_t field_count);
ArchbirdStatus ab_okf_pub_relation_simple(AbOkfPublication *pub,
                                          AbOkfPubRelationList *relations,
                                          const char *kind, const char *target);
void ab_okf_pub_fields_free(AbOkfPublication *pub, AbOkfPubField *fields,
                            size_t count);
ArchbirdStatus ab_okf_pub_json_text(AbOkfPublication *pub, AbString *out,
                                    const AbString *value);
ArchbirdStatus ab_okf_pub_json_literal(AbOkfPublication *pub, AbString *out,
                                       const char *value);
ArchbirdStatus ab_okf_pub_json_u64(AbOkfPublication *pub, AbString *out,
                                   uint64_t value);
ArchbirdStatus ab_okf_pub_json_bool(AbOkfPublication *pub, AbString *out,
                                    int value);
ArchbirdStatus ab_okf_pub_json_value(AbOkfPublication *pub, AbString *out,
                                     const AbValue *value);
ArchbirdStatus ab_okf_pub_json_string_array(AbOkfPublication *pub,
                                            AbString *out,
                                            const AbValue *values);

ArchbirdStatus ab_okf_pub_add_concept(AbOkfPublication *pub,
                                      const AbOkfConceptSpec *spec);
ArchbirdStatus ab_okf_pub_add_file(AbOkfPublication *pub, const char *path,
                                   const uint8_t *text, size_t length);
const AbOkfPubConcept *ab_okf_pub_find_concept(const AbOkfPublication *pub,
                                               const char *path);

ArchbirdStatus
ab_okf_pub_load(AbOkfPublication *pub, const uint8_t *map_json,
                size_t map_length, const uint8_t *verification_json,
                size_t verification_length, const uint8_t *proposal_json,
                size_t proposal_length, const uint8_t *contract_json,
                size_t contract_length, const uint8_t *result_json,
                size_t result_length, const uint8_t *normalization_json,
                size_t normalization_length);
void ab_okf_pub_free(AbOkfPublication *pub);

ArchbirdStatus ab_okf_pub_map(AbOkfPublication *pub);
ArchbirdStatus ab_okf_pub_verify(AbOkfPublication *pub);
ArchbirdStatus ab_okf_pub_act(AbOkfPublication *pub);
ArchbirdStatus ab_okf_pub_finish(AbOkfPublication *pub, AbBuffer *out);

#endif
