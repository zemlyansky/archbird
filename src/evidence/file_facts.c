#include "archbird_internal.h"

#include "file_facts.h"
#include "model.h"
#include "project_internal.h"
#include "render_internal.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

typedef struct FactRefs {
  ArchbirdEngine *engine;
  const AbFact **items;
  size_t count;
} FactRefs;

#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      goto done;                                                               \
  } while (0)

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value->length == length &&
         (length == 0 || memcmp(value->data, literal, length) == 0);
}

static ArchbirdStatus json_string(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value->data, value->length);
}

static ArchbirdStatus json_sha(AbBuffer *buffer, const uint8_t digest[32]) {
  char hex[65];
  archbird_sha256_hex(digest, hex);
  return ab_buffer_json_string(buffer, hex, 64);
}

static const AbObjectField *attribute(const AbFact *fact, const char *name) {
  size_t index;
  for (index = 0; index < fact->attribute_count; index++) {
    int compared;
    AbString wanted = {(char *)name, strlen(name)};
    compared = ab_string_compare(&fact->attributes[index].name, &wanted);
    if (compared == 0)
      return &fact->attributes[index];
    if (compared > 0)
      return NULL;
  }
  return NULL;
}

static uint64_t integer_attribute(const AbFact *fact, const char *name) {
  const AbObjectField *field = attribute(fact, name);
  uint64_t value = 0;
  size_t index;
  if (!field || field->value.kind != AB_VALUE_INTEGER)
    return 0;
  for (index = 0; index < field->value.as.text.length; index++) {
    uint8_t digit = (uint8_t)field->value.as.text.data[index];
    if (digit < '0' || digit > '9' || value > (UINT64_MAX - (digit - '0')) / 10)
      return 0;
    value = value * 10 + (digit - '0');
  }
  return value;
}

static const AbString *string_attribute(const AbFact *fact, const char *name) {
  const AbObjectField *field = attribute(fact, name);
  if (!field || field->value.kind != AB_VALUE_STRING)
    return NULL;
  return &field->value.as.text;
}

static ArchbirdStatus require_named_fact(ArchbirdEngine *engine,
                                         const AbFact *fact,
                                         const char *domain) {
  if (fact->has_name && fact->name.length)
    return ARCHBIRD_OK;
  return archbird_error_set(
      engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
      domain && domain[0] ? "file-fact domain requires a nonempty name"
                          : "file-fact reduction requires a nonempty name");
}

static ArchbirdStatus require_string_attribute(ArchbirdEngine *engine,
                                               const AbFact *fact,
                                               const char *name,
                                               const AbString **out_value) {
  const AbString *value = string_attribute(fact, name);
  if (!value)
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "file-fact reduction requires a string attribute");
  *out_value = value;
  return ARCHBIRD_OK;
}

static ArchbirdStatus collect_facts(ArchbirdEngine *engine,
                                    const ArchbirdProject *project,
                                    const AbManifestFile *file,
                                    const char *domain, const char *kind,
                                    FactRefs *out) {
  size_t start;
  size_t end;
  size_t index;
  memset(out, 0, sizeof(*out));
  out->engine = engine;
  ab_project_merged_fact_range(project, &file->path, domain, &start, &end);
  if (end > start) {
    out->items =
        (const AbFact **)ab_malloc(engine, (end - start) * sizeof(*out->items));
    if (!out->items)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory reducing file facts");
  }
  for (index = start; index < end; index++) {
    const AbFact *fact = ab_project_merged_fact_by_path(project, index);
    if (kind && !string_literal(&fact->kind, kind))
      continue;
    out->items[out->count++] = fact;
  }
  return ARCHBIRD_OK;
}

static void fact_refs_free(FactRefs *refs) {
  ab_free(refs->engine, refs->items);
  memset(refs, 0, sizeof(*refs));
}

static int named_fact_compare(const void *left_raw, const void *right_raw) {
  const AbFact *left = *(const AbFact *const *)left_raw;
  const AbFact *right = *(const AbFact *const *)right_raw;
  int compared = ab_string_compare(&left->name, &right->name);
  if (compared != 0)
    return compared;
  if (left->span_start != right->span_start)
    return left->span_start < right->span_start ? -1 : 1;
  if (left->span_end != right->span_end)
    return left->span_end < right->span_end ? -1 : 1;
  return ab_string_compare(&left->id, &right->id);
}

