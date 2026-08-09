#include "interchange/reports/source_report.h"

#include "base/archbird_internal.h"
#include "base/path_match.h"
#include "base/sha256.h"
#include "base/utf8.h"
#include "interchange/reports/report_utils.h"
#include "projection/projection_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct AbSourceRange {
  size_t start;
  size_t end;
  const AbValue *symbol;
} AbSourceRange;

typedef struct AbSourceFileRef {
  const AbString *path;
} AbSourceFileRef;

typedef struct AbMatchedSymbolRef {
  const AbString *path;
  const AbString *name;
  const AbString *kind;
  const AbString *scope;
  uint64_t line;
} AbMatchedSymbolRef;

typedef struct AbSourceSymbolRef {
  const AbValue *row;
  const AbString *name;
  const AbString *kind;
  const AbString *scope;
  uint64_t line;
} AbSourceSymbolRef;

typedef struct AbSourceArtifactIndex {
  AbSourceFileRef *files;
  size_t file_count;
  AbSourceFileRef *direct_files;
  size_t direct_file_count;
  AbMatchedSymbolRef *matched;
  size_t matched_count;
} AbSourceArtifactIndex;

static ArchbirdStatus source_error(ArchbirdEngine *engine,
                                   ArchbirdStatus status, const char *message) {
  return archbird_error_set(engine, status, ARCHBIRD_NO_OFFSET,
                            "source view: %s", message);
}

static int string_is(const AbValue *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->kind == AB_VALUE_STRING &&
         value->as.text.length == length &&
         (!length || memcmp(value->as.text.data, literal, length) == 0);
}

static int lowercase_sha256(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || value->as.text.length != 64)
    return 0;
  for (index = 0; index < 64; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return 0;
  }
  return 1;
}

static int stable_token(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || !value->as.text.length)
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
          (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
          byte == '.' || byte == ':'))
      return 0;
  }
  return 1;
}

static int repository_path(const AbValue *value) {
  size_t index;
  size_t part_start = 0;
  if (!value || value->kind != AB_VALUE_STRING || !value->as.text.length ||
      value->as.text.data[0] == '/' ||
      (value->as.text.length >= 2 &&
       ((value->as.text.data[0] >= 'A' && value->as.text.data[0] <= 'Z') ||
        (value->as.text.data[0] >= 'a' && value->as.text.data[0] <= 'z')) &&
       value->as.text.data[1] == ':'))
    return 0;
  for (index = 0; index <= value->as.text.length; index++) {
    unsigned char byte = index < value->as.text.length
                             ? (unsigned char)value->as.text.data[index]
                             : (unsigned char)'/';
    if (byte == '\\' || byte == '\0')
      return 0;
    if (byte != '/')
      continue;
    if (index == part_start ||
        (index - part_start == 1 && value->as.text.data[part_start] == '.') ||
        (index - part_start == 2 && value->as.text.data[part_start] == '.' &&
         value->as.text.data[part_start + 1] == '.'))
      return 0;
    part_start = index + 1;
  }
  return 1;
}

static const AbValue *required_string(const AbValue *object, const char *name) {
  const AbValue *value = ab_value_member(object, name);
  return value && value->kind == AB_VALUE_STRING ? value : NULL;
}

static const AbValue *required_array(const AbValue *object, const char *name) {
  const AbValue *value = ab_value_member(object, name);
  return value && value->kind == AB_VALUE_ARRAY ? value : NULL;
}

static int source_file_ref_compare(const void *left_raw,
                                   const void *right_raw) {
  const AbSourceFileRef *left = (const AbSourceFileRef *)left_raw;
  const AbSourceFileRef *right = (const AbSourceFileRef *)right_raw;
  return ab_string_compare(left->path, right->path);
}

static const AbSourceFileRef *find_source_file(const AbSourceFileRef *files,
                                               size_t file_count,
                                               const AbString *path) {
  size_t low = 0;
  size_t high = file_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(files[middle].path, path);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &files[middle];
  }
  return NULL;
}

static int source_text_safe(const uint8_t *source, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    size_t scalar = ab_utf8_scalar_length(source, length, offset);
    if (!scalar)
      return 0;
    if ((source[offset] < 0x20 && source[offset] != '\t' &&
         source[offset] != '\n' && source[offset] != '\r') ||
        source[offset] == 0x7f ||
        (scalar == 2 && source[offset] == 0xc2 && source[offset + 1] >= 0x80 &&
         source[offset + 1] <= 0x9f))
      return 0;
    offset += scalar;
  }
  return 1;
}

