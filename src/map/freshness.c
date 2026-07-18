#include <archbird/archbird.h>

#include "archbird_internal.h"
#include "json_value.h"
#include "render_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct FreshnessFile {
  const AbString *path;
  const AbString *sha256;
} FreshnessFile;

typedef struct FreshnessIndex {
  FreshnessFile *items;
  size_t count;
} FreshnessIndex;

typedef struct FreshnessIdentity {
  const AbString *artifact;
  const AbString *project;
  const AbString *input_sha256;
  const AbString *config_sha256;
  const AbString *tool_implementation_sha256;
  const AbString *discovery_sha256;
  int has_config;
  int has_discovery;
  int has_tool;
  int complete_file_inventory;
} FreshnessIdentity;

typedef struct FreshnessContext {
  ArchbirdEngine *engine;
  FreshnessIdentity snapshot;
  FreshnessIdentity current;
  FreshnessIndex snapshot_files;
  FreshnessIndex current_files;
} FreshnessContext;

typedef enum FreshnessComparison {
  FRESHNESS_CURRENT = 0,
  FRESHNESS_CHANGED = 1,
  FRESHNESS_UNKNOWN = 2
} FreshnessComparison;

static ArchbirdStatus freshness_error(FreshnessContext *context,
                                      const char *message) {
  return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                            ARCHBIRD_NO_OFFSET, "%s", message);
}

static int valid_sha256(const AbString *value) {
  size_t index;
  if (!value || value->length != 64)
    return 0;
  for (index = 0; index < value->length; index++)
    if (value->data[index] < '0' ||
        (value->data[index] > '9' && value->data[index] < 'a') ||
        value->data[index] > 'f')
      return 0;
  return 1;
}

static int valid_repository_path(const AbString *value) {
  size_t segment = 0;
  size_t index;
  const char *path;
  size_t length;
  if (!value)
    return 0;
  path = value->data;
  length = value->length;
  if (!length || path[0] == '/' || path[length - 1] == '/' ||
      (length >= 2 &&
       ((path[0] >= 'A' && path[0] <= 'Z') ||
        (path[0] >= 'a' && path[0] <= 'z')) &&
       path[1] == ':'))
    return 0;
  for (index = 0; index <= length; index++) {
    if (index < length && path[index] != '/') {
      if (path[index] == '\\' || path[index] == '\0')
        return 0;
      continue;
    }
    if (index == segment || (index - segment == 1 && path[segment] == '.') ||
        (index - segment == 2 && path[segment] == '.' &&
         path[segment + 1] == '.'))
      return 0;
    segment = index + 1;
  }
  return 1;
}

static const AbString *required_string(FreshnessContext *context,
                                       const AbValue *object, const char *field,
                                       const char *message) {
  const AbValue *value = ab_value_member(object, field);
  if (!value || value->kind != AB_VALUE_STRING) {
    (void)freshness_error(context, message);
    return NULL;
  }
  return &value->as.text;
}

static const AbString *required_sha256(FreshnessContext *context,
                                       const AbValue *object, const char *field,
                                       const char *message) {
  const AbString *value = required_string(context, object, field, message);
  if (value && !valid_sha256(value)) {
    (void)freshness_error(context, message);
    return NULL;
  }
  return value;
}

static const AbString *optional_sha256(FreshnessContext *context,
                                       const AbValue *object, const char *field,
                                       int *present, const char *message) {
  const AbValue *value = ab_value_member(object, field);
  *present = value != NULL;
  if (!value)
    return NULL;
  if (value->kind != AB_VALUE_STRING || !valid_sha256(&value->as.text)) {
    (void)freshness_error(context, message);
    return NULL;
  }
  return &value->as.text;
}

