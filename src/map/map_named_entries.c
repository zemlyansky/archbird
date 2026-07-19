#include "map_internal.h"

#include "archbird_internal.h"
#include "lexical/tokenizer.h"
#include "utf8.h"

#include <stdlib.h>
#include <string.h>

static int string_compare(const void *left_raw, const void *right_raw) {
  return ab_string_compare((const AbString *)left_raw,
                           (const AbString *)right_raw);
}

static int path_compare(const void *left_raw, const void *right_raw) {
  const AbMapNamedEntryPath *left = (const AbMapNamedEntryPath *)left_raw;
  const AbMapNamedEntryPath *right = (const AbMapNamedEntryPath *)right_raw;
  return ab_string_compare(&left->path, &right->path);
}

static int entry_compare(const void *left_raw, const void *right_raw) {
  const AbMapNamedEntry *left = (const AbMapNamedEntry *)left_raw;
  const AbMapNamedEntry *right = (const AbMapNamedEntry *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static int array_contains(const AbStringArray *array, const uint8_t *data,
                          size_t length) {
  size_t index;
  for (index = 0; index < array->count; index++) {
    if (array->items[index].length == length &&
        (!length || memcmp(array->items[index].data, data, length) == 0))
      return 1;
  }
  return 0;
}

static ArchbirdStatus append_unique(ArchbirdEngine *engine,
                                    AbStringArray *array, const char *data,
                                    size_t length) {
  AbString *resized;
  if (array_contains(array, (const uint8_t *)data, length))
    return ARCHBIRD_OK;
  resized = (AbString *)ab_realloc(engine, array->items,
                                   (array->count + 1) * sizeof(*array->items));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting named entries");
  array->items = resized;
  memset(&array->items[array->count], 0, sizeof(*array->items));
  if (ab_string_copy(engine, &array->items[array->count], data, length) !=
      ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  array->count++;
  return ARCHBIRD_OK;
}

static int c_suffix(const AbString *path) {
  static const char *const suffixes[] = {".c", ".h", ".cc", ".cpp", ".hpp"};
  size_t index;
  for (index = 0; index < sizeof(suffixes) / sizeof(suffixes[0]); index++) {
    size_t length = strlen(suffixes[index]);
    if (length <= path->length && memcmp(path->data + path->length - length,
                                         suffixes[index], length) == 0)
      return 1;
  }
  return 0;
}

static int path_matches(const AbConfigNamedEntry *spec,
                        const AbManifestFile *file) {
  size_t index;
  for (index = 0; index < spec->globs.count; index++) {
    if (ab_map_collection_match(&file->path, &spec->globs.items[index]))
      return 1;
  }
  return 0;
}

static void string_token(const AbTokenList *tokens, size_t index,
                         const char **out_data, size_t *out_length) {
  const AbToken *token = &tokens->items[index];
  const char *data = (const char *)tokens->source + token->start;
  size_t length = token->end - token->start;
  if (length >= 2)
    data++, length -= 2;
  *out_data = data;
  *out_length = length;
}

static ArchbirdStatus extract_file(AbMapState *state,
                                   const AbConfigNamedEntry *spec,
                                   const AbManifestFile *file,
                                   AbStringArray *out) {
  const uint8_t *source = ab_project_source_bytes(
      state->project, (size_t)(file - state->manifest->files));
  static const uint8_t empty[] = "";
  AbTokenList tokens;
  uint32_t flags =
      c_suffix(&file->path) ? AB_LEX_C_PREPROCESSOR : AB_LEX_JAVASCRIPT;
  size_t index;
  ArchbirdStatus status;
  if (!source)
    source = empty;
  status =
      ab_tokenize(state->engine, source, file->byte_length, flags, &tokens);
  if (status != ARCHBIRD_OK)
    return status;
  for (index = 0; status == ARCHBIRD_OK && index + 1 < tokens.count; index++) {
    size_t cursor;
    size_t depth = 0;
    size_t argument = 0;
    const AbToken *token = &tokens.items[index];
    if (token->kind != AB_TOKEN_IDENTIFIER ||
        !array_contains(&spec->functions, source + token->start,
                        token->end - token->start) ||
        !ab_token_equals(&tokens, index + 1, "("))
      continue;
    for (cursor = index + 2; cursor < tokens.count; cursor++) {
      if (ab_token_equals(&tokens, cursor, "(") ||
          ab_token_equals(&tokens, cursor, "[") ||
          ab_token_equals(&tokens, cursor, "{")) {
        depth++;
      } else if (ab_token_equals(&tokens, cursor, ")") ||
                 ab_token_equals(&tokens, cursor, "]") ||
                 ab_token_equals(&tokens, cursor, "}")) {
        if (ab_token_equals(&tokens, cursor, ")") && depth == 0)
          break;
        if (depth)
          depth--;
      } else if (ab_token_equals(&tokens, cursor, ",") && depth == 0) {
        argument++;
      } else if (argument == spec->argument && depth == 0 &&
                 tokens.items[cursor].kind == AB_TOKEN_STRING) {
        const char *data;
        size_t length;
        string_token(&tokens, cursor, &data, &length);
        status = append_unique(state->engine, out, data, length);
        break;
      }
    }
  }
  ab_token_list_free(&tokens);
  if (status == ARCHBIRD_OK && out->count > 1)
    qsort(out->items, out->count, sizeof(*out->items), string_compare);
  return status;
}

ArchbirdStatus ab_map_analyze_named_entries(AbMapState *state) {
  size_t spec_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (state->config->named_entry_count) {
    state->named_entries = (AbMapNamedEntry *)ab_calloc(
        state->engine, state->config->named_entry_count,
        sizeof(*state->named_entries));
    if (!state->named_entries)
      return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting named entry groups");
  }
  state->named_entry_count = state->config->named_entry_count;
  for (spec_index = 0;
       status == ARCHBIRD_OK && spec_index < state->config->named_entry_count;
       spec_index++) {
    const AbConfigNamedEntry *spec = &state->config->named_entries[spec_index];
    AbMapNamedEntry *entry = &state->named_entries[spec_index];
    size_t file_index;
    status = ab_string_copy(state->engine, &entry->name, spec->name.data,
                            spec->name.length);
    for (file_index = 0;
         status == ARCHBIRD_OK && file_index < state->manifest->file_count;
         file_index++) {
      const AbManifestFile *file = &state->manifest->files[file_index];
      AbMapNamedEntryPath row = {0};
      AbMapNamedEntryPath *resized;
      if (!path_matches(spec, file))
        continue;
      status = extract_file(state, spec, file, &row.names);
      if (status != ARCHBIRD_OK || !row.names.count) {
        size_t name;
        for (name = 0; name < row.names.count; name++)
          ab_string_free(state->engine, &row.names.items[name]);
        ab_free(state->engine, row.names.items);
        continue;
      }
      status = ab_string_copy(state->engine, &row.path, file->path.data,
                              file->path.length);
      if (status != ARCHBIRD_OK) {
        size_t name;
        for (name = 0; name < row.names.count; name++)
          ab_string_free(state->engine, &row.names.items[name]);
        ab_free(state->engine, row.names.items);
        break;
      }
      resized = (AbMapNamedEntryPath *)ab_realloc(state->engine, entry->paths,
                                                  (entry->path_count + 1) *
                                                      sizeof(*entry->paths));
      if (!resized) {
        ab_string_free(state->engine, &row.path);
        {
          size_t name;
          for (name = 0; name < row.names.count; name++)
            ab_string_free(state->engine, &row.names.items[name]);
        }
        ab_free(state->engine, row.names.items);
        status = archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory collecting named paths");
        break;
      }
      entry->paths = resized;
      entry->paths[entry->path_count++] = row;
    }
    if (status == ARCHBIRD_OK && entry->path_count > 1)
      qsort(entry->paths, entry->path_count, sizeof(*entry->paths),
            path_compare);
  }
  if (status == ARCHBIRD_OK && state->named_entry_count > 1)
    qsort(state->named_entries, state->named_entry_count,
          sizeof(*state->named_entries), entry_compare);
  return status;
}

ArchbirdStatus ab_map_render_named_entries(AbBuffer *buffer,
                                           const AbMapState *state) {
  size_t entry_index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "{");
  for (entry_index = 0;
       status == ARCHBIRD_OK && entry_index < state->named_entry_count;
       entry_index++) {
    const AbMapNamedEntry *entry = &state->named_entries[entry_index];
    size_t path_index;
    if (entry_index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, entry->name.data, entry->name.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ":{");
    for (path_index = 0;
         status == ARCHBIRD_OK && path_index < entry->path_count;
         path_index++) {
      const AbMapNamedEntryPath *path = &entry->paths[path_index];
      size_t name_index;
      if (path_index)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_json_string(buffer, path->path.data, path->path.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ":[");
      for (name_index = 0;
           status == ARCHBIRD_OK && name_index < path->names.count;
           name_index++) {
        if (name_index)
          status = ab_buffer_literal(buffer, ",");
        if (status == ARCHBIRD_OK)
          status =
              ab_buffer_json_string(buffer, path->names.items[name_index].data,
                                    path->names.items[name_index].length);
      }
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "]");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}