static ArchbirdStatus markdown_text(AbBuffer *out, const AbString *value) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < value->length; index++) {
    unsigned char byte = (unsigned char)value->data[index];
    if (byte == 0xc2 && index + 1 < value->length &&
        (unsigned char)value->data[index + 1] >= 0x80 &&
        (unsigned char)value->data[index + 1] <= 0x9f) {
      status = ab_report_appendf(out, "\\u00%02x",
                                 (unsigned char)value->data[index + 1]);
      index++;
      continue;
    }
    if (byte < 0x20 || byte == 0x7f) {
      status = ab_report_appendf(out, "\\x%02x", (unsigned)byte);
      continue;
    }
    if (byte == '\\' || byte == '`' || byte == '*' || byte == '_' ||
        byte == '[' || byte == ']' || byte == '<' || byte == '>')
      status = ab_buffer_literal(out, "\\");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(out, &byte, 1);
  }
  return status;
}

static ArchbirdStatus render_file_header(AbBuffer *out, const AbValue *file) {
  const AbValue *path = required_string(file, "path");
  const AbValue *language = required_string(file, "language");
  const AbValue *sha = required_string(file, "sha256");
  ArchbirdStatus status = ab_buffer_literal(out, "## ");
  if (status == ARCHBIRD_OK)
    status = markdown_text(out, &path->as.text);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, "\n\n");
  if (status == ARCHBIRD_OK)
    status = ab_report_appendf(out, "Language: `%.*s`  \n",
                               (int)language->as.text.length,
                               language->as.text.data);
  if (status == ARCHBIRD_OK)
    status = ab_report_appendf(out, "Source SHA-256: `%.*s`\n\n",
                               (int)sha->as.text.length, sha->as.text.data);
  return status;
}

static ArchbirdStatus
append_bounded_record(AbBuffer *out, const AbBuffer *record, size_t max_chars,
                      size_t *out_omitted_records, size_t *out_omitted_bytes) {
  if (max_chars == SIZE_MAX ||
      (out->length <= max_chars && record->length <= max_chars - out->length))
    return ab_buffer_append(out, record->data, record->length);
  (*out_omitted_records)++;
  *out_omitted_bytes += record->length;
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_outline(AbBuffer *out, const AbValue *file,
                                     size_t max_chars,
                                     size_t *out_omitted_records,
                                     size_t *out_omitted_bytes) {
  const AbValue *symbols = required_array(file, "symbols");
  size_t index;
  ArchbirdStatus status;
  if (!symbols || !symbols->as.array.count)
    return ab_buffer_literal(out, "_No indexed declarations._\n\n");
  for (index = 0; index < symbols->as.array.count; index++) {
    const AbValue *symbol = &symbols->as.array.items[index];
    const AbValue *name = required_string(symbol, "name");
    const AbValue *kind = required_string(symbol, "kind");
    const AbValue *signature = required_string(symbol, "signature");
    AbBuffer record;
    uint64_t line = 0;
    if (!name || !kind || !signature ||
        !ab_value_u64(ab_value_member(symbol, "line"), &line))
      return source_error(out->engine, ARCHBIRD_INVALID_SCHEMA,
                          "artifact contains an invalid symbol row");
    ab_buffer_init(&record, out->engine);
    status = ab_report_appendf(&record, "- line %llu · `%.*s` · ",
                               (unsigned long long)line,
                               (int)kind->as.text.length, kind->as.text.data);
    if (status == ARCHBIRD_OK)
      status = markdown_text(&record, &name->as.text);
    if (status == ARCHBIRD_OK && signature->as.text.length) {
      status = ab_buffer_literal(&record, " · ");
      if (status == ARCHBIRD_OK)
        status = markdown_text(&record, &signature->as.text);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&record, "\n");
    if (status == ARCHBIRD_OK)
      status = append_bounded_record(out, &record, max_chars,
                                     out_omitted_records, out_omitted_bytes);
    ab_buffer_free(&record);
    if (status != ARCHBIRD_OK)
      return status;
  }
  if (max_chars == SIZE_MAX || out->length < max_chars)
    return ab_buffer_literal(out, "\n");
  return ARCHBIRD_OK;
}

static size_t fence_length(const uint8_t *source, size_t length) {
  size_t longest = 0;
  size_t current = 0;
  size_t index;
  for (index = 0; index < length; index++) {
    if (source[index] == '`') {
      current++;
      if (current > longest)
        longest = current;
    } else {
      current = 0;
    }
  }
  return longest >= 3 ? longest + 1 : 3;
}

static ArchbirdStatus render_source_fence(AbBuffer *out,
                                          const AbString *language,
                                          const uint8_t *source,
                                          size_t length) {
  size_t fence = fence_length(source, length);
  size_t index;
  int safe_language = language && language->length;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; safe_language && index < language->length; index++) {
    unsigned char byte = (unsigned char)language->data[index];
    if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
          (byte >= '0' && byte <= '9') || byte == '-' || byte == '_'))
      safe_language = 0;
  }
  for (index = 0; status == ARCHBIRD_OK && index < fence; index++)
    status = ab_buffer_literal(out, "`");
  if (status == ARCHBIRD_OK && safe_language)
    status = ab_buffer_append(out, language->data, language->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, "\n");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(out, source, length);
  if (status == ARCHBIRD_OK && (!length || source[length - 1] != '\n'))
    status = ab_buffer_literal(out, "\n");
  for (index = 0; status == ARCHBIRD_OK && index < fence; index++)
    status = ab_buffer_literal(out, "`");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, "\n\n");
  return status;
}

