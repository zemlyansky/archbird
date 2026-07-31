#include "make/provider_capability.h"

#include "artifact_validation.h"
#include "config.h"
#include "project_internal.h"
#include "sha256.h"

#include <string.h>

typedef struct AbActMakeProvider {
  const AbString *definition_sha256;
  const AbString *surface;
  const AbString *path;
  const AbString *variable;
  const AbConfigProvider *configuration;
  const AbValue *mapped_surface;
} AbActMakeProvider;

typedef struct AbActMakeToken {
  AbString value;
  int matched;
} AbActMakeToken;

typedef struct AbActMakeInsertion {
  AbString token;
  AbString anchor;
  ArchbirdMakeVariableTokenPosition position;
  const AbString *anchor_name;
  size_t common_prefix;
  size_t length_distance;
  int matched;
} AbActMakeInsertion;

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static int text_is(const AbValue *value, const char *literal) {
  return ab_artifact_text_is(value, literal);
}

static int string_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         memcmp(value->data, literal, length) == 0;
}

static int buffer_write(void *user_data, const uint8_t *bytes, size_t length) {
  return ab_buffer_append((AbBuffer *)user_data, bytes, length) == ARCHBIRD_OK
             ? 0
             : 1;
}

static ArchbirdStatus reject(AbActContext *context, ArchbirdStatus status,
                             const char *message) {
  return archbird_error_set(ab_act_executor_engine(context), status,
                            ARCHBIRD_NO_OFFSET,
                            "act Make provider capability: %s", message);
}

static const AbValue *find_named_row(const AbValue *rows,
                                     const AbString *name) {
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *candidate = field(row, "name");
    if (candidate && candidate->kind == AB_VALUE_STRING &&
        ab_string_equal(&candidate->as.text, name))
      return row;
  }
  return NULL;
}

static int parse_mapped_provider(const AbValue *value,
                                 const AbString **definition_sha256,
                                 const AbString **path, AbString *variable) {
  static const char prefix[] = "make-variable:";
  const AbValue *mapped_definition = field(value, "definition_sha256");
  const AbValue *mapped_path = field(value, "path");
  const AbValue *source = field(value, "source");
  if (!ab_artifact_sha256(mapped_definition) || !mapped_path ||
      mapped_path->kind != AB_VALUE_STRING || !source ||
      source->kind != AB_VALUE_STRING ||
      source->as.text.length <= sizeof(prefix) - 1 ||
      memcmp(source->as.text.data, prefix, sizeof(prefix) - 1) != 0)
    return 0;
  *definition_sha256 = &mapped_definition->as.text;
  *path = &mapped_path->as.text;
  variable->data = source->as.text.data + sizeof(prefix) - 1;
  variable->length = source->as.text.length - (sizeof(prefix) - 1);
  return 1;
}

static int mapped_provider_equal(const AbValue *value,
                                 const AbActMakeProvider *provider) {
  const AbString *path = NULL;
  const AbString *definition_sha256 = NULL;
  AbString variable = {0};
  return parse_mapped_provider(value, &definition_sha256, &path, &variable) &&
         ab_string_equal(definition_sha256, provider->definition_sha256) &&
         ab_string_equal(path, provider->path) &&
         ab_string_equal(&variable, provider->variable);
}

static int row_has_provider(const AbValue *row,
                            const AbActMakeProvider *provider) {
  const AbValue *declarations = field(row, "declarations");
  size_t index;
  if (!declarations || declarations->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < declarations->as.array.count; index++)
    if (mapped_provider_equal(&declarations->as.array.items[index], provider))
      return 1;
  return 0;
}

static int target_is_resolved(const AbValue *row) {
  const AbValue *candidates = field(row, "candidates");
  return row && text_is(field(row, "declaration"), "declared") &&
         text_is(field(row, "resolution"), "unique") && candidates &&
         candidates->kind == AB_VALUE_ARRAY && candidates->as.array.count == 1;
}

static int target_is_implemented(const AbValue *row) {
  const AbValue *candidates = field(row, "candidates");
  const AbValue *uses = field(row, "uses");
  return row && text_is(field(row, "declaration"), "undeclared") &&
         text_is(field(row, "resolution"), "unique") && candidates &&
         candidates->kind == AB_VALUE_ARRAY &&
         candidates->as.array.count == 1 && uses &&
         uses->kind == AB_VALUE_ARRAY && uses->as.array.count;
}

