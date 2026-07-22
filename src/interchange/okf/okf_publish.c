#include "okf_publish_internal.h"

#include <stdlib.h>
#include <string.h>

static ArchbirdStatus invalid(AbOkfPublication *pub, const char *message) {
  return ab_okf_pub_error(pub, message);
}

static int lowercase_sha256(const AbString *value) {
  size_t index;
  if (!value || value->length != 64)
    return 0;
  for (index = 0; index < value->length; index++) {
    unsigned char byte = (unsigned char)value->data[index];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return 0;
  }
  return 1;
}

static int nonblank(const AbString *value) {
  size_t index;
  if (!value || !value->length)
    return 0;
  for (index = 0; index < value->length; index++) {
    unsigned char byte = (unsigned char)value->data[index];
    if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n')
      return 1;
  }
  return 0;
}

static int tool_valid(const AbValue *tool) {
  const AbString *name = ab_okf_pub_text(tool, "name");
  const AbString *version = ab_okf_pub_text(tool, "version");
  const AbString *digest = ab_okf_pub_text(tool, "implementation_sha256");
  return name && name->length == 8 && !memcmp(name->data, "archbird", 8) &&
         nonblank(version) && lowercase_sha256(digest);
}

static ArchbirdStatus source_init(AbOkfPublication *pub, AbOkfPubSource *source,
                                  const char *artifact, const AbValue *root,
                                  const uint8_t *raw, size_t raw_length,
                                  const AbString *evidence) {
  ArchbirdStatus status;
  uint64_t schema;
  const AbValue *tool = ab_value_member(root, "tool");
  if (!ab_okf_pub_u64(root, "schema_version", &schema) || !tool_valid(tool) ||
      !lowercase_sha256(evidence))
    return invalid(pub, "invalid OKF source artifact identity");
  memset(source, 0, sizeof(*source));
  source->artifact = artifact;
  source->root = root;
  source->tool = tool;
  source->schema_version = schema;
  status = ab_okf_pub_copy(pub, &source->evidence_sha256, evidence->data,
                           evidence->length);
  if (status != ARCHBIRD_OK)
    return status;
  return ab_okf_pub_sha256(raw, raw_length, source->file_sha256);
}