static int imported_name_fact_compare(const void *left_raw,
                                      const void *right_raw) {
  const AbFact *left = *(const AbFact *const *)left_raw;
  const AbFact *right = *(const AbFact *const *)right_raw;
  const AbString *left_module = string_attribute(left, "module");
  const AbString *right_module = string_attribute(right, "module");
  if (left_module && right_module) {
    int compared = ab_string_compare(left_module, right_module);
    if (compared != 0)
      return compared;
  } else if (left_module || right_module) {
    return left_module ? 1 : -1;
  }
  return named_fact_compare(left_raw, right_raw);
}

static int symbol_fact_compare(const void *left_raw, const void *right_raw) {
  const AbFact *left = *(const AbFact *const *)left_raw;
  const AbFact *right = *(const AbFact *const *)right_raw;
  const AbString *left_scope;
  const AbString *right_scope;
  const AbString *left_signature;
  const AbString *right_signature;
  uint64_t left_line = integer_attribute(left, "line");
  uint64_t right_line = integer_attribute(right, "line");
  int compared;
  if (left_line != right_line)
    return left_line < right_line ? -1 : 1;
  compared = ab_string_compare(&left->name, &right->name);
  if (compared != 0)
    return compared;
  compared = ab_string_compare(&left->kind, &right->kind);
  if (compared != 0)
    return compared;
  left_scope = string_attribute(left, "scope");
  right_scope = string_attribute(right, "scope");
  if (left_scope && right_scope) {
    compared = ab_string_compare(left_scope, right_scope);
    if (compared != 0)
      return compared;
  } else if (left_scope || right_scope) {
    return left_scope ? 1 : -1;
  }
  left_signature = string_attribute(left, "signature");
  right_signature = string_attribute(right, "signature");
  if (left_signature && right_signature)
    return ab_string_compare(left_signature, right_signature);
  return (left_signature != NULL) - (right_signature != NULL);
}

static int same_symbol(const AbFact *left, const AbFact *right) {
  const AbFact *values[] = {left, right};
  return symbol_fact_compare(&values[0], &values[1]) == 0;
}

static ArchbirdStatus render_named_array(AbBuffer *buffer,
                                         ArchbirdEngine *engine,
                                         const ArchbirdProject *project,
                                         const AbManifestFile *file,
                                         const char *domain, const char *kind) {
  FactRefs refs;
  ArchbirdStatus status =
      collect_facts(engine, project, file, domain, kind, &refs);
  size_t index;
  int first = 1;
  if (status != ARCHBIRD_OK)
    return status;
  if (refs.count > 1)
    qsort(refs.items, refs.count, sizeof(*refs.items), named_fact_compare);
  status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < refs.count; index++) {
    status = require_named_fact(engine, refs.items[index], domain);
    if (status != ARCHBIRD_OK)
      break;
    if (index > 0 &&
        ab_string_equal(&refs.items[index - 1]->name, &refs.items[index]->name))
      continue;
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &refs.items[index]->name);
    first = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  fact_refs_free(&refs);
  return status;
}

static ArchbirdStatus
render_string_map(AbBuffer *buffer, ArchbirdEngine *engine,
                  const ArchbirdProject *project, const AbManifestFile *file,
                  const char *domain, const char *attribute_name) {
  FactRefs refs;
  ArchbirdStatus status =
      collect_facts(engine, project, file, domain, NULL, &refs);
  size_t index;
  int first = 1;
  if (status != ARCHBIRD_OK)
    return status;
  if (refs.count > 1)
    qsort(refs.items, refs.count, sizeof(*refs.items), named_fact_compare);
  status = ab_buffer_literal(buffer, "{");
  for (index = 0; status == ARCHBIRD_OK && index < refs.count; index++) {
    const AbString *value = NULL;
    status = require_named_fact(engine, refs.items[index], domain);
    if (status == ARCHBIRD_OK)
      status = require_string_attribute(engine, refs.items[index],
                                        attribute_name, &value);
    if (status != ARCHBIRD_OK)
      break;
    if (index > 0 && ab_string_equal(&refs.items[index - 1]->name,
                                     &refs.items[index]->name)) {
      const AbString *previous =
          string_attribute(refs.items[index - 1], attribute_name);
      if (!previous || !ab_string_equal(previous, value))
        status = archbird_error_set(
            engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
            "file-fact string map has conflicting values for one key");
      continue;
    }
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &refs.items[index]->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, value);
    first = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  fact_refs_free(&refs);
  return status;
}

