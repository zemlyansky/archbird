#include <archbird/archbird.h>

#include "archbird_internal.h"
#include "json_value.h"
#include "render_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct DiffPair {
  AbString key;
  AbString value;
} DiffPair;

typedef struct DiffIndex {
  DiffPair *items;
  size_t count;
  size_t capacity;
} DiffIndex;

typedef struct DiffSection {
  const char *name;
  DiffIndex before;
  DiffIndex after;
} DiffSection;

typedef struct DiffContext {
  ArchbirdEngine *engine;
} DiffContext;

static ArchbirdStatus diff_error(DiffContext *context, const char *message) {
  return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                            ARCHBIRD_NO_OFFSET, "%s", message);
}

static const AbValue *required_member(DiffContext *context,
                                      const AbValue *object, const char *name,
                                      AbValueKind kind) {
  const AbValue *value = ab_value_member(object, name);
  if (!value || value->kind != kind) {
    archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                       ARCHBIRD_NO_OFFSET,
                       "diff map field '%s' has the wrong type", name);
    return NULL;
  }
  return value;
}

static const AbValue *optional_array(const AbValue *object, const char *name) {
  static const AbValue empty = {.kind = AB_VALUE_ARRAY};
  const AbValue *value = ab_value_member(object, name);
  return value ? value : &empty;
}

static int string_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static void diff_index_free(ArchbirdEngine *engine, DiffIndex *index) {
  size_t item;
  for (item = 0; item < index->count; item++) {
    ab_string_free(engine, &index->items[item].key);
    ab_string_free(engine, &index->items[item].value);
  }
  ab_free(engine, index->items);
  memset(index, 0, sizeof(*index));
}

static ArchbirdStatus index_add(DiffContext *context, DiffIndex *index,
                                const char *key, size_t key_length,
                                const char *value, size_t value_length) {
  DiffPair *resized;
  DiffPair *pair;
  ArchbirdStatus status;
  if (!key_length)
    return ARCHBIRD_OK;
  if (index->count == index->capacity) {
    size_t capacity = index->capacity ? index->capacity * 2 : 32;
    resized = (DiffPair *)ab_realloc(context->engine, index->items,
                                     capacity * sizeof(*index->items));
    if (!resized)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing map diff");
    index->items = resized;
    index->capacity = capacity;
  }
  pair = &index->items[index->count];
  memset(pair, 0, sizeof(*pair));
  status = ab_string_copy(context->engine, &pair->key, key, key_length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(context->engine, &pair->value, value, value_length);
  if (status != ARCHBIRD_OK) {
    ab_string_free(context->engine, &pair->key);
    ab_string_free(context->engine, &pair->value);
    return status;
  }
  index->count++;
  return ARCHBIRD_OK;
}

static int pair_compare(const void *left_raw, const void *right_raw) {
  return ab_string_compare(&((const DiffPair *)left_raw)->key,
                           &((const DiffPair *)right_raw)->key);
}

static ArchbirdStatus index_finish(DiffContext *context, DiffIndex *index) {
  size_t item;
  if (index->count > 1)
    qsort(index->items, index->count, sizeof(*index->items), pair_compare);
  for (item = 1; item < index->count; item++) {
    if (ab_string_equal(&index->items[item - 1].key, &index->items[item].key))
      return archbird_error_set(
          context->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "diff map has duplicate structural identity '%.*s'",
          (int)index->items[item].key.length, index->items[item].key.data);
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_buffers(DiffContext *context, DiffIndex *index,
                                  AbBuffer *key, AbBuffer *value) {
  return index_add(context, index, (const char *)key->data, key->length,
                   (const char *)value->data, value->length);
}

static ArchbirdStatus render_text(AbBuffer *buffer, const AbValue *value) {
  if (value->kind == AB_VALUE_STRING || value->kind == AB_VALUE_INTEGER)
    return ab_buffer_append(buffer, value->as.text.data, value->as.text.length);
  if (value->kind == AB_VALUE_REAL)
    return ab_value_render(buffer, value);
  if (value->kind == AB_VALUE_BOOL)
    return ab_buffer_literal(buffer, value->as.boolean ? "True" : "False");
  if (value->kind == AB_VALUE_NULL)
    return ab_buffer_literal(buffer, "None");
  return ab_value_render(buffer, value);
}

static int value_string_pointer_compare(const void *left_raw,
                                        const void *right_raw) {
  const AbValue *left = *(const AbValue *const *)left_raw;
  const AbValue *right = *(const AbValue *const *)right_raw;
  return ab_string_compare(&left->as.text, &right->as.text);
}

static ArchbirdStatus render_sorted_strings(DiffContext *context,
                                            AbBuffer *buffer,
                                            const AbValue *array) {
  const AbValue **items = NULL;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!array || array->kind != AB_VALUE_ARRAY)
    return diff_error(context, "diff expected an array of strings");
  if (array->as.array.count) {
    items = (const AbValue **)ab_malloc(context->engine,
                                        array->as.array.count * sizeof(*items));
    if (!items)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory sorting diff values");
  }
  for (index = 0; index < array->as.array.count; index++) {
    if (array->as.array.items[index].kind != AB_VALUE_STRING) {
      ab_free(context->engine, items);
      return diff_error(context, "diff expected an array of strings");
    }
    items[index] = &array->as.array.items[index];
  }
  if (array->as.array.count > 1)
    qsort(items, array->as.array.count, sizeof(*items),
          value_string_pointer_compare);
  status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < array->as.array.count;
       index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, items[index]->as.text.data,
                                     items[index]->as.text.length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  ab_free(context->engine, items);
  return status;
}

static int owned_string_compare(const void *left_raw, const void *right_raw) {
  return ab_string_compare((const AbString *)left_raw,
                           (const AbString *)right_raw);
}

static ArchbirdStatus render_sorted_values(DiffContext *context,
                                           AbBuffer *buffer,
                                           const AbValue *array) {
  AbString *items = NULL;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!array || array->kind != AB_VALUE_ARRAY)
    return diff_error(context, "diff expected an array");
  if (array->as.array.count) {
    items = (AbString *)ab_calloc(context->engine, array->as.array.count,
                                  sizeof(*items));
    if (!items)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory sorting diff rows");
  }
  for (index = 0; status == ARCHBIRD_OK && index < array->as.array.count;
       index++) {
    AbBuffer rendered;
    ab_buffer_init(&rendered, context->engine);
    status = ab_value_render(&rendered, &array->as.array.items[index]);
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(context->engine, &items[index],
                              (const char *)rendered.data, rendered.length);
    ab_buffer_free(&rendered);
  }
  if (status == ARCHBIRD_OK && array->as.array.count > 1)
    qsort(items, array->as.array.count, sizeof(*items), owned_string_compare);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < array->as.array.count;
       index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(buffer, items[index].data, items[index].length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  for (index = 0; index < array->as.array.count; index++)
    ab_string_free(context->engine, &items[index]);
  ab_free(context->engine, items);
  return status;
}

/* Relation candidates and evidence are set-valued in the Map schema. Their
 * serialized order may change when provider merge order changes, but that is
 * not an architectural change. Other arrays remain order-sensitive. */
static ArchbirdStatus render_relation_row(DiffContext *context,
                                          AbBuffer *buffer,
                                          const AbValue *row) {
  size_t field;
  ArchbirdStatus status;
  if (!row || row->kind != AB_VALUE_OBJECT)
    return diff_error(context, "symbol relation must be an object");
  status = ab_buffer_literal(buffer, "{");
  for (field = 0; status == ARCHBIRD_OK && field < row->as.object.count;
       field++) {
    const AbObjectField *member = &row->as.object.fields[field];
    if (field)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, member->name.data, member->name.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ":");
    if (status != ARCHBIRD_OK)
      break;
    if (string_is(&member->name, "candidates") ||
        string_is(&member->name, "evidence"))
      status = render_sorted_values(context, buffer, &member->value);
    else
      status = ab_value_render(buffer, &member->value);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

static ArchbirdStatus key_parts(AbBuffer *buffer, const AbString **parts,
                                size_t count) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, "|");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(buffer, parts[index]->data, parts[index]->length);
  }
  return status;
}