static ArchbirdStatus decode_identity(FreshnessContext *context,
                                      const AbValue *document, int current,
                                      FreshnessIdentity *out) {
  const AbValue *artifact;
  const AbValue *schema;
  const AbValue *evidence;
  const AbValue *tool;
  const AbValue *discovery;
  uint64_t version;
  memset(out, 0, sizeof(*out));
  if (!document || document->kind != AB_VALUE_OBJECT)
    return freshness_error(context,
                           "freshness input must be an Archbird object");
  artifact = ab_value_member(document, "artifact");
  schema = ab_value_member(document, "schema_version");
  if (!artifact || artifact->kind != AB_VALUE_STRING ||
      !ab_value_u64(schema, &version) || version < ARCHBIRD_MAP_SCHEMA_MIN ||
      version > ARCHBIRD_MAP_SCHEMA_CURRENT)
    return freshness_error(
        context, "freshness input must use a supported Map/Query schema");
  if (current) {
    if (!ab_value_string_is(artifact, "map"))
      return freshness_error(context, "freshness current input must be a Map");
  } else if (!ab_value_string_is(artifact, "map") &&
             !ab_value_string_is(artifact, "query")) {
    return freshness_error(context,
                           "freshness snapshot must be a Map or Query");
  }
  out->artifact = &artifact->as.text;
  out->complete_file_inventory = ab_value_string_is(artifact, "map");
  out->project = required_string(context, document, "project",
                                 "freshness input project must be a string");
  evidence = ab_value_member(document, "evidence");
  if (!evidence || evidence->kind != AB_VALUE_OBJECT)
    return freshness_error(context,
                           "freshness input evidence must be an object");
  out->input_sha256 = required_sha256(
      context, evidence, "input_sha256",
      "freshness evidence.input_sha256 must be a SHA-256 digest");
  out->config_sha256 = optional_sha256(
      context, evidence, "config_sha256", &out->has_config,
      "freshness evidence.config_sha256 must be a SHA-256 digest");
  tool = ab_value_member(document, current ? "tool" : "source_tool");
  if (!tool && !current && ab_value_string_is(artifact, "map"))
    tool = ab_value_member(document, "tool");
  out->has_tool = tool != NULL;
  if (tool) {
    if (tool->kind != AB_VALUE_OBJECT)
      return freshness_error(context,
                             "freshness source tool must be an object");
    out->tool_implementation_sha256 = required_sha256(
        context, tool, "implementation_sha256",
        "freshness tool implementation_sha256 must be a SHA-256 digest");
  }
  discovery = ab_value_member(document, "discovery");
  out->has_discovery = discovery != NULL;
  if (discovery) {
    if (discovery->kind != AB_VALUE_OBJECT)
      return freshness_error(context, "freshness discovery must be an object");
    out->discovery_sha256 =
        required_sha256(context, discovery, "sha256",
                        "freshness discovery.sha256 must be a SHA-256 digest");
  }
  if (!out->project || !out->input_sha256 ||
      (out->has_config && !out->config_sha256) ||
      (out->has_tool && !out->tool_implementation_sha256) ||
      (out->has_discovery && !out->discovery_sha256))
    return ARCHBIRD_INVALID_SCHEMA;
  return ARCHBIRD_OK;
}

static int freshness_file_compare(const void *left_raw, const void *right_raw) {
  const FreshnessFile *left = (const FreshnessFile *)left_raw;
  const FreshnessFile *right = (const FreshnessFile *)right_raw;
  return ab_string_compare(left->path, right->path);
}

