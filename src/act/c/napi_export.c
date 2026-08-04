#include "c/napi_export.h"

#include "artifact_validation.h"
#include "tree_sitter/api.h"

#include <stdint.h>
#include <string.h>

#define AB_ACT_C_MAX_NAPI_EXPORTS 8192u
#define AB_ACT_C_MAX_SYNTAX_DEPTH 1024u

const TSLanguage *tree_sitter_c(void);

typedef struct AbActCNapiEntry {
  AbString name;
  AbString callback;
  size_t block_start;
  size_t block_end;
  size_t name_start;
  size_t name_end;
  size_t callback_start;
  size_t callback_end;
} AbActCNapiEntry;

typedef struct AbActCNapiScan {
  AbActContext *context;
  const uint8_t *source;
  size_t source_length;
  AbActCNapiEntry *entries;
  size_t count;
  ArchbirdStatus status;
} AbActCNapiScan;

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static ArchbirdStatus reject(AbActContext *context, const char *message) {
  return archbird_error_set(ab_act_executor_engine(context),
                            ARCHBIRD_POLICY_REJECTED, ARCHBIRD_NO_OFFSET,
                            "act C N-API export executor: %s", message);
}

static int node_is(TSNode node, const char *type) {
  return !ts_node_is_null(node) && strcmp(ts_node_type(node), type) == 0;
}

static int node_text_is(TSNode node, const uint8_t *source,
                        size_t source_length, const char *literal) {
  size_t length = strlen(literal);
  uint32_t start = ts_node_start_byte(node);
  uint32_t end = ts_node_end_byte(node);
  return end >= start && end <= source_length && end - start == length &&
         memcmp(source + start, literal, length) == 0;
}

static int plain_string(TSNode node, const uint8_t *source,
                        size_t source_length, AbString *out, size_t *out_start,
                        size_t *out_end) {
  uint32_t start = ts_node_start_byte(node);
  uint32_t end = ts_node_end_byte(node);
  if (!node_is(node, "string_literal") || end <= start + 1 ||
      end > source_length || source[start] != '"' || source[end - 1] != '"')
    return 0;
  out->data = (char *)source + start + 1;
  out->length = end - start - 2;
  *out_start = start + 1;
  *out_end = end - 1;
  return out->length != 0;
}

static int identifier(TSNode node, const uint8_t *source, size_t source_length,
                      AbString *out, size_t *out_start, size_t *out_end) {
  uint32_t start = ts_node_start_byte(node);
  uint32_t end = ts_node_end_byte(node);
  if (!node_is(node, "identifier") || end <= start || end > source_length)
    return 0;
  out->data = (char *)source + start;
  out->length = end - start;
  *out_start = start;
  *out_end = end;
  return 1;
}

static int line_block(const uint8_t *source, size_t source_length,
                      size_t node_start, size_t node_end, size_t *out_start,
                      size_t *out_end) {
  size_t start = node_start;
  size_t end = node_end;
  size_t cursor;
  while (start && source[start - 1] != '\n' && source[start - 1] != '\r')
    start--;
  for (cursor = start; cursor < node_start; cursor++)
    if (source[cursor] != ' ' && source[cursor] != '\t')
      return 0;
  while (end < source_length && (source[end] == ' ' || source[end] == '\t'))
    end++;
  if (end < source_length && source[end] == ',') {
    end++;
    while (end < source_length && (source[end] == ' ' || source[end] == '\t'))
      end++;
  }
  if (end < source_length && source[end] == '\r')
    end++;
  if (end < source_length && source[end] == '\n')
    end++;
  else if (end != source_length)
    return 0;
  *out_start = start;
  *out_end = end;
  return 1;
}