static ArchbirdStatus simple_rows(DiffContext *context, const AbValue *map,
                                  const char *section, const char *key_field,
                                  const char *value_field, DiffIndex *out) {
  const AbValue *rows = required_member(context, map, section, AB_VALUE_ARRAY);
  size_t index;
  ArchbirdStatus status = rows ? ARCHBIRD_OK : ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *key =
        required_member(context, row, key_field, AB_VALUE_STRING);
    const AbValue *value =
        required_member(context, row, value_field, AB_VALUE_STRING);
    if (!key || !value)
      return ARCHBIRD_INVALID_SCHEMA;
    status = index_add(context, out, key->as.text.data, key->as.text.length,
                       value->as.text.data, value->as.text.length);
  }
  return status == ARCHBIRD_OK ? index_finish(context, out) : status;
}

static ArchbirdStatus edge_index(DiffContext *context, const AbValue *map,
                                 DiffIndex *edges, DiffIndex *bridges) {
  const AbValue *rows = required_member(context, map, "edges", AB_VALUE_ARRAY);
  size_t index;
  ArchbirdStatus status = rows ? ARCHBIRD_OK : ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *kind =
        required_member(context, row, "kind", AB_VALUE_STRING);
    const AbValue *source =
        required_member(context, row, "source", AB_VALUE_STRING);
    const AbValue *target =
        required_member(context, row, "target", AB_VALUE_STRING);
    const AbValue *names =
        required_member(context, row, "names", AB_VALUE_ARRAY);
    const AbString *parts[3];
    AbBuffer key;
    AbBuffer value;
    if (!kind || !source || !target || !names)
      return ARCHBIRD_INVALID_SCHEMA;
    parts[0] = &kind->as.text;
    parts[1] = &source->as.text;
    parts[2] = &target->as.text;
    ab_buffer_init(&key, context->engine);
    ab_buffer_init(&value, context->engine);
    status = key_parts(&key, parts, 3);
    if (status == ARCHBIRD_OK)
      status = render_sorted_strings(context, &value, names);
    if (status == ARCHBIRD_OK)
      status = add_buffers(context, edges, &key, &value);
    if (status == ARCHBIRD_OK && kind->as.text.length >= 7 &&
        memcmp(kind->as.text.data, "bridge:", 7) == 0)
      status = add_buffers(context, bridges, &key, &value);
    ab_buffer_free(&value);
    ab_buffer_free(&key);
  }
  if (status == ARCHBIRD_OK)
    status = index_finish(context, edges);
  if (status == ARCHBIRD_OK)
    status = index_finish(context, bridges);
  return status;
}

static int pair_full_compare(const void *left_raw, const void *right_raw) {
  const DiffPair *left = (const DiffPair *)left_raw;
  const DiffPair *right = (const DiffPair *)right_raw;
  int compared = ab_string_compare(&left->key, &right->key);
  return compared ? compared : ab_string_compare(&left->value, &right->value);
}

static ArchbirdStatus call_index(DiffContext *context, const AbValue *map,
                                 DiffIndex *out) {
  const AbValue *rows =
      required_member(context, map, "call_resolutions", AB_VALUE_ARRAY);
  DiffIndex grouped = {0};
  size_t index;
  ArchbirdStatus status = rows ? ARCHBIRD_OK : ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *source =
        required_member(context, row, "source", AB_VALUE_STRING);
    const AbValue *name =
        required_member(context, row, "name", AB_VALUE_STRING);
    const AbValue *kind =
        required_member(context, row, "kind", AB_VALUE_STRING);
    const AbValue *count =
        required_member(context, row, "count", AB_VALUE_INTEGER);
    const AbValue *candidates =
        required_member(context, row, "candidates", AB_VALUE_ARRAY);
    const AbString *parts[2];
    AbBuffer key;
    AbBuffer value;
    if (!source || !name || !kind || !count || !candidates)
      return ARCHBIRD_INVALID_SCHEMA;
    parts[0] = &source->as.text;
    parts[1] = &name->as.text;
    ab_buffer_init(&key, context->engine);
    ab_buffer_init(&value, context->engine);
    status = key_parts(&key, parts, 2);
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(&value, kind->as.text.data, kind->as.text.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&value, "|");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(&value, count->as.text.data, count->as.text.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&value, "|");
    if (status == ARCHBIRD_OK)
      status = render_sorted_strings(context, &value, candidates);
    if (status == ARCHBIRD_OK)
      status = add_buffers(context, &grouped, &key, &value);
    ab_buffer_free(&value);
    ab_buffer_free(&key);
  }
  if (status == ARCHBIRD_OK && grouped.count > 1)
    qsort(grouped.items, grouped.count, sizeof(*grouped.items),
          pair_full_compare);
  if (status == ARCHBIRD_OK) {
    size_t start = 0;
    while (start < grouped.count) {
      size_t end = start + 1;
      AbBuffer value;
      while (end < grouped.count && ab_string_equal(&grouped.items[start].key,
                                                    &grouped.items[end].key))
        end++;
      ab_buffer_init(&value, context->engine);
      if (end == start + 1) {
        status = ab_buffer_append(&value, grouped.items[start].value.data,
                                  grouped.items[start].value.length);
      } else {
        size_t item;
        status = ab_buffer_literal(&value, "[");
        for (item = start; status == ARCHBIRD_OK && item < end; item++) {
          if (item != start)
            status = ab_buffer_literal(&value, ",");
          if (status == ARCHBIRD_OK)
            status =
                ab_buffer_json_string(&value, grouped.items[item].value.data,
                                      grouped.items[item].value.length);
        }
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&value, "]");
      }
      if (status == ARCHBIRD_OK)
        status = index_add(context, out, grouped.items[start].key.data,
                           grouped.items[start].key.length,
                           (const char *)value.data, value.length);
      ab_buffer_free(&value);
      if (status != ARCHBIRD_OK)
        break;
      start = end;
    }
  }
  diff_index_free(context->engine, &grouped);
  return status == ARCHBIRD_OK ? index_finish(context, out) : status;
}

