#include "path/path_artifact.h"

#include "base/artifact_validation.h"
#include "projection/projection_model.h"

#include <string.h>

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "%s", message);
}

static int string_in(const AbValue *value, const char *const *values,
                     size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    if (ab_value_string_is(value, values[index]))
      return 1;
  return 0;
}

static int nonempty_string(const AbValue *value) {
  return value && value->kind == AB_VALUE_STRING && value->as.text.length;
}

static int sorted_unique_nonempty_strings(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_ARRAY || !value->as.array.count)
    return 0;
  for (index = 0; index < value->as.array.count; index++) {
    const AbValue *item = &value->as.array.items[index];
    if (!nonempty_string(item) ||
        (index && ab_string_compare(&value->as.array.items[index - 1].as.text,
                                    &item->as.text) >= 0))
      return 0;
  }
  return 1;
}

static int endpoint_kind(const AbValue *value) {
  size_t index;
  int separator = 0;
  if (!nonempty_string(value))
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    if (byte == '-') {
      if (!index || separator || index + 1 == value->as.text.length)
        return 0;
      separator = 1;
    } else {
      if (!((byte >= 'a' && byte <= 'z') ||
            (index && byte >= '0' && byte <= '9')))
        return 0;
      separator = 0;
    }
  }
  return 1;
}