static int parse_macro_entry(AbActCNapiScan *scan, TSNode node,
                             AbActCNapiEntry *out) {
  TSNode function = ts_node_child_by_field_name(node, "function", 8);
  TSNode arguments = ts_node_child_by_field_name(node, "arguments", 9);
  TSNode public_name;
  TSNode callback;
  uint32_t count;
  if (!node_is(node, "call_expression") ||
      !node_text_is(function, scan->source, scan->source_length,
                    "DECLARE_NAPI_METHOD") ||
      !node_is(arguments, "argument_list"))
    return 0;
  count = ts_node_named_child_count(arguments);
  if (count != 2)
    return 0;
  public_name = ts_node_named_child(arguments, 0);
  callback = ts_node_named_child(arguments, 1);
  if (!plain_string(public_name, scan->source, scan->source_length, &out->name,
                    &out->name_start, &out->name_end) ||
      !identifier(callback, scan->source, scan->source_length, &out->callback,
                  &out->callback_start, &out->callback_end))
    return 0;
  return line_block(scan->source, scan->source_length, ts_node_start_byte(node),
                    ts_node_end_byte(node), &out->block_start, &out->block_end);
}

static int parse_descriptor_entry(AbActCNapiScan *scan, TSNode node,
                                  AbActCNapiEntry *out) {
  TSNode public_name;
  TSNode null_value;
  TSNode callback;
  uint32_t count;
  if (!node_is(node, "initializer_list"))
    return 0;
  count = ts_node_named_child_count(node);
  if (count < 3)
    return 0;
  public_name = ts_node_named_child(node, 0);
  null_value = ts_node_named_child(node, 1);
  callback = ts_node_named_child(node, 2);
  if (!node_text_is(null_value, scan->source, scan->source_length, "NULL") ||
      !plain_string(public_name, scan->source, scan->source_length, &out->name,
                    &out->name_start, &out->name_end) ||
      !identifier(callback, scan->source, scan->source_length, &out->callback,
                  &out->callback_start, &out->callback_end) ||
      out->callback.length <= 5 || memcmp(out->callback.data, "napi_", 5) != 0)
    return 0;
  return line_block(scan->source, scan->source_length, ts_node_start_byte(node),
                    ts_node_end_byte(node), &out->block_start, &out->block_end);
}

static void scan_node(AbActCNapiScan *scan, TSNode node, size_t depth) {
  AbActCNapiEntry entry;
  uint32_t index;
  uint32_t count;
  if (scan->status != ARCHBIRD_OK)
    return;
  if (depth > AB_ACT_C_MAX_SYNTAX_DEPTH) {
    scan->status =
        reject(scan->context, "C syntax exceeds the N-API edit depth limit");
    return;
  }
  memset(&entry, 0, sizeof(entry));
  if (parse_macro_entry(scan, node, &entry) ||
      parse_descriptor_entry(scan, node, &entry)) {
    if (scan->count >= AB_ACT_C_MAX_NAPI_EXPORTS) {
      scan->status =
          reject(scan->context, "C source has too many N-API export entries");
      return;
    }
    scan->entries[scan->count++] = entry;
  }
  count = ts_node_named_child_count(node);
  for (index = 0; index < count; index++)
    scan_node(scan, ts_node_named_child(node, index), depth + 1);
}

static int provider_row_equal(const AbValue *row,
                              const AbActCProviderCapability *provider) {
  const AbValue *definition_sha256 = field(row, "definition_sha256");
  const AbValue *path = field(row, "path");
  return definition_sha256 && definition_sha256->kind == AB_VALUE_STRING &&
         path && path->kind == AB_VALUE_STRING &&
         ab_artifact_text_is(field(row, "source"), "exports") &&
         ab_string_equal(&definition_sha256->as.text,
                         provider->definition_sha256) &&
         ab_string_equal(&path->as.text, provider->path);
}

static int mapped_export_name(const AbActCProviderCapability *provider,
                              const AbString *name) {
  const AbValue *names = field(provider->mapped_surface, "names");
  size_t index;
  for (index = 0;
       names && names->kind == AB_VALUE_ARRAY && index < names->as.array.count;
       index++) {
    const AbValue *row = &names->as.array.items[index];
    const AbValue *candidate = field(row, "name");
    const AbValue *declarations = field(row, "declarations");
    size_t declaration_index;
    if (!candidate || candidate->kind != AB_VALUE_STRING ||
        !ab_string_equal(&candidate->as.text, name))
      continue;
    for (declaration_index = 0;
         declarations && declarations->kind == AB_VALUE_ARRAY &&
         declaration_index < declarations->as.array.count;
         declaration_index++)
      if (provider_row_equal(&declarations->as.array.items[declaration_index],
                             provider))
        return 1;
  }
  return 0;
}