static ArchbirdStatus relation_index(DiffContext *context, const AbValue *map,
                                     const char *field, DiffIndex *out) {
  const AbValue *rows = optional_array(map, field);
  DiffIndex grouped = {0};
  size_t index;
  ArchbirdStatus status =
      rows->kind == AB_VALUE_ARRAY
          ? ARCHBIRD_OK
          : diff_error(context, "symbol relations must be arrays");
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *source =
        required_member(context, row, "source", AB_VALUE_OBJECT);
    const AbValue *name =
        required_member(context, row, "name", AB_VALUE_STRING);
    const AbValue *source_path;
    const AbValue *source_symbol;
    const AbValue *source_scope;
    const AbValue *context_value;
    const AbValue *container;
    AbString empty = {(char *)"", 0};
    AbString symbol_scope = {(char *)"symbol", 6};
    const AbString *parts[6];
    AbBuffer key;
    AbBuffer value;
    if (!source || !name)
      return ARCHBIRD_INVALID_SCHEMA;
    source_path = required_member(context, source, "path", AB_VALUE_STRING);
    source_symbol = ab_value_member(source, "symbol");
    source_scope = ab_value_member(source, "scope");
    context_value = ab_value_member(row, "context");
    container = ab_value_member(row, "container");
    if (!source_path ||
        (source_symbol && source_symbol->kind != AB_VALUE_STRING) ||
        (source_scope && source_scope->kind != AB_VALUE_STRING) ||
        (!!source_symbol == !!source_scope) ||
        (context_value && context_value->kind != AB_VALUE_STRING) ||
        (container && container->kind != AB_VALUE_STRING))
      return ARCHBIRD_INVALID_SCHEMA;
    parts[0] = &source_path->as.text;
    parts[1] = source_symbol ? &symbol_scope : &source_scope->as.text;
    parts[2] = source_symbol ? &source_symbol->as.text : &empty;
    parts[3] = &name->as.text;
    parts[4] = context_value ? &context_value->as.text : &empty;
    parts[5] = container ? &container->as.text : &empty;
    ab_buffer_init(&key, context->engine);
    ab_buffer_init(&value, context->engine);
    status = key_parts(&key, parts, 6);
    if (status == ARCHBIRD_OK)
      status = render_relation_row(context, &value, row);
    if (status == ARCHBIRD_OK)
      status = add_buffers(context, &grouped, &key, &value);
    ab_buffer_free(&value);
    ab_buffer_free(&key);
  }
  if (status == ARCHBIRD_OK && grouped.count > 1)
    qsort(grouped.items, grouped.count, sizeof(*grouped.items),
          pair_full_compare);
  if (status == ARCHBIRD_OK) {
    size_t start = 0;
    while (start < grouped.count) {
      size_t end = start + 1;
      size_t item;
      AbBuffer value;
      while (end < grouped.count && ab_string_equal(&grouped.items[start].key,
                                                    &grouped.items[end].key))
        end++;
      ab_buffer_init(&value, context->engine);
      status = ab_buffer_literal(&value, "[");
      for (item = start; status == ARCHBIRD_OK && item < end; item++) {
        if (item != start)
          status = ab_buffer_literal(&value, ",");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&value, grouped.items[item].value.data,
                                    grouped.items[item].value.length);
      }
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, "]");
      if (status == ARCHBIRD_OK)
        status = index_add(context, out, grouped.items[start].key.data,
                           grouped.items[start].key.length,
                           (const char *)value.data, value.length);
      ab_buffer_free(&value);
      if (status != ARCHBIRD_OK)
        break;
      start = end;
    }
  }
  diff_index_free(context->engine, &grouped);
  return status == ARCHBIRD_OK ? index_finish(context, out) : status;
}

static ArchbirdStatus symbol_index(DiffContext *context, const AbValue *map,
                                   int public_only, DiffIndex *out) {
  const AbValue *files = required_member(context, map, "files", AB_VALUE_ARRAY);
  DiffIndex grouped = {0};
  size_t file_index;
  ArchbirdStatus status = files ? ARCHBIRD_OK : ARCHBIRD_INVALID_SCHEMA;
  for (file_index = 0;
       status == ARCHBIRD_OK && file_index < files->as.array.count;
       file_index++) {
    const AbValue *file = &files->as.array.items[file_index];
    const AbValue *path =
        required_member(context, file, "path", AB_VALUE_STRING);
    const AbValue *symbols =
        required_member(context, file, "symbols", AB_VALUE_ARRAY);
    size_t symbol;
    if (!path || !symbols)
      return ARCHBIRD_INVALID_SCHEMA;
    for (symbol = 0; symbol < symbols->as.array.count; symbol++) {
      const AbValue *row = &symbols->as.array.items[symbol];
      const AbValue *name =
          required_member(context, row, "name", AB_VALUE_STRING);
      const AbValue *kind =
          required_member(context, row, "kind", AB_VALUE_STRING);
      const AbValue *scope =
          required_member(context, row, "scope", AB_VALUE_STRING);
      const AbValue *signature =
          required_member(context, row, "signature", AB_VALUE_STRING);
      const AbString *parts[4];
      AbBuffer key;
      if (!name || !kind || !scope || !signature)
        return ARCHBIRD_INVALID_SCHEMA;
      if (public_only && !string_is(&scope->as.text, "public"))
        continue;
      parts[0] = &path->as.text;
      parts[1] = &name->as.text;
      parts[2] = &kind->as.text;
      parts[3] = &scope->as.text;
      ab_buffer_init(&key, context->engine);
      status = key_parts(&key, parts, 4);
      if (status == ARCHBIRD_OK)
        status =
            index_add(context, &grouped, (const char *)key.data, key.length,
                      signature->as.text.data, signature->as.text.length);
      ab_buffer_free(&key);
      if (status != ARCHBIRD_OK)
        break;
    }
  }
  if (status == ARCHBIRD_OK && grouped.count > 1)
    qsort(grouped.items, grouped.count, sizeof(*grouped.items),
          pair_full_compare);
  if (status == ARCHBIRD_OK) {
    size_t start = 0;
    while (start < grouped.count) {
      size_t end = start + 1;
      size_t item;
      AbBuffer value;
      while (end < grouped.count && ab_string_equal(&grouped.items[start].key,
                                                    &grouped.items[end].key))
        end++;
      ab_buffer_init(&value, context->engine);
      status = ab_buffer_literal(&value, "[");
      for (item = start; status == ARCHBIRD_OK && item < end; item++) {
        if (item != start)
          status = ab_buffer_literal(&value, ",");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_json_string(&value, grouped.items[item].value.data,
                                         grouped.items[item].value.length);
      }
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, "]");
      if (status == ARCHBIRD_OK)
        status = index_add(context, out, grouped.items[start].key.data,
                           grouped.items[start].key.length,
                           (const char *)value.data, value.length);
      ab_buffer_free(&value);
      if (status != ARCHBIRD_OK)
        break;
      start = end;
    }
  }
  diff_index_free(context->engine, &grouped);
  return status == ARCHBIRD_OK ? index_finish(context, out) : status;
}