static ArchbirdStatus validate_evidence(ArchbirdEngine *engine,
                                        const AbValue *rows) {
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return invalid(engine, "Path projection evidence must be an array");
  for (index = 0; index < rows->as.array.count; index++) {
    AbProjectionEvidence evidence = {0};
    ArchbirdStatus status = ab_projection_evidence_decode_artifact(
        engine, &rows->as.array.items[index], &evidence);
    ab_projection_evidence_free(engine, &evidence);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_projection_item(ArchbirdEngine *engine,
                                               const AbValue *item) {
  static const char *const fields[] = {
      "attributes", "evidence", "key", "label", "message", "state", "value",
  };
  static const char *const states[] = {"current", "stale", "unknown"};
  const AbValue *attributes = ab_value_member(item, "attributes");
  const AbValue *message = ab_value_member(item, "message");
  const AbValue *state = ab_value_member(item, "state");
  if (!ab_artifact_object_exact(item, fields,
                                sizeof(fields) / sizeof(fields[0])) ||
      !attributes || attributes->kind != AB_VALUE_OBJECT ||
      !nonempty_string(ab_value_member(item, "key")) ||
      !nonempty_string(ab_value_member(item, "label")) || !message ||
      message->kind != AB_VALUE_STRING ||
      !string_in(state, states, sizeof(states) / sizeof(states[0])) ||
      !ab_value_member(item, "value"))
    return invalid(engine, "Path contains an invalid projection item");
  return validate_evidence(engine, ab_value_member(item, "evidence"));
}

static ArchbirdStatus validate_request_endpoint(ArchbirdEngine *engine,
                                                const AbValue *endpoint) {
  static const char *const fields[] = {"kind", "patterns"};
  if (!ab_artifact_object_exact(endpoint, fields,
                                sizeof(fields) / sizeof(fields[0])) ||
      !endpoint_kind(ab_value_member(endpoint, "kind")) ||
      !sorted_unique_nonempty_strings(ab_value_member(endpoint, "patterns")))
    return invalid(engine, "Path contains an invalid normalized endpoint");
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_request(ArchbirdEngine *engine,
                                       const AbValue *request) {
  static const char *const fields[] = {
      "artifact",  "direction",       "level",     "max_depth",
      "max_paths", "producer_policy", "relations", "schema_version",
      "source",    "target",
  };
  static const char *const directions[] = {"downstream", "upstream", "both"};
  static const char *const levels[] = {"component", "file", "symbol"};
  static const char *const policies[] = {"compatible", "current"};
  static const char *const relations[] = {
      "bridges", "builds",   "calls",      "declarations",
      "imports", "packages", "references", "tests",
  };
  const AbValue *relation_rows = ab_value_member(request, "relations");
  uint64_t schema;
  uint64_t max_depth;
  uint64_t max_paths;
  size_t index;
  ArchbirdStatus status;
  if (!ab_artifact_object_exact(request, fields,
                                sizeof(fields) / sizeof(fields[0])) ||
      !ab_value_string_is(ab_value_member(request, "artifact"),
                          "path-request") ||
      !ab_artifact_safe_integer(ab_value_member(request, "schema_version"),
                                &schema) ||
      schema != 1 ||
      !string_in(ab_value_member(request, "direction"), directions,
                 sizeof(directions) / sizeof(directions[0])) ||
      !string_in(ab_value_member(request, "level"), levels,
                 sizeof(levels) / sizeof(levels[0])) ||
      !string_in(ab_value_member(request, "producer_policy"), policies,
                 sizeof(policies) / sizeof(policies[0])) ||
      !ab_artifact_safe_integer(ab_value_member(request, "max_depth"),
                                &max_depth) ||
      max_depth > 64 ||
      !ab_artifact_safe_integer(ab_value_member(request, "max_paths"),
                                &max_paths) ||
      !max_paths || max_paths > 100 ||
      !sorted_unique_nonempty_strings(relation_rows))
    return invalid(engine, "Path contains an invalid normalized request");
  for (index = 0; index < relation_rows->as.array.count; index++)
    if (!string_in(&relation_rows->as.array.items[index], relations,
                   sizeof(relations) / sizeof(relations[0])))
      return invalid(engine, "Path request contains an invalid relation");
  if (ab_value_string_is(ab_value_member(request, "level"), "symbol"))
    for (index = 0; index < relation_rows->as.array.count; index++)
      if (!ab_value_string_is(&relation_rows->as.array.items[index], "calls") &&
          !ab_value_string_is(&relation_rows->as.array.items[index],
                              "references"))
        return invalid(engine,
                       "symbol Path request contains a non-symbol relation");
  status =
      validate_request_endpoint(engine, ab_value_member(request, "source"));
  if (status == ARCHBIRD_OK)
    status =
        validate_request_endpoint(engine, ab_value_member(request, "target"));
  return status;
}

static ArchbirdStatus validate_endpoint(ArchbirdEngine *engine,
                                        const AbValue *endpoint) {
  static const char *const fields[] = {"candidates", "state"};
  static const char *const states[] = {"resolved", "ambiguous", "unresolved"};
  const AbValue *candidates = ab_value_member(endpoint, "candidates");
  const AbValue *state = ab_value_member(endpoint, "state");
  size_t index;
  if (!ab_artifact_object_exact(endpoint, fields,
                                sizeof(fields) / sizeof(fields[0])) ||
      !candidates || candidates->kind != AB_VALUE_ARRAY ||
      !string_in(state, states, sizeof(states) / sizeof(states[0])) ||
      (ab_value_string_is(state, "unresolved") &&
       candidates->as.array.count != 0) ||
      (ab_value_string_is(state, "resolved") &&
       candidates->as.array.count != 1) ||
      (ab_value_string_is(state, "ambiguous") &&
       candidates->as.array.count < 2))
    return invalid(engine, "Path contains an inconsistent resolved endpoint");
  for (index = 0; index < candidates->as.array.count; index++) {
    ArchbirdStatus status =
        validate_projection_item(engine, &candidates->as.array.items[index]);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static int strings_equal(const AbValue *left, const AbValue *right) {
  return left && right && left->kind == AB_VALUE_STRING &&
         right->kind == AB_VALUE_STRING &&
         ab_string_equal(&left->as.text, &right->as.text);
}

static const AbValue *endpoint_candidate(const AbValue *endpoint,
                                         const AbValue *node) {
  const AbValue *candidates = ab_value_member(endpoint, "candidates");
  size_t index;
  for (index = 0; index < candidates->as.array.count; index++) {
    const AbValue *candidate = &candidates->as.array.items[index];
    const AbValue *attributes = ab_value_member(candidate, "attributes");
    if (strings_equal(ab_value_member(attributes, "id"), node))
      return candidate;
  }
  return NULL;
}

static int string_array_has(const AbValue *values, const AbValue *needle) {
  size_t index;
  if (!values || values->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < values->as.array.count; index++)
    if (strings_equal(&values->as.array.items[index], needle))
      return 1;
  return 0;
}

static ArchbirdStatus validate_path(ArchbirdEngine *engine, const AbValue *path,
                                    const AbValue *source_endpoint,
                                    const AbValue *target_endpoint,
                                    const AbValue *request, uint64_t max_depth,
                                    uint64_t shortest, int *all_current) {
  static const char *const fields[] = {
      "length", "nodes", "source", "state", "steps", "target",
  };
  static const char *const traversals[] = {"forward", "reverse"};
  static const char *const path_states[] = {"current", "unknown"};
  static const char *const resolutions[] = {
      "unique", "candidate", "ambiguous", "builtin", "unresolved",
  };
  const AbValue *nodes = ab_value_member(path, "nodes");
  const AbValue *steps = ab_value_member(path, "steps");
  const AbValue *path_state = ab_value_member(path, "state");
  const AbValue *request_relations = ab_value_member(request, "relations");
  const AbValue *source_candidate;
  const AbValue *target_candidate;
  uint64_t length;
  size_t index;
  int every_step_proven = 1;
  if (!ab_artifact_object_exact(path, fields,
                                sizeof(fields) / sizeof(fields[0])) ||
      !ab_artifact_safe_integer(ab_value_member(path, "length"), &length) ||
      length > max_depth || length != shortest || !nodes ||
      nodes->kind != AB_VALUE_ARRAY || !nodes->as.array.count || !steps ||
      steps->kind != AB_VALUE_ARRAY || nodes->as.array.count != length + 1 ||
      steps->as.array.count != length ||
      !string_in(path_state, path_states,
                 sizeof(path_states) / sizeof(path_states[0])) ||
      !strings_equal(ab_value_member(path, "source"),
                     &nodes->as.array.items[0]) ||
      !strings_equal(ab_value_member(path, "target"),
                     &nodes->as.array.items[nodes->as.array.count - 1]))
    return invalid(engine, "Path contains an inconsistent witness");
  source_candidate =
      endpoint_candidate(source_endpoint, &nodes->as.array.items[0]);
  target_candidate = endpoint_candidate(
      target_endpoint, &nodes->as.array.items[nodes->as.array.count - 1]);
  if (!source_candidate || !target_candidate)
    return invalid(engine, "Path witness does not use resolved endpoints");
  if (ab_value_string_is(path_state, "current") &&
      (!ab_value_string_is(ab_value_member(source_candidate, "state"),
                           "current") ||
       !ab_value_string_is(ab_value_member(target_candidate, "state"),
                           "current")))
    return invalid(engine, "current Path witness has an uncertain endpoint");
  for (index = 0; index < nodes->as.array.count; index++)
    if (!nonempty_string(&nodes->as.array.items[index]))
      return invalid(engine, "Path witness contains an invalid node");
  for (index = 0; index < steps->as.array.count; index++) {
    static const char *const step_fields[] = {"relation", "traversal"};
    const AbValue *step = &steps->as.array.items[index];
    const AbValue *relation = ab_value_member(step, "relation");
    const AbValue *traversal = ab_value_member(step, "traversal");
    const AbValue *attributes = ab_value_member(relation, "attributes");
    const AbValue *evidence = ab_value_member(relation, "evidence");
    const AbValue *family = ab_value_member(attributes, "family");
    const AbValue *resolution = ab_value_member(attributes, "resolution");
    const AbValue *relation_source = ab_value_member(attributes, "source");
    const AbValue *relation_target = ab_value_member(attributes, "target");
    const AbValue *relation_state = ab_value_member(relation, "state");
    int reverse;
    ArchbirdStatus status;
    if (!ab_artifact_object_exact(
            step, step_fields, sizeof(step_fields) / sizeof(step_fields[0])) ||
        !string_in(traversal, traversals,
                   sizeof(traversals) / sizeof(traversals[0])))
      return invalid(engine, "Path contains an invalid witness step");
    status = validate_projection_item(engine, relation);
    if (status != ARCHBIRD_OK)
      return status;
    if (!attributes || attributes->kind != AB_VALUE_OBJECT ||
        !nonempty_string(family) ||
        !string_array_has(request_relations, family) ||
        !nonempty_string(ab_value_member(attributes, "relation_kind")) ||
        !nonempty_string(relation_source) ||
        !nonempty_string(relation_target) ||
        (resolution &&
         !string_in(resolution, resolutions,
                    sizeof(resolutions) / sizeof(resolutions[0]))))
      return invalid(engine, "Path witness relation is malformed");
    reverse = ab_value_string_is(traversal, "reverse");
    if (!strings_equal(reverse ? relation_target : relation_source,
                       &nodes->as.array.items[index]) ||
        !strings_equal(reverse ? relation_source : relation_target,
                       &nodes->as.array.items[index + 1]))
      return invalid(engine,
                     "Path witness relation does not connect adjacent nodes");
    if (!ab_value_string_is(relation_state, "current") ||
        !evidence->as.array.count ||
        (resolution && !ab_value_string_is(resolution, "unique") &&
         !ab_value_string_is(resolution, "builtin")))
      every_step_proven = 0;
  }
  if (ab_value_string_is(path_state, "current") && !every_step_proven)
    return invalid(engine, "current Path witness is not evidence-qualified");
  if (!ab_value_string_is(path_state, "current"))
    *all_current = 0;
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_path_artifact_validate(ArchbirdEngine *engine,
                                         const AbValue *artifact) {
  static const char *const fields[] = {
      "artifact",
      "evidence",
      "graph",
      "outcome",
      "path_sha256",
      "paths",
      "producer_compatibility",
      "project",
      "reason",
      "request",
      "schema_version",
      "search",
      "source",
      "source_tool",
      "target",
  };
  static const char *const graph_fields[] = {
      "classification",
      "completeness",
      "coverage",
      "projection_definition_sha256",
      "projection_result_sha256",
  };
  static const char *const search_fields[] = {
      "frontier",   "max_depth",       "max_paths",
      "path_count", "shortest_length", "truncated",
  };
  static const char *const classifications[] = {
      "complete",
      "bounded",
      "incomplete",
      "unknown",
  };
  static const char *const compatibilities[] = {
      "current",
      "different",
      "unknown",
  };
  static const char *const outcomes[] = {"found", "absent", "unknown"};
  static const char *const reasons[] = {
      "witnesses",           "path-limit",       "candidate-witnesses",
      "endpoint-unresolved", "graph-incomplete", "depth-frontier",
      "exhaustive",
  };
  const AbValue *digest = ab_value_member(artifact, "path_sha256");
  const AbValue *graph = ab_value_member(artifact, "graph");
  const AbValue *request = ab_value_member(artifact, "request");
  const AbValue *source = ab_value_member(artifact, "source");
  const AbValue *target = ab_value_member(artifact, "target");
  const AbValue *paths = ab_value_member(artifact, "paths");
  const AbValue *search = ab_value_member(artifact, "search");
  const AbValue *outcome = ab_value_member(artifact, "outcome");
  const AbValue *reason = ab_value_member(artifact, "reason");
  const AbValue *shortest_value;
  const AbValue *coverage;
  const AbValue *coverage_state = NULL;
  const AbValue *completeness;
  const AbValue *graph_classification;
  const AbValue *ledger_classification;
  uint64_t schema;
  uint64_t max_depth;
  uint64_t max_paths;
  uint64_t path_count;
  uint64_t shortest = 0;
  uint64_t request_max_depth;
  uint64_t request_max_paths;
  size_t index;
  int has_shortest;
  int all_current = 1;
  char actual_digest[65];
  ArchbirdStatus status;
  if (!engine || !artifact)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!ab_artifact_object_exact(artifact, fields,
                                sizeof(fields) / sizeof(fields[0])) ||
      !ab_value_string_is(ab_value_member(artifact, "artifact"), "path") ||
      !ab_artifact_safe_integer(ab_value_member(artifact, "schema_version"),
                                &schema) ||
      schema != 1 || !nonempty_string(ab_value_member(artifact, "project")) ||
      !ab_value_member(artifact, "source_tool") ||
      ab_value_member(artifact, "source_tool")->kind != AB_VALUE_OBJECT ||
      !ab_value_member(artifact, "evidence") ||
      ab_value_member(artifact, "evidence")->kind != AB_VALUE_OBJECT ||
      !string_in(ab_value_member(artifact, "producer_compatibility"),
                 compatibilities,
                 sizeof(compatibilities) / sizeof(compatibilities[0])) ||
      !string_in(outcome, outcomes, sizeof(outcomes) / sizeof(outcomes[0])) ||
      !string_in(reason, reasons, sizeof(reasons) / sizeof(reasons[0])) ||
      !ab_artifact_sha256(digest))
    return invalid(engine, "invalid canonical Path artifact");
  status = ab_artifact_value_sha256_without_field(engine, artifact,
                                                  "path_sha256", actual_digest);
  if (status != ARCHBIRD_OK)
    return status;
  if (memcmp(actual_digest, digest->as.text.data, 64))
    return invalid(engine, "canonical Path SHA-256 does not match its content");
  graph_classification = ab_value_member(graph, "classification");
  if (!ab_artifact_object_exact(graph, graph_fields,
                                sizeof(graph_fields) /
                                    sizeof(graph_fields[0])) ||
      !string_in(graph_classification, classifications,
                 sizeof(classifications) / sizeof(classifications[0])) ||
      !(completeness = ab_value_member(graph, "completeness")) ||
      completeness->kind != AB_VALUE_OBJECT ||
      !(ledger_classification =
            ab_value_member(completeness, "classification")) ||
      !string_in(ledger_classification, classifications,
                 sizeof(classifications) / sizeof(classifications[0])) ||
      !ab_artifact_sha256(
          ab_value_member(graph, "projection_definition_sha256")) ||
      !ab_artifact_sha256(ab_value_member(graph, "projection_result_sha256")))
    return invalid(engine, "Path graph identity is invalid");
  coverage = ab_value_member(graph, "coverage");
  if (!coverage)
    return invalid(engine, "Path graph coverage is missing");
  if (coverage->kind != AB_VALUE_NULL) {
    status = validate_projection_item(engine, coverage);
    if (status != ARCHBIRD_OK)
      return status;
    coverage_state = ab_value_member(coverage, "state");
    if (!ab_value_string_is(coverage_state, "current") &&
        !ab_value_string_is(coverage_state, "stale") &&
        !ab_value_string_is(coverage_state, "unknown"))
      return invalid(engine, "Path graph coverage state is invalid");
  }
  /*
   * The selection ledger deliberately excludes contextual repository
   * coverage. Path may therefore downgrade an otherwise complete graph when
   * that separate coverage item is stale or unknown.
   */
  if (!strings_equal(graph_classification, ledger_classification) &&
      !(ab_value_string_is(graph_classification, "incomplete") &&
        ab_value_string_is(ledger_classification, "complete") &&
        coverage_state && !ab_value_string_is(coverage_state, "current")))
    return invalid(
        engine,
        "Path graph classification does not match its completeness evidence");
  if (ab_value_string_is(ledger_classification, "complete") && coverage_state &&
      !ab_value_string_is(coverage_state, "current") &&
      !ab_value_string_is(graph_classification, "incomplete"))
    return invalid(
        engine,
        "Path graph classification ignores incomplete repository coverage");
  status = validate_request(engine, request);
  if (status == ARCHBIRD_OK)
    status = validate_endpoint(engine, source);
  if (status == ARCHBIRD_OK)
    status = validate_endpoint(engine, target);
  if (status != ARCHBIRD_OK)
    return status;
  if (!ab_artifact_object_exact(search, search_fields,
                                sizeof(search_fields) /
                                    sizeof(search_fields[0])) ||
      !ab_artifact_boolean(ab_value_member(search, "frontier")) ||
      !ab_artifact_boolean(ab_value_member(search, "truncated")) ||
      !ab_artifact_safe_integer(ab_value_member(search, "max_depth"),
                                &max_depth) ||
      max_depth > 64 ||
      !ab_artifact_safe_integer(ab_value_member(search, "max_paths"),
                                &max_paths) ||
      !max_paths || max_paths > 100 ||
      !ab_artifact_safe_integer(ab_value_member(search, "path_count"),
                                &path_count) ||
      !(shortest_value = ab_value_member(search, "shortest_length")) ||
      (shortest_value->kind != AB_VALUE_NULL &&
       (!ab_artifact_safe_integer(shortest_value, &shortest) || shortest > 64)))
    return invalid(engine, "Path search ledger is invalid");
  has_shortest = shortest_value->kind != AB_VALUE_NULL;
  if (!ab_artifact_safe_integer(ab_value_member(request, "max_depth"),
                                &request_max_depth) ||
      !ab_artifact_safe_integer(ab_value_member(request, "max_paths"),
                                &request_max_paths) ||
      max_depth != request_max_depth || max_paths != request_max_paths ||
      !paths || paths->kind != AB_VALUE_ARRAY ||
      path_count != paths->as.array.count ||
      has_shortest != (paths->as.array.count != 0) ||
      (has_shortest && shortest > max_depth) ||
      (ab_value_member(search, "truncated")->as.boolean &&
       path_count != max_paths))
    return invalid(engine, "Path search ledger does not match its witnesses");
  for (index = 0; index < paths->as.array.count; index++) {
    status = validate_path(engine, &paths->as.array.items[index], source,
                           target, request, max_depth, shortest, &all_current);
    if (status != ARCHBIRD_OK)
      return status;
  }
  if (ab_value_string_is(outcome, "found")) {
    if (!paths->as.array.count || !all_current ||
        !(ab_value_string_is(reason, "witnesses") ||
          ab_value_string_is(reason, "path-limit")) ||
        ab_value_member(search, "truncated")->as.boolean !=
            ab_value_string_is(reason, "path-limit"))
      return invalid(engine, "found Path outcome has no proven witnesses");
  } else if (ab_value_string_is(outcome, "absent")) {
    if (paths->as.array.count || !ab_value_string_is(reason, "exhaustive") ||
        !ab_value_string_is(ab_value_member(graph, "classification"),
                            "complete") ||
        ab_value_member(search, "frontier")->as.boolean ||
        ab_value_member(search, "truncated")->as.boolean ||
        ab_value_string_is(ab_value_member(source, "state"), "unresolved") ||
        ab_value_string_is(ab_value_member(target, "state"), "unresolved"))
      return invalid(engine, "absent Path outcome is not exhaustive");
  } else if (paths->as.array.count) {
    if (!ab_value_string_is(reason, "candidate-witnesses") || all_current)
      return invalid(engine,
                     "unknown Path witnesses are not evidence-qualified");
  } else if (ab_value_string_is(reason, "endpoint-unresolved")) {
    if (!ab_value_string_is(ab_value_member(source, "state"), "unresolved") &&
        !ab_value_string_is(ab_value_member(target, "state"), "unresolved"))
      return invalid(engine, "endpoint-unresolved Path has resolved endpoints");
  } else if (ab_value_string_is(reason, "graph-incomplete")) {
    if (ab_value_string_is(ab_value_member(graph, "classification"),
                           "complete") ||
        ab_value_string_is(ab_value_member(source, "state"), "unresolved") ||
        ab_value_string_is(ab_value_member(target, "state"), "unresolved"))
      return invalid(engine, "graph-incomplete Path claims a complete graph");
  } else if (ab_value_string_is(reason, "depth-frontier")) {
    if (!ab_value_string_is(ab_value_member(graph, "classification"),
                            "complete") ||
        !ab_value_member(search, "frontier")->as.boolean ||
        ab_value_string_is(ab_value_member(source, "state"), "unresolved") ||
        ab_value_string_is(ab_value_member(target, "state"), "unresolved"))
      return invalid(engine, "depth-frontier Path has no exhaustive frontier");
  } else {
    return invalid(engine, "unknown Path outcome has an inconsistent reason");
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_path_artifact_load(ArchbirdEngine *engine,
                                     const uint8_t *json, size_t json_length,
                                     AbPathArtifact *out) {
  ArchbirdStatus status;
  if (!engine || (!json && json_length) || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  out->engine = engine;
  status = ab_json_value_decode(engine, json, json_length, &out->root);
  if (status == ARCHBIRD_OK)
    status = ab_path_artifact_validate(engine, &out->root);
  if (status != ARCHBIRD_OK)
    ab_path_artifact_free(out);
  return status;
}

void ab_path_artifact_free(AbPathArtifact *artifact) {
  if (!artifact)
    return;
  if (artifact->engine)
    ab_value_free(artifact->engine, &artifact->root);
  memset(artifact, 0, sizeof(*artifact));
}