static int old_is_inactive_declaration(const AbValue *row,
                                       const AbActMakeProvider *provider) {
  const AbValue *candidates = field(row, "candidates");
  const AbValue *uses = field(row, "uses");
  const AbValue *declarations = field(row, "declarations");
  return row && text_is(field(row, "declaration"), "declared") &&
         text_is(field(row, "resolution"), "unresolved") && candidates &&
         candidates->kind == AB_VALUE_ARRAY && !candidates->as.array.count &&
         uses && uses->kind == AB_VALUE_ARRAY && !uses->as.array.count &&
         declarations && declarations->kind == AB_VALUE_ARRAY &&
         declarations->as.array.count == 1 &&
         mapped_provider_equal(&declarations->as.array.items[0], provider);
}

static int provider_has_resolved_declaration(const AbValue *surface,
                                             const AbActMakeProvider *provider,
                                             const AbString *excluded_name) {
  const AbValue *names = field(surface, "names");
  size_t index;
  if (!names || names->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < names->as.array.count; index++) {
    const AbValue *row = &names->as.array.items[index];
    const AbValue *name = field(row, "name");
    if (name && name->kind == AB_VALUE_STRING &&
        !ab_string_equal(&name->as.text, excluded_name) &&
        row_has_provider(row, provider) && target_is_resolved(row))
      return 1;
  }
  return 0;
}

static const AbConfigProvider *
find_configured_provider(const AbMapConfig *config, const AbString *surface,
                         const AbString *path, const AbString *variable,
                         const AbString *definition_sha256) {
  const AbConfigProvider *matched = NULL;
  size_t bridge_index;
  if (!config)
    return NULL;
  for (bridge_index = 0; bridge_index < config->bridge_count; bridge_index++) {
    const AbConfigBridge *bridge = &config->bridges[bridge_index];
    size_t provider_index;
    if (!ab_string_equal(&bridge->name, surface))
      continue;
    for (provider_index = 0; provider_index < bridge->provider_count;
         provider_index++) {
      const AbConfigProvider *provider = &bridge->providers[provider_index];
      char candidate_sha256[65];
      if (!string_is(&provider->kind, "make_variable") ||
          !ab_string_equal(&provider->path, path) ||
          !ab_string_equal(&provider->variable, variable) ||
          ab_config_provider_definition_sha256(provider, candidate_sha256) !=
              ARCHBIRD_OK ||
          definition_sha256->length != 64 ||
          memcmp(candidate_sha256, definition_sha256->data, 64) != 0)
        continue;
      if (matched)
        return NULL;
      matched = provider;
    }
  }
  return matched;
}

static ArchbirdStatus load_provider(AbActContext *context,
                                    const AbValue *operation,
                                    AbActMakeProvider *out) {
  const AbValue *provider = field(operation, "provider");
  const AbValue *definition_sha256 = field(provider, "definition_sha256");
  const AbValue *kind = field(provider, "kind");
  const AbValue *path = field(provider, "path");
  const AbValue *variable = field(provider, "variable");
  const AbValue *surface = field(operation, "surface");
  const AbValue *mapped_providers;
  size_t index;
  size_t matched = 0;
  memset(out, 0, sizeof(*out));
  if (!text_is(kind, "make_variable") ||
      !ab_artifact_sha256(definition_sha256) ||
      !ab_artifact_repository_path(path) || !variable ||
      variable->kind != AB_VALUE_STRING || !surface ||
      surface->kind != AB_VALUE_STRING)
    return reject(context, ARCHBIRD_INVALID_SCHEMA,
                  "operation has no valid configured provider identity");
  out->definition_sha256 = &definition_sha256->as.text;
  out->surface = &surface->as.text;
  out->path = &path->as.text;
  out->variable = &variable->as.text;
  out->configuration = find_configured_provider(
      ab_project_config(ab_act_executor_project(context)), out->surface,
      out->path, out->variable, out->definition_sha256);
  out->mapped_surface = find_named_row(
      field(ab_act_executor_map(context), "surfaces"), out->surface);
  if (!out->configuration || !out->mapped_surface)
    return reject(context, ARCHBIRD_CONFLICT,
                  "configured provider differs from the current Map");
  mapped_providers = field(out->mapped_surface, "providers");
  for (index = 0;
       mapped_providers && mapped_providers->kind == AB_VALUE_ARRAY &&
       index < mapped_providers->as.array.count;
       index++)
    if (mapped_provider_equal(&mapped_providers->as.array.items[index], out))
      matched++;
  if (matched != 1)
    return reject(context, ARCHBIRD_CONFLICT,
                  "provider identity is not unique in the current Map");
  return ARCHBIRD_OK;
}