static ArchbirdStatus package_indexes(DiffContext *context, const AbValue *map,
                                      DiffIndex *exports, DiffIndex *origins,
                                      DiffIndex *dependencies,
                                      DiffIndex *entrypoints,
                                      DiffIndex *entrypoint_surfaces) {
  const AbValue *rows =
      required_member(context, map, "packages", AB_VALUE_ARRAY);
  size_t index;
  ArchbirdStatus status = rows ? ARCHBIRD_OK : ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *label =
        required_member(context, row, "name", AB_VALUE_STRING);
    const AbValue *export_rows =
        required_member(context, row, "exports", AB_VALUE_ARRAY);
    const AbValue *origin_rows =
        required_member(context, row, "export_origins", AB_VALUE_OBJECT);
    const AbValue *dependency_rows =
        required_member(context, row, "dependencies", AB_VALUE_ARRAY);
    const AbValue *entrypoint_rows =
        required_member(context, row, "entrypoints", AB_VALUE_OBJECT);
    const AbValue *surface_rows = optional_array(row, "entrypoint_surfaces");
    size_t item;
    if (!label || !export_rows || !origin_rows || !dependency_rows ||
        !entrypoint_rows || surface_rows->kind != AB_VALUE_ARRAY)
      return ARCHBIRD_INVALID_SCHEMA;
    for (item = 0; status == ARCHBIRD_OK && item < export_rows->as.array.count;
         item++) {
      const AbValue *name = &export_rows->as.array.items[item];
      const AbString *parts[2];
      AbBuffer key;
      if (name->kind != AB_VALUE_STRING)
        return diff_error(context, "package exports must be strings");
      parts[0] = &label->as.text;
      parts[1] = &name->as.text;
      ab_buffer_init(&key, context->engine);
      status = key_parts(&key, parts, 2);
      if (status == ARCHBIRD_OK)
        status = index_add(context, exports, (const char *)key.data, key.length,
                           "present", 7);
      ab_buffer_free(&key);
    }
    for (item = 0; status == ARCHBIRD_OK && item < origin_rows->as.object.count;
         item++) {
      const AbObjectField *origin = &origin_rows->as.object.fields[item];
      const AbString *parts[2] = {&label->as.text, &origin->name};
      AbBuffer key;
      AbBuffer value;
      ab_buffer_init(&key, context->engine);
      ab_buffer_init(&value, context->engine);
      status = key_parts(&key, parts, 2);
      if (status == ARCHBIRD_OK)
        status = render_sorted_strings(context, &value, &origin->value);
      if (status == ARCHBIRD_OK)
        status = add_buffers(context, origins, &key, &value);
      ab_buffer_free(&value);
      ab_buffer_free(&key);
    }
    for (item = 0;
         status == ARCHBIRD_OK && item < dependency_rows->as.array.count;
         item++) {
      const AbValue *dependency = &dependency_rows->as.array.items[item];
      const AbValue *name =
          required_member(context, dependency, "name", AB_VALUE_STRING);
      const AbValue *scope =
          required_member(context, dependency, "scope", AB_VALUE_STRING);
      const AbValue *requirement =
          required_member(context, dependency, "requirement", AB_VALUE_STRING);
      const AbString *parts[2];
      AbBuffer key;
      AbBuffer value;
      if (!name || !scope || !requirement)
        return ARCHBIRD_INVALID_SCHEMA;
      parts[0] = &label->as.text;
      parts[1] = &name->as.text;
      ab_buffer_init(&key, context->engine);
      ab_buffer_init(&value, context->engine);
      status = key_parts(&key, parts, 2);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&value, scope->as.text.data,
                                  scope->as.text.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, "|");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&value, requirement->as.text.data,
                                  requirement->as.text.length);
      if (status == ARCHBIRD_OK)
        status = add_buffers(context, dependencies, &key, &value);
      ab_buffer_free(&value);
      ab_buffer_free(&key);
    }
    for (item = 0;
         status == ARCHBIRD_OK && item < entrypoint_rows->as.object.count;
         item++) {
      const AbObjectField *entry = &entrypoint_rows->as.object.fields[item];
      const AbString *parts[2] = {&label->as.text, &entry->name};
      AbBuffer key;
      if (entry->value.kind != AB_VALUE_STRING)
        return diff_error(context,
                          "package entrypoint target must be a string");
      ab_buffer_init(&key, context->engine);
      status = key_parts(&key, parts, 2);
      if (status == ARCHBIRD_OK)
        status =
            index_add(context, entrypoints, (const char *)key.data, key.length,
                      entry->value.as.text.data, entry->value.as.text.length);
      ab_buffer_free(&key);
    }
    for (item = 0; status == ARCHBIRD_OK && item < surface_rows->as.array.count;
         item++) {
      const AbValue *surface = &surface_rows->as.array.items[item];
      const AbValue *path =
          required_member(context, surface, "path", AB_VALUE_STRING);
      const AbString *parts[2];
      AbBuffer key;
      AbBuffer value;
      if (!path)
        return ARCHBIRD_INVALID_SCHEMA;
      parts[0] = &label->as.text;
      parts[1] = &path->as.text;
      ab_buffer_init(&key, context->engine);
      ab_buffer_init(&value, context->engine);
      status = key_parts(&key, parts, 2);
      if (status == ARCHBIRD_OK)
        status = ab_value_render(&value, surface);
      if (status == ARCHBIRD_OK)
        status = add_buffers(context, entrypoint_surfaces, &key, &value);
      ab_buffer_free(&value);
      ab_buffer_free(&key);
    }
  }
  if (status == ARCHBIRD_OK)
    status = index_finish(context, exports);
  if (status == ARCHBIRD_OK)
    status = index_finish(context, origins);
  if (status == ARCHBIRD_OK)
    status = index_finish(context, dependencies);
  if (status == ARCHBIRD_OK)
    status = index_finish(context, entrypoints);
  if (status == ARCHBIRD_OK)
    status = index_finish(context, entrypoint_surfaces);
  return status;
}

static ArchbirdStatus test_route_index(DiffContext *context, const AbValue *map,
                                       DiffIndex *out) {
  const AbValue *tests = required_member(context, map, "tests", AB_VALUE_ARRAY);
  size_t index;
  ArchbirdStatus status = tests ? ARCHBIRD_OK : ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; status == ARCHBIRD_OK && index < tests->as.array.count;
       index++) {
    const AbValue *test = &tests->as.array.items[index];
    const AbValue *path =
        required_member(context, test, "path", AB_VALUE_STRING);
    const AbValue *group =
        required_member(context, test, "group", AB_VALUE_STRING);
    const AbValue *routes =
        required_member(context, test, "routes", AB_VALUE_OBJECT);
    size_t route;
    if (!path || !group || !routes)
      return ARCHBIRD_INVALID_SCHEMA;
    for (route = 0; route < routes->as.object.count; route++) {
      const AbObjectField *target = &routes->as.object.fields[route];
      const AbString *parts[3] = {&group->as.text, &path->as.text,
                                  &target->name};
      AbBuffer key;
      AbBuffer value;
      ab_buffer_init(&key, context->engine);
      ab_buffer_init(&value, context->engine);
      status = key_parts(&key, parts, 3);
      if (status == ARCHBIRD_OK)
        status = render_text(&value, &target->value);
      if (status == ARCHBIRD_OK)
        status = add_buffers(context, out, &key, &value);
      ab_buffer_free(&value);
      ab_buffer_free(&key);
      if (status != ARCHBIRD_OK)
        break;
    }
  }
  return status == ARCHBIRD_OK ? index_finish(context, out) : status;
}