static int string_pointer_compare(const void *left_raw, const void *right_raw) {
  const AbString *left = *(const AbString *const *)left_raw;
  const AbString *right = *(const AbString *const *)right_raw;
  return ab_string_compare(left, right);
}

static ArchbirdStatus
render_string_array_map(AbBuffer *buffer, ArchbirdEngine *engine,
                        const ArchbirdProject *project,
                        const AbManifestFile *file, const char *domain,
                        const char *group_domain, const char *group_attribute) {
  FactRefs refs;
  FactRefs groups;
  const AbString **keys = NULL;
  size_t key_count = 0;
  size_t index;
  int first_group = 1;
  ArchbirdStatus status =
      collect_facts(engine, project, file, domain, NULL, &refs);
  if (status != ARCHBIRD_OK)
    return status;
  status = collect_facts(engine, project, file, group_domain, NULL, &groups);
  if (status != ARCHBIRD_OK) {
    fact_refs_free(&refs);
    return status;
  }
  if (refs.count + groups.count) {
    keys = (const AbString **)ab_malloc(engine, (refs.count + groups.count) *
                                                    sizeof(*keys));
    if (!keys) {
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory reducing string-array map");
      goto done;
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < groups.count; index++) {
    status = require_named_fact(engine, groups.items[index], group_domain);
    if (status == ARCHBIRD_OK)
      keys[key_count++] = &groups.items[index]->name;
  }
  for (index = 0; status == ARCHBIRD_OK && index < refs.count; index++) {
    const AbString *group = NULL;
    status = require_named_fact(engine, refs.items[index], domain);
    if (status == ARCHBIRD_OK)
      status = require_string_attribute(engine, refs.items[index],
                                        group_attribute, &group);
    if (status == ARCHBIRD_OK)
      keys[key_count++] = group;
  }
  if (status != ARCHBIRD_OK)
    goto done;
  if (refs.count > 1)
    qsort(refs.items, refs.count, sizeof(*refs.items),
          imported_name_fact_compare);
  if (key_count > 1)
    qsort(keys, key_count, sizeof(*keys), string_pointer_compare);
  status = ab_buffer_literal(buffer, "{");
  for (index = 0; status == ARCHBIRD_OK && index < key_count; index++) {
    size_t item;
    const AbString *previous_name = NULL;
    if (index > 0 && ab_string_equal(keys[index - 1], keys[index]))
      continue;
    if (!first_group)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, keys[index]);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ":[");
    for (item = 0; status == ARCHBIRD_OK && item < refs.count; item++) {
      const AbString *candidate =
          string_attribute(refs.items[item], group_attribute);
      if (!candidate || !ab_string_equal(candidate, keys[index]))
        continue;
      if (previous_name &&
          ab_string_equal(previous_name, &refs.items[item]->name))
        continue;
      if (previous_name)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &refs.items[item]->name);
      previous_name = &refs.items[item]->name;
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "]");
    first_group = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
done:
  ab_free(engine, keys);
  fact_refs_free(&groups);
  fact_refs_free(&refs);
  return status;
}

static ArchbirdStatus render_count_map(AbBuffer *buffer, ArchbirdEngine *engine,
                                       const ArchbirdProject *project,
                                       const AbManifestFile *file,
                                       const char *domain) {
  FactRefs refs;
  ArchbirdStatus status =
      collect_facts(engine, project, file, domain, NULL, &refs);
  size_t index = 0;
  int first = 1;
  if (status != ARCHBIRD_OK)
    return status;
  if (refs.count > 1)
    qsort(refs.items, refs.count, sizeof(*refs.items), named_fact_compare);
  status = ab_buffer_literal(buffer, "{");
  while (status == ARCHBIRD_OK && index < refs.count) {
    size_t end = index + 1;
    while (end < refs.count &&
           ab_string_equal(&refs.items[index]->name, &refs.items[end]->name))
      end++;
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &refs.items[index]->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, end - index);
    first = 0;
    index = end;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  fact_refs_free(&refs);
  return status;
}