static ArchbirdStatus source_sha256(const ArchbirdSourceView *source,
                                    char out[65]) {
  uint8_t digest[32];
  ArchbirdStatus status =
      archbird_sha256(source->bytes, source->byte_length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, out);
  return status;
}

static ArchbirdStatus make_token(ArchbirdEngine *engine, const AbString *name,
                                 int underscored, AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = underscored ? ab_buffer_literal(&buffer, "_") : ARCHBIRD_OK;
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_append(&buffer, (const uint8_t *)name->data, name->length);
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus try_replacement(AbActContext *context,
                                      const ArchbirdSourceView *source,
                                      const AbActMakeProvider *provider,
                                      const AbString *expected,
                                      const AbString *replacement,
                                      ArchbirdMakeVariableTokenEditResult *out,
                                      AbBuffer *bytes, int *matched) {
  ArchbirdMakeVariableTokenEditOptions options;
  char sha256[65];
  ArchbirdStatus status = source_sha256(source, sha256);
  *matched = 0;
  if (status != ARCHBIRD_OK)
    return status;
  archbird_make_variable_token_edit_options_init(&options);
  options.source_sha256 = sha256;
  options.source_sha256_length = 64;
  options.variable = (const uint8_t *)provider->variable->data;
  options.variable_length = provider->variable->length;
  options.expected_token = (const uint8_t *)expected->data;
  options.expected_token_length = expected->length;
  options.replacement_token = (const uint8_t *)replacement->data;
  options.replacement_token_length = replacement->length;
  archbird_make_variable_token_edit_result_init(out);
  ab_buffer_init(bytes, ab_act_executor_engine(context));
  status = archbird_make_variable_token_edit(
      ab_act_executor_engine(context), source->bytes, source->byte_length,
      &options, out, buffer_write, bytes);
  if (status == ARCHBIRD_OK) {
    *matched = 1;
  } else if (status == ARCHBIRD_POLICY_REJECTED && out->matched_tokens == 0) {
    archbird_error_clear(ab_act_executor_engine(context));
    status = ARCHBIRD_OK;
  }
  return status;
}

static ArchbirdStatus ground_replacement(AbActContext *context,
                                         const AbActMakeProvider *provider,
                                         const AbString *from,
                                         const AbString *to,
                                         const AbString *item_id) {
  ArchbirdEngine *engine = ab_act_executor_engine(context);
  ArchbirdSourceView source;
  AbActMakeToken selected = {0};
  AbBuffer selected_bytes;
  ArchbirdMakeVariableTokenEditResult selected_result;
  size_t form;
  ArchbirdStatus status =
      ab_act_executor_source(context, provider->path, &source);
  ab_buffer_init(&selected_bytes, engine);
  memset(&selected_result, 0, sizeof(selected_result));
  for (form = 0; status == ARCHBIRD_OK && form < 2; form++) {
    AbString expected = {0};
    AbString replacement = {0};
    ArchbirdMakeVariableTokenEditResult result;
    AbBuffer bytes;
    int matched = 0;
    status = make_token(engine, from, (int)form, &expected);
    if (status == ARCHBIRD_OK && to)
      status = make_token(engine, to, (int)form, &replacement);
    if (status == ARCHBIRD_OK)
      status = try_replacement(context, &source, provider, &expected,
                               &replacement, &result, &bytes, &matched);
    else
      ab_buffer_init(&bytes, engine);
    if (status == ARCHBIRD_OK && matched) {
      if (selected.matched) {
        status = reject(context, ARCHBIRD_CONFLICT,
                        "capability has multiple exact Make spellings");
      } else {
        selected.value = expected;
        memset(&expected, 0, sizeof(expected));
        selected_result = result;
        selected_bytes = bytes;
        memset(&bytes, 0, sizeof(bytes));
        selected.matched = 1;
      }
    }
    ab_buffer_free(&bytes);
    ab_string_free(engine, &replacement);
    ab_string_free(engine, &expected);
  }
  if (status == ARCHBIRD_OK && !selected.matched)
    status = reject(context, ARCHBIRD_POLICY_REJECTED,
                    "capability has no exact Make token");
  if (status == ARCHBIRD_OK)
    status = ab_act_executor_replace_exact(
        context, item_id, provider->path, selected_result.start_byte,
        selected_result.end_byte, source.bytes + selected_result.start_byte,
        selected_result.end_byte - selected_result.start_byte,
        selected_bytes.data, selected_bytes.length);
  ab_buffer_free(&selected_bytes);
  ab_string_free(engine, &selected.value);
  return status;
}

static size_t common_prefix(const AbString *left, const AbString *right) {
  size_t common = left->length < right->length ? left->length : right->length;
  size_t index = 0;
  while (index < common && left->data[index] == right->data[index])
    index++;
  return index;
}

static size_t length_distance(const AbString *left, const AbString *right) {
  return left->length > right->length ? left->length - right->length
                                      : right->length - left->length;
}

static int insertion_better(const AbString *target, const AbString *candidate,
                            const AbString *candidate_token,
                            const AbActMakeInsertion *best) {
  size_t prefix = common_prefix(target, candidate);
  size_t distance = length_distance(target, candidate);
  int compared;
  if (!best->matched || prefix != best->common_prefix)
    return !best->matched || prefix > best->common_prefix;
  if (distance != best->length_distance)
    return distance < best->length_distance;
  compared = ab_string_compare(candidate, best->anchor_name);
  if (compared)
    return compared < 0;
  return ab_string_compare(candidate_token, &best->anchor) < 0;
}

static ArchbirdStatus try_insertion(
    AbActContext *context, const ArchbirdSourceView *source,
    const AbActMakeProvider *provider, const AbString *token,
    const AbString *anchor, ArchbirdMakeVariableTokenPosition position,
    ArchbirdMakeVariableTokenInsertResult *out, AbBuffer *bytes, int *matched) {
  ArchbirdMakeVariableTokenInsertOptions options;
  char sha256[65];
  ArchbirdStatus status = source_sha256(source, sha256);
  *matched = 0;
  if (status != ARCHBIRD_OK)
    return status;
  archbird_make_variable_token_insert_options_init(&options);
  options.source_sha256 = sha256;
  options.source_sha256_length = 64;
  options.variable = (const uint8_t *)provider->variable->data;
  options.variable_length = provider->variable->length;
  options.token = (const uint8_t *)token->data;
  options.token_length = token->length;
  options.anchor_token = (const uint8_t *)anchor->data;
  options.anchor_token_length = anchor->length;
  options.position = position;
  archbird_make_variable_token_insert_result_init(out);
  ab_buffer_init(bytes, ab_act_executor_engine(context));
  status = archbird_make_variable_token_insert(
      ab_act_executor_engine(context), source->bytes, source->byte_length,
      &options, out, buffer_write, bytes);
  if (status == ARCHBIRD_OK) {
    *matched = 1;
  } else if (status == ARCHBIRD_POLICY_REJECTED && out->matched_tokens == 0) {
    archbird_error_clear(ab_act_executor_engine(context));
    status = ARCHBIRD_OK;
  }
  return status;
}

static ArchbirdStatus ground_insertion(AbActContext *context,
                                       const AbActMakeProvider *provider,
                                       const AbString *capability,
                                       const AbString *item_id) {
  ArchbirdEngine *engine = ab_act_executor_engine(context);
  const AbValue *names = field(provider->mapped_surface, "names");
  ArchbirdSourceView source;
  AbActMakeInsertion selected = {0};
  ArchbirdMakeVariableTokenInsertResult selected_result;
  AbBuffer selected_bytes;
  size_t index;
  ArchbirdStatus status =
      ab_act_executor_source(context, provider->path, &source);
  ab_buffer_init(&selected_bytes, engine);
  memset(&selected_result, 0, sizeof(selected_result));
  for (index = 0;
       status == ARCHBIRD_OK && names && names->kind == AB_VALUE_ARRAY &&
       index < names->as.array.count;
       index++) {
    const AbValue *row = &names->as.array.items[index];
    const AbValue *candidate = field(row, "name");
    size_t form;
    if (!candidate || candidate->kind != AB_VALUE_STRING ||
        ab_string_equal(&candidate->as.text, capability) ||
        !row_has_provider(row, provider))
      continue;
    for (form = 0; status == ARCHBIRD_OK && form < 2; form++) {
      AbString token = {0};
      AbString anchor = {0};
      ArchbirdMakeVariableTokenPosition position =
          ab_string_compare(capability, &candidate->as.text) < 0
              ? ARCHBIRD_MAKE_TOKEN_BEFORE
              : ARCHBIRD_MAKE_TOKEN_AFTER;
      ArchbirdMakeVariableTokenInsertResult result;
      AbBuffer bytes;
      int matched = 0;
      status = make_token(engine, capability, (int)form, &token);
      if (status == ARCHBIRD_OK)
        status = make_token(engine, &candidate->as.text, (int)form, &anchor);
      if (status == ARCHBIRD_OK)
        status = try_insertion(context, &source, provider, &token, &anchor,
                               position, &result, &bytes, &matched);
      else
        ab_buffer_init(&bytes, engine);
      if (status == ARCHBIRD_OK && matched &&
          insertion_better(capability, &candidate->as.text, &anchor,
                           &selected)) {
        ab_string_free(engine, &selected.token);
        ab_string_free(engine, &selected.anchor);
        ab_buffer_free(&selected_bytes);
        selected.token = token;
        selected.anchor = anchor;
        selected.position = position;
        selected.anchor_name = &candidate->as.text;
        selected.common_prefix = common_prefix(capability, &candidate->as.text);
        selected.length_distance =
            length_distance(capability, &candidate->as.text);
        selected_result = result;
        selected_bytes = bytes;
        selected.matched = 1;
        memset(&token, 0, sizeof(token));
        memset(&anchor, 0, sizeof(anchor));
        memset(&bytes, 0, sizeof(bytes));
      }
      ab_buffer_free(&bytes);
      ab_string_free(engine, &anchor);
      ab_string_free(engine, &token);
    }
  }
  if (status == ARCHBIRD_OK && !selected.matched)
    status = reject(context, ARCHBIRD_POLICY_REJECTED,
                    "capability has no exact Make insertion anchor");
  if (status == ARCHBIRD_OK)
    status = ab_act_executor_insert_make_token(
        context, item_id, provider->path, selected_result.start_byte,
        selected_bytes.data, selected_bytes.length, provider->variable,
        &selected.anchor, &selected.token, selected.position);
  ab_buffer_free(&selected_bytes);
  ab_string_free(engine, &selected.anchor);
  ab_string_free(engine, &selected.token);
  return status;
}

ArchbirdStatus ab_act_make_provider_capability(AbActContext *context,
                                               const AbValue *operation,
                                               const AbString *item_id) {
  const AbValue *action = field(operation, "action");
  const AbValue *capability = field(operation, "capability");
  const AbValue *from = field(operation, "from");
  const AbValue *to = field(operation, "to");
  AbActMakeProvider provider;
  const AbValue *row;
  ArchbirdStatus status = ab_act_executor_begin(
      context, item_id, "archbird.native.make.provider-capability@1");
  if (status == ARCHBIRD_OK)
    status = load_provider(context, operation, &provider);
  if (status != ARCHBIRD_OK)
    return status;
  if (text_is(action, "add_provider_capability")) {
    row = find_named_row(field(provider.mapped_surface, "names"),
                         &capability->as.text);
    if ((!target_is_resolved(row) && !target_is_implemented(row)) ||
        row_has_provider(row, &provider))
      return reject(context, ARCHBIRD_CONFLICT,
                    "current Map does not require this provider capability");
    return ground_insertion(context, &provider, &capability->as.text, item_id);
  }
  if (text_is(action, "remove_provider_capability")) {
    row = find_named_row(field(provider.mapped_surface, "names"),
                         &capability->as.text);
    if (!old_is_inactive_declaration(row, &provider) ||
        !provider_has_resolved_declaration(provider.mapped_surface, &provider,
                                           &capability->as.text))
      return reject(context, ARCHBIRD_CONFLICT,
                    "current Map does not prove a stale provider capability");
    return ground_replacement(context, &provider, &capability->as.text, NULL,
                              item_id);
  }
  if (!text_is(action, "rename_provider_capability"))
    return reject(context, ARCHBIRD_INVALID_SCHEMA,
                  "operation has an unsupported provider action");
  row = find_named_row(field(provider.mapped_surface, "names"), &from->as.text);
  if (!old_is_inactive_declaration(row, &provider))
    return reject(context, ARCHBIRD_CONFLICT,
                  "current Map does not prove the old provider capability");
  row = find_named_row(field(provider.mapped_surface, "names"), &to->as.text);
  if (!target_is_resolved(row) || row_has_provider(row, &provider))
    return reject(context, ARCHBIRD_CONFLICT,
                  "current Map does not prove the replacement capability");
  return ground_replacement(context, &provider, &from->as.text, &to->as.text,
                            item_id);
}