static ArchbirdStatus
test_route_evidence_rows(DiffContext *context, DiffIndex *out,
                         const AbString *group, const AbString *path,
                         const AbString *selector, const AbValue *rows) {
  static char empty_data[] = "";
  static const AbString empty = {empty_data, 0};
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!rows)
    return ARCHBIRD_OK;
  if (rows->kind != AB_VALUE_ARRAY)
    return diff_error(context, "test route evidence must be an array");
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *target =
        required_member(context, row, "target", AB_VALUE_STRING);
    const AbValue *target_symbol =
        required_member(context, row, "target_symbol", AB_VALUE_STRING);
    const AbValue *provenance =
        required_member(context, row, "provenance", AB_VALUE_STRING);
    const AbValue *relation =
        required_member(context, row, "relation", AB_VALUE_STRING);
    const AbValue *provider =
        required_member(context, row, "provider", AB_VALUE_STRING);
    const AbValue *fact_id = ab_value_member(row, "fact_id");
    const AbValue *observation = ab_value_member(row, "observation_sha256");
    const AbString *correlation = &empty;
    const AbString *parts[9];
    AbBuffer key;
    AbBuffer value;
    if (!target || !target_symbol || !provenance || !relation || !provider ||
        (fact_id && fact_id->kind != AB_VALUE_STRING) ||
        (observation && observation->kind != AB_VALUE_STRING))
      return ARCHBIRD_INVALID_SCHEMA;
    if (observation && observation->as.text.length)
      correlation = &observation->as.text;
    else if (fact_id)
      correlation = &fact_id->as.text;
    parts[0] = group;
    parts[1] = path;
    parts[2] = selector;
    parts[3] = &target->as.text;
    parts[4] = &target_symbol->as.text;
    parts[5] = &provenance->as.text;
    parts[6] = &relation->as.text;
    parts[7] = &provider->as.text;
    parts[8] = correlation;
    ab_buffer_init(&key, context->engine);
    ab_buffer_init(&value, context->engine);
    status = key_parts(&key, parts, sizeof(parts) / sizeof(parts[0]));
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&value, row);
    if (status == ARCHBIRD_OK)
      status = add_buffers(context, out, &key, &value);
    ab_buffer_free(&value);
    ab_buffer_free(&key);
  }
  return status;
}

static ArchbirdStatus test_route_evidence_index(DiffContext *context,
                                                const AbValue *map,
                                                DiffIndex *out) {
  const AbValue *tests = required_member(context, map, "tests", AB_VALUE_ARRAY);
  size_t index;
  ArchbirdStatus status = tests ? ARCHBIRD_OK : ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; status == ARCHBIRD_OK && index < tests->as.array.count;
       index++) {
    const AbValue *test = &tests->as.array.items[index];
    const AbValue *path =
        required_member(context, test, "path", AB_VALUE_STRING);
    const AbValue *group =
        required_member(context, test, "group", AB_VALUE_STRING);
    const AbValue *cases =
        required_member(context, test, "cases", AB_VALUE_ARRAY);
    size_t case_index;
    if (!path || !group || !cases)
      return ARCHBIRD_INVALID_SCHEMA;
    if (!cases->as.array.count)
      status = test_route_evidence_rows(
          context, out, &group->as.text, &path->as.text, &path->as.text,
          ab_value_member(test, "route_evidence"));
    for (case_index = 0;
         status == ARCHBIRD_OK && case_index < cases->as.array.count;
         case_index++) {
      const AbValue *test_case = &cases->as.array.items[case_index];
      const AbValue *selector =
          required_member(context, test_case, "selector", AB_VALUE_STRING);
      if (!selector)
        return ARCHBIRD_INVALID_SCHEMA;
      status = test_route_evidence_rows(
          context, out, &group->as.text, &path->as.text, &selector->as.text,
          ab_value_member(test_case, "route_evidence"));
    }
  }
  return status == ARCHBIRD_OK ? index_finish(context, out) : status;
}

static ArchbirdStatus component_index(DiffContext *context, const AbValue *map,
                                      DiffIndex *out) {
  const AbValue *components =
      required_member(context, map, "components", AB_VALUE_ARRAY);
  size_t index;
  ArchbirdStatus status = components ? ARCHBIRD_OK : ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; status == ARCHBIRD_OK && index < components->as.array.count;
       index++) {
    const AbValue *component = &components->as.array.items[index];
    const AbValue *name =
        required_member(context, component, "name", AB_VALUE_STRING);
    const AbValue *files =
        required_member(context, component, "files", AB_VALUE_ARRAY);
    const AbValue *outgoing =
        required_member(context, component, "outgoing", AB_VALUE_OBJECT);
    AbBuffer key;
    AbBuffer value;
    size_t route;
    if (!name || !files || !outgoing)
      return ARCHBIRD_INVALID_SCHEMA;
    ab_buffer_init(&key, context->engine);
    ab_buffer_init(&value, context->engine);
    status = ab_buffer_append(&key, name->as.text.data, name->as.text.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&key, "|files");
    if (status == ARCHBIRD_OK)
      status = render_sorted_strings(context, &value, files);
    if (status == ARCHBIRD_OK)
      status = add_buffers(context, out, &key, &value);
    ab_buffer_free(&value);
    ab_buffer_free(&key);
    for (route = 0; status == ARCHBIRD_OK && route < outgoing->as.object.count;
         route++) {
      const AbObjectField *target = &outgoing->as.object.fields[route];
      const AbString *parts[2] = {&name->as.text, &target->name};
      ab_buffer_init(&key, context->engine);
      ab_buffer_init(&value, context->engine);
      status = key_parts(&key, parts, 2);
      if (status == ARCHBIRD_OK)
        status = render_sorted_strings(context, &value, &target->value);
      if (status == ARCHBIRD_OK)
        status = add_buffers(context, out, &key, &value);
      ab_buffer_free(&value);
      ab_buffer_free(&key);
    }
  }
  return status == ARCHBIRD_OK ? index_finish(context, out) : status;
}