static int unique_wrapper(const AbValue *map, const AbString *path,
                          const AbString *name) {
  const AbValue *files = field(map, "files");
  size_t file_index;
  size_t matches = 0;
  for (file_index = 0; files && files->kind == AB_VALUE_ARRAY &&
                       file_index < files->as.array.count;
       file_index++) {
    const AbValue *file = &files->as.array.items[file_index];
    const AbValue *file_path = field(file, "path");
    const AbValue *symbols = field(file, "symbols");
    size_t symbol_index;
    if (!file_path || file_path->kind != AB_VALUE_STRING ||
        !ab_string_equal(&file_path->as.text, path) ||
        !ab_artifact_text_is(field(file, "language"), "c"))
      continue;
    for (symbol_index = 0; symbols && symbols->kind == AB_VALUE_ARRAY &&
                           symbol_index < symbols->as.array.count;
         symbol_index++) {
      const AbValue *symbol = &symbols->as.array.items[symbol_index];
      const AbValue *symbol_name = field(symbol, "name");
      if (symbol_name && symbol_name->kind == AB_VALUE_STRING &&
          ab_string_equal(&symbol_name->as.text, name) &&
          ab_artifact_text_is(field(symbol, "kind"), "function") &&
          !field(symbol, "syntax_recovery"))
        matches++;
    }
  }
  return matches == 1;
}

static int source_paths_match(const AbValue *paths, const AbString *path) {
  return paths && paths->kind == AB_VALUE_ARRAY && paths->as.array.count == 1 &&
         paths->as.array.items[0].kind == AB_VALUE_STRING &&
         ab_string_equal(&paths->as.array.items[0].as.text, path);
}

static const AbActCNapiEntry *
select_peer(const AbActCProviderCapability *provider,
            const AbActCNapiEntry *entries, size_t count, int *insert_after) {
  const AbActCNapiEntry *before = NULL;
  const AbActCNapiEntry *after = NULL;
  size_t index;
  for (index = 0; index < count; index++) {
    int compared;
    if (!mapped_export_name(provider, &entries[index].name))
      continue;
    compared = ab_string_compare(&entries[index].name, provider->capability);
    if (compared < 0 &&
        (!before || ab_string_compare(&entries[index].name, &before->name) > 0))
      before = &entries[index];
    if (compared > 0 &&
        (!after || ab_string_compare(&entries[index].name, &after->name) < 0))
      after = &entries[index];
  }
  if (before) {
    *insert_after = 1;
    return before;
  }
  *insert_after = 0;
  return after;
}