static ArchbirdStatus build_file_index(FreshnessContext *context,
                                       const AbValue *document,
                                       FreshnessIndex *out) {
  const AbValue *files = ab_value_member(document, "files");
  size_t index;
  if (!files || files->kind != AB_VALUE_ARRAY)
    return freshness_error(context, "freshness input files must be an array");
  if (!files->as.array.count)
    return ARCHBIRD_OK;
  out->items = (FreshnessFile *)ab_calloc(
      context->engine, files->as.array.count, sizeof(*out->items));
  if (!out->items)
    return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory indexing freshness files");
  out->count = files->as.array.count;
  for (index = 0; index < out->count; index++) {
    const AbValue *row = &files->as.array.items[index];
    if (row->kind != AB_VALUE_OBJECT)
      return freshness_error(context, "freshness files[] must be an object");
    out->items[index].path = required_string(
        context, row, "path", "freshness files[].path must be a string");
    out->items[index].sha256 =
        required_sha256(context, row, "sha256",
                        "freshness files[].sha256 must be a SHA-256 digest");
    if (!out->items[index].path || !out->items[index].sha256)
      return ARCHBIRD_INVALID_SCHEMA;
    if (!valid_repository_path(out->items[index].path))
      return freshness_error(
          context,
          "freshness files[].path must be repository-relative and canonical");
  }
  if (out->count > 1)
    qsort(out->items, out->count, sizeof(*out->items), freshness_file_compare);
  for (index = 1; index < out->count; index++)
    if (ab_string_equal(out->items[index - 1].path, out->items[index].path))
      return freshness_error(context,
                             "freshness files contain a duplicate path");
  return ARCHBIRD_OK;
}

static void freshness_index_free(ArchbirdEngine *engine,
                                 FreshnessIndex *index) {
  ab_free(engine, index->items);
  memset(index, 0, sizeof(*index));
}

static FreshnessComparison compare_optional_digest(const AbString *left,
                                                   int has_left,
                                                   const AbString *right,
                                                   int has_right) {
  if (!has_left && !has_right)
    return FRESHNESS_CURRENT;
  if (!has_left || !has_right)
    return FRESHNESS_UNKNOWN;
  return ab_string_equal(left, right) ? FRESHNESS_CURRENT : FRESHNESS_CHANGED;
}

static const char *comparison_name(FreshnessComparison comparison) {
  if (comparison == FRESHNESS_CURRENT)
    return "current";
  if (comparison == FRESHNESS_CHANGED)
    return "changed";
  return "unknown";
}

static ArchbirdStatus render_string_value(AbBuffer *buffer,
                                          const AbString *value) {
  return ab_buffer_json_string(buffer, value->data, value->length);
}

static ArchbirdStatus render_identity(AbBuffer *buffer,
                                      const FreshnessIdentity *identity) {
  ArchbirdStatus status = ab_buffer_literal(buffer, "{\"artifact\":");
  if (status == ARCHBIRD_OK)
    status = render_string_value(buffer, identity->artifact);
  if (status == ARCHBIRD_OK && identity->has_config)
    status = ab_buffer_literal(buffer, ",\"config_sha256\":");
  if (status == ARCHBIRD_OK && identity->has_config)
    status = render_string_value(buffer, identity->config_sha256);
  if (status == ARCHBIRD_OK && identity->has_discovery)
    status = ab_buffer_literal(buffer, ",\"discovery_sha256\":");
  if (status == ARCHBIRD_OK && identity->has_discovery)
    status = render_string_value(buffer, identity->discovery_sha256);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"input_sha256\":");
  if (status == ARCHBIRD_OK)
    status = render_string_value(buffer, identity->input_sha256);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"project\":");
  if (status == ARCHBIRD_OK)
    status = render_string_value(buffer, identity->project);
  if (status == ARCHBIRD_OK && identity->has_tool)
    status = ab_buffer_literal(buffer, ",\"tool_implementation_sha256\":");
  if (status == ARCHBIRD_OK && identity->has_tool)
    status = render_string_value(buffer, identity->tool_implementation_sha256);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