static ArchbirdStatus parity_index(DiffContext *context, const AbValue *map,
                                   DiffIndex *out) {
  const AbValue *parity =
      required_member(context, map, "parity", AB_VALUE_ARRAY);
  size_t parity_index;
  ArchbirdStatus status = parity ? ARCHBIRD_OK : ARCHBIRD_INVALID_SCHEMA;
  for (parity_index = 0;
       status == ARCHBIRD_OK && parity_index < parity->as.array.count;
       parity_index++) {
    const AbValue *row = &parity->as.array.items[parity_index];
    const AbValue *name =
        required_member(context, row, "name", AB_VALUE_STRING);
    const AbValue *members =
        required_member(context, row, "members", AB_VALUE_ARRAY);
    size_t member_index;
    if (!name || !members)
      return ARCHBIRD_INVALID_SCHEMA;
    for (member_index = 0;
         status == ARCHBIRD_OK && member_index < members->as.array.count;
         member_index++) {
      const AbValue *member = &members->as.array.items[member_index];
      const AbValue *label =
          required_member(context, member, "label", AB_VALUE_STRING);
      const AbValue *missing =
          required_member(context, member, "missing", AB_VALUE_ARRAY);
      size_t missing_index;
      if (!label || !missing)
        return ARCHBIRD_INVALID_SCHEMA;
      for (missing_index = 0; missing_index < missing->as.array.count;
           missing_index++) {
        const AbValue *value = &missing->as.array.items[missing_index];
        const AbString *parts[3];
        AbBuffer key;
        if (value->kind != AB_VALUE_STRING)
          return diff_error(context, "parity missing values must be strings");
        parts[0] = &name->as.text;
        parts[1] = &label->as.text;
        parts[2] = &value->as.text;
        ab_buffer_init(&key, context->engine);
        status = key_parts(&key, parts, 3);
        if (status == ARCHBIRD_OK)
          status = index_add(context, out, (const char *)key.data, key.length,
                             "missing", 7);
        ab_buffer_free(&key);
        if (status != ARCHBIRD_OK)
          break;
      }
    }
  }
  return status == ARCHBIRD_OK ? index_finish(context, out) : status;
}

static ArchbirdStatus projection_value(DiffContext *context, const AbValue *row,
                                       const char *const *fields,
                                       size_t field_count, AbBuffer *out) {
  size_t field;
  ArchbirdStatus status = ab_buffer_literal(out, "{");
  for (field = 0; status == ARCHBIRD_OK && field < field_count; field++) {
    const AbValue *value = ab_value_member(row, fields[field]);
    if (!value)
      return archbird_error_set(
          context->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "diff projection field '%s' is missing", fields[field]);
    if (field)
      status = ab_buffer_literal(out, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(out, fields[field], strlen(fields[field]));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(out, ":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(out, value);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, "}");
  return status;
}

static ArchbirdStatus artifact_index(DiffContext *context, const AbValue *map,
                                     DiffIndex *out) {
  const AbValue *rows =
      required_member(context, map, "artifacts", AB_VALUE_ARRAY);
  size_t index;
  ArchbirdStatus status = rows ? ARCHBIRD_OK : ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *name =
        required_member(context, row, "name", AB_VALUE_STRING);
    const AbValue *builds =
        required_member(context, row, "builds", AB_VALUE_ARRAY);
    const AbValue *depends =
        required_member(context, row, "depends_on", AB_VALUE_ARRAY);
    const AbValue *inputs =
        required_member(context, row, "inputs", AB_VALUE_ARRAY);
    const AbValue *loaded =
        required_member(context, row, "loaded_by", AB_VALUE_ARRAY);
    const AbValue *output =
        required_member(context, row, "output", AB_VALUE_STRING);
    AbBuffer value;
    if (!name || !builds || !depends || !inputs || !loaded || !output)
      return ARCHBIRD_INVALID_SCHEMA;
    ab_buffer_init(&value, context->engine);
    status = ab_buffer_literal(&value, "{\"builds\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&value, builds);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&value, ",\"depends_on\":");
    if (status == ARCHBIRD_OK)
      status = render_sorted_strings(context, &value, depends);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&value, ",\"inputs\":");
    if (status == ARCHBIRD_OK)
      status = render_sorted_values(context, &value, inputs);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&value, ",\"loaded_by\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&value, loaded);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&value, ",\"output\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&value, output);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&value, "}");
    if (status == ARCHBIRD_OK)
      status = index_add(context, out, name->as.text.data, name->as.text.length,
                         (const char *)value.data, value.length);
    ab_buffer_free(&value);
  }
  return status == ARCHBIRD_OK ? index_finish(context, out) : status;
}

static ArchbirdStatus build_index(DiffContext *context, const AbValue *map,
                                  DiffIndex *out) {
  static const char *const fields[] = {"conditions", "deps", "paths"};
  const AbValue *rows = required_member(context, map, "builds", AB_VALUE_ARRAY);
  size_t index;
  ArchbirdStatus status = rows ? ARCHBIRD_OK : ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *source =
        required_member(context, row, "source", AB_VALUE_STRING);
    const AbValue *name =
        required_member(context, row, "name", AB_VALUE_STRING);
    const AbString *parts[2];
    AbBuffer key;
    AbBuffer value;
    if (!source || !name)
      return ARCHBIRD_INVALID_SCHEMA;
    parts[0] = &source->as.text;
    parts[1] = &name->as.text;
    ab_buffer_init(&key, context->engine);
    ab_buffer_init(&value, context->engine);
    status = key_parts(&key, parts, 2);
    if (status == ARCHBIRD_OK)
      status = projection_value(context, row, fields,
                                sizeof(fields) / sizeof(fields[0]), &value);
    if (status == ARCHBIRD_OK)
      status = add_buffers(context, out, &key, &value);
    ab_buffer_free(&value);
    ab_buffer_free(&key);
  }
  return status == ARCHBIRD_OK ? index_finish(context, out) : status;
}