static ArchbirdStatus render_symbols(AbBuffer *buffer, ArchbirdEngine *engine,
                                     const ArchbirdProject *project,
                                     const AbManifestFile *file) {
  static const AbString empty = {(char *)"", 0};
  FactRefs refs;
  ArchbirdStatus status =
      collect_facts(engine, project, file, "symbols", NULL, &refs);
  size_t index;
  int first = 1;
  if (status != ARCHBIRD_OK)
    return status;
  if (refs.count > 1)
    qsort(refs.items, refs.count, sizeof(*refs.items), symbol_fact_compare);
  status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < refs.count; index++) {
    const AbFact *fact = refs.items[index];
    const AbString *scope = string_attribute(fact, "scope");
    const AbString *signature = string_attribute(fact, "signature");
    const AbString *syntax_recovery = string_attribute(fact, "syntax_recovery");
    if (index > 0 && same_symbol(refs.items[index - 1], fact))
      continue;
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"kind\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &fact->kind);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"line\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, integer_attribute(fact, "line"));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"name\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &fact->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"scope\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, scope ? scope : &empty);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"signature\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, signature ? signature : &empty);
    if (status == ARCHBIRD_OK && syntax_recovery) {
      status = ab_buffer_literal(buffer, ",\"syntax_recovery\":");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, syntax_recovery);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
    first = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  fact_refs_free(&refs);
  return status;
}

static int candidate_role(const AbString *role) {
  static const char suffix[] = "-candidate";
  return role->length >= sizeof(suffix) - 1 &&
         !memcmp(role->data + role->length - (sizeof(suffix) - 1), suffix,
                 sizeof(suffix) - 1);
}

static ArchbirdStatus render_candidate_roles(AbBuffer *buffer,
                                             const AbManifestFile *file) {
  size_t index;
  int first = 1;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < file->roles.count; index++) {
    if (!candidate_role(&file->roles.items[index]))
      continue;
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &file->roles.items[index]);
    first = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_file_facts_row(AbBuffer *buffer,
                                            ArchbirdEngine *engine,
                                            const ArchbirdProject *project,
                                            const AbManifestFile *file,
                                            int include_candidate_roles) {
  static const AbString empty = {(char *)"", 0};
  ArchbirdStatus status;
#define FILE_TRY(expression)                                                   \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      return status;                                                           \
  } while (0)
  FILE_TRY(ab_buffer_literal(buffer, "{\"bytes\":"));
  FILE_TRY(ab_buffer_u64(buffer, file->byte_length));
  FILE_TRY(ab_buffer_literal(buffer, ",\"call_counts\":"));
  FILE_TRY(render_count_map(buffer, engine, project, file, "calls"));
  FILE_TRY(ab_buffer_literal(buffer, ",\"calls\":"));
  FILE_TRY(render_named_array(buffer, engine, project, file, "calls", NULL));
  FILE_TRY(ab_buffer_literal(buffer, ",\"export_origins\":"));
  FILE_TRY(render_string_map(buffer, engine, project, file, "export-origins",
                             "origin"));
  FILE_TRY(ab_buffer_literal(buffer, ",\"exports\":"));
  FILE_TRY(render_named_array(buffer, engine, project, file, "exports", NULL));
  FILE_TRY(ab_buffer_literal(buffer, ",\"imported_names\":"));
  FILE_TRY(render_string_array_map(buffer, engine, project, file,
                                   "imported-names", "imported-name-groups",
                                   "module"));
  FILE_TRY(ab_buffer_literal(buffer, ",\"imports\":"));
  FILE_TRY(render_named_array(buffer, engine, project, file, "imports", NULL));
  FILE_TRY(ab_buffer_literal(buffer, ",\"language\":"));
  FILE_TRY(json_string(buffer, file->has_language ? &file->language : &empty));
  FILE_TRY(ab_buffer_literal(buffer, ",\"layer\":"));
  FILE_TRY(json_string(buffer, file->has_layer ? &file->layer : &empty));
  FILE_TRY(ab_buffer_literal(buffer, ",\"messages\":{\"receives\":"));
  FILE_TRY(
      render_named_array(buffer, engine, project, file, "messages", "receive"));
  FILE_TRY(ab_buffer_literal(buffer, ",\"sends\":"));
  FILE_TRY(
      render_named_array(buffer, engine, project, file, "messages", "send"));
  FILE_TRY(ab_buffer_literal(buffer, "},\"method_call_counts\":"));
  FILE_TRY(render_count_map(buffer, engine, project, file, "method-calls"));
  FILE_TRY(ab_buffer_literal(buffer, ",\"method_calls\":"));
  FILE_TRY(
      render_named_array(buffer, engine, project, file, "method-calls", NULL));
  FILE_TRY(ab_buffer_literal(buffer, ",\"path\":"));
  FILE_TRY(json_string(buffer, &file->path));
  FILE_TRY(ab_buffer_literal(buffer, ",\"reexport_candidates\":"));
  FILE_TRY(render_named_array(buffer, engine, project, file,
                              "reexport-candidates", NULL));
  if (include_candidate_roles) {
    size_t index;
    int has_candidate = 0;
    for (index = 0; index < file->roles.count; index++)
      if (candidate_role(&file->roles.items[index])) {
        has_candidate = 1;
        break;
      }
    if (has_candidate) {
      FILE_TRY(ab_buffer_literal(buffer, ",\"roles\":"));
      FILE_TRY(render_candidate_roles(buffer, file));
    }
  }
  FILE_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
  FILE_TRY(json_sha(buffer, file->sha256));
  FILE_TRY(ab_buffer_literal(buffer, ",\"symbols\":"));
  FILE_TRY(render_symbols(buffer, engine, project, file));
  FILE_TRY(ab_buffer_literal(buffer, "}"));