static ArchbirdStatus render_entry(AbActContext *context,
                                   const ArchbirdSourceView *source,
                                   const AbActCNapiEntry *peer,
                                   const AbString *capability,
                                   const AbString *wrapper, AbBuffer *out) {
  ArchbirdStatus status;
  if (peer->block_start > peer->name_start ||
      peer->name_start > peer->name_end ||
      peer->name_end > peer->callback_start ||
      peer->callback_start > peer->callback_end ||
      peer->callback_end > peer->block_end ||
      peer->block_end > source->byte_length)
    return reject(context, "N-API peer entry has inconsistent source spans");
  status = ab_buffer_append(out, source->bytes + peer->block_start,
                            peer->name_start - peer->block_start);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(out, capability->data, capability->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(out, source->bytes + peer->name_end,
                              peer->callback_start - peer->name_end);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(out, wrapper->data, wrapper->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(out, source->bytes + peer->callback_end,
                              peer->block_end - peer->callback_end);
  return status;
}

ArchbirdStatus ab_act_c_napi_export_provider_capability(
    AbActContext *context, const AbActCProviderCapability *provider,
    const AbValue *operation, const AbString *item_id) {
  ArchbirdEngine *engine = ab_act_executor_engine(context);
  ArchbirdSourceView source = {0};
  AbActCNapiEntry *entries = NULL;
  AbActCNapiScan scan;
  AbBuffer wrapper;
  AbBuffer replacement;
  TSParser *parser = NULL;
  TSTree *tree = NULL;
  TSNode root;
  const AbActCNapiEntry *peer = NULL;
  size_t insert_at = 0;
  size_t target_count = 0;
  size_t index;
  int insert_after = 0;
  ArchbirdStatus status = ab_act_executor_begin(
      context, item_id, "archbird.native.c.napi-export@1");
  memset(&scan, 0, sizeof(scan));
  ab_buffer_init(&wrapper, engine);
  ab_buffer_init(&replacement, engine);
  if (status == ARCHBIRD_OK &&
      !source_paths_match(field(operation, "source_paths"), provider->path))
    status =
        reject(context, "the declared source closure differs from the C export "
                        "provider");
  if (status == ARCHBIRD_OK)
    status = ab_act_executor_source(context, provider->path, &source);
  if (status == ARCHBIRD_OK &&
      (source.byte_length > UINT32_MAX || !source.bytes))
    status = reject(context, "C export source exceeds the syntax edit limit");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&wrapper, "napi_");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&wrapper, provider->capability->data,
                              provider->capability->length);
  if (status == ARCHBIRD_OK) {
    AbString wrapper_name = {(char *)wrapper.data, wrapper.length};
    if (!unique_wrapper(ab_act_executor_map(context), provider->path,
                        &wrapper_name))
      status = reject(context,
                      "the provider file has no unique exact N-API wrapper");
  }
  if (status == ARCHBIRD_OK) {
    entries = (AbActCNapiEntry *)ab_calloc(engine, AB_ACT_C_MAX_NAPI_EXPORTS,
                                           sizeof(*entries));
    if (!entries)
      status = ARCHBIRD_OUT_OF_MEMORY;
  }
  if (status == ARCHBIRD_OK) {
    parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser, tree_sitter_c()))
      status = reject(context, "cannot initialize the C syntax executor");
  }
  if (status == ARCHBIRD_OK) {
    tree = ts_parser_parse_string(parser, NULL, (const char *)source.bytes,
                                  (uint32_t)source.byte_length);
    if (!tree)
      status = reject(context, "cannot parse the current C provider source");
  }
  if (status == ARCHBIRD_OK) {
    root = ts_tree_root_node(tree);
    if (ts_node_has_error(root))
      status =
          reject(context, "current C provider source contains syntax recovery");
  }
  if (status == ARCHBIRD_OK) {
    memset(&scan, 0, sizeof(scan));
    scan.context = context;
    scan.source = source.bytes;
    scan.source_length = source.byte_length;
    scan.entries = entries;
    scan.status = ARCHBIRD_OK;
    scan_node(&scan, root, 0);
    status = scan.status;
  }
  for (index = 0; status == ARCHBIRD_OK && index < scan.count; index++)
    if (ab_string_equal(&scan.entries[index].name, provider->capability))
      target_count++;
  if (status == ARCHBIRD_OK && target_count)
    status = reject(context,
                    "the target N-API export already exists in current source");
  if (status == ARCHBIRD_OK)
    peer = select_peer(provider, scan.entries, scan.count, &insert_after);
  if (status == ARCHBIRD_OK && !peer)
    status =
        reject(context, "the provider has no exact mapped N-API peer entry");
  if (status == ARCHBIRD_OK) {
    AbString wrapper_name = {(char *)wrapper.data, wrapper.length};
    status = render_entry(context, &source, peer, provider->capability,
                          &wrapper_name, &replacement);
  }
  if (status == ARCHBIRD_OK) {
    insert_at = insert_after ? peer->block_end : peer->block_start;
    status = ab_act_executor_replace_exact(
        context, item_id, provider->path, insert_at, insert_at,
        source.bytes + insert_at, 0, replacement.data, replacement.length);
  }
  if (tree)
    ts_tree_delete(tree);
  if (parser)
    ts_parser_delete(parser);
  ab_buffer_free(&replacement);
  ab_buffer_free(&wrapper);
  ab_free(engine, entries);
  return status;
}