static ArchbirdStatus surface_index(DiffContext *context, const AbValue *map,
                                    DiffIndex *out) {
  static const char *const metadata_fields[] = {"kind", "provider_configured",
                                                "providers"};
  static const char *const name_fields[] = {
      "candidates",   "declaration", "declaration_signatures",
      "declarations", "ignored",     "implementation_signatures",
      "resolution",   "uses"};
  const AbValue *surfaces =
      required_member(context, map, "surfaces", AB_VALUE_ARRAY);
  size_t surface_index;
  ArchbirdStatus status = surfaces ? ARCHBIRD_OK : ARCHBIRD_INVALID_SCHEMA;
  for (surface_index = 0;
       status == ARCHBIRD_OK && surface_index < surfaces->as.array.count;
       surface_index++) {
    const AbValue *surface = &surfaces->as.array.items[surface_index];
    const AbValue *bridge =
        required_member(context, surface, "name", AB_VALUE_STRING);
    const AbValue *names =
        required_member(context, surface, "names", AB_VALUE_ARRAY);
    AbBuffer key;
    AbBuffer value;
    size_t name_index;
    if (!bridge || !names)
      return ARCHBIRD_INVALID_SCHEMA;
    ab_buffer_init(&key, context->engine);
    ab_buffer_init(&value, context->engine);
    status =
        ab_buffer_append(&key, bridge->as.text.data, bridge->as.text.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&key, "|@surface");
    if (status == ARCHBIRD_OK)
      status = projection_value(
          context, surface, metadata_fields,
          sizeof(metadata_fields) / sizeof(metadata_fields[0]), &value);
    if (status == ARCHBIRD_OK)
      status = add_buffers(context, out, &key, &value);
    ab_buffer_free(&value);
    ab_buffer_free(&key);
    for (name_index = 0;
         status == ARCHBIRD_OK && name_index < names->as.array.count;
         name_index++) {
      const AbValue *row = &names->as.array.items[name_index];
      const AbValue *name =
          required_member(context, row, "name", AB_VALUE_STRING);
      const AbString *parts[2];
      if (!name || !bridge->as.text.length || !name->as.text.length)
        return diff_error(context, "invalid bridge surface identity");
      parts[0] = &bridge->as.text;
      parts[1] = &name->as.text;
      ab_buffer_init(&key, context->engine);
      ab_buffer_init(&value, context->engine);
      status = key_parts(&key, parts, 2);
      if (status == ARCHBIRD_OK)
        status = projection_value(context, row, name_fields,
                                  sizeof(name_fields) / sizeof(name_fields[0]),
                                  &value);
      if (status == ARCHBIRD_OK)
        status = add_buffers(context, out, &key, &value);
      ab_buffer_free(&value);
      ab_buffer_free(&key);
    }
  }
  return status == ARCHBIRD_OK ? index_finish(context, out) : status;
}

static ArchbirdStatus tool_index(DiffContext *context, const AbValue *map,
                                 DiffIndex *out) {
  const AbValue *tool = required_member(context, map, "tool", AB_VALUE_OBJECT);
  AbBuffer value;
  ArchbirdStatus status;
  if (!tool)
    return ARCHBIRD_INVALID_SCHEMA;
  ab_buffer_init(&value, context->engine);
  status = ab_value_render(&value, tool);
  if (status == ARCHBIRD_OK)
    status = index_add(context, out, "tool", 4, (const char *)value.data,
                       value.length);
  ab_buffer_free(&value);
  return status == ARCHBIRD_OK ? index_finish(context, out) : status;
}

enum {
  DIFF_ARTIFACTS,
  DIFF_BRIDGE_SURFACES,
  DIFF_BRIDGES,
  DIFF_BUILD_ROUTES,
  DIFF_CALL_RESOLUTIONS,
  DIFF_COMPONENT_ROUTES,
  DIFF_EDGES,
  DIFF_ENTRYPOINTS,
  DIFF_FILES,
  DIFF_PACKAGE_DEPENDENCIES,
  DIFF_PACKAGE_ENTRYPOINT_SURFACES,
  DIFF_PACKAGE_EXPORT_ORIGINS,
  DIFF_PACKAGE_EXPORTS,
  DIFF_PARITY_GAPS,
  DIFF_PUBLIC_SYMBOLS,
  DIFF_SYMBOLS,
  DIFF_SYMBOL_CALLS,
  DIFF_SYMBOL_REFERENCES,
  DIFF_TEST_ROUTE_EVIDENCE,
  DIFF_TEST_ROUTES,
  DIFF_TOOL,
  DIFF_SECTION_COUNT
};

static void sections_init(DiffSection sections[DIFF_SECTION_COUNT]) {
  static const char *const names[] = {
      "artifacts",
      "bridge_surfaces",
      "bridges",
      "build_routes",
      "call_resolutions",
      "component_routes",
      "edges",
      "entrypoints",
      "files",
      "package_dependencies",
      "package_entrypoint_surfaces",
      "package_export_origins",
      "package_exports",
      "parity_gaps",
      "public_symbols",
      "symbols",
      "symbol_calls",
      "symbol_references",
      "test_route_evidence",
      "test_routes",
      "tool",
  };
  size_t index;
  memset(sections, 0, DIFF_SECTION_COUNT * sizeof(*sections));
  for (index = 0; index < DIFF_SECTION_COUNT; index++)
    sections[index].name = names[index];
}

static void sections_free(ArchbirdEngine *engine,
                          DiffSection sections[DIFF_SECTION_COUNT]) {
  size_t index;
  for (index = 0; index < DIFF_SECTION_COUNT; index++) {
    diff_index_free(engine, &sections[index].before);
    diff_index_free(engine, &sections[index].after);
  }
}

static ArchbirdStatus build_indexes(DiffContext *context, const AbValue *map,
                                    DiffSection sections[DIFF_SECTION_COUNT],
                                    int after) {
#define INDEX_AT(which)                                                        \
  (after ? &sections[(which)].after : &sections[(which)].before)
  ArchbirdStatus status =
      artifact_index(context, map, INDEX_AT(DIFF_ARTIFACTS));
  if (status == ARCHBIRD_OK)
    status = surface_index(context, map, INDEX_AT(DIFF_BRIDGE_SURFACES));
  if (status == ARCHBIRD_OK)
    status =
        edge_index(context, map, INDEX_AT(DIFF_EDGES), INDEX_AT(DIFF_BRIDGES));
  if (status == ARCHBIRD_OK)
    status = build_index(context, map, INDEX_AT(DIFF_BUILD_ROUTES));
  if (status == ARCHBIRD_OK)
    status = call_index(context, map, INDEX_AT(DIFF_CALL_RESOLUTIONS));
  if (status == ARCHBIRD_OK)
    status = component_index(context, map, INDEX_AT(DIFF_COMPONENT_ROUTES));
  if (status == ARCHBIRD_OK)
    status = simple_rows(context, map, "files", "path", "sha256",
                         INDEX_AT(DIFF_FILES));
  if (status == ARCHBIRD_OK)
    status = package_indexes(context, map, INDEX_AT(DIFF_PACKAGE_EXPORTS),
                             INDEX_AT(DIFF_PACKAGE_EXPORT_ORIGINS),
                             INDEX_AT(DIFF_PACKAGE_DEPENDENCIES),
                             INDEX_AT(DIFF_ENTRYPOINTS),
                             INDEX_AT(DIFF_PACKAGE_ENTRYPOINT_SURFACES));
  if (status == ARCHBIRD_OK)
    status = parity_index(context, map, INDEX_AT(DIFF_PARITY_GAPS));
  if (status == ARCHBIRD_OK)
    status = symbol_index(context, map, 1, INDEX_AT(DIFF_PUBLIC_SYMBOLS));
  if (status == ARCHBIRD_OK)
    status = symbol_index(context, map, 0, INDEX_AT(DIFF_SYMBOLS));
  if (status == ARCHBIRD_OK)
    status = relation_index(context, map, "symbol_calls",
                            INDEX_AT(DIFF_SYMBOL_CALLS));
  if (status == ARCHBIRD_OK)
    status = relation_index(context, map, "symbol_references",
                            INDEX_AT(DIFF_SYMBOL_REFERENCES));
  if (status == ARCHBIRD_OK)
    status = test_route_evidence_index(context, map,
                                       INDEX_AT(DIFF_TEST_ROUTE_EVIDENCE));
  if (status == ARCHBIRD_OK)
    status = test_route_index(context, map, INDEX_AT(DIFF_TEST_ROUTES));
  if (status == ARCHBIRD_OK)
    status = tool_index(context, map, INDEX_AT(DIFF_TOOL));
#undef INDEX_AT
  return status;
}