static ArchbirdStatus render_path_array(AbBuffer *buffer,
                                        const FreshnessIndex *left,
                                        const FreshnessIndex *right) {
  size_t left_index = 0;
  size_t right_index = 0;
  size_t rendered = 0;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  while (status == ARCHBIRD_OK && left_index < left->count) {
    int compared;
    while (right_index < right->count &&
           ab_string_compare(right->items[right_index].path,
                             left->items[left_index].path) < 0)
      right_index++;
    compared = right_index == right->count
                   ? 1
                   : ab_string_compare(left->items[left_index].path,
                                       right->items[right_index].path);
    if (compared < 0 || right_index == right->count) {
      if (rendered++)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = render_string_value(buffer, left->items[left_index].path);
    }
    left_index++;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus
render_changed_files(AbBuffer *buffer, const FreshnessIndex *snapshot,
                     const FreshnessIndex *current, size_t *out_unchanged,
                     size_t *out_changed, size_t *out_missing) {
  size_t before = 0;
  size_t after = 0;
  size_t rendered = 0;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  *out_unchanged = 0;
  *out_changed = 0;
  *out_missing = 0;
  while (status == ARCHBIRD_OK && before < snapshot->count) {
    int compared;
    while (after < current->count &&
           ab_string_compare(current->items[after].path,
                             snapshot->items[before].path) < 0)
      after++;
    if (after == current->count) {
      *out_missing += snapshot->count - before;
      break;
    }
    compared = ab_string_compare(snapshot->items[before].path,
                                 current->items[after].path);
    if (compared < 0) {
      (*out_missing)++;
    } else if (compared == 0) {
      if (ab_string_equal(snapshot->items[before].sha256,
                          current->items[after].sha256)) {
        (*out_unchanged)++;
      } else {
        if (rendered++)
          status = ab_buffer_literal(buffer, ",");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(buffer, "{\"after_sha256\":");
        if (status == ARCHBIRD_OK)
          status = render_string_value(buffer, current->items[after].sha256);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(buffer, ",\"before_sha256\":");
        if (status == ARCHBIRD_OK)
          status = render_string_value(buffer, snapshot->items[before].sha256);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(buffer, ",\"path\":");
        if (status == ARCHBIRD_OK)
          status = render_string_value(buffer, snapshot->items[before].path);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(buffer, "}");
        (*out_changed)++;
      }
      after++;
    }
    before++;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_missing_files(AbBuffer *buffer,
                                           const FreshnessIndex *snapshot,
                                           const FreshnessIndex *current) {
  return render_path_array(buffer, snapshot, current);
}

static ArchbirdStatus render_added_files(AbBuffer *buffer,
                                         const FreshnessContext *context) {
  if (!context->snapshot.complete_file_inventory)
    return ab_buffer_literal(buffer, "[]");
  return render_path_array(buffer, &context->current_files,
                           &context->snapshot_files);
}

static ArchbirdStatus render_freshness(FreshnessContext *context,
                                       AbBuffer *buffer) {
  FreshnessComparison source = ab_string_equal(context->snapshot.input_sha256,
                                               context->current.input_sha256)
                                   ? FRESHNESS_CURRENT
                                   : FRESHNESS_CHANGED;
  FreshnessComparison config = compare_optional_digest(
      context->snapshot.config_sha256, context->snapshot.has_config,
      context->current.config_sha256, context->current.has_config);
  FreshnessComparison discovery = compare_optional_digest(
      context->snapshot.discovery_sha256, context->snapshot.has_discovery,
      context->current.discovery_sha256, context->current.has_discovery);
  FreshnessComparison producer = compare_optional_digest(
      context->snapshot.tool_implementation_sha256, context->snapshot.has_tool,
      context->current.tool_implementation_sha256, context->current.has_tool);
  int same_project =
      ab_string_equal(context->snapshot.project, context->current.project);
  const char *status_name;
  size_t unchanged = 0;
  size_t changed = 0;
  size_t missing = 0;
  int unattributed;
  ArchbirdStatus status;
  if (!same_project)
    status_name = "not-applicable";
  else if (source == FRESHNESS_CHANGED || config == FRESHNESS_CHANGED)
    status_name = "stale";
  else if (producer == FRESHNESS_CHANGED || discovery == FRESHNESS_CHANGED)
    status_name = "context-drift";
  else if (config == FRESHNESS_UNKNOWN || producer == FRESHNESS_UNKNOWN ||
           discovery == FRESHNESS_UNKNOWN)
    status_name = "unknown";
  else
    status_name = "current";
  status = ab_buffer_literal(buffer, "{\"artifact\":\"map-freshness\","
                                     "\"comparisons\":{\"configuration\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, comparison_name(config),
                                   strlen(comparison_name(config)));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"discovery\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, comparison_name(discovery),
                                   strlen(comparison_name(discovery)));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"producer\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, comparison_name(producer),
                                   strlen(comparison_name(producer)));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"project\":");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_literal(buffer, same_project ? "\"current\"" : "\"changed\"");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"source\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, comparison_name(source),
                                   strlen(comparison_name(source)));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "},\"current\":");
  if (status == ARCHBIRD_OK)
    status = render_identity(buffer, &context->current);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"files\":{\"added\":");
  if (status == ARCHBIRD_OK)
    status = render_added_files(buffer, context);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"changed\":");
  if (status == ARCHBIRD_OK)
    status = render_changed_files(buffer, &context->snapshot_files,
                                  &context->current_files, &unchanged, &changed,
                                  &missing);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"missing\":");
  if (status == ARCHBIRD_OK)
    status = render_missing_files(buffer, &context->snapshot_files,
                                  &context->current_files);
  unattributed =
      source == FRESHNESS_CHANGED && !context->snapshot.complete_file_inventory;
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"scope\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, context->snapshot.complete_file_inventory
                                           ? "\"mapped-inventory\""
                                           : "\"selected-snapshot-files\"");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"unattributed_input_drift\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, unattributed ? "true" : "false");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"unchanged\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(buffer, unchanged);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "},\"schema_version\":1,\"snapshot\":");
  if (status == ARCHBIRD_OK)
    status = render_identity(buffer, &context->snapshot);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"status\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, status_name, strlen(status_name));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        buffer,
        ",\"tool\":{\"implementation_sha256\":\"" ARCHBIRD_IMPLEMENTATION_SHA256
        "\",\"name\":\"archbird\",\"version\":\"" ARCHBIRD_VERSION "\"}}");
  return status;
}