static ArchbirdStatus load_normalization(AbOkfPublication *pub,
                                         const uint8_t *json, size_t length) {
  const AbValue *schema;
  uint64_t version;
  size_t index;
  size_t previous;
  ArchbirdStatus status;
  if (!json && !length)
    return ARCHBIRD_OK;
  status =
      ab_json_value_decode(pub->engine, json, length, &pub->normalization.root);
  if (status != ARCHBIRD_OK)
    return status;
  schema = ab_value_member(&pub->normalization.root, "schema_version");
  pub->normalization.rows = ab_okf_pub_array(&pub->normalization.root, "rows");
  if (!ab_value_u64(schema, &version) || version != 1 ||
      !ab_value_string_is(ab_value_member(&pub->normalization.root, "artifact"),
                          "okf-text-normalization") ||
      !pub->normalization.rows)
    return invalid(pub, "invalid OKF text-normalization artifact");
  for (index = 0; index < pub->normalization.rows->as.array.count; index++) {
    const AbValue *row = &pub->normalization.rows->as.array.items[index];
    const AbString *text = ab_okf_pub_text(row, "text");
    const AbString *slug = ab_okf_pub_text(row, "slug_ascii");
    const AbString *folded = ab_okf_pub_text(row, "casefold");
    size_t byte;
    if (!text || !slug || !folded)
      return invalid(pub, "invalid OKF text-normalization row");
    for (byte = 0; byte < slug->length; byte++)
      if ((unsigned char)slug->data[byte] >= 0x80)
        return invalid(pub, "OKF slug normalization must be ASCII");
    for (previous = 0; previous < index; previous++) {
      const AbString *other = ab_okf_pub_text(
          &pub->normalization.rows->as.array.items[previous], "text");
      if (other && ab_string_equal(other, text))
        return invalid(pub, "duplicate OKF text-normalization row");
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus load_map(AbOkfPublication *pub, const uint8_t *json,
                               size_t length) {
  const AbValue *schema;
  const AbValue *evidence;
  const AbString *project;
  const AbString *input;
  uint64_t version;
  ArchbirdStatus status =
      ab_json_value_decode(pub->engine, json, length, &pub->map);
  if (status != ARCHBIRD_OK)
    return status;
  schema = ab_value_member(&pub->map, "schema_version");
  evidence = ab_okf_pub_member(&pub->map, "evidence", AB_VALUE_OBJECT);
  project = ab_okf_pub_text(&pub->map, "project");
  input = ab_okf_pub_text(evidence, "input_sha256");
  if (!ab_value_u64(schema, &version) || version < ARCHBIRD_MAP_SCHEMA_MIN ||
      version > ARCHBIRD_MAP_SCHEMA_CURRENT ||
      !ab_value_string_is(ab_value_member(&pub->map, "artifact"), "map") ||
      !project || !nonblank(project) || !evidence || !input ||
      !lowercase_sha256(input) ||
      !lowercase_sha256(ab_okf_pub_text(evidence, "config_sha256")) ||
      !tool_valid(ab_value_member(&pub->map, "tool")))
    return invalid(pub, "invalid canonical saved Map for OKF publication");
  status = ab_okf_pub_copy(pub, &pub->project, project->data, project->length);
  if (status == ARCHBIRD_OK)
    status = source_init(pub, &pub->map_source, "map", &pub->map, json, length,
                         input);
  return status;
}

static int text_equal(const AbString *left, const AbString *right) {
  return left && right && ab_string_equal(left, right);
}

static int verification_matches_map(const AbValue *evaluation,
                                    const AbOkfPublication *pub) {
  const AbValue *map_evidence = ab_value_member(&pub->map, "evidence");
  const AbValue *map_tool = ab_value_member(&pub->map, "tool");
  return text_equal(ab_okf_pub_text(evaluation, "project"),
                    ab_okf_pub_text(&pub->map, "project")) &&
         text_equal(ab_okf_pub_text(evaluation, "map_input_sha256"),
                    ab_okf_pub_text(map_evidence, "input_sha256")) &&
         text_equal(ab_okf_pub_text(evaluation, "map_config_sha256"),
                    ab_okf_pub_text(map_evidence, "config_sha256")) &&
         text_equal(
             ab_okf_pub_text(evaluation, "map_producer_implementation_sha256"),
             ab_okf_pub_text(map_tool, "implementation_sha256"));
}

static ArchbirdStatus load_verification(AbOkfPublication *pub,
                                        const uint8_t *json, size_t length) {
  const AbString *digest;
  ArchbirdStatus status =
      ab_act_verification_load(pub->engine, json, length, &pub->verification);
  if (status != ARCHBIRD_OK)
    return status;
  if (!verification_matches_map(pub->verification.evaluation, pub))
    return invalid(pub,
                   "OKF verification evaluation does not match Map identity");
  digest = &(AbString){pub->verification.sha256, 64};
  status = source_init(pub, &pub->verification_source, "verification",
                       &pub->verification.root, json, length, digest);
  if (status == ARCHBIRD_OK)
    pub->has_verification = 1;
  return status;
}

static ArchbirdStatus load_proposal(AbOkfPublication *pub, const uint8_t *json,
                                    size_t length) {
  const AbString *verification_sha;
  AbString digest;
  ArchbirdStatus status;
  if (!pub->has_verification)
    return invalid(pub, "OKF proposal requires verification");
  status = ab_act_proposal_load(pub->engine, json, length, &pub->proposal);
  if (status != ARCHBIRD_OK)
    return status;
  verification_sha =
      ab_okf_pub_text(pub->proposal.source, "verification_sha256");
  if (!verification_sha || verification_sha->length != 64 ||
      memcmp(verification_sha->data, pub->verification.sha256, 64))
    return invalid(pub, "OKF proposal verification identity mismatch");
  digest = (AbString){pub->proposal.sha256, 64};
  status = source_init(pub, &pub->proposal_source, "change-proposal",
                       &pub->proposal.root, json, length, &digest);
  if (status == ARCHBIRD_OK)
    pub->has_proposal = 1;
  return status;
}

static const AbString *proposal_constraint(const AbActProposalView *proposal) {
  return ab_okf_pub_text(proposal->origin, "constraint");
}

static const AbString *proposal_fingerprint(const AbActProposalView *proposal) {
  const AbValue *finding =
      ab_okf_pub_member(proposal->origin, "finding", AB_VALUE_OBJECT);
  return ab_okf_pub_text(finding, "fingerprint");
}

static ArchbirdStatus load_contract(AbOkfPublication *pub, const uint8_t *json,
                                    size_t length) {
  const AbString *proposal_sha;
  const AbString *origin_constraint;
  const AbString *origin_fingerprint;
  AbString digest;
  ArchbirdStatus status;
  if (!pub->has_proposal)
    return invalid(pub, "OKF contract requires proposal");
  status = ab_act_contract_load(pub->engine, json, length, &pub->contract);
  if (status != ARCHBIRD_OK)
    return status;
  proposal_sha = ab_okf_pub_text(&pub->contract.root, "proposal_sha256");
  origin_constraint = ab_okf_pub_text(pub->contract.origin, "constraint");
  origin_fingerprint = ab_okf_pub_text(pub->contract.origin, "fingerprint");
  if (!proposal_sha || proposal_sha->length != 64 ||
      memcmp(proposal_sha->data, pub->proposal.sha256, 64) ||
      !text_equal(origin_constraint, proposal_constraint(&pub->proposal)) ||
      !text_equal(origin_fingerprint, proposal_fingerprint(&pub->proposal)))
    return invalid(pub, "OKF contract does not match proposal");
  digest = (AbString){pub->contract.sha256, 64};
  status = source_init(pub, &pub->contract_source, "change-contract",
                       &pub->contract.root, json, length, &digest);
  if (status == ARCHBIRD_OK)
    pub->has_contract = 1;
  return status;
}

static int string_array(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < value->as.array.count; index++)
    if (value->as.array.items[index].kind != AB_VALUE_STRING)
      return 0;
  return 1;
}

static int sealed_result(AbOkfPublication *pub, const AbValue *root,
                         char digest[65]) {
  const AbString *declared = ab_okf_pub_text(root, "sha256");
  if (!lowercase_sha256(declared) ||
      ab_act_value_digest_without_sha256(pub->engine, root, digest) !=
          ARCHBIRD_OK)
    return 0;
  return !memcmp(declared->data, digest, 64);
}

static ArchbirdStatus load_result(AbOkfPublication *pub, const uint8_t *json,
                                  size_t length) {
  const AbValue *outcomes;
  const AbString *proposal_sha;
  const AbString *contract_sha;
  const AbString *before_sha;
  const AbString *status_text;
  const AbString *freshness;
  char digest_hex[65];
  AbString digest;
  size_t index;
  uint64_t schema_version;
  ArchbirdStatus status;
  if (!pub->has_verification || !pub->has_proposal || !pub->has_contract)
    return invalid(pub,
                   "OKF result requires verification, proposal, and contract");
  status = ab_json_value_decode(pub->engine, json, length, &pub->result);
  if (status != ARCHBIRD_OK)
    return status;
  outcomes = ab_okf_pub_array(&pub->result, "outcomes");
  proposal_sha = ab_okf_pub_text(&pub->result, "proposal_sha256");
  contract_sha = ab_okf_pub_text(&pub->result, "contract_sha256");
  before_sha = ab_okf_pub_text(&pub->result, "before_verification_sha256");
  status_text = ab_okf_pub_text(&pub->result, "status");
  freshness = ab_okf_pub_text(&pub->result, "freshness");
  if (!ab_value_string_is(ab_value_member(&pub->result, "artifact"),
                          "change-result") ||
      !ab_value_u64(ab_value_member(&pub->result, "schema_version"),
                    &schema_version) ||
      schema_version != 2 ||
      !ab_value_string_is(ab_value_member(&pub->result, "provenance"),
                          "derived") ||
      !tool_valid(ab_value_member(&pub->result, "tool")) || !outcomes ||
      !string_array(ab_value_member(&pub->result, "diagnostics")) ||
      !proposal_sha || proposal_sha->length != 64 ||
      memcmp(proposal_sha->data, pub->proposal.sha256, 64) || !contract_sha ||
      contract_sha->length != 64 ||
      memcmp(contract_sha->data, pub->contract.sha256, 64) || !before_sha ||
      before_sha->length != 64 ||
      memcmp(before_sha->data, pub->verification.sha256, 64) ||
      !nonblank(status_text) || !nonblank(freshness) ||
      !sealed_result(pub, &pub->result, digest_hex))
    return invalid(pub, "invalid or mismatched OKF change result");
  for (index = 0; index < outcomes->as.array.count; index++) {
    const AbValue *row = &outcomes->as.array.items[index];
    if (!nonblank(ab_okf_pub_text(row, "id")) ||
        !nonblank(ab_okf_pub_text(row, "kind")) ||
        !nonblank(ab_okf_pub_text(row, "status")) ||
        !nonblank(ab_okf_pub_text(row, "message")) ||
        !ab_okf_pub_array(row, "evidence"))
      return invalid(pub, "invalid OKF change-result outcome");
  }
  digest = (AbString){digest_hex, 64};
  status = source_init(pub, &pub->result_source, "change-result", &pub->result,
                       json, length, &digest);
  if (status == ARCHBIRD_OK)
    pub->has_result = 1;
  return status;
}

ArchbirdStatus
ab_okf_pub_load(AbOkfPublication *pub, const uint8_t *map_json,
                size_t map_length, const uint8_t *verification_json,
                size_t verification_length, const uint8_t *proposal_json,
                size_t proposal_length, const uint8_t *contract_json,
                size_t contract_length, const uint8_t *result_json,
                size_t result_length, const uint8_t *normalization_json,
                size_t normalization_length) {
  ArchbirdStatus status;
  if (!pub || !pub->engine || (!map_json && map_length) || !map_json ||
      (!verification_json && verification_length) ||
      (!proposal_json && proposal_length) ||
      (!contract_json && contract_length) || (!result_json && result_length) ||
      (!normalization_json && normalization_length))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = load_normalization(pub, normalization_json, normalization_length);
  if (status == ARCHBIRD_OK)
    status = load_map(pub, map_json, map_length);
  if (status == ARCHBIRD_OK && (verification_json || verification_length))
    status = load_verification(pub, verification_json, verification_length);
  if (status == ARCHBIRD_OK && (proposal_json || proposal_length))
    status = load_proposal(pub, proposal_json, proposal_length);
  if (status == ARCHBIRD_OK && (contract_json || contract_length))
    status = load_contract(pub, contract_json, contract_length);
  if (status == ARCHBIRD_OK && (result_json || result_length))
    status = load_result(pub, result_json, result_length);
  if (status == ARCHBIRD_OK && contract_json && !proposal_json)
    status = invalid(pub, "OKF contract requires proposal");
  if (status == ARCHBIRD_OK && result_json &&
      (!verification_json || !proposal_json || !contract_json))
    status = invalid(pub, "OKF result requires complete source chain");
  return status;
}

void ab_okf_pub_free(AbOkfPublication *pub) {
  size_t index;
  if (!pub)
    return;
  for (index = 0; index < pub->concept_count; index++) {
    AbOkfPubConcept *row = &pub->concepts[index];
    ab_string_free(pub->engine, &row->path);
    ab_string_free(pub->engine, &row->type_name);
    ab_string_free(pub->engine, &row->title);
    ab_string_free(pub->engine, &row->sort_key);
    ab_string_free(pub->engine, &row->description);
    ab_string_free(pub->engine, &row->text);
  }
  for (index = 0; index < pub->file_count; index++) {
    ab_string_free(pub->engine, &pub->files[index].path);
    ab_string_free(pub->engine, &pub->files[index].text);
  }
  ab_free(pub->engine, pub->concepts);
  ab_free(pub->engine, pub->files);
  ab_string_free(pub->engine, &pub->project);
  ab_string_free(pub->engine, &pub->content_sha256);
  ab_string_free(pub->engine, &pub->aggregate_sha256);
  ab_string_free(pub->engine, &pub->result_source.evidence_sha256);
  ab_string_free(pub->engine, &pub->contract_source.evidence_sha256);
  ab_string_free(pub->engine, &pub->proposal_source.evidence_sha256);
  ab_string_free(pub->engine, &pub->verification_source.evidence_sha256);
  ab_string_free(pub->engine, &pub->map_source.evidence_sha256);
  ab_value_free(pub->engine, &pub->result);
  ab_act_contract_view_free(&pub->contract);
  ab_act_proposal_view_free(&pub->proposal);
  ab_act_verification_free(&pub->verification);
  ab_value_free(pub->engine, &pub->map);
  ab_value_free(pub->engine, &pub->normalization.root);
  memset(pub, 0, sizeof(*pub));
}

ArchbirdStatus
archbird_okf_publish(ArchbirdEngine *engine, const uint8_t *map_json,
                     size_t map_length, const uint8_t *verification_json,
                     size_t verification_length, const uint8_t *proposal_json,
                     size_t proposal_length, const uint8_t *contract_json,
                     size_t contract_length, const uint8_t *result_json,
                     size_t result_length, const uint8_t *normalization_json,
                     size_t normalization_length, uint32_t json_flags,
                     ArchbirdWriteFn write_fn, void *user_data) {
  AbOkfPublication pub = {0};
  AbBuffer output;
  ArchbirdStatus status;
  if (!engine || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  pub.engine = engine;
  ab_buffer_init(&output, engine);
  status = ab_okf_pub_load(
      &pub, map_json, map_length, verification_json, verification_length,
      proposal_json, proposal_length, contract_json, contract_length,
      result_json, result_length, normalization_json, normalization_length);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_map(&pub);
  if (status == ARCHBIRD_OK && pub.has_verification)
    status = ab_okf_pub_verify(&pub);
  if (status == ARCHBIRD_OK && pub.has_proposal)
    status = ab_okf_pub_act(&pub);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_finish(&pub, &output);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, output.data, output.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&output);
  ab_okf_pub_free(&pub);
  return status;
}