static int pair_key_order(const DiffPair *left, const DiffPair *right) {
  return ab_string_compare(&left->key, &right->key);
}

static ArchbirdStatus render_added_removed(AbBuffer *buffer,
                                           const DiffIndex *primary,
                                           const DiffIndex *other) {
  size_t primary_index = 0;
  size_t other_index = 0;
  int first = 1;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  while (status == ARCHBIRD_OK && primary_index < primary->count) {
    int compared;
    while (other_index < other->count &&
           pair_key_order(&other->items[other_index],
                          &primary->items[primary_index]) < 0)
      other_index++;
    compared = other_index < other->count
                   ? pair_key_order(&primary->items[primary_index],
                                    &other->items[other_index])
                   : -1;
    if (compared != 0) {
      if (!first)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_json_string(
            buffer, primary->items[primary_index].key.data,
            primary->items[primary_index].key.length);
      first = 0;
    }
    primary_index++;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_changed(AbBuffer *buffer, const DiffIndex *before,
                                     const DiffIndex *after) {
  size_t before_index = 0;
  size_t after_index = 0;
  int first = 1;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  while (status == ARCHBIRD_OK && before_index < before->count &&
         after_index < after->count) {
    int compared = pair_key_order(&before->items[before_index],
                                  &after->items[after_index]);
    if (compared < 0) {
      before_index++;
      continue;
    }
    if (compared > 0) {
      after_index++;
      continue;
    }
    if (!ab_string_equal(&before->items[before_index].value,
                         &after->items[after_index].value)) {
      AbBuffer message;
      ab_buffer_init(&message, buffer->engine);
      status = ab_buffer_append(&message, before->items[before_index].key.data,
                                before->items[before_index].key.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&message, ": ");
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_append(&message, before->items[before_index].value.data,
                             before->items[before_index].value.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&message, " -> ");
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_append(&message, after->items[after_index].value.data,
                             after->items[after_index].value.length);
      if (status == ARCHBIRD_OK && !first)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_json_string(buffer, (const char *)message.data,
                                       message.length);
      ab_buffer_free(&message);
      first = 0;
    }
    before_index++;
    after_index++;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus
render_sections(AbBuffer *buffer,
                const DiffSection sections[DIFF_SECTION_COUNT]) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "{");
  for (index = 0; status == ARCHBIRD_OK && index < DIFF_SECTION_COUNT;
       index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, sections[index].name,
                                     strlen(sections[index].name));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ":{\"added\":");
    if (status == ARCHBIRD_OK)
      status = render_added_removed(buffer, &sections[index].after,
                                    &sections[index].before);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"changed\":");
    if (status == ARCHBIRD_OK)
      status = render_changed(buffer, &sections[index].before,
                              &sections[index].after);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"removed\":");
    if (status == ARCHBIRD_OK)
      status = render_added_removed(buffer, &sections[index].before,
                                    &sections[index].after);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

static ArchbirdStatus validate_map(DiffContext *context, const AbValue *map) {
  const AbValue *artifact;
  const AbValue *schema;
  uint64_t version;
  if (!map || map->kind != AB_VALUE_OBJECT)
    return diff_error(context, "diff input must be a map object");
  artifact = ab_value_member(map, "artifact");
  schema = ab_value_member(map, "schema_version");
  if (!ab_value_string_is(artifact, "map") || !ab_value_u64(schema, &version) ||
      version < ARCHBIRD_MAP_SCHEMA_MIN ||
      version > ARCHBIRD_MAP_SCHEMA_CURRENT)
    return diff_error(context, "diff input must be an Archbird map "
                               "schema " ARCHBIRD_MAP_SCHEMA_SUPPORTED_TEXT);
  if (!required_member(context, map, "project", AB_VALUE_STRING) ||
      !required_member(context, map, "evidence", AB_VALUE_OBJECT))
    return ARCHBIRD_INVALID_SCHEMA;
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_identity(DiffContext *context, AbBuffer *buffer,
                                      const AbValue *map) {
  const AbValue *project = ab_value_member(map, "project");
  const AbValue *evidence = ab_value_member(map, "evidence");
  const AbValue *input = ab_value_member(evidence, "input_sha256");
  const AbValue *discovery = ab_value_member(map, "discovery");
  ArchbirdStatus status;
  if (!input || input->kind != AB_VALUE_STRING)
    return diff_error(context, "map evidence input_sha256 is missing");
  status = ab_buffer_literal(buffer, "{");
  if (status == ARCHBIRD_OK && discovery) {
    if (discovery->kind != AB_VALUE_OBJECT)
      return diff_error(context, "map discovery evidence must be an object");
    status = ab_buffer_literal(buffer, "\"discovery\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, discovery);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "\"input_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, input);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"project\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, project);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

ArchbirdStatus archbird_map_diff(ArchbirdEngine *engine,
                                 const uint8_t *before_json,
                                 size_t before_length,
                                 const uint8_t *after_json, size_t after_length,
                                 uint32_t json_flags, ArchbirdWriteFn write_fn,
                                 void *user_data) {
  AbValue before = {0};
  AbValue after = {0};
  DiffContext context = {engine};
  DiffSection sections[DIFF_SECTION_COUNT];
  AbBuffer buffer;
  ArchbirdStatus status;
  if (!engine || (!before_json && before_length) ||
      (!after_json && after_length) || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_build_identity_validate(engine);
  if (status != ARCHBIRD_OK)
    return status;
  sections_init(sections);
  ab_buffer_init(&buffer, engine);
  status = ab_json_value_decode(engine, before_json, before_length, &before);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, after_json, after_length, &after);
  if (status == ARCHBIRD_OK)
    status = validate_map(&context, &before);
  if (status == ARCHBIRD_OK)
    status = validate_map(&context, &after);
  if (status == ARCHBIRD_OK)
    status = build_indexes(&context, &before, sections, 0);
  if (status == ARCHBIRD_OK)
    status = build_indexes(&context, &after, sections, 1);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "{\"after\":");
  if (status == ARCHBIRD_OK)
    status = render_identity(&context, &buffer, &after);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"artifact\":\"diff\",\"before\":");
  if (status == ARCHBIRD_OK)
    status = render_identity(&context, &buffer, &before);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"schema_version\":7,\"sections\":");
  if (status == ARCHBIRD_OK)
    status = render_sections(&buffer, sections);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        &buffer,
        ",\"tool\":{\"implementation_sha256\":\"" ARCHBIRD_IMPLEMENTATION_SHA256
        "\",\"name\":\"archbird\",\"version\":\"" ARCHBIRD_VERSION "\"}}");
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                        json_flags, write_fn, user_data);
  sections_free(engine, sections);
  ab_value_free(engine, &after);
  ab_value_free(engine, &before);
  ab_buffer_free(&buffer);
  return status;
}