#undef FILE_TRY
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_render_file_facts_row(AbBuffer *buffer,
                                        ArchbirdEngine *engine,
                                        const ArchbirdProject *project,
                                        const AbManifestFile *file) {
  return render_file_facts_row(buffer, engine, project, file, 0);
}

ArchbirdStatus ab_render_map_file_facts_row(AbBuffer *buffer,
                                            ArchbirdEngine *engine,
                                            const ArchbirdProject *project,
                                            const AbManifestFile *file) {
  return render_file_facts_row(buffer, engine, project, file, 1);
}

ArchbirdStatus ab_file_symbol_count(ArchbirdEngine *engine,
                                    const ArchbirdProject *project,
                                    const AbManifestFile *file,
                                    size_t *out_count) {
  FactRefs refs;
  ArchbirdStatus status;
  size_t index;
  size_t count = 0;
  if (!engine || !project || !file || !out_count)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = collect_facts(engine, project, file, "symbols", NULL, &refs);
  if (status != ARCHBIRD_OK)
    return status;
  if (refs.count > 1)
    qsort(refs.items, refs.count, sizeof(*refs.items), symbol_fact_compare);
  for (index = 0; index < refs.count; index++) {
    if (index == 0 || !same_symbol(refs.items[index - 1], refs.items[index]))
      count++;
  }
  fact_refs_free(&refs);
  *out_count = count;
  return ARCHBIRD_OK;
}

ArchbirdStatus archbird_project_render_file_facts(
    ArchbirdEngine *engine, const ArchbirdProject *project, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data) {
  const AbSourceManifest *manifest;
  AbBuffer buffer;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!engine || !project || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!ab_project_providers_finalized(project))
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "provider merge must be finalized first");
  manifest = ab_project_manifest(project);
  ab_buffer_init(&buffer, engine);
  RENDER_TRY(ab_buffer_literal(
      &buffer, "{\"artifact\":\"archbird-file-facts\",\"files\":["));
  for (index = 0; index < manifest->file_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(&buffer, ","));
    RENDER_TRY(ab_render_file_facts_row(&buffer, engine, project,
                                        &manifest->files[index]));
  }
  RENDER_TRY(ab_buffer_literal(&buffer, "],\"project\":"));
  RENDER_TRY(json_string(&buffer, &manifest->project));
  RENDER_TRY(ab_buffer_literal(
      &buffer, ",\"schema_version\":1,\"source_manifest_sha256\":"));
  RENDER_TRY(json_sha(&buffer, ab_project_manifest_sha256_bytes(project)));
  RENDER_TRY(ab_buffer_literal(&buffer, "}"));
  status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                      json_flags, write_fn, user_data);
done:
  ab_buffer_free(&buffer);
  return status;
}

#undef RENDER_TRY