ArchbirdStatus
archbird_map_freshness(ArchbirdEngine *engine, const uint8_t *snapshot_json,
                       size_t snapshot_length, const uint8_t *current_map_json,
                       size_t current_map_length, uint32_t json_flags,
                       ArchbirdWriteFn write_fn, void *user_data) {
  FreshnessContext context = {0};
  AbValue snapshot = {0};
  AbValue current = {0};
  AbBuffer buffer;
  ArchbirdStatus status;
  if (!engine || (!snapshot_json && snapshot_length) ||
      (!current_map_json && current_map_length) || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_build_identity_validate(engine);
  if (status != ARCHBIRD_OK)
    return status;
  context.engine = engine;
  ab_buffer_init(&buffer, engine);
  status =
      ab_json_value_decode(engine, snapshot_json, snapshot_length, &snapshot);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, current_map_json, current_map_length,
                                  &current);
  if (status == ARCHBIRD_OK)
    status = decode_identity(&context, &snapshot, 0, &context.snapshot);
  if (status == ARCHBIRD_OK)
    status = decode_identity(&context, &current, 1, &context.current);
  if (status == ARCHBIRD_OK)
    status = build_file_index(&context, &snapshot, &context.snapshot_files);
  if (status == ARCHBIRD_OK)
    status = build_file_index(&context, &current, &context.current_files);
  if (status == ARCHBIRD_OK)
    status = render_freshness(&context, &buffer);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                        json_flags, write_fn, user_data);
  freshness_index_free(engine, &context.current_files);
  freshness_index_free(engine, &context.snapshot_files);
  ab_value_free(engine, &current);
  ab_value_free(engine, &snapshot);
  ab_buffer_free(&buffer);
  return status;
}