static int range_compare(const void *left_raw, const void *right_raw) {
  const AbSourceRange *left = (const AbSourceRange *)left_raw;
  const AbSourceRange *right = (const AbSourceRange *)right_raw;
  if (left->start != right->start)
    return left->start < right->start ? -1 : 1;
  if (left->end != right->end)
    return left->end > right->end ? -1 : 1;
  return 0;
}

static int
source_symbol_key_compare(const AbString *left_name, const AbString *left_kind,
                          const AbString *left_scope, uint64_t left_line,
                          const AbString *right_name,
                          const AbString *right_kind,
                          const AbString *right_scope, uint64_t right_line) {
  int compared = ab_string_compare(left_name, right_name);
  if (compared)
    return compared;
  compared = ab_string_compare(left_kind, right_kind);
  if (compared)
    return compared;
  compared = ab_string_compare(left_scope, right_scope);
  if (compared)
    return compared;
  return (left_line > right_line) - (left_line < right_line);
}

static int matched_symbol_ref_compare(const void *left_raw,
                                      const void *right_raw) {
  const AbMatchedSymbolRef *left = (const AbMatchedSymbolRef *)left_raw;
  const AbMatchedSymbolRef *right = (const AbMatchedSymbolRef *)right_raw;
  int compared = ab_string_compare(left->path, right->path);
  return compared
             ? compared
             : source_symbol_key_compare(left->name, left->kind, left->scope,
                                         left->line, right->name, right->kind,
                                         right->scope, right->line);
}

static int source_symbol_ref_compare(const void *left_raw,
                                     const void *right_raw) {
  const AbSourceSymbolRef *left = (const AbSourceSymbolRef *)left_raw;
  const AbSourceSymbolRef *right = (const AbSourceSymbolRef *)right_raw;
  return source_symbol_key_compare(left->name, left->kind, left->scope,
                                   left->line, right->name, right->kind,
                                   right->scope, right->line);
}

static int source_symbol_matched_compare(const AbSourceSymbolRef *symbol,
                                         const AbMatchedSymbolRef *matched) {
  return source_symbol_key_compare(symbol->name, symbol->kind, symbol->scope,
                                   symbol->line, matched->name, matched->kind,
                                   matched->scope, matched->line);
}

static void matched_symbol_range(const AbSourceArtifactIndex *index,
                                 const AbString *path, size_t *out_start,
                                 size_t *out_count) {
  size_t low = 0;
  size_t high = index->matched_count;
  size_t start;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (ab_string_compare(index->matched[middle].path, path) < 0)
      low = middle + 1;
    else
      high = middle;
  }
  start = low;
  high = index->matched_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (ab_string_compare(index->matched[middle].path, path) <= 0)
      low = middle + 1;
    else
      high = middle;
  }
  *out_start = start;
  *out_count = low - start;
}

static void source_symbol_range(const AbSourceSymbolRef *symbols,
                                size_t symbol_count,
                                const AbMatchedSymbolRef *matched,
                                size_t *out_start, size_t *out_count) {
  size_t low = 0;
  size_t high = symbol_count;
  size_t start;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (source_symbol_matched_compare(&symbols[middle], matched) < 0)
      low = middle + 1;
    else
      high = middle;
  }
  start = low;
  high = symbol_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (source_symbol_matched_compare(&symbols[middle], matched) <= 0)
      low = middle + 1;
    else
      high = middle;
  }
  *out_start = start;
  *out_count = low - start;
}

static ArchbirdStatus
collect_matched_ranges(ArchbirdEngine *engine, const AbValue *file,
                       const AbMatchedSymbolRef *matched, size_t matched_count,
                       size_t source_length, AbSourceRange **out_ranges,
                       size_t *out_count, size_t *out_unavailable) {
  const AbValue *symbols = required_array(file, "symbols");
  AbSourceSymbolRef *symbol_index = NULL;
  AbSourceRange *ranges = NULL;
  size_t count = 0;
  size_t unavailable = 0;
  size_t matched_index;
  *out_ranges = NULL;
  *out_count = 0;
  *out_unavailable = 0;
  if (!symbols || (matched_count && !matched))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (matched_count) {
    size_t symbol_index_index;
    ranges = (AbSourceRange *)ab_calloc(engine, matched_count, sizeof(*ranges));
    if (!ranges)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting source extents");
    symbol_index = (AbSourceSymbolRef *)ab_calloc(
        engine, symbols->as.array.count, sizeof(*symbol_index));
    if (!symbol_index && symbols->as.array.count) {
      ab_free(engine, ranges);
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing source symbols");
    }
    for (symbol_index_index = 0; symbol_index_index < symbols->as.array.count;
         symbol_index_index++) {
      const AbValue *symbol = &symbols->as.array.items[symbol_index_index];
      AbSourceSymbolRef *reference = &symbol_index[symbol_index_index];
      reference->row = symbol;
      reference->name = &required_string(symbol, "name")->as.text;
      reference->kind = &required_string(symbol, "kind")->as.text;
      reference->scope = &required_string(symbol, "scope")->as.text;
      (void)ab_value_u64(ab_value_member(symbol, "line"), &reference->line);
    }
    if (symbols->as.array.count > 1)
      qsort(symbol_index, symbols->as.array.count, sizeof(*symbol_index),
            source_symbol_ref_compare);
  }
  for (matched_index = 0; matched_index < matched_count; matched_index++) {
    const AbMatchedSymbolRef *wanted = &matched[matched_index];
    size_t symbol_start = 0;
    size_t symbol_count = 0;
    source_symbol_range(symbol_index, symbols->as.array.count, wanted,
                        &symbol_start, &symbol_count);
    if (symbol_count == 1) {
      const AbValue *symbol = symbol_index[symbol_start].row;
      const AbValue *extent;
      uint64_t start = 0;
      uint64_t end = 0;
      extent = ab_value_member(symbol, "extent");
      if (!extent) {
        unavailable++;
      } else {
        if (extent->kind != AB_VALUE_OBJECT ||
            !ab_value_u64(ab_value_member(extent, "start"), &start) ||
            !ab_value_u64(ab_value_member(extent, "end"), &end) ||
            start >= end || end > source_length) {
          ab_free(engine, symbol_index);
          ab_free(engine, ranges);
          return source_error(engine, ARCHBIRD_INVALID_SCHEMA,
                              "artifact contains an invalid source extent");
        }
        ranges[count].start = (size_t)start;
        ranges[count].end = (size_t)end;
        ranges[count].symbol = symbol;
        count++;
      }
    } else if (!symbol_count) {
      ab_free(engine, symbol_index);
      ab_free(engine, ranges);
      return source_error(engine, ARCHBIRD_INVALID_SCHEMA,
                          "Query matched symbol is absent from its source Map");
    } else {
      unavailable++;
    }
  }
  ab_free(engine, symbol_index);
  if (count > 1)
    qsort(ranges, count, sizeof(*ranges), range_compare);
  if (count > 1) {
    size_t read_index;
    size_t write_index = 0;
    for (read_index = 0; read_index < count; read_index++) {
      if (write_index && ranges[read_index].end <= ranges[write_index - 1].end)
        continue;
      ranges[write_index++] = ranges[read_index];
    }
    count = write_index;
  }
  *out_ranges = ranges;
  *out_count = count;
  *out_unavailable = unavailable;
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_range(AbBuffer *out, const AbValue *file,
                                   const AbSourceRange *range,
                                   const uint8_t *source) {
  const AbValue *name = required_string(range->symbol, "name");
  const AbValue *kind = required_string(range->symbol, "kind");
  const AbValue *language = required_string(file, "language");
  uint64_t line = 0;
  ArchbirdStatus status;
  if (!name || !kind ||
      !ab_value_u64(ab_value_member(range->symbol, "line"), &line))
    return source_error(out->engine, ARCHBIRD_INVALID_SCHEMA,
                        "artifact contains an invalid ranged symbol");
  status = ab_buffer_literal(out, "### ");
  if (status == ARCHBIRD_OK)
    status = markdown_text(out, &name->as.text);
  if (status == ARCHBIRD_OK)
    status = ab_report_appendf(out, " · `%.*s` · line %llu\n\n",
                               (int)kind->as.text.length, kind->as.text.data,
                               (unsigned long long)line);
  if (status == ARCHBIRD_OK)
    status = render_source_fence(out, &language->as.text, source + range->start,
                                 range->end - range->start);
  return status;
}

static ArchbirdStatus render_unavailable_extent_notice(AbBuffer *out,
                                                       size_t unavailable) {
  return ab_report_appendf(
      out,
      "_%zu matched declaration(s) could not be bound to one exact source "
      "extent; their Map signatures remain available in the focused Query "
      "view._\n\n",
      unavailable);
}

static void source_artifact_index_free(ArchbirdEngine *engine,
                                       AbSourceArtifactIndex *index) {
  ab_free(engine, index->matched);
  ab_free(engine, index->direct_files);
  ab_free(engine, index->files);
  memset(index, 0, sizeof(*index));
}

static ArchbirdStatus
validate_source_artifact(ArchbirdEngine *engine, const AbValue *artifact,
                         const AbValue **out_project, const AbValue **out_files,
                         const AbValue **out_evidence, int *out_is_query,
                         AbSourceArtifactIndex *out_index) {
  const AbValue *project;
  const AbValue *files;
  const AbValue *evidence;
  const AbValue *matched = NULL;
  const AbValue *path_patterns = NULL;
  int is_query;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  memset(out_index, 0, sizeof(*out_index));
  if (!artifact || artifact->kind != AB_VALUE_OBJECT)
    return source_error(engine, ARCHBIRD_INVALID_SCHEMA,
                        "input is not a Map or Query artifact");
  is_query = string_is(ab_value_member(artifact, "artifact"), "query");
  if (!is_query && !string_is(ab_value_member(artifact, "artifact"), "map"))
    return source_error(engine, ARCHBIRD_INVALID_SCHEMA,
                        "input is not a Map or Query artifact");
  if (!is_query)
    status = ab_projection_map_validate(engine, artifact, "source view Map");
  if (status != ARCHBIRD_OK)
    return status;
  project = required_string(artifact, "project");
  files = required_array(artifact, "files");
  evidence = ab_value_member(artifact, "evidence");
  if (!project || !files || !evidence || evidence->kind != AB_VALUE_OBJECT ||
      !lowercase_sha256(ab_value_member(evidence, "config_sha256")) ||
      !lowercase_sha256(ab_value_member(evidence, "input_sha256")))
    return source_error(engine, ARCHBIRD_INVALID_SCHEMA,
                        "artifact identity or file inventory is invalid");
  if (is_query) {
    matched = required_array(artifact, "matched_symbols");
    {
      const AbValue *query = ab_value_member(artifact, "query");
      const AbValue *plan = query && query->kind == AB_VALUE_OBJECT
                                ? ab_value_member(query, "plan")
                                : NULL;
      const AbValue *selection = plan && plan->kind == AB_VALUE_OBJECT
                                     ? ab_value_member(plan, "selection")
                                     : NULL;
      if (selection && selection->kind == AB_VALUE_OBJECT)
        path_patterns = required_array(selection, "paths");
    }
    if (!matched || !path_patterns)
      return source_error(engine, ARCHBIRD_INVALID_SCHEMA,
                          "Query source selection is incomplete");
    for (index = 0; index < path_patterns->as.array.count; index++) {
      const AbValue *pattern = &path_patterns->as.array.items[index];
      if (pattern->kind != AB_VALUE_STRING || !pattern->as.text.length)
        return source_error(engine, ARCHBIRD_INVALID_SCHEMA,
                            "Query source path selector is invalid");
    }
  }
  for (index = 0; index < files->as.array.count; index++) {
    const AbValue *file = &files->as.array.items[index];
    const AbValue *symbols = required_array(file, "symbols");
    const AbValue *bytes = ab_value_member(file, "bytes");
    const AbValue *path = ab_value_member(file, "path");
    uint64_t byte_length = 0;
    size_t symbol_index;
    if (file->kind != AB_VALUE_OBJECT || !repository_path(path) ||
        !lowercase_sha256(ab_value_member(file, "sha256")) ||
        !stable_token(ab_value_member(file, "language")) || !symbols ||
        (!is_query && !ab_value_u64(bytes, &byte_length)) ||
        (is_query && bytes && !ab_value_u64(bytes, &byte_length)))
      return source_error(engine, ARCHBIRD_INVALID_SCHEMA,
                          "artifact contains an invalid source file row");
    if (!is_query && index &&
        ab_string_compare(
            &ab_value_member(&files->as.array.items[index - 1], "path")
                 ->as.text,
            &ab_value_member(file, "path")->as.text) >= 0)
      return source_error(engine, ARCHBIRD_INVALID_SCHEMA,
                          "Map source files are not sorted and unique");
    for (symbol_index = 0; symbol_index < symbols->as.array.count;
         symbol_index++) {
      const AbValue *symbol = &symbols->as.array.items[symbol_index];
      const AbValue *extent;
      uint64_t line = 0;
      uint64_t start = 0;
      uint64_t end = 0;
      if (symbol->kind != AB_VALUE_OBJECT || !required_string(symbol, "name") ||
          !stable_token(ab_value_member(symbol, "kind")) ||
          !required_string(symbol, "scope") ||
          !required_string(symbol, "signature") ||
          !ab_value_u64(ab_value_member(symbol, "line"), &line) || !line)
        return source_error(engine, ARCHBIRD_INVALID_SCHEMA,
                            "artifact contains an invalid source symbol");
      extent = ab_value_member(symbol, "extent");
      if (extent && (extent->kind != AB_VALUE_OBJECT ||
                     !ab_value_u64(ab_value_member(extent, "start"), &start) ||
                     !ab_value_u64(ab_value_member(extent, "end"), &end) ||
                     start >= end || (bytes && end > byte_length)))
        return source_error(engine, ARCHBIRD_INVALID_SCHEMA,
                            "artifact contains an invalid source extent");
    }
  }
  if (is_query) {
    if (files->as.array.count) {
      out_index->files = (AbSourceFileRef *)ab_calloc(
          engine, files->as.array.count, sizeof(*out_index->files));
      if (!out_index->files)
        return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory indexing selected sources");
      out_index->file_count = files->as.array.count;
      for (index = 0; index < files->as.array.count; index++) {
        out_index->files[index].path =
            &required_string(&files->as.array.items[index], "path")->as.text;
      }
      if (out_index->file_count > 1)
        qsort(out_index->files, out_index->file_count,
              sizeof(*out_index->files), source_file_ref_compare);
      for (index = 1; index < out_index->file_count; index++) {
        if (ab_string_equal(out_index->files[index - 1].path,
                            out_index->files[index].path)) {
          status = source_error(engine, ARCHBIRD_INVALID_SCHEMA,
                                "Query source files are duplicated");
          goto fail;
        }
      }
    }
    if (matched->as.array.count) {
      out_index->matched = (AbMatchedSymbolRef *)ab_calloc(
          engine, matched->as.array.count, sizeof(*out_index->matched));
      if (!out_index->matched) {
        status = archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory indexing matched symbols");
        goto fail;
      }
      out_index->matched_count = matched->as.array.count;
    }
    if (path_patterns->as.array.count && out_index->file_count) {
      out_index->direct_files = (AbSourceFileRef *)ab_calloc(
          engine, out_index->file_count, sizeof(*out_index->direct_files));
      if (!out_index->direct_files) {
        status = archbird_error_set(
            engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
            "out of memory indexing direct source selections");
        goto fail;
      }
      for (index = 0; index < files->as.array.count; index++) {
        const AbValue *path =
            required_string(&files->as.array.items[index], "path");
        size_t pattern_index;
        int direct = 0;
        for (pattern_index = 0; pattern_index < path_patterns->as.array.count;
             pattern_index++) {
          if (ab_map_path_selector_match(
                  &path->as.text,
                  &path_patterns->as.array.items[pattern_index].as.text)) {
            direct = 1;
            break;
          }
        }
        if (direct)
          out_index->direct_files[out_index->direct_file_count++].path =
              &path->as.text;
      }
      if (!out_index->direct_file_count) {
        ab_free(engine, out_index->direct_files);
        out_index->direct_files = NULL;
      } else if (out_index->direct_file_count > 1) {
        qsort(out_index->direct_files, out_index->direct_file_count,
              sizeof(*out_index->direct_files), source_file_ref_compare);
      }
    }
    for (index = 0; index < matched->as.array.count; index++) {
      const AbValue *row = &matched->as.array.items[index];
      const AbValue *path = required_string(row, "path");
      const AbValue *name = required_string(row, "name");
      const AbValue *kind = ab_value_member(row, "kind");
      const AbValue *scope = required_string(row, "scope");
      AbMatchedSymbolRef *reference = &out_index->matched[index];
      uint64_t line = 0;
      if (row->kind != AB_VALUE_OBJECT || !repository_path(path) || !name ||
          !stable_token(kind) || !scope ||
          !ab_value_u64(ab_value_member(row, "line"), &line) || !line ||
          !find_source_file(out_index->files, out_index->file_count,
                            &path->as.text)) {
        status =
            source_error(engine, ARCHBIRD_INVALID_SCHEMA,
                         "Query matched symbols are invalid or unselected");
        goto fail;
      }
      reference->path = &path->as.text;
      reference->name = &name->as.text;
      reference->kind = &kind->as.text;
      reference->scope = &scope->as.text;
      reference->line = line;
    }
    if (out_index->matched_count > 1)
      qsort(out_index->matched, out_index->matched_count,
            sizeof(*out_index->matched), matched_symbol_ref_compare);
    if (out_index->matched_count > 1) {
      size_t read_index;
      size_t write_index = 1;
      for (read_index = 1; read_index < out_index->matched_count;
           read_index++) {
        if (matched_symbol_ref_compare(&out_index->matched[write_index - 1],
                                       &out_index->matched[read_index]) == 0)
          continue;
        out_index->matched[write_index++] = out_index->matched[read_index];
      }
      out_index->matched_count = write_index;
    }
  }
  *out_project = project;
  *out_files = files;
  *out_evidence = evidence;
  *out_is_query = is_query;
  return ARCHBIRD_OK;

fail:
  source_artifact_index_free(engine, out_index);
  return status;
}

static ArchbirdStatus render_selected_file(
    ArchbirdEngine *engine, const AbValue *file, int is_query, int direct_file,
    const AbMatchedSymbolRef *matched, size_t matched_count,
    ArchbirdReportDetail detail, size_t max_chars, size_t *out_omitted_records,
    size_t *out_omitted_bytes, AbSourceReportLookupFn source_lookup,
    void *source_user_data, AbBuffer *block) {
  const AbValue *path = required_string(file, "path");
  const AbValue *sha = required_string(file, "sha256");
  const uint8_t *source = NULL;
  size_t source_length = 0;
  ArchbirdStatus status;
  status =
      source_lookup(source_user_data, &path->as.text, &source, &source_length);
  if (status != ARCHBIRD_OK)
    return source_error(
        engine, ARCHBIRD_CONFLICT,
        "selected source is not present in the supplied project");
  if (!source && source_length)
    return source_error(engine, ARCHBIRD_CONFLICT,
                        "selected source bytes are unavailable");
  {
    uint8_t digest[32];
    char source_sha[65];
    status = archbird_sha256(source, source_length, digest);
    if (status != ARCHBIRD_OK)
      return status;
    archbird_sha256_hex(digest, source_sha);
    if (memcmp(source_sha, sha->as.text.data, 64) != 0)
      return source_error(
          engine, ARCHBIRD_CONFLICT,
          "selected source bytes do not match the artifact SHA-256");
  }
  status = render_file_header(block, file);
  if (status != ARCHBIRD_OK)
    return status;
  if (detail == ARCHBIRD_REPORT_DETAIL_COMPACT)
    return render_outline(block, file, max_chars, out_omitted_records,
                          out_omitted_bytes);
  if (!source_text_safe(source, source_length))
    return ab_buffer_literal(
        block,
        "_Source is not safe UTF-8 text; exact bytes were validated but are "
        "not embedded in Markdown._\n\n");
  if (detail == ARCHBIRD_REPORT_DETAIL_FULL) {
    const AbValue *language = required_string(file, "language");
    return render_source_fence(block, &language->as.text, source,
                               source_length);
  }
  if (is_query && direct_file) {
    const AbValue *language = required_string(file, "language");
    return render_source_fence(block, &language->as.text, source,
                               source_length);
  }
  if (is_query) {
    AbSourceRange *ranges = NULL;
    size_t range_count = 0;
    size_t unavailable = 0;
    size_t index;
    status = collect_matched_ranges(engine, file, matched, matched_count,
                                    source_length, &ranges, &range_count,
                                    &unavailable);
    if (status != ARCHBIRD_OK) {
      ab_free(engine, ranges);
      return status;
    }
    if (unavailable)
      status = render_unavailable_extent_notice(block, unavailable);
    if (range_count) {
      for (index = 0; status == ARCHBIRD_OK && index < range_count; index++) {
        AbBuffer record;
        ab_buffer_init(&record, engine);
        status = render_range(&record, file, &ranges[index], source);
        if (status == ARCHBIRD_OK)
          status =
              append_bounded_record(block, &record, max_chars,
                                    out_omitted_records, out_omitted_bytes);
        ab_buffer_free(&record);
      }
      ab_free(engine, ranges);
      return status;
    }
    ab_free(engine, ranges);
    if (unavailable) {
      return status == ARCHBIRD_OK
                 ? render_outline(block, file, max_chars, out_omitted_records,
                                  out_omitted_bytes)
                 : status;
    }
    return render_outline(block, file, max_chars, out_omitted_records,
                          out_omitted_bytes);
  }
  return render_outline(block, file, max_chars, out_omitted_records,
                        out_omitted_bytes);
}

ArchbirdStatus
ab_source_report_markdown(ArchbirdEngine *engine, const AbValue *artifact,
                          ArchbirdReportDetail detail, size_t max_chars,
                          AbSourceReportLookupFn source_lookup,
                          void *source_user_data, AbBuffer *out) {
  const AbValue *project_name = NULL;
  const AbValue *files = NULL;
  const AbValue *evidence = NULL;
  AbSourceArtifactIndex artifact_index = {0};
  int is_query = 0;
  size_t index;
  size_t omitted = 0;
  size_t omitted_records = 0;
  size_t omitted_bytes = 0;
  ArchbirdStatus status;
  if (!engine || !artifact || !source_lookup || !out ||
      detail < ARCHBIRD_REPORT_DETAIL_COMPACT ||
      detail > ARCHBIRD_REPORT_DETAIL_FULL)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (detail == ARCHBIRD_REPORT_DETAIL_FULL && max_chars)
    return source_error(engine, ARCHBIRD_INVALID_ARGUMENT,
                        "full detail cannot be combined with max_chars");
  status = validate_source_artifact(engine, artifact, &project_name, &files,
                                    &evidence, &is_query, &artifact_index);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_buffer_literal(out, "# ");
  if (status == ARCHBIRD_OK)
    status = markdown_text(out, &project_name->as.text);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, " source\n\n");
  if (status == ARCHBIRD_OK)
    status = ab_report_appendf(
        out, "Selection: `%s`  \nDetail: `%s`  \nSelected files: %zu  \n",
        is_query ? "Query" : "Map",
        detail == ARCHBIRD_REPORT_DETAIL_COMPACT    ? "compact"
        : detail == ARCHBIRD_REPORT_DETAIL_STANDARD ? "standard"
                                                    : "full",
        files->as.array.count);
  if (status == ARCHBIRD_OK) {
    const AbValue *input = required_string(evidence, "input_sha256");
    status = ab_report_appendf(out, "Map input: `%.*s`\n\n",
                               (int)input->as.text.length, input->as.text.data);
  }
  if (status == ARCHBIRD_OK && max_chars && out->length > max_chars)
    status = source_error(engine, ARCHBIRD_LIMIT_EXCEEDED,
                          "max_chars is too small for source metadata");
  for (index = 0; status == ARCHBIRD_OK && index < files->as.array.count;
       index++) {
    const AbValue *file = &files->as.array.items[index];
    const AbValue *path = required_string(file, "path");
    const AbMatchedSymbolRef *matched = NULL;
    int direct_file = 0;
    size_t matched_start = 0;
    size_t matched_count = 0;
    size_t block_limit = SIZE_MAX;
    AbBuffer block;
    if (!path || !repository_path(path)) {
      status = source_error(engine, ARCHBIRD_INVALID_SCHEMA,
                            "selection contains an invalid source path");
      break;
    }
    if (is_query) {
      matched_symbol_range(&artifact_index, &path->as.text, &matched_start,
                           &matched_count);
      matched = matched_count ? artifact_index.matched + matched_start : NULL;
      direct_file = find_source_file(artifact_index.direct_files,
                                     artifact_index.direct_file_count,
                                     &path->as.text) != NULL;
    }
    if (max_chars) {
      static const size_t ledger_reserve = 320;
      size_t remaining = out->length < max_chars ? max_chars - out->length : 0;
      block_limit = remaining > ledger_reserve ? remaining - ledger_reserve : 0;
    }
    ab_buffer_init(&block, engine);
    status = render_selected_file(engine, file, is_query, direct_file, matched,
                                  matched_count, detail, block_limit,
                                  &omitted_records, &omitted_bytes,
                                  source_lookup, source_user_data, &block);
    if (status == ARCHBIRD_OK && max_chars && block.length > block_limit) {
      omitted++;
      omitted_bytes += block.length;
    } else if (status == ARCHBIRD_OK) {
      status = ab_buffer_append(out, block.data, block.length);
    }
    ab_buffer_free(&block);
  }
  if (status == ARCHBIRD_OK && (omitted || omitted_records)) {
    AbBuffer ledger;
    ab_buffer_init(&ledger, engine);
    status = ab_buffer_literal(&ledger, "## Omitted source\n\n");
    if (status == ARCHBIRD_OK && omitted)
      status = ab_report_appendf(&ledger, "- Files: %zu\n", omitted);
    if (status == ARCHBIRD_OK && omitted_records)
      status = ab_report_appendf(&ledger, "- Declaration records: %zu\n",
                                 omitted_records);
    if (status == ARCHBIRD_OK)
      status = ab_report_appendf(&ledger,
                                 "- Complete rendered bytes omitted: %zu\n"
                                 "- Reason: `max_chars`\n",
                                 omitted_bytes);
    if (status == ARCHBIRD_OK &&
        (!max_chars || (out->length <= max_chars &&
                        ledger.length <= max_chars - out->length)))
      status = ab_buffer_append(out, ledger.data, ledger.length);
    else if (status == ARCHBIRD_OK)
      status = source_error(engine, ARCHBIRD_LIMIT_EXCEEDED,
                            "max_chars is too small for source metadata");
    ab_buffer_free(&ledger);
  }
  source_artifact_index_free(engine, &artifact_index);
  return status;
}
