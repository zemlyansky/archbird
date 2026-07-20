#include "scanner.h"

#include "archbird_internal.h"
#include "fact_builder.h"
#include "project_internal.h"
#include "protobuf.h"
#include "render_internal.h"
#include "sha256.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ARCHBIRD_SCIP_IMPLEMENTATION_SHA256
#define ARCHBIRD_SCIP_IMPLEMENTATION_SHA256                                    \
  "0000000000000000000000000000000000000000000000000000000000000000"
#endif

#define SCIP_DEFINITION_ROLE 1u
#define SCIP_IMPORT_ROLE 2u
#define SCIP_MAX_SYMBOL_BYTES ((size_t)16 * 1024)

typedef struct ScipSlice {
  const uint8_t *data;
  size_t length;
} ScipSlice;

typedef struct ScipRange {
  uint32_t values[4];
  size_t count;
  int present;
} ScipRange;

typedef struct ScipOccurrence {
  ScipSlice symbol;
  uint32_t roles;
  uint32_t syntax_kind;
  ScipRange range;
} ScipOccurrence;

typedef struct ScipRelationship {
  ScipSlice symbol;
  unsigned flags;
} ScipRelationship;

typedef struct ScipSymbol {
  ScipSlice symbol;
  ScipSlice display_name;
  ScipSlice signature;
  ScipSlice enclosing_symbol;
  uint32_t kind;
  ScipRelationship *relationships;
  size_t relationship_count;
  size_t relationship_capacity;
} ScipSymbol;

typedef struct ScipDocument {
  AbString path;
  ScipSlice relative_path;
  ScipSlice language;
  ScipSlice text;
  size_t *line_starts;
  size_t line_count;
  uint32_t position_encoding;
  ScipOccurrence *occurrences;
  size_t occurrence_count;
  size_t occurrence_capacity;
  ScipSymbol *symbols;
  size_t symbol_count;
  size_t symbol_capacity;
  size_t manifest_index;
  int mapped;
  int duplicate;
  int invalid_path;
  int stale_ranges;
  int has_text;
  int source_mismatch;
} ScipDocument;

typedef struct ScipMetadata {
  uint32_t protocol_version;
  ScipSlice tool_name;
  ScipSlice tool_version;
  ScipSlice project_root;
  uint32_t text_encoding;
  size_t tool_argument_count;
} ScipMetadata;

typedef struct ScipDefinition {
  size_t document_index;
  ScipSlice symbol;
  ScipSlice path;
  size_t start;
  size_t end;
  int anchored;
} ScipDefinition;

typedef struct ScipLink {
  const char *kind;
  size_t source_document;
  size_t target_document;
  ScipSlice symbol;
} ScipLink;

typedef struct ScipReferenceGroup {
  size_t document_index;
  ScipSlice symbol;
  uint32_t roles;
  uint32_t syntax_kind;
  size_t start;
  size_t end;
  size_t occurrence_count;
  int syntax_kind_mixed;
} ScipReferenceGroup;

typedef struct ScipSymbolMetadata {
  ScipSlice display_name;
  uint32_t kind;
  int display_name_conflict;
  int display_name_from_source;
  int kind_conflict;
} ScipSymbolMetadata;

typedef struct ScipAnalysis {
  ArchbirdEngine *engine;
  const ArchbirdProject *project;
  const AbSourceManifest *manifest;
  const AbConfigIndex *spec;
  const AbManifestFile *index_file;
  ScipMetadata metadata;
  ScipDocument *documents;
  size_t document_count;
  size_t document_capacity;
  ScipDefinition *definitions;
  size_t definition_count;
  size_t definition_capacity;
  ScipLink *links;
  size_t link_count;
  size_t link_capacity;
  ScipReferenceGroup *reference_groups;
  size_t reference_group_count;
  size_t reference_group_capacity;
  ScipSlice *external_symbol_names;
  size_t external_symbol_name_count;
  size_t external_symbol_name_capacity;
  size_t external_symbols;
  size_t occurrences;
  size_t symbols;
  size_t references;
  size_t reference_facts;
  size_t resolved_unique;
  size_t resolved_ambiguous;
  size_t unresolved;
  size_t relationships;
  size_t relationship_edges;
  size_t documents_mapped;
  size_t documents_stale;
  size_t documents_source_verified;
  size_t documents_source_unverified;
  size_t source_mismatches;
  size_t unspecified_position_documents;
  size_t configured_position_documents;
  size_t invalid_ranges;
  size_t invalid_symbols;
  size_t invalid_relationship_symbols;
  size_t invalid_document_paths;
  size_t diagnostic_ordinal;
  AbBundleBuilder builder;
} ScipAnalysis;

static int utf8_codepoint(const uint8_t *bytes, size_t remaining,
                          size_t *out_length, uint32_t *out_value);

static int slice_equal(ScipSlice left, ScipSlice right) {
  return left.length == right.length &&
         (!left.length || memcmp(left.data, right.data, left.length) == 0);
}

static int slice_compare(ScipSlice left, ScipSlice right) {
  size_t common = left.length < right.length ? left.length : right.length;
  int compared = common ? memcmp(left.data, right.data, common) : 0;
  if (compared)
    return compared;
  return left.length < right.length ? -1 : left.length > right.length;
}

static int slice_has_nul(ScipSlice value) {
  return value.length && memchr(value.data, 0, value.length) != NULL;
}

static int slice_is_utf8(ScipSlice value) {
  size_t offset = 0;
  while (offset < value.length) {
    size_t width;
    uint32_t scalar;
    if (!utf8_codepoint(value.data + offset, value.length - offset, &width,
                        &scalar))
      return 0;
    offset += width;
  }
  return 1;
}

static ArchbirdStatus validate_string(ScipAnalysis *analysis, ScipSlice value,
                                      const char *context) {
  if (slice_is_utf8(value))
    return ARCHBIRD_OK;
  return archbird_error_set(analysis->engine, ARCHBIRD_INVALID_SCHEMA,
                            ARCHBIRD_NO_OFFSET, "SCIP %s is not valid UTF-8",
                            context);
}

static ArchbirdStatus grow(ScipAnalysis *analysis, void **items,
                           size_t *capacity, size_t count, size_t item_size,
                           const char *label) {
  size_t next;
  void *resized;
  if (count < *capacity)
    return ARCHBIRD_OK;
  if (count >= analysis->engine->options.max_values)
    return archbird_error_set(analysis->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "SCIP %s count exceeds the value limit", label);
  next = *capacity ? (*capacity > analysis->engine->options.max_values / 2
                          ? analysis->engine->options.max_values
                          : *capacity * 2)
                   : 16;
  if (next > analysis->engine->options.max_values)
    next = analysis->engine->options.max_values;
  if (next <= count || item_size > SIZE_MAX / next)
    return archbird_error_set(analysis->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "SCIP %s storage exceeds native size", label);
  resized = ab_realloc(analysis->engine, *items, next * item_size);
  if (!resized)
    return archbird_error_set(analysis->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory decoding SCIP %s", label);
  *items = resized;
  *capacity = next;
  return ARCHBIRD_OK;
}

static int canonical_relative_path(ScipSlice path) {
  size_t start = 0;
  size_t index;
  if (!path.length || path.data[0] == '/' ||
      path.data[path.length - 1] == '/' || slice_has_nul(path))
    return 0;
  for (index = 0; index <= path.length; index++) {
    if (index < path.length && path.data[index] != '/') {
      if (path.data[index] == '\\')
        return 0;
      continue;
    }
    if (index == start || (index - start == 1 && path.data[start] == '.') ||
        (index - start == 2 && path.data[start] == '.' &&
         path.data[start + 1] == '.'))
      return 0;
    start = index + 1;
  }
  return 1;
}

static const AbManifestFile *manifest_file(const AbSourceManifest *manifest,
                                           const char *path, size_t length,
                                           size_t *out_index) {
  AbString wanted = {(char *)path, length};
  size_t low = 0;
  size_t high = manifest->file_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(&manifest->files[middle].path, &wanted);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else {
      if (out_index)
        *out_index = middle;
      return &manifest->files[middle];
    }
  }
  return NULL;
}

static ArchbirdStatus prefixed_path(ScipAnalysis *analysis, ScipSlice relative,
                                    AbString *out) {
  size_t prefix = analysis->spec->path_prefix.length;
  size_t length = prefix + (prefix ? 1 : 0) + relative.length;
  char *bytes;
  if (length < prefix || length < relative.length)
    return archbird_error_set(analysis->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "SCIP document path exceeds native size");
  bytes = (char *)ab_malloc(analysis->engine, length + 1);
  if (!bytes)
    return archbird_error_set(analysis->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory storing SCIP document path");
  if (prefix)
    memcpy(bytes, analysis->spec->path_prefix.data, prefix);
  if (prefix)
    bytes[prefix] = '/';
  if (relative.length)
    memcpy(bytes + prefix + (prefix ? 1 : 0), relative.data, relative.length);
  bytes[length] = '\0';
  out->data = bytes;
  out->length = length;
  return ARCHBIRD_OK;
}

static ArchbirdStatus parse_tool_info(ScipAnalysis *analysis,
                                      const AbPbField *message) {
  AbPbCursor cursor;
  AbPbField field;
  int has;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_pb_cursor_init(&cursor, message->bytes, message->length);
  while (status == ARCHBIRD_OK &&
         (status = ab_pb_next(analysis->engine, &cursor, &field, &has)) ==
             ARCHBIRD_OK &&
         has) {
    if ((field.number == 1 || field.number == 2 || field.number == 3) &&
        field.wire != 2)
      return archbird_error_set(analysis->engine, ARCHBIRD_INVALID_SCHEMA,
                                cursor.offset,
                                "SCIP ToolInfo string has invalid wire type");
    if (field.number == 1)
      analysis->metadata.tool_name = (ScipSlice){field.bytes, field.length};
    else if (field.number == 2)
      analysis->metadata.tool_version = (ScipSlice){field.bytes, field.length};
    else if (field.number == 3)
      analysis->metadata.tool_argument_count++;
    if (status == ARCHBIRD_OK &&
        (field.number == 1 || field.number == 2 || field.number == 3))
      status = validate_string(analysis, (ScipSlice){field.bytes, field.length},
                               field.number == 1   ? "tool name"
                               : field.number == 2 ? "tool version"
                                                   : "tool argument");
  }
  return status;
}

static ArchbirdStatus parse_metadata(ScipAnalysis *analysis,
                                     const AbPbField *message) {
  AbPbCursor cursor;
  AbPbField field;
  int has;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_pb_cursor_init(&cursor, message->bytes, message->length);
  while (status == ARCHBIRD_OK &&
         (status = ab_pb_next(analysis->engine, &cursor, &field, &has)) ==
             ARCHBIRD_OK &&
         has) {
    if ((field.number == 1 || field.number == 4) && field.wire != 0)
      return archbird_error_set(analysis->engine, ARCHBIRD_INVALID_SCHEMA,
                                cursor.offset,
                                "SCIP Metadata enum has invalid wire type");
    if (field.number == 1)
      analysis->metadata.protocol_version = (uint32_t)field.integer;
    else if (field.number == 2) {
      if (field.wire != 2)
        return archbird_error_set(
            analysis->engine, ARCHBIRD_INVALID_SCHEMA, cursor.offset,
            "SCIP Metadata ToolInfo has invalid wire type");
      status = parse_tool_info(analysis, &field);
    } else if (field.number == 3) {
      if (field.wire != 2)
        return archbird_error_set(
            analysis->engine, ARCHBIRD_INVALID_SCHEMA, cursor.offset,
            "SCIP Metadata project root has invalid wire type");
      analysis->metadata.project_root = (ScipSlice){field.bytes, field.length};
      status = validate_string(analysis, analysis->metadata.project_root,
                               "project root");
    } else if (field.number == 4)
      analysis->metadata.text_encoding = (uint32_t)field.integer;
  }
  return status;
}

static ArchbirdStatus parse_typed_range(ScipAnalysis *analysis,
                                        const AbPbField *message, int multi,
                                        ScipRange *out) {
  AbPbCursor cursor;
  AbPbField field;
  uint32_t values[4] = {0, 0, 0, 0};
  int has;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (message->wire != 2)
    return archbird_error_set(analysis->engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "SCIP typed range has invalid wire type");
  ab_pb_cursor_init(&cursor, message->bytes, message->length);
  while (status == ARCHBIRD_OK &&
         (status = ab_pb_next(analysis->engine, &cursor, &field, &has)) ==
             ARCHBIRD_OK &&
         has) {
    if (field.number <= (uint32_t)(multi ? 4 : 3)) {
      if (field.wire != 0 || field.integer > INT32_MAX)
        return archbird_error_set(analysis->engine, ARCHBIRD_INVALID_SCHEMA,
                                  cursor.offset,
                                  "SCIP typed range coordinate is invalid");
      values[field.number - 1] = (uint32_t)field.integer;
    }
  }
  if (status != ARCHBIRD_OK)
    return status;
  if (multi) {
    memcpy(out->values, values, sizeof(values));
    out->count = 4;
  } else {
    out->values[0] = values[0];
    out->values[1] = values[1];
    out->values[2] = values[2];
    out->count = 3;
  }
  out->present = 1;
  return ARCHBIRD_OK;
}

static ArchbirdStatus parse_occurrence(ScipAnalysis *analysis,
                                       const AbPbField *message,
                                       ScipOccurrence *out) {
  AbPbCursor cursor;
  AbPbField field;
  ScipRange deprecated = {{0, 0, 0, 0}, 0, 0};
  ScipRange typed = {{0, 0, 0, 0}, 0, 0};
  int has;
  ArchbirdStatus status = ARCHBIRD_OK;
  memset(out, 0, sizeof(*out));
  ab_pb_cursor_init(&cursor, message->bytes, message->length);
  while (status == ARCHBIRD_OK &&
         (status = ab_pb_next(analysis->engine, &cursor, &field, &has)) ==
             ARCHBIRD_OK &&
         has) {
    if (field.number == 1) {
      status = ab_pb_packed_i32(analysis->engine, &field, deprecated.values, 4,
                                &deprecated.count);
      deprecated.present = status == ARCHBIRD_OK;
    } else if (field.number == 2) {
      if (field.wire != 2)
        return archbird_error_set(
            analysis->engine, ARCHBIRD_INVALID_SCHEMA, cursor.offset,
            "SCIP occurrence symbol has invalid wire type");
      out->symbol = (ScipSlice){field.bytes, field.length};
      status = validate_string(analysis, out->symbol, "occurrence symbol");
    } else if (field.number == 3 || field.number == 5) {
      if (field.wire != 0 || field.integer > UINT32_MAX)
        return archbird_error_set(analysis->engine, ARCHBIRD_INVALID_SCHEMA,
                                  cursor.offset,
                                  "SCIP occurrence integer is invalid");
      if (field.number == 3)
        out->roles = (uint32_t)field.integer;
      else
        out->syntax_kind = (uint32_t)field.integer;
    } else if (field.number == 8)
      status = parse_typed_range(analysis, &field, 0, &typed);
    else if (field.number == 9)
      status = parse_typed_range(analysis, &field, 1, &typed);
  }
  if (status == ARCHBIRD_OK)
    out->range = typed.present ? typed : deprecated;
  if (status == ARCHBIRD_OK && out->range.present && out->range.count != 3 &&
      out->range.count != 4)
    status = archbird_error_set(
        analysis->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "SCIP occurrence range must have 3 or 4 values");
  return status;
}

static ArchbirdStatus parse_relationship(ScipAnalysis *analysis,
                                         const AbPbField *message,
                                         ScipRelationship *out) {
  AbPbCursor cursor;
  AbPbField field;
  int has;
  ArchbirdStatus status = ARCHBIRD_OK;
  memset(out, 0, sizeof(*out));
  ab_pb_cursor_init(&cursor, message->bytes, message->length);
  while (status == ARCHBIRD_OK &&
         (status = ab_pb_next(analysis->engine, &cursor, &field, &has)) ==
             ARCHBIRD_OK &&
         has) {
    if (field.number == 1) {
      if (field.wire != 2)
        return archbird_error_set(
            analysis->engine, ARCHBIRD_INVALID_SCHEMA, cursor.offset,
            "SCIP relationship symbol has invalid wire type");
      out->symbol = (ScipSlice){field.bytes, field.length};
      status = validate_string(analysis, out->symbol, "relationship symbol");
    } else if (field.number >= 2 && field.number <= 5) {
      if (field.wire != 0)
        return archbird_error_set(
            analysis->engine, ARCHBIRD_INVALID_SCHEMA, cursor.offset,
            "SCIP relationship flag has invalid wire type");
      if (field.integer)
        out->flags |= 1u << (field.number - 2);
    }
  }
  return status;
}

static ArchbirdStatus parse_signature(ScipAnalysis *analysis,
                                      const AbPbField *message,
                                      ScipSlice *out) {
  AbPbCursor cursor;
  AbPbField field;
  int has;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_pb_cursor_init(&cursor, message->bytes, message->length);
  while (status == ARCHBIRD_OK &&
         (status = ab_pb_next(analysis->engine, &cursor, &field, &has)) ==
             ARCHBIRD_OK &&
         has) {
    if (field.number == 5) {
      if (field.wire != 2)
        return archbird_error_set(analysis->engine, ARCHBIRD_INVALID_SCHEMA,
                                  cursor.offset,
                                  "SCIP signature text has invalid wire type");
      *out = (ScipSlice){field.bytes, field.length};
      status = validate_string(analysis, *out, "signature text");
    }
  }
  return status;
}

static ArchbirdStatus parse_symbol(ScipAnalysis *analysis,
                                   const AbPbField *message, ScipSymbol *out) {
  AbPbCursor cursor;
  AbPbField field;
  int has;
  ArchbirdStatus status = ARCHBIRD_OK;
  memset(out, 0, sizeof(*out));
  ab_pb_cursor_init(&cursor, message->bytes, message->length);
  while (status == ARCHBIRD_OK &&
         (status = ab_pb_next(analysis->engine, &cursor, &field, &has)) ==
             ARCHBIRD_OK &&
         has) {
    if (field.number == 1 || field.number == 6 || field.number == 8) {
      if (field.wire != 2)
        return archbird_error_set(analysis->engine, ARCHBIRD_INVALID_SCHEMA,
                                  cursor.offset,
                                  "SCIP symbol string has invalid wire type");
      if (field.number == 1)
        out->symbol = (ScipSlice){field.bytes, field.length};
      else if (field.number == 6)
        out->display_name = (ScipSlice){field.bytes, field.length};
      else
        out->enclosing_symbol = (ScipSlice){field.bytes, field.length};
      status = validate_string(analysis,
                               field.number == 1   ? out->symbol
                               : field.number == 6 ? out->display_name
                                                   : out->enclosing_symbol,
                               field.number == 1   ? "symbol identity"
                               : field.number == 6 ? "symbol display name"
                                                   : "enclosing symbol");
    } else if (field.number == 4) {
      ScipRelationship *relationship;
      status = grow(analysis, (void **)&out->relationships,
                    &out->relationship_capacity, out->relationship_count,
                    sizeof(*out->relationships), "relationships");
      if (status != ARCHBIRD_OK)
        break;
      relationship = &out->relationships[out->relationship_count++];
      status = parse_relationship(analysis, &field, relationship);
    } else if (field.number == 5) {
      if (field.wire != 0 || field.integer > UINT32_MAX)
        return archbird_error_set(analysis->engine, ARCHBIRD_INVALID_SCHEMA,
                                  cursor.offset, "SCIP symbol kind is invalid");
      out->kind = (uint32_t)field.integer;
    } else if (field.number == 7) {
      if (field.wire != 2)
        return archbird_error_set(analysis->engine, ARCHBIRD_INVALID_SCHEMA,
                                  cursor.offset,
                                  "SCIP signature has invalid wire type");
      status = parse_signature(analysis, &field, &out->signature);
    }
  }
  return status;
}

static ArchbirdStatus parse_external_symbol(ScipAnalysis *analysis,
                                            const AbPbField *message) {
  AbPbCursor cursor;
  AbPbField field;
  int has;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_pb_cursor_init(&cursor, message->bytes, message->length);
  while (status == ARCHBIRD_OK &&
         (status = ab_pb_next(analysis->engine, &cursor, &field, &has)) ==
             ARCHBIRD_OK &&
         has) {
    if (field.number == 1) {
      if (field.wire != 2)
        return archbird_error_set(
            analysis->engine, ARCHBIRD_INVALID_SCHEMA, cursor.offset,
            "SCIP external symbol name has invalid wire type");
      status =
          grow(analysis, (void **)&analysis->external_symbol_names,
               &analysis->external_symbol_name_capacity,
               analysis->external_symbol_name_count,
               sizeof(*analysis->external_symbol_names), "external symbols");
      if (status == ARCHBIRD_OK)
        analysis
            ->external_symbol_names[analysis->external_symbol_name_count++] =
            (ScipSlice){field.bytes, field.length};
      if (status == ARCHBIRD_OK)
        status = validate_string(
            analysis,
            analysis
                ->external_symbol_names[analysis->external_symbol_name_count -
                                        1],
            "external symbol identity");
      break;
    }
  }
  return status;
}

static void symbol_free(ScipAnalysis *analysis, ScipSymbol *symbol) {
  ab_free(analysis->engine, symbol->relationships);
  memset(symbol, 0, sizeof(*symbol));
}

static void document_free(ScipAnalysis *analysis, ScipDocument *document) {
  size_t index;
  for (index = 0; index < document->symbol_count; index++)
    symbol_free(analysis, &document->symbols[index]);
  ab_free(analysis->engine, document->symbols);
  ab_free(analysis->engine, document->occurrences);
  ab_free(analysis->engine, document->line_starts);
  ab_string_free(analysis->engine, &document->path);
  memset(document, 0, sizeof(*document));
}

static ArchbirdStatus parse_document(ScipAnalysis *analysis,
                                     const AbPbField *message,
                                     ScipDocument *out) {
  AbPbCursor cursor;
  AbPbField field;
  int has;
  ArchbirdStatus status = ARCHBIRD_OK;
  memset(out, 0, sizeof(*out));
  ab_pb_cursor_init(&cursor, message->bytes, message->length);
  while (status == ARCHBIRD_OK &&
         (status = ab_pb_next(analysis->engine, &cursor, &field, &has)) ==
             ARCHBIRD_OK &&
         has) {
    if (field.number == 1 || field.number == 4) {
      if (field.wire != 2) {
        status = archbird_error_set(
            analysis->engine, ARCHBIRD_INVALID_SCHEMA, cursor.offset,
            "SCIP document string has invalid wire type");
        break;
      }
      if (field.number == 1)
        out->relative_path = (ScipSlice){field.bytes, field.length};
      else
        out->language = (ScipSlice){field.bytes, field.length};
      status = validate_string(
          analysis, field.number == 1 ? out->relative_path : out->language,
          field.number == 1 ? "document relative path" : "document language");
    } else if (field.number == 5) {
      if (field.wire != 2) {
        status = archbird_error_set(analysis->engine, ARCHBIRD_INVALID_SCHEMA,
                                    cursor.offset,
                                    "SCIP document text has invalid wire type");
        break;
      }
      out->text = (ScipSlice){field.bytes, field.length};
      out->has_text = 1;
      status = validate_string(analysis, out->text, "embedded document text");
    } else if (field.number == 2) {
      ScipOccurrence *occurrence;
      status =
          grow(analysis, (void **)&out->occurrences, &out->occurrence_capacity,
               out->occurrence_count, sizeof(*out->occurrences), "occurrences");
      if (status != ARCHBIRD_OK)
        break;
      occurrence = &out->occurrences[out->occurrence_count++];
      status = parse_occurrence(analysis, &field, occurrence);
    } else if (field.number == 3) {
      ScipSymbol *symbol;
      status = grow(analysis, (void **)&out->symbols, &out->symbol_capacity,
                    out->symbol_count, sizeof(*out->symbols), "symbols");
      if (status != ARCHBIRD_OK)
        break;
      symbol = &out->symbols[out->symbol_count++];
      status = parse_symbol(analysis, &field, symbol);
    } else if (field.number == 6) {
      if (field.wire != 0 || field.integer > UINT32_MAX) {
        status = archbird_error_set(
            analysis->engine, ARCHBIRD_INVALID_SCHEMA, cursor.offset,
            "SCIP document position encoding is invalid");
        break;
      }
      out->position_encoding = (uint32_t)field.integer;
    }
  }
  if (status != ARCHBIRD_OK)
    document_free(analysis, out);
  return status;
}

static int document_compare(const void *left_raw, const void *right_raw) {
  const ScipDocument *left = (const ScipDocument *)left_raw;
  const ScipDocument *right = (const ScipDocument *)right_raw;
  AbString left_path = left->path;
  AbString right_path = right->path;
  int compared = ab_string_compare(&left_path, &right_path);
  if (compared)
    return compared;
  return slice_compare(left->relative_path, right->relative_path);
}

static ArchbirdStatus parse_index(ScipAnalysis *analysis, const uint8_t *bytes,
                                  size_t length) {
  AbPbCursor cursor;
  AbPbField field;
  size_t metadata_count = 0;
  int payload_started = 0;
  int has;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_pb_cursor_init(&cursor, bytes, length);
  while (status == ARCHBIRD_OK &&
         (status = ab_pb_next(analysis->engine, &cursor, &field, &has)) ==
             ARCHBIRD_OK &&
         has) {
    if (field.number == 1) {
      if (field.wire != 2 || metadata_count || payload_started)
        return archbird_error_set(
            analysis->engine, ARCHBIRD_INVALID_SCHEMA, cursor.offset,
            "SCIP metadata must appear exactly once at the start of the index");
      metadata_count++;
      status = parse_metadata(analysis, &field);
    } else {
      if (!metadata_count)
        return archbird_error_set(
            analysis->engine, ARCHBIRD_INVALID_SCHEMA, cursor.offset,
            "SCIP metadata must be the first index field");
      payload_started = 1;
      if (field.number == 2) {
        ScipDocument *document;
        if (field.wire != 2)
          return archbird_error_set(analysis->engine, ARCHBIRD_INVALID_SCHEMA,
                                    cursor.offset,
                                    "SCIP document has invalid wire type");
        status = grow(analysis, (void **)&analysis->documents,
                      &analysis->document_capacity, analysis->document_count,
                      sizeof(*analysis->documents), "documents");
        if (status != ARCHBIRD_OK)
          break;
        document = &analysis->documents[analysis->document_count++];
        status = parse_document(analysis, &field, document);
        if (status == ARCHBIRD_OK) {
          if (!canonical_relative_path(document->relative_path)) {
            document->invalid_path = 1;
            analysis->invalid_document_paths++;
          } else {
            status = prefixed_path(analysis, document->relative_path,
                                   &document->path);
          }
        }
      } else if (field.number == 3) {
        if (field.wire != 2)
          return archbird_error_set(
              analysis->engine, ARCHBIRD_INVALID_SCHEMA, cursor.offset,
              "SCIP external symbol has invalid wire type");
        analysis->external_symbols++;
        status = parse_external_symbol(analysis, &field);
      }
    }
  }
  if (status == ARCHBIRD_OK && metadata_count != 1)
    status = archbird_error_set(analysis->engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET,
                                "SCIP index is missing metadata");
  if (status != ARCHBIRD_OK)
    return status;
  if (analysis->document_count > 1)
    qsort(analysis->documents, analysis->document_count,
          sizeof(*analysis->documents), document_compare);
  return ARCHBIRD_OK;
}

static int utf8_codepoint(const uint8_t *bytes, size_t remaining,
                          size_t *out_length, uint32_t *out_value) {
  uint8_t first;
  uint32_t value;
  size_t length;
  size_t index;
  if (!remaining)
    return 0;
  first = bytes[0];
  if (first < 0x80) {
    *out_length = 1;
    *out_value = first;
    return 1;
  }
  if (first >= 0xc2 && first <= 0xdf) {
    length = 2;
    value = first & 0x1fu;
  } else if (first >= 0xe0 && first <= 0xef) {
    length = 3;
    value = first & 0x0fu;
  } else if (first >= 0xf0 && first <= 0xf4) {
    length = 4;
    value = first & 0x07u;
  } else {
    return 0;
  }
  if (remaining < length)
    return 0;
  for (index = 1; index < length; index++) {
    if ((bytes[index] & 0xc0u) != 0x80u)
      return 0;
    value = (value << 6) | (bytes[index] & 0x3fu);
  }
  if ((length == 3 && value < 0x800) || (length == 4 && value < 0x10000) ||
      (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff)
    return 0;
  *out_length = length;
  *out_value = value;
  return 1;
}

static ArchbirdStatus build_line_index(ScipAnalysis *analysis,
                                       const ArchbirdProject *project,
                                       ScipDocument *document) {
  const uint8_t *source;
  size_t length;
  size_t count = 1;
  size_t index;
  size_t write_index = 1;
  if (!document->mapped || document->line_starts)
    return ARCHBIRD_OK;
  source = ab_project_source_bytes(project, document->manifest_index);
  length = analysis->manifest->files[document->manifest_index].byte_length;
  for (index = 0; index < length; index++) {
    if (source[index] != '\n')
      continue;
    if (count >= analysis->engine->options.max_values)
      return archbird_error_set(analysis->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "SCIP source line index exceeds max_values");
    count++;
  }
  if (count > SIZE_MAX / sizeof(*document->line_starts))
    return archbird_error_set(analysis->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "SCIP source line index exceeds native size");
  document->line_starts = (size_t *)ab_malloc(
      analysis->engine, count * sizeof(*document->line_starts));
  if (!document->line_starts)
    return archbird_error_set(analysis->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory indexing SCIP source lines");
  document->line_starts[0] = 0;
  for (index = 0; index < length; index++)
    if (source[index] == '\n')
      document->line_starts[write_index++] = index + 1;
  document->line_count = count;
  return ARCHBIRD_OK;
}

static int line_bounds(const ScipDocument *document, size_t length,
                       uint32_t line, size_t *out_start, size_t *out_end) {
  size_t start;
  if ((size_t)line >= document->line_count)
    return 0;
  start = document->line_starts[line];
  *out_start = start;
  *out_end = (size_t)line + 1 < document->line_count
                 ? document->line_starts[line + 1] - 1
                 : length;
  return 1;
}

static int position_offset(const ScipDocument *document, const uint8_t *source,
                           size_t length, uint32_t line, uint32_t character,
                           uint32_t encoding, size_t *out) {
  size_t start;
  size_t end;
  size_t offset;
  uint32_t units = 0;
  if (!line_bounds(document, length, line, &start, &end))
    return 0;
  if (encoding == 1) {
    if ((size_t)character > end - start)
      return 0;
    *out = start + character;
    if (*out < end && (source[*out] & 0xc0u) == 0x80u)
      return 0;
    return 1;
  }
  if (encoding != 2 && encoding != 3)
    return 0;
  offset = start;
  while (offset < end && units < character) {
    size_t width;
    uint32_t value;
    uint32_t increment;
    if (!utf8_codepoint(source + offset, end - offset, &width, &value))
      return 0;
    increment = encoding == 2 && value > 0xffff ? 2u : 1u;
    if (units + increment > character)
      return 0;
    units += increment;
    offset += width;
  }
  if (units != character)
    return 0;
  *out = offset;
  return 1;
}

static int occurrence_span(const ScipAnalysis *analysis,
                           const ArchbirdProject *project,
                           const ScipDocument *document,
                           const ScipOccurrence *occurrence, size_t *out_start,
                           size_t *out_end) {
  const uint8_t *source;
  const ScipRange *range = &occurrence->range;
  uint32_t start_line;
  uint32_t start_character;
  uint32_t end_line;
  uint32_t end_character;
  uint32_t position_encoding = document->position_encoding
                                   ? document->position_encoding
                                   : analysis->spec->position_encoding_fallback;
  size_t length;
  size_t start;
  size_t end;
  if (!document->mapped || !range->present ||
      (analysis->metadata.text_encoding != 0 &&
       analysis->metadata.text_encoding != 1) ||
      (position_encoding != 1 && position_encoding != 2 &&
       position_encoding != 3))
    return 0;
  source = ab_project_source_bytes(project, document->manifest_index);
  length =
      ab_project_manifest(project)->files[document->manifest_index].byte_length;
  start_line = range->values[0];
  start_character = range->values[1];
  if (range->count == 3) {
    end_line = start_line;
    end_character = range->values[2];
  } else {
    end_line = range->values[2];
    end_character = range->values[3];
  }
  if (!position_offset(document, source, length, start_line, start_character,
                       position_encoding, &start) ||
      !position_offset(document, source, length, end_line, end_character,
                       position_encoding, &end) ||
      start > end)
    return 0;
  *out_start = start;
  *out_end = end;
  return 1;
}

static int local_symbol(ScipSlice symbol) {
  static const char prefix[] = "local ";
  return symbol.length >= sizeof(prefix) - 1 &&
         memcmp(symbol.data, prefix, sizeof(prefix) - 1) == 0;
}

static int valid_symbol(ScipSlice symbol) {
  return symbol.length && symbol.length <= SCIP_MAX_SYMBOL_BYTES &&
         !slice_has_nul(symbol);
}

static ScipSymbolMetadata document_symbol_metadata(const ScipDocument *document,
                                                   ScipSlice identity) {
  ScipSymbolMetadata result;
  size_t index;
  memset(&result, 0, sizeof(result));
  for (index = 0; index < document->symbol_count; index++) {
    const ScipSymbol *candidate = &document->symbols[index];
    if (!slice_equal(candidate->symbol, identity))
      continue;
    if (candidate->display_name.length) {
      if (!result.display_name.length)
        result.display_name = candidate->display_name;
      else if (!slice_equal(result.display_name, candidate->display_name))
        result.display_name_conflict = 1;
    }
    if (candidate->kind) {
      if (!result.kind)
        result.kind = candidate->kind;
      else if (result.kind != candidate->kind)
        result.kind_conflict = 1;
    }
  }
  if (result.display_name_conflict)
    result.display_name = (ScipSlice){NULL, 0};
  if (result.kind_conflict)
    result.kind = 0;
  return result;
}

static int source_display_name_candidate(ScipSlice candidate) {
  size_t index;
  int has_name_character = 0;
  for (index = 0; index < candidate.length; index++) {
    uint8_t byte = candidate.data[index];
    if (byte <= 0x20 || byte == 0x7f)
      return 0;
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '_' || byte == '$' ||
        byte >= 0x80)
      has_name_character = 1;
  }
  return has_name_character;
}

static void add_definition_source_name(ScipAnalysis *analysis,
                                       size_t definition_start,
                                       size_t definition_end,
                                       ScipSymbolMetadata *metadata) {
  size_t index;
  if (metadata->display_name.length || metadata->display_name_conflict)
    return;
  for (index = definition_start; index < definition_end; index++) {
    const ScipDefinition *definition = &analysis->definitions[index];
    const ScipDocument *document;
    const AbManifestFile *file;
    const uint8_t *source;
    ScipSlice candidate;
    if (!definition->anchored)
      continue;
    document = &analysis->documents[definition->document_index];
    if (!document->mapped || document->duplicate || document->stale_ranges ||
        document->source_mismatch)
      continue;
    file = &analysis->manifest->files[document->manifest_index];
    if (definition->start >= definition->end ||
        definition->end > file->byte_length ||
        definition->end - definition->start > SCIP_MAX_SYMBOL_BYTES)
      continue;
    source =
        ab_project_source_bytes(analysis->project, document->manifest_index);
    candidate.data = source + definition->start;
    candidate.length = definition->end - definition->start;
    if (slice_has_nul(candidate) || !slice_is_utf8(candidate) ||
        !source_display_name_candidate(candidate))
      continue;
    if (!metadata->display_name.length) {
      metadata->display_name = candidate;
      metadata->display_name_from_source = 1;
    } else if (!slice_equal(metadata->display_name, candidate)) {
      metadata->display_name = (ScipSlice){NULL, 0};
      metadata->display_name_from_source = 0;
      metadata->display_name_conflict = 1;
      return;
    }
  }
}

static int definition_compare(const void *left_raw, const void *right_raw) {
  const ScipDefinition *left = (const ScipDefinition *)left_raw;
  const ScipDefinition *right = (const ScipDefinition *)right_raw;
  int left_local = local_symbol(left->symbol);
  int right_local = local_symbol(right->symbol);
  int compared;
  if (left_local != right_local)
    return left_local ? 1 : -1;
  if (left_local) {
    compared = slice_compare(left->path, right->path);
    if (compared)
      return compared;
  }
  compared = slice_compare(left->symbol, right->symbol);
  if (compared)
    return compared;
  if (left->document_index != right->document_index)
    return left->document_index < right->document_index ? -1 : 1;
  if (left->start != right->start)
    return left->start < right->start ? -1 : 1;
  if (left->end != right->end)
    return left->end < right->end ? -1 : 1;
  return 0;
}

static int definition_key_compare(const ScipDefinition *definition,
                                  size_t document_index, ScipSlice path,
                                  ScipSlice symbol) {
  int definition_local = local_symbol(definition->symbol);
  int wanted_local = local_symbol(symbol);
  int compared;
  (void)document_index;
  if (definition_local != wanted_local)
    return definition_local ? 1 : -1;
  if (definition_local) {
    compared = slice_compare(definition->path, path);
    if (compared)
      return compared;
  }
  return slice_compare(definition->symbol, symbol);
}

static ArchbirdStatus add_definition(ScipAnalysis *analysis,
                                     size_t document_index, ScipSlice symbol,
                                     size_t start, size_t end, int anchored) {
  ScipDefinition *definition;
  ArchbirdStatus status;
  if (!valid_symbol(symbol))
    return ARCHBIRD_OK;
  status = grow(analysis, (void **)&analysis->definitions,
                &analysis->definition_capacity, analysis->definition_count,
                sizeof(*analysis->definitions), "definitions");
  if (status != ARCHBIRD_OK)
    return status;
  definition = &analysis->definitions[analysis->definition_count++];
  definition->document_index = document_index;
  definition->symbol = symbol;
  definition->path = (ScipSlice){
      (const uint8_t *)analysis->documents[document_index].path.data,
      analysis->documents[document_index].path.length};
  definition->start = start;
  definition->end = end;
  definition->anchored = anchored;
  return ARCHBIRD_OK;
}

static void definition_range(const ScipAnalysis *analysis,
                             size_t document_index, ScipSlice symbol,
                             size_t *out_start, size_t *out_end) {
  ScipSlice path = {
      (const uint8_t *)analysis->documents[document_index].path.data,
      analysis->documents[document_index].path.length};
  size_t low = 0;
  size_t high = analysis->definition_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = definition_key_compare(&analysis->definitions[middle],
                                          document_index, path, symbol);
    if (compared < 0)
      low = middle + 1;
    else
      high = middle;
  }
  *out_start = low;
  while (low < analysis->definition_count &&
         definition_key_compare(&analysis->definitions[low], document_index,
                                path, symbol) == 0)
    low++;
  *out_end = low;
}

static size_t unique_definition_documents(const ScipAnalysis *analysis,
                                          size_t start, size_t end,
                                          size_t *single_document) {
  size_t count = 0;
  size_t previous = SIZE_MAX;
  size_t index;
  for (index = start; index < end; index++) {
    size_t document;
    if (!analysis->definitions[index].anchored)
      continue;
    document = analysis->definitions[index].document_index;
    if (document != previous) {
      previous = document;
      if (count == 0)
        *single_document = document;
      count++;
    }
  }
  return count;
}

static size_t first_anchored_definition(const ScipAnalysis *analysis,
                                        size_t start, size_t end) {
  size_t index;
  for (index = start; index < end; index++)
    if (analysis->definitions[index].anchored)
      return index;
  return SIZE_MAX;
}

static int external_symbol_compare(const void *left_raw,
                                   const void *right_raw) {
  return slice_compare(*(const ScipSlice *)left_raw,
                       *(const ScipSlice *)right_raw);
}

static int external_symbol(const ScipAnalysis *analysis, ScipSlice symbol) {
  size_t low = 0;
  size_t high = analysis->external_symbol_name_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared =
        slice_compare(analysis->external_symbol_names[middle], symbol);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return 1;
  }
  return 0;
}

static ArchbirdStatus add_link(ScipAnalysis *analysis, const char *kind,
                               size_t source_document, size_t target_document,
                               ScipSlice symbol) {
  ScipLink *link;
  ArchbirdStatus status;
  if (source_document == target_document)
    return ARCHBIRD_OK;
  status = grow(analysis, (void **)&analysis->links, &analysis->link_capacity,
                analysis->link_count, sizeof(*analysis->links), "links");
  if (status != ARCHBIRD_OK)
    return status;
  link = &analysis->links[analysis->link_count++];
  link->kind = kind;
  link->source_document = source_document;
  link->target_document = target_document;
  link->symbol = symbol;
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_reference_group(ScipAnalysis *analysis,
                                          size_t document_index,
                                          const ScipOccurrence *occurrence,
                                          size_t start, size_t end) {
  ScipReferenceGroup *group;
  ArchbirdStatus status =
      grow(analysis, (void **)&analysis->reference_groups,
           &analysis->reference_group_capacity, analysis->reference_group_count,
           sizeof(*analysis->reference_groups), "reference groups");
  if (status != ARCHBIRD_OK)
    return status;
  group = &analysis->reference_groups[analysis->reference_group_count++];
  memset(group, 0, sizeof(*group));
  group->document_index = document_index;
  group->symbol = occurrence->symbol;
  group->roles = occurrence->roles;
  group->syntax_kind = occurrence->syntax_kind;
  group->start = start;
  group->end = end;
  group->occurrence_count = 1;
  return ARCHBIRD_OK;
}

static int reference_group_compare(const void *left_raw,
                                   const void *right_raw) {
  const ScipReferenceGroup *left = (const ScipReferenceGroup *)left_raw;
  const ScipReferenceGroup *right = (const ScipReferenceGroup *)right_raw;
  int left_import = (left->roles & SCIP_IMPORT_ROLE) != 0;
  int right_import = (right->roles & SCIP_IMPORT_ROLE) != 0;
  int compared;
  if (left->document_index != right->document_index)
    return left->document_index < right->document_index ? -1 : 1;
  if (left_import != right_import)
    return left_import ? 1 : -1;
  compared = slice_compare(left->symbol, right->symbol);
  if (compared)
    return compared;
  if (left->start != right->start)
    return left->start < right->start ? -1 : 1;
  if (left->end != right->end)
    return left->end < right->end ? -1 : 1;
  if (left->roles != right->roles)
    return left->roles < right->roles ? -1 : 1;
  if (left->syntax_kind != right->syntax_kind)
    return left->syntax_kind < right->syntax_kind ? -1 : 1;
  return 0;
}

static int reference_group_same_key(const ScipReferenceGroup *left,
                                    const ScipReferenceGroup *right) {
  return left->document_index == right->document_index &&
         ((left->roles & SCIP_IMPORT_ROLE) != 0) ==
             ((right->roles & SCIP_IMPORT_ROLE) != 0) &&
         slice_equal(left->symbol, right->symbol);
}

static ArchbirdStatus normalize_reference_groups(ScipAnalysis *analysis) {
  size_t read_index;
  size_t write_index = 0;
  if (analysis->reference_group_count > 1)
    qsort(analysis->reference_groups, analysis->reference_group_count,
          sizeof(*analysis->reference_groups), reference_group_compare);
  for (read_index = 0; read_index < analysis->reference_group_count;
       read_index++) {
    ScipReferenceGroup *current = &analysis->reference_groups[read_index];
    if (write_index &&
        reference_group_same_key(&analysis->reference_groups[write_index - 1],
                                 current)) {
      ScipReferenceGroup *selected =
          &analysis->reference_groups[write_index - 1];
      if (selected->occurrence_count > SIZE_MAX - current->occurrence_count)
        return archbird_error_set(
            analysis->engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
            "SCIP reference occurrence count exceeds native size");
      selected->occurrence_count += current->occurrence_count;
      selected->roles |= current->roles;
      if (selected->syntax_kind != current->syntax_kind)
        selected->syntax_kind_mixed = 1;
      continue;
    }
    if (write_index != read_index)
      analysis->reference_groups[write_index] = *current;
    write_index++;
  }
  analysis->reference_group_count = write_index;
  analysis->reference_facts = write_index;
  return ARCHBIRD_OK;
}

static int link_compare(const void *left_raw, const void *right_raw) {
  const ScipLink *left = (const ScipLink *)left_raw;
  const ScipLink *right = (const ScipLink *)right_raw;
  int compared = strcmp(left->kind, right->kind);
  if (compared)
    return compared;
  if (left->source_document != right->source_document)
    return left->source_document < right->source_document ? -1 : 1;
  if (left->target_document != right->target_document)
    return left->target_document < right->target_document ? -1 : 1;
  return slice_compare(left->symbol, right->symbol);
}

static int link_equal(const ScipLink *left, const ScipLink *right) {
  return strcmp(left->kind, right->kind) == 0 &&
         left->source_document == right->source_document &&
         left->target_document == right->target_document &&
         slice_equal(left->symbol, right->symbol);
}

static int same_edge(const ScipLink *left, const ScipLink *right) {
  return strcmp(left->kind, right->kind) == 0 &&
         left->source_document == right->source_document &&
         left->target_document == right->target_document;
}

static const char *symbol_kind(uint32_t kind) {
  switch (kind) {
  case 7:
    return "class";
  case 8:
    return "constant";
  case 9:
    return "constructor";
  case 11:
    return "enum";
  case 12:
    return "enum-member";
  case 15:
    return "field";
  case 17:
    return "function";
  case 21:
    return "interface";
  case 25:
    return "macro";
  case 26:
    return "method";
  case 29:
    return "module";
  case 30:
    return "namespace";
  case 35:
    return "package";
  case 37:
    return "parameter";
  case 41:
    return "property";
  case 49:
    return "struct";
  case 53:
    return "trait";
  case 54:
    return "type";
  case 55:
    return "type-alias";
  case 59:
    return "union";
  case 60:
    return "value";
  case 61:
    return "variable";
  default:
    return "symbol";
  }
}

static const char *display_name_state(const ScipSymbolMetadata *metadata) {
  if (metadata->display_name_conflict)
    return "conflict";
  if (metadata->display_name_from_source)
    return "source-range";
  return metadata->display_name.length ? "provided" : "unavailable";
}

static size_t document_line(const ScipDocument *document, size_t offset) {
  size_t low = 0;
  size_t high = document->line_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (document->line_starts[middle] <= offset)
      low = middle + 1;
    else
      high = middle;
  }
  return low ? low : 1;
}

static ArchbirdStatus analyze_documents(ScipAnalysis *analysis,
                                        const ArchbirdProject *project) {
  size_t document_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (document_index = 0; document_index < analysis->document_count;
       document_index++) {
    ScipDocument *document = &analysis->documents[document_index];
    size_t index;
    if (document->invalid_path)
      continue;
    if ((document_index &&
         ab_string_equal(&document->path,
                         &analysis->documents[document_index - 1].path)) ||
        (document_index + 1 < analysis->document_count &&
         ab_string_equal(&document->path,
                         &analysis->documents[document_index + 1].path))) {
      document->duplicate = 1;
      continue;
    }
    document->mapped =
        manifest_file(analysis->manifest, document->path.data,
                      document->path.length, &document->manifest_index) != NULL;
    if (document->mapped) {
      const AbManifestFile *source =
          &analysis->manifest->files[document->manifest_index];
      const uint8_t *source_bytes =
          ab_project_source_bytes(project, document->manifest_index);
      analysis->documents_mapped++;
      if (!document->has_text)
        analysis->documents_source_unverified++;
      else if (document->text.length == source->byte_length &&
               (!document->text.length ||
                memcmp(document->text.data, source_bytes,
                       document->text.length) == 0))
        analysis->documents_source_verified++;
      else {
        document->source_mismatch = 1;
        analysis->source_mismatches++;
      }
      status = build_line_index(analysis, project, document);
      if (status != ARCHBIRD_OK)
        return status;
    }
    if (!document->position_encoding) {
      if (analysis->spec->position_encoding_fallback)
        analysis->configured_position_documents++;
      else
        analysis->unspecified_position_documents++;
    }
    analysis->occurrences += document->occurrence_count;
    analysis->symbols += document->symbol_count;
    for (index = 0; index < document->occurrence_count; index++) {
      ScipOccurrence *occurrence = &document->occurrences[index];
      size_t start;
      size_t end;
      int exact;
      if (!valid_symbol(occurrence->symbol))
        continue;
      if (!(occurrence->roles & SCIP_DEFINITION_ROLE))
        analysis->references++;
      exact = occurrence_span(analysis, project, document, occurrence, &start,
                              &end);
      if (document->mapped && !exact) {
        analysis->invalid_ranges++;
        document->stale_ranges = 1;
      }
    }
    if (document->stale_ranges || document->source_mismatch)
      analysis->documents_stale++;
    for (index = 0; status == ARCHBIRD_OK && index < document->symbol_count;
         index++) {
      ScipSymbol *symbol = &document->symbols[index];
      size_t relation_index;
      if (!valid_symbol(symbol->symbol)) {
        analysis->invalid_symbols++;
        continue;
      }
      status =
          add_definition(analysis, document_index, symbol->symbol, 0, 0, 0);
      for (relation_index = 0;
           status == ARCHBIRD_OK && relation_index < symbol->relationship_count;
           relation_index++) {
        ScipRelationship *relationship = &symbol->relationships[relation_index];
        analysis->relationships++;
        if (!valid_symbol(relationship->symbol)) {
          analysis->invalid_relationship_symbols++;
          continue;
        }
        if (relationship->flags & (1u << 3))
          status = add_definition(analysis, document_index,
                                  relationship->symbol, 0, 0, 0);
      }
    }
    for (index = 0; status == ARCHBIRD_OK && index < document->occurrence_count;
         index++) {
      ScipOccurrence *occurrence = &document->occurrences[index];
      size_t start = 0;
      size_t end = 0;
      int exact;
      if (!valid_symbol(occurrence->symbol))
        continue;
      if (document->stale_ranges || document->source_mismatch)
        continue;
      exact = occurrence_span(analysis, project, document, occurrence, &start,
                              &end);
      if (!(occurrence->roles & SCIP_DEFINITION_ROLE))
        continue;
      if (exact)
        status = add_definition(analysis, document_index, occurrence->symbol,
                                start, end, 1);
    }
  }
  if (status != ARCHBIRD_OK)
    return status;
  if (analysis->definition_count > 1)
    qsort(analysis->definitions, analysis->definition_count,
          sizeof(*analysis->definitions), definition_compare);
  if (analysis->external_symbol_name_count > 1)
    qsort(analysis->external_symbol_names, analysis->external_symbol_name_count,
          sizeof(*analysis->external_symbol_names), external_symbol_compare);
  return ARCHBIRD_OK;
}

static void hash_u64(ArchbirdSha256Context *context, uint64_t value) {
  uint8_t bytes[8];
  size_t index;
  for (index = 0; index < sizeof(bytes); index++)
    bytes[sizeof(bytes) - index - 1] = (uint8_t)(value >> (index * 8));
  (void)archbird_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_slice(ArchbirdSha256Context *context, const uint8_t *bytes,
                       size_t length) {
  hash_u64(context, length);
  (void)archbird_sha256_update(context, bytes, length);
}

static ArchbirdStatus target_identity(ScipAnalysis *analysis,
                                      size_t document_index, ScipSlice symbol,
                                      AbString *out) {
  ArchbirdSha256Context context;
  uint8_t digest[32];
  char hex[65];
  char value[77];
  const ScipDocument *document = &analysis->documents[document_index];
  archbird_sha256_init(&context);
  hash_slice(&context, (const uint8_t *)analysis->spec->name.data,
             analysis->spec->name.length);
  hash_slice(&context, (const uint8_t *)document->path.data,
             document->path.length);
  hash_slice(&context, symbol.data, symbol.length);
  archbird_sha256_final(&context, digest);
  archbird_sha256_hex(digest, hex);
  memcpy(value, "scip-symbol:", 12);
  memcpy(value + 12, hex, 65);
  return ab_string_copy(analysis->engine, out, value, 76);
}

static ArchbirdStatus external_identity(ScipAnalysis *analysis,
                                        ScipSlice symbol, AbString *out) {
  ArchbirdSha256Context context;
  uint8_t digest[32];
  char hex[65];
  char value[79];
  archbird_sha256_init(&context);
  hash_slice(&context, (const uint8_t *)analysis->spec->name.data,
             analysis->spec->name.length);
  hash_slice(&context, symbol.data, symbol.length);
  archbird_sha256_final(&context, digest);
  archbird_sha256_hex(digest, hex);
  memcpy(value, "scip-external:", 14);
  memcpy(value + 14, hex, 65);
  return ab_string_copy(analysis->engine, out, value, 78);
}

static int string_compare_raw(const void *left_raw, const void *right_raw) {
  return ab_string_compare((const AbString *)left_raw,
                           (const AbString *)right_raw);
}

static ArchbirdStatus resolution_targets(ScipAnalysis *analysis,
                                         size_t definition_start,
                                         size_t definition_end,
                                         AbString **out_targets,
                                         size_t *out_count) {
  AbString *targets = NULL;
  size_t capacity = 0;
  size_t previous = SIZE_MAX;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out_targets = NULL;
  *out_count = 0;
  for (index = definition_start; index < definition_end; index++) {
    const ScipDefinition *definition = &analysis->definitions[index];
    AbString *resized;
    if (!definition->anchored)
      continue;
    if (definition->document_index == previous)
      continue;
    previous = definition->document_index;
    if (*out_count == capacity) {
      size_t next = capacity ? capacity * 2 : 4;
      resized = (AbString *)ab_realloc(analysis->engine, targets,
                                       next * sizeof(*targets));
      if (!resized) {
        status = archbird_error_set(
            analysis->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
            "out of memory collecting SCIP resolution targets");
        break;
      }
      targets = resized;
      memset(targets + capacity, 0, (next - capacity) * sizeof(*targets));
      capacity = next;
    }
    status = target_identity(analysis, definition->document_index,
                             definition->symbol, &targets[*out_count]);
    if (status != ARCHBIRD_OK)
      break;
    (*out_count)++;
  }
  if (status == ARCHBIRD_OK && *out_count > 1)
    qsort(targets, *out_count, sizeof(*targets), string_compare_raw);
  if (status != ARCHBIRD_OK) {
    for (index = 0; index < *out_count; index++)
      ab_string_free(analysis->engine, &targets[index]);
    ab_free(analysis->engine, targets);
    *out_count = 0;
    return status;
  }
  *out_targets = targets;
  return ARCHBIRD_OK;
}

static void resolution_targets_free(ScipAnalysis *analysis, AbString *targets,
                                    size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    ab_string_free(analysis->engine, &targets[index]);
  ab_free(analysis->engine, targets);
}

static ArchbirdStatus append_key_part(AbBuffer *key, const uint8_t *bytes,
                                      size_t length) {
  static const uint8_t separator = 0;
  ArchbirdStatus status = ab_buffer_append(key, bytes, length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(key, &separator, 1);
  return status;
}

static ArchbirdStatus add_index_attributes(ScipAnalysis *analysis,
                                           AbFact *fact) {
  ArchbirdStatus status = ab_fact_add_string_attribute(
      analysis->engine, fact, "index",
      (const uint8_t *)analysis->spec->name.data, analysis->spec->name.length);
  if (status == ARCHBIRD_OK && analysis->spec->variant.length)
    status = ab_fact_add_string_attribute(
        analysis->engine, fact, "variant",
        (const uint8_t *)analysis->spec->variant.data,
        analysis->spec->variant.length);
  return status;
}

static int document_evidence_current(const ScipDocument *document) {
  return document->mapped && document->has_text && !document->source_mismatch &&
         !document->stale_ranges;
}

static const char *resolution_evidence_state(const ScipAnalysis *analysis,
                                             size_t source_document,
                                             size_t definition_start,
                                             size_t definition_end) {
  size_t index;
  if (!document_evidence_current(&analysis->documents[source_document]))
    return "unknown";
  for (index = definition_start; index < definition_end; index++) {
    const ScipDefinition *definition = &analysis->definitions[index];
    if (definition->anchored &&
        !document_evidence_current(
            &analysis->documents[definition->document_index]))
      return "unknown";
  }
  return "current";
}

static ArchbirdStatus add_evidence_state(ScipAnalysis *analysis, AbFact *fact,
                                         const char *state) {
  return ab_fact_add_string_attribute(analysis->engine, fact, "evidence_state",
                                      (const uint8_t *)state, strlen(state));
}

static ArchbirdStatus
add_semantic_identity_attributes(ScipAnalysis *analysis, AbFact *fact,
                                 ScipSlice identity,
                                 const ScipSymbolMetadata *metadata) {
  const char *name_state = display_name_state(metadata);
  ArchbirdStatus status =
      ab_fact_add_string_attribute(analysis->engine, fact, "semantic_symbol",
                                   identity.data, identity.length);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(
        analysis->engine, fact, "display_name_state",
        (const uint8_t *)name_state, strlen(name_state));
  if (status == ARCHBIRD_OK && metadata->kind)
    status = ab_fact_add_u64_attribute(analysis->engine, fact, "symbol_kind",
                                       metadata->kind);
  if (status == ARCHBIRD_OK && metadata->kind_conflict)
    status = ab_fact_add_string_attribute(
        analysis->engine, fact, "symbol_kind_state",
        (const uint8_t *)"conflict", sizeof("conflict") - 1);
  return status;
}

static ArchbirdStatus
add_index_diagnostic_fact(ScipAnalysis *analysis, const char *severity,
                          const char *code, const char *message,
                          const AbString *diagnostic_path) {
  AbBuffer key;
  AbBuffer full_code;
  AbBuffer full_message;
  AbFact *fact = NULL;
  char ordinal[32];
  int ordinal_length;
  ArchbirdStatus status;
  ab_buffer_init(&key, analysis->engine);
  ab_buffer_init(&full_code, analysis->engine);
  ab_buffer_init(&full_message, analysis->engine);
  ordinal_length =
      snprintf(ordinal, sizeof(ordinal), "%zu", analysis->diagnostic_ordinal++);
  status =
      ordinal_length < 0 || (size_t)ordinal_length >= sizeof(ordinal)
          ? ARCHBIRD_LIMIT_EXCEEDED
          : append_key_part(&key, (const uint8_t *)analysis->spec->name.data,
                            analysis->spec->name.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&key, ordinal, (size_t)ordinal_length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&full_code, "scip-");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&full_code, code);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&full_message, analysis->spec->name.data,
                              analysis->spec->name.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&full_message, ": ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&full_message, message);
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_add_fact_at(
        &analysis->builder, analysis->index_file, "index-diagnostics",
        "diagnostic", "compiler-index", 0, 0, key.data, key.length, NULL, 0,
        &fact);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(analysis->engine, fact, "code",
                                          full_code.data, full_code.length);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(
        analysis->engine, fact, "diagnostic_path",
        (const uint8_t *)diagnostic_path->data, diagnostic_path->length);
  if (status == ARCHBIRD_OK)
    status = add_index_attributes(analysis, fact);
  if (status == ARCHBIRD_OK)
    status =
        ab_fact_add_string_attribute(analysis->engine, fact, "message",
                                     full_message.data, full_message.length);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(analysis->engine, fact, "severity",
                                          (const uint8_t *)severity,
                                          strlen(severity));
  ab_buffer_free(&full_message);
  ab_buffer_free(&full_code);
  ab_buffer_free(&key);
  return status;
}

static ArchbirdStatus add_symbol_fact(ScipAnalysis *analysis,
                                      size_t document_index,
                                      const ScipSymbol *symbol) {
  const ScipDocument *document = &analysis->documents[document_index];
  ScipSymbolMetadata metadata;
  ScipSlice name;
  const AbManifestFile *file;
  AbBuffer key;
  AbFact *fact = NULL;
  ArchbirdStatus status;
  if (!document->mapped || !valid_symbol(symbol->symbol))
    return ARCHBIRD_OK;
  metadata = document_symbol_metadata(document, symbol->symbol);
  name = metadata.display_name.length ? metadata.display_name : symbol->symbol;
  file = &analysis->manifest->files[document->manifest_index];
  ab_buffer_init(&key, analysis->engine);
  status = append_key_part(&key, (const uint8_t *)analysis->spec->name.data,
                           analysis->spec->name.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&key, symbol->symbol.data, symbol->symbol.length);
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_add_fact_at(
        &analysis->builder, file, "semantic-symbols", symbol_kind(symbol->kind),
        "semantic-identity", 0, 0, key.data, key.length, name.data, name.length,
        &fact);
  if (status == ARCHBIRD_OK)
    status = add_index_attributes(analysis, fact);
  if (status == ARCHBIRD_OK)
    status = add_evidence_state(
        analysis, fact,
        document_evidence_current(document) ? "current" : "unknown");
  if (status == ARCHBIRD_OK)
    status = add_semantic_identity_attributes(analysis, fact, symbol->symbol,
                                              &metadata);
  if (status == ARCHBIRD_OK && metadata.display_name.length)
    status = ab_fact_add_string_attribute(
        analysis->engine, fact, "display_name", metadata.display_name.data,
        metadata.display_name.length);
  if (status == ARCHBIRD_OK && symbol->signature.length)
    status = ab_fact_add_string_attribute(analysis->engine, fact, "signature",
                                          symbol->signature.data,
                                          symbol->signature.length);
  if (status == ARCHBIRD_OK && symbol->enclosing_symbol.length)
    status = ab_fact_add_string_attribute(
        analysis->engine, fact, "enclosing_symbol",
        symbol->enclosing_symbol.data, symbol->enclosing_symbol.length);
  ab_buffer_free(&key);
  return status;
}

static ArchbirdStatus add_projected_symbol_fact(
    ScipAnalysis *analysis, size_t document_index, ScipSlice identity,
    const ScipSymbolMetadata *metadata, size_t start, size_t end) {
  const ScipDocument *document = &analysis->documents[document_index];
  const AbManifestFile *file =
      &analysis->manifest->files[document->manifest_index];
  AbBuffer key;
  AbFact *fact = NULL;
  ArchbirdStatus status;
  /* A source-range name is safe for presentation and edge labels, but it does
     not prove that one semantic identity owns the source span. Macro expansion
     can attach several SCIP identities to the same token. Only producer-owned
     display metadata may introduce a generic symbol fact; source-range names
     remain on the keyed semantic fact. */
  if (!metadata->display_name.length || metadata->display_name_conflict ||
      metadata->display_name_from_source)
    return ARCHBIRD_OK;
  ab_buffer_init(&key, analysis->engine);
  status = append_key_part(&key, (const uint8_t *)analysis->spec->name.data,
                           analysis->spec->name.length);
  if (status == ARCHBIRD_OK)
    status = append_key_part(&key, identity.data, identity.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&key, start);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&key, ":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&key, end);
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_add_fact_at(
        &analysis->builder, file, "symbols", symbol_kind(metadata->kind),
        "semantic-identity", start, end, key.data, key.length,
        metadata->display_name.data, metadata->display_name.length, &fact);
  if (status == ARCHBIRD_OK)
    status = add_index_attributes(analysis, fact);
  if (status == ARCHBIRD_OK)
    status = add_evidence_state(
        analysis, fact,
        document_evidence_current(document) ? "current" : "unknown");
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_u64_attribute(analysis->engine, fact, "line",
                                       document_line(document, start));
  if (status == ARCHBIRD_OK)
    status =
        add_semantic_identity_attributes(analysis, fact, identity, metadata);
  ab_buffer_free(&key);
  return status;
}

static ArchbirdStatus
add_occurrence_fact(ScipAnalysis *analysis, size_t document_index,
                    const ScipOccurrence *occurrence, int definition,
                    size_t definition_start, size_t definition_end,
                    size_t start, size_t end, size_t occurrence_count,
                    const char *range_state, int syntax_kind_mixed) {
  const ScipDocument *document = &analysis->documents[document_index];
  const AbManifestFile *file;
  AbBuffer key;
  AbFact *fact = NULL;
  AbString *targets = NULL;
  size_t target_count = 0;
  size_t target_document = SIZE_MAX;
  size_t definition_document_count = 0;
  ScipSymbolMetadata metadata;
  ScipSlice name;
  const char *state;
  const char *kind;
  ArchbirdStatus status;
  if (!document->mapped || !valid_symbol(occurrence->symbol))
    return ARCHBIRD_OK;
  memset(&metadata, 0, sizeof(metadata));
  if (definition) {
    metadata = document_symbol_metadata(document, occurrence->symbol);
    add_definition_source_name(analysis, definition_start, definition_end,
                               &metadata);
  } else {
    definition_document_count = unique_definition_documents(
        analysis, definition_start, definition_end, &target_document);
    if (definition_document_count == 1)
      metadata = document_symbol_metadata(&analysis->documents[target_document],
                                          occurrence->symbol);
    add_definition_source_name(analysis, definition_start, definition_end,
                               &metadata);
  }
  name =
      metadata.display_name.length ? metadata.display_name : occurrence->symbol;
  file = &analysis->manifest->files[document->manifest_index];
  ab_buffer_init(&key, analysis->engine);
  status = append_key_part(&key, (const uint8_t *)analysis->spec->name.data,
                           analysis->spec->name.length);
  if (status == ARCHBIRD_OK)
    status = append_key_part(&key, occurrence->symbol.data,
                             occurrence->symbol.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&key, start);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&key, ":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&key, end);
  kind = definition
             ? "definition"
             : (occurrence->roles & SCIP_IMPORT_ROLE ? "import" : "reference");
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_add_fact_at(
        &analysis->builder, file,
        definition ? "semantic-definitions" : "reference-targets", kind,
        definition ? "semantic-identity" : "semantic-target", start, end,
        key.data, key.length, name.data, name.length, &fact);
  /* Compact references use only one representative range for a document/target
     group. SCIP can also attach several semantic identities to one exact range
     (for example, macro-expanded locals). These protocol facts therefore keep
     key-qualified identity. The separately projected, explicitly display-named
     source symbol is the span-correlated enrichment surface. */
  if (status == ARCHBIRD_OK)
    ab_fact_set_keyed_correlation(fact);
  if (status == ARCHBIRD_OK)
    status = add_index_attributes(analysis, fact);
  if (status == ARCHBIRD_OK)
    status = add_evidence_state(
        analysis, fact,
        definition
            ? (document_evidence_current(document) ? "current" : "unknown")
            : resolution_evidence_state(analysis, document_index,
                                        definition_start, definition_end));
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_u64_attribute(analysis->engine, fact, "roles",
                                       occurrence->roles);
  if (status == ARCHBIRD_OK && !syntax_kind_mixed)
    status = ab_fact_add_u64_attribute(analysis->engine, fact, "syntax_kind",
                                       occurrence->syntax_kind);
  if (status == ARCHBIRD_OK && syntax_kind_mixed)
    status = ab_fact_add_string_attribute(
        analysis->engine, fact, "syntax_kind_state", (const uint8_t *)"mixed",
        sizeof("mixed") - 1);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_u64_attribute(analysis->engine, fact,
                                       "occurrence_count", occurrence_count);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(analysis->engine, fact, "range_state",
                                          (const uint8_t *)range_state,
                                          strlen(range_state));
  if (status == ARCHBIRD_OK)
    status = add_semantic_identity_attributes(analysis, fact,
                                              occurrence->symbol, &metadata);
  if (status == ARCHBIRD_OK && definition)
    status = add_projected_symbol_fact(
        analysis, document_index, occurrence->symbol, &metadata, start, end);
  if (status != ARCHBIRD_OK || definition)
    goto done;
  status = resolution_targets(analysis, definition_start, definition_end,
                              &targets, &target_count);
  if (status != ARCHBIRD_OK)
    goto done;
  if (!target_count && external_symbol(analysis, occurrence->symbol)) {
    targets = (AbString *)ab_calloc(analysis->engine, 1, sizeof(*targets));
    if (!targets) {
      status = archbird_error_set(analysis->engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory storing external SCIP target");
      goto done;
    }
    target_count = 1;
    status = external_identity(analysis, occurrence->symbol, targets);
    if (status != ARCHBIRD_OK)
      goto done;
    state = "external";
  } else if (target_count == 1)
    state = "unique";
  else if (target_count > 1)
    state = "ambiguous";
  else
    state = "unresolved";
  status = ab_fact_set_resolution(analysis->engine, fact, state, targets,
                                  target_count, NULL);
  if (status == ARCHBIRD_OK && strcmp(state, "unique") == 0) {
    size_t definition =
        first_anchored_definition(analysis, definition_start, definition_end);
    size_t target_document = analysis->definitions[definition].document_index;
    const ScipDocument *target = &analysis->documents[target_document];
    status = ab_fact_add_string_attribute(analysis->engine, fact, "target_path",
                                          (const uint8_t *)target->path.data,
                                          target->path.length);
    if (status == ARCHBIRD_OK)
      status = ab_fact_add_string_attribute(
          analysis->engine, fact, "target_symbol", name.data, name.length);
    if (status == ARCHBIRD_OK)
      status = ab_fact_add_string_attribute(
          analysis->engine, fact, "target_semantic_symbol",
          occurrence->symbol.data, occurrence->symbol.length);
    if (status == ARCHBIRD_OK)
      status =
          ab_fact_add_u64_attribute(analysis->engine, fact, "target_span_start",
                                    analysis->definitions[definition].start);
    if (status == ARCHBIRD_OK)
      status =
          ab_fact_add_u64_attribute(analysis->engine, fact, "target_span_end",
                                    analysis->definitions[definition].end);
  }
done:
  resolution_targets_free(analysis, targets, target_count);
  ab_buffer_free(&key);
  return status;
}

static ArchbirdStatus
add_relationship_fact(ScipAnalysis *analysis, size_t document_index,
                      ScipSlice source_symbol, ScipSlice target_symbol,
                      const char *kind, size_t definition_start,
                      size_t definition_end) {
  const ScipDocument *document = &analysis->documents[document_index];
  const AbManifestFile *file;
  AbBuffer key;
  AbFact *fact = NULL;
  AbString *targets = NULL;
  size_t target_count = 0;
  size_t target_document = SIZE_MAX;
  size_t definition_document_count;
  ScipSymbolMetadata source_metadata;
  ScipSymbolMetadata target_metadata;
  ScipSlice source_name;
  ScipSlice target_name;
  const char *state;
  ArchbirdStatus status;
  if (!document->mapped)
    return ARCHBIRD_OK;
  source_metadata = document_symbol_metadata(document, source_symbol);
  {
    size_t source_start;
    size_t source_end;
    definition_range(analysis, document_index, source_symbol, &source_start,
                     &source_end);
    add_definition_source_name(analysis, source_start, source_end,
                               &source_metadata);
  }
  memset(&target_metadata, 0, sizeof(target_metadata));
  definition_document_count = unique_definition_documents(
      analysis, definition_start, definition_end, &target_document);
  if (definition_document_count == 1)
    target_metadata = document_symbol_metadata(
        &analysis->documents[target_document], target_symbol);
  add_definition_source_name(analysis, definition_start, definition_end,
                             &target_metadata);
  source_name = source_metadata.display_name.length
                    ? source_metadata.display_name
                    : source_symbol;
  target_name = target_metadata.display_name.length
                    ? target_metadata.display_name
                    : target_symbol;
  file = &analysis->manifest->files[document->manifest_index];
  ab_buffer_init(&key, analysis->engine);
  status = append_key_part(&key, (const uint8_t *)analysis->spec->name.data,
                           analysis->spec->name.length);
  if (status == ARCHBIRD_OK)
    status = append_key_part(&key, source_symbol.data, source_symbol.length);
  if (status == ARCHBIRD_OK)
    status = append_key_part(&key, target_symbol.data, target_symbol.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&key, kind);
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_add_fact_at(
        &analysis->builder, file, "semantic-relationships", kind,
        "semantic-target", 0, 0, key.data, key.length, source_name.data,
        source_name.length, &fact);
  if (status == ARCHBIRD_OK)
    status = add_index_attributes(analysis, fact);
  if (status == ARCHBIRD_OK)
    status = add_evidence_state(
        analysis, fact,
        resolution_evidence_state(analysis, document_index, definition_start,
                                  definition_end));
  if (status == ARCHBIRD_OK)
    status = add_semantic_identity_attributes(analysis, fact, source_symbol,
                                              &source_metadata);
  if (status == ARCHBIRD_OK)
    status =
        ab_fact_add_string_attribute(analysis->engine, fact, "target_symbol",
                                     target_name.data, target_name.length);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(
        analysis->engine, fact, "target_semantic_symbol", target_symbol.data,
        target_symbol.length);
  if (status == ARCHBIRD_OK) {
    const char *state = display_name_state(&target_metadata);
    status = ab_fact_add_string_attribute(
        analysis->engine, fact, "target_display_name_state",
        (const uint8_t *)state, strlen(state));
  }
  if (status == ARCHBIRD_OK)
    status = resolution_targets(analysis, definition_start, definition_end,
                                &targets, &target_count);
  if (status != ARCHBIRD_OK)
    goto done;
  if (!target_count && external_symbol(analysis, target_symbol)) {
    targets = (AbString *)ab_calloc(analysis->engine, 1, sizeof(*targets));
    if (!targets) {
      status = archbird_error_set(
          analysis->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
          "out of memory storing external SCIP relationship target");
      goto done;
    }
    target_count = 1;
    status = external_identity(analysis, target_symbol, targets);
    if (status != ARCHBIRD_OK)
      goto done;
    state = "external";
  } else if (target_count == 1)
    state = "unique";
  else if (target_count > 1)
    state = "ambiguous";
  else
    state = "unresolved";
  status = ab_fact_set_resolution(analysis->engine, fact, state, targets,
                                  target_count, NULL);
  if (status == ARCHBIRD_OK && strcmp(state, "unique") == 0) {
    size_t definition =
        first_anchored_definition(analysis, definition_start, definition_end);
    size_t target_document = analysis->definitions[definition].document_index;
    const ScipDocument *target = &analysis->documents[target_document];
    status = ab_fact_add_string_attribute(analysis->engine, fact, "target_path",
                                          (const uint8_t *)target->path.data,
                                          target->path.length);
    if (status == ARCHBIRD_OK)
      status =
          ab_fact_add_u64_attribute(analysis->engine, fact, "target_span_start",
                                    analysis->definitions[definition].start);
    if (status == ARCHBIRD_OK)
      status =
          ab_fact_add_u64_attribute(analysis->engine, fact, "target_span_end",
                                    analysis->definitions[definition].end);
  }
done:
  resolution_targets_free(analysis, targets, target_count);
  ab_buffer_free(&key);
  return status;
}

static ArchbirdStatus process_references(ScipAnalysis *analysis,
                                         const ArchbirdProject *project) {
  static const struct {
    unsigned flag;
    const char *kind;
  } relationship_kinds[] = {{1u << 0, "scip-related-reference"},
                            {1u << 1, "scip-implementation"},
                            {1u << 2, "scip-type-definition"},
                            {1u << 3, "scip-definition"}};
  size_t document_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (document_index = 0;
       status == ARCHBIRD_OK && document_index < analysis->document_count;
       document_index++) {
    ScipDocument *document = &analysis->documents[document_index];
    size_t index;
    if (document->duplicate || document->stale_ranges ||
        document->source_mismatch)
      continue;
    for (index = 0; status == ARCHBIRD_OK && index < document->symbol_count;
         index++) {
      ScipSymbol *symbol = &document->symbols[index];
      size_t relationship_index;
      if (!valid_symbol(symbol->symbol))
        continue;
      status = add_symbol_fact(analysis, document_index, symbol);
      for (relationship_index = 0;
           status == ARCHBIRD_OK &&
           relationship_index < symbol->relationship_count;
           relationship_index++) {
        ScipRelationship *relationship =
            &symbol->relationships[relationship_index];
        size_t definition_start;
        size_t definition_end;
        size_t target_document = SIZE_MAX;
        size_t target_count;
        size_t source_start;
        size_t source_end;
        int source_anchored;
        size_t kind_index;
        if (!valid_symbol(relationship->symbol))
          continue;
        definition_range(analysis, document_index, relationship->symbol,
                         &definition_start, &definition_end);
        definition_range(analysis, document_index, symbol->symbol,
                         &source_start, &source_end);
        source_anchored = first_anchored_definition(analysis, source_start,
                                                    source_end) != SIZE_MAX;
        target_count = unique_definition_documents(
            analysis, definition_start, definition_end, &target_document);
        for (kind_index = 0; status == ARCHBIRD_OK &&
                             kind_index < sizeof(relationship_kinds) /
                                              sizeof(relationship_kinds[0]);
             kind_index++) {
          if (!(relationship->flags & relationship_kinds[kind_index].flag))
            continue;
          status = add_relationship_fact(analysis, document_index,
                                         symbol->symbol, relationship->symbol,
                                         relationship_kinds[kind_index].kind,
                                         definition_start, definition_end);
          if (status == ARCHBIRD_OK && source_anchored && target_count == 1 &&
              document->mapped && analysis->documents[target_document].mapped &&
              target_document != document_index) {
            status = add_link(analysis, relationship_kinds[kind_index].kind,
                              document_index, target_document, symbol->symbol);
            if (status == ARCHBIRD_OK)
              analysis->relationship_edges++;
          }
        }
      }
    }
    for (index = 0; status == ARCHBIRD_OK && index < document->occurrence_count;
         index++) {
      ScipOccurrence *occurrence = &document->occurrences[index];
      size_t definition_start;
      size_t definition_end;
      size_t target_document = SIZE_MAX;
      size_t target_count;
      size_t span_start;
      size_t span_end;
      int exact;
      if (!valid_symbol(occurrence->symbol))
        continue;
      definition_range(analysis, document_index, occurrence->symbol,
                       &definition_start, &definition_end);
      if (occurrence->roles & SCIP_DEFINITION_ROLE) {
        exact = occurrence_span(analysis, project, document, occurrence,
                                &span_start, &span_end);
        if (exact)
          status = add_occurrence_fact(analysis, document_index, occurrence, 1,
                                       definition_start, definition_end,
                                       span_start, span_end, 1, "exact", 0);
        continue;
      }
      exact = occurrence_span(analysis, project, document, occurrence,
                              &span_start, &span_end);
      if (!exact)
        continue;
      target_count = unique_definition_documents(
          analysis, definition_start, definition_end, &target_document);
      if (target_count == 1)
        analysis->resolved_unique++;
      else if (target_count > 1)
        analysis->resolved_ambiguous++;
      else
        analysis->unresolved++;
      status = add_reference_group(analysis, document_index, occurrence,
                                   span_start, span_end);
    }
  }
  if (status == ARCHBIRD_OK)
    status = normalize_reference_groups(analysis);
  for (document_index = 0; status == ARCHBIRD_OK &&
                           document_index < analysis->reference_group_count;
       document_index++) {
    const ScipReferenceGroup *group =
        &analysis->reference_groups[document_index];
    ScipOccurrence occurrence;
    size_t definition_start;
    size_t definition_end;
    size_t target_document = SIZE_MAX;
    size_t target_count;
    memset(&occurrence, 0, sizeof(occurrence));
    occurrence.symbol = group->symbol;
    occurrence.roles = group->roles;
    occurrence.syntax_kind = group->syntax_kind;
    definition_range(analysis, group->document_index, group->symbol,
                     &definition_start, &definition_end);
    target_count = unique_definition_documents(
        analysis, definition_start, definition_end, &target_document);
    status = add_occurrence_fact(
        analysis, group->document_index, &occurrence, 0, definition_start,
        definition_end, group->start, group->end, group->occurrence_count,
        "representative", group->syntax_kind_mixed);
    if (status == ARCHBIRD_OK && target_count == 1 &&
        analysis->documents[group->document_index].mapped &&
        analysis->documents[target_document].mapped &&
        target_document != group->document_index)
      status = add_link(analysis,
                        group->roles & SCIP_IMPORT_ROLE ? "scip-import"
                                                        : "scip-reference",
                        group->document_index, target_document, group->symbol);
  }
  return status;
}

static size_t normalize_links(ScipAnalysis *analysis) {
  size_t read_index;
  size_t write_index = 0;
  size_t edge_count = 0;
  if (analysis->link_count > 1)
    qsort(analysis->links, analysis->link_count, sizeof(*analysis->links),
          link_compare);
  for (read_index = 0; read_index < analysis->link_count; read_index++) {
    if (write_index && link_equal(&analysis->links[write_index - 1],
                                  &analysis->links[read_index]))
      continue;
    if (write_index != read_index)
      analysis->links[write_index] = analysis->links[read_index];
    if (!write_index || !same_edge(&analysis->links[write_index - 1],
                                   &analysis->links[write_index]))
      edge_count++;
    write_index++;
  }
  analysis->link_count = write_index;
  return edge_count;
}

static ArchbirdStatus add_summary_fact(ScipAnalysis *analysis,
                                       size_t edge_count,
                                       const char *evidence_state) {
  AbFact *fact = NULL;
  char sha256[65];
  ArchbirdStatus status;
  archbird_sha256_hex(analysis->index_file->sha256, sha256);
  status = ab_bundle_builder_add_fact_at(
      &analysis->builder, analysis->index_file, "index-summaries", "summary",
      "compiler-index", 0, 0, (const uint8_t *)analysis->spec->name.data,
      analysis->spec->name.length, (const uint8_t *)analysis->spec->name.data,
      analysis->spec->name.length, &fact);
#define ADD_STRING(name, value, length)                                        \
  do {                                                                         \
    if (status == ARCHBIRD_OK)                                                 \
      status = ab_fact_add_string_attribute(analysis->engine, fact, name,      \
                                            (const uint8_t *)(value), length); \
  } while (0)
#define ADD_COUNT(name, value)                                                 \
  do {                                                                         \
    if (status == ARCHBIRD_OK)                                                 \
      status = ab_fact_add_u64_attribute(analysis->engine, fact, name, value); \
  } while (0)
  ADD_COUNT("documents_mapped", analysis->documents_mapped);
  ADD_COUNT("documents_stale", analysis->documents_stale);
  ADD_COUNT("documents_source_unverified",
            analysis->documents_source_unverified);
  ADD_COUNT("documents_source_verified", analysis->documents_source_verified);
  ADD_COUNT("documents_total", analysis->document_count);
  ADD_COUNT("edge_count", edge_count);
  ADD_STRING("evidence_state", evidence_state, strlen(evidence_state));
  ADD_STRING("format", analysis->spec->format.data,
             analysis->spec->format.length);
  ADD_STRING("index_path", analysis->spec->path.data,
             analysis->spec->path.length);
  ADD_COUNT("occurrences", analysis->occurrences);
  ADD_COUNT("invalid_ranges", analysis->invalid_ranges);
  ADD_COUNT("position_encoding_fallback_documents",
            analysis->configured_position_documents);
  ADD_STRING("path_prefix", analysis->spec->path_prefix.data,
             analysis->spec->path_prefix.length);
  ADD_STRING("variant", analysis->spec->variant.data,
             analysis->spec->variant.length);
  ADD_COUNT("references", analysis->references);
  ADD_COUNT("reference_facts", analysis->reference_facts);
  ADD_COUNT("relationship_edges", analysis->relationship_edges);
  ADD_COUNT("relationships", analysis->relationships);
  ADD_COUNT("resolved_ambiguous", analysis->resolved_ambiguous);
  ADD_COUNT("resolved_unique", analysis->resolved_unique);
  ADD_STRING("sha256", sha256, 64);
  ADD_COUNT("symbols", analysis->symbols);
  ADD_COUNT("source_mismatches", analysis->source_mismatches);
  ADD_STRING("tool_name", analysis->metadata.tool_name.data,
             analysis->metadata.tool_name.length);
  ADD_STRING("tool_version", analysis->metadata.tool_version.data,
             analysis->metadata.tool_version.length);
  ADD_COUNT("unresolved", analysis->unresolved);
  ADD_COUNT("external_symbols", analysis->external_symbols);
  ADD_COUNT("protocol_version", analysis->metadata.protocol_version);
  ADD_COUNT("text_encoding", analysis->metadata.text_encoding);
  ADD_COUNT("tool_argument_count", analysis->metadata.tool_argument_count);
#undef ADD_COUNT
#undef ADD_STRING
  return status;
}

static const char *analysis_evidence_state(const ScipAnalysis *analysis,
                                           int parsed) {
  size_t index;
  if (!parsed || !analysis->metadata.tool_name.length ||
      analysis->metadata.text_encoding > 1 ||
      analysis->unspecified_position_documents ||
      analysis->invalid_document_paths || analysis->invalid_symbols ||
      analysis->invalid_relationship_symbols)
    return "unknown";
  for (index = 0; index < analysis->document_count; index++)
    if (analysis->documents[index].duplicate)
      return "unknown";
  if (analysis->invalid_ranges)
    return "stale";
  if (analysis->source_mismatches)
    return "stale";
  if (!analysis->documents_mapped ||
      analysis->documents_source_verified != analysis->documents_mapped)
    return "unknown";
  return "current";
}

static ArchbirdStatus add_analysis_diagnostics(ScipAnalysis *analysis) {
  ArchbirdStatus status = ARCHBIRD_OK;
  size_t index;
  char message[160];
  if (!analysis->metadata.tool_name.length)
    status = add_index_diagnostic_fact(analysis, "warning", "missing-tool-name",
                                       "indexer tool name is empty",
                                       &analysis->spec->path);
  if (status == ARCHBIRD_OK && analysis->invalid_document_paths) {
    int length = snprintf(
        message, sizeof(message),
        "%zu document path(s) are not canonical and repository-relative",
        analysis->invalid_document_paths);
    if (length < 0 || (size_t)length >= sizeof(message))
      status = ARCHBIRD_LIMIT_EXCEEDED;
    else
      status =
          add_index_diagnostic_fact(analysis, "error", "invalid-document-path",
                                    message, &analysis->spec->path);
  }
  if (status == ARCHBIRD_OK && analysis->metadata.text_encoding > 1)
    status = add_index_diagnostic_fact(
        analysis, "warning", "unsupported-text-encoding",
        "source bytes are not declared UTF-8; occurrence spans are unknown",
        &analysis->spec->path);
  for (index = 0; status == ARCHBIRD_OK && index < analysis->document_count;
       index++) {
    const ScipDocument *document = &analysis->documents[index];
    if (!document->duplicate ||
        (index && ab_string_equal(&document->path,
                                  &analysis->documents[index - 1].path)))
      continue;
    status = add_index_diagnostic_fact(analysis, "error", "duplicate-document",
                                       "document path appears more than once",
                                       &document->path);
  }
  if (status == ARCHBIRD_OK && analysis->unspecified_position_documents) {
    int length = snprintf(message, sizeof(message),
                          "%zu document(s) omit the position encoding; "
                          "occurrence spans are unknown; review the producer "
                          "and configure indexes[].position_encoding_fallback",
                          analysis->unspecified_position_documents);
    if (length < 0 || (size_t)length >= sizeof(message))
      status = ARCHBIRD_LIMIT_EXCEEDED;
    else
      status = add_index_diagnostic_fact(analysis, "warning",
                                         "unspecified-position-encoding",
                                         message, &analysis->spec->path);
  }
  if (status == ARCHBIRD_OK && analysis->configured_position_documents) {
    static const char *const names[] = {"", "utf8", "utf16", "utf32"};
    int length = snprintf(
        message, sizeof(message),
        "%zu document(s) use the reviewed %s position-encoding fallback",
        analysis->configured_position_documents,
        names[analysis->spec->position_encoding_fallback]);
    if (length < 0 || (size_t)length >= sizeof(message))
      status = ARCHBIRD_LIMIT_EXCEEDED;
    else
      status = add_index_diagnostic_fact(analysis, "info",
                                         "configured-position-encoding",
                                         message, &analysis->spec->path);
  }
  if (status == ARCHBIRD_OK && analysis->invalid_ranges) {
    int length =
        snprintf(message, sizeof(message),
                 "%zu occurrence range(s) do not map to supplied source bytes",
                 analysis->invalid_ranges);
    if (length < 0 || (size_t)length >= sizeof(message))
      status = ARCHBIRD_LIMIT_EXCEEDED;
    else
      status = add_index_diagnostic_fact(analysis, "warning", "invalid-range",
                                         message, &analysis->spec->path);
  }
  if (status == ARCHBIRD_OK && analysis->source_mismatches) {
    int length = snprintf(message, sizeof(message),
                          "%zu embedded document text value(s) do not match "
                          "supplied source bytes",
                          analysis->source_mismatches);
    if (length < 0 || (size_t)length >= sizeof(message))
      status = ARCHBIRD_LIMIT_EXCEEDED;
    else
      status = add_index_diagnostic_fact(analysis, "warning", "source-mismatch",
                                         message, &analysis->spec->path);
  }
  if (status == ARCHBIRD_OK && analysis->documents_source_unverified) {
    int length = snprintf(message, sizeof(message),
                          "%zu mapped document(s) omit embedded source text; "
                          "index freshness is unknown",
                          analysis->documents_source_unverified);
    if (length < 0 || (size_t)length >= sizeof(message))
      status = ARCHBIRD_LIMIT_EXCEEDED;
    else
      status =
          add_index_diagnostic_fact(analysis, "warning", "source-unverified",
                                    message, &analysis->spec->path);
  }
  if (status == ARCHBIRD_OK && analysis->invalid_symbols) {
    int length = snprintf(message, sizeof(message),
                          "%zu symbol(s) are empty or exceed %zu bytes",
                          analysis->invalid_symbols, SCIP_MAX_SYMBOL_BYTES);
    if (length < 0 || (size_t)length >= sizeof(message))
      status = ARCHBIRD_LIMIT_EXCEEDED;
    else
      status = add_index_diagnostic_fact(analysis, "error", "invalid-symbol",
                                         message, &analysis->spec->path);
  }
  if (status == ARCHBIRD_OK && analysis->invalid_relationship_symbols) {
    int length =
        snprintf(message, sizeof(message),
                 "%zu relationship symbol(s) are empty or exceed %zu bytes",
                 analysis->invalid_relationship_symbols, SCIP_MAX_SYMBOL_BYTES);
    if (length < 0 || (size_t)length >= sizeof(message))
      status = ARCHBIRD_LIMIT_EXCEEDED;
    else
      status = add_index_diagnostic_fact(analysis, "error",
                                         "invalid-relationship-symbol", message,
                                         &analysis->spec->path);
  }
  return status;
}

static unsigned hex_nibble(char value) {
  if (value >= '0' && value <= '9')
    return (unsigned)(value - '0');
  return (unsigned)(value - 'a' + 10);
}

static void decode_digest(const char value[65], uint8_t out[32]) {
  size_t index;
  for (index = 0; index < 32; index++)
    out[index] = (uint8_t)((hex_nibble(value[index * 2]) << 4) |
                           hex_nibble(value[index * 2 + 1]));
}

static void configuration_digest(const AbConfigIndex *spec, uint8_t out[32]) {
  ArchbirdSha256Context context;
  uint8_t policy[2];
  archbird_sha256_init(&context);
  hash_slice(&context, (const uint8_t *)"scip", 4);
  hash_slice(&context, (const uint8_t *)spec->name.data, spec->name.length);
  hash_slice(&context, (const uint8_t *)spec->path.data, spec->path.length);
  hash_slice(&context, (const uint8_t *)spec->path_prefix.data,
             spec->path_prefix.length);
  hash_slice(&context, (const uint8_t *)spec->variant.data,
             spec->variant.length);
  policy[0] = (uint8_t)spec->position_encoding_fallback;
  policy[1] = (uint8_t)(spec->required != 0);
  hash_slice(&context, policy, sizeof(policy));
  archbird_sha256_final(&context, out);
}

static void analysis_free(ScipAnalysis *analysis) {
  size_t index;
  ab_bundle_builder_abort(&analysis->builder);
  for (index = 0; index < analysis->document_count; index++)
    document_free(analysis, &analysis->documents[index]);
  ab_free(analysis->engine, analysis->documents);
  ab_free(analysis->engine, analysis->definitions);
  ab_free(analysis->engine, analysis->links);
  ab_free(analysis->engine, analysis->reference_groups);
  ab_free(analysis->engine, analysis->external_symbol_names);
  memset(analysis, 0, sizeof(*analysis));
}

static ArchbirdStatus init_analysis_builder(ScipAnalysis *analysis,
                                            ArchbirdProject *project,
                                            int parsed) {
  uint8_t implementation[32];
  uint8_t configuration[32];
  const char *semantic_coverage = parsed ? "bounded" : "none";
  const char *summary_coverage = parsed ? "complete" : "bounded";
  ArchbirdStatus status;
  decode_digest(ARCHBIRD_SCIP_IMPLEMENTATION_SHA256, implementation);
  configuration_digest(analysis->spec, configuration);
  status = ab_bundle_builder_init_project(
      &analysis->builder, analysis->engine, analysis->manifest,
      ab_project_manifest_sha256_bytes(project), "archbird-scip", "1",
      implementation, configuration);
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_set_runtime(&analysis->builder,
                                           "portable-protobuf-wire-v1");
#define CAPABILITY(domain, coverage, claim, boundary)                          \
  do {                                                                         \
    if (status == ARCHBIRD_OK)                                                 \
      status = ab_bundle_builder_add_capability(&analysis->builder, domain,    \
                                                coverage, claim, boundary);    \
  } while (0)
  CAPABILITY("index-diagnostics", "bounded", "compiler-index",
             "diagnostics derived from one supplied SCIP index");
  CAPABILITY("index-summaries", summary_coverage, "compiler-index",
             parsed
                 ? "coverage counters for one complete supplied SCIP index"
                 : "identity and rejection state for one supplied SCIP index");
  CAPABILITY("reference-targets", semantic_coverage, "semantic-target",
             "one representative exact anchor and occurrence count per "
             "mapped document, target symbol, and import/reference kind");
  CAPABILITY("semantic-definitions", semantic_coverage, "semantic-identity",
             "SCIP definition occurrences within supplied mapped source");
  CAPABILITY("semantic-relationships", semantic_coverage, "semantic-target",
             "SCIP symbol relationships within supplied mapped source");
  CAPABILITY("semantic-symbols", semantic_coverage, "semantic-identity",
             "SCIP symbol metadata within supplied mapped source");
  CAPABILITY("symbols", semantic_coverage, "semantic-identity",
             "SCIP definitions with exact mapped source anchors and explicit "
             "producer display names");
#undef CAPABILITY
  return status;
}

static ArchbirdStatus scan_index(ArchbirdEngine *engine,
                                 ArchbirdProject *project,
                                 const AbConfigIndex *spec,
                                 ArchbirdProviderMode mode) {
  const AbSourceManifest *manifest = ab_project_manifest(project);
  const AbManifestFile *index_file;
  const uint8_t *bytes;
  size_t index_file_index;
  ScipAnalysis analysis;
  AbProviderBundle bundle;
  char parse_error[sizeof(engine->error)];
  size_t parse_offset = ARCHBIRD_NO_OFFSET;
  size_t edge_count = 0;
  int parsed = 1;
  ArchbirdStatus status;
  memset(&analysis, 0, sizeof(analysis));
  memset(&bundle, 0, sizeof(bundle));
  index_file = manifest_file(manifest, spec->path.data, spec->path.length,
                             &index_file_index);
  if (!index_file)
    return ARCHBIRD_OK;
  bytes = ab_project_source_bytes(project, index_file_index);
  analysis.engine = engine;
  analysis.project = project;
  analysis.manifest = manifest;
  analysis.spec = spec;
  analysis.index_file = index_file;
  status = parse_index(&analysis, bytes, index_file->byte_length);
  if (status == ARCHBIRD_OK)
    status = analyze_documents(&analysis, project);
  if (status == ARCHBIRD_INVALID_SCHEMA || status == ARCHBIRD_LIMIT_EXCEEDED) {
    (void)snprintf(parse_error, sizeof(parse_error), "%s",
                   archbird_engine_error(engine));
    parse_offset = archbird_engine_error_offset(engine);
    analysis_free(&analysis);
    memset(&analysis, 0, sizeof(analysis));
    analysis.engine = engine;
    analysis.project = project;
    analysis.manifest = manifest;
    analysis.spec = spec;
    analysis.index_file = index_file;
    archbird_error_clear(engine);
    parsed = 0;
    status = ARCHBIRD_OK;
  } else if (status != ARCHBIRD_OK) {
    goto done;
  }
  status = init_analysis_builder(&analysis, project, parsed);
  if (status == ARCHBIRD_OK && parsed)
    status = process_references(&analysis, project);
  if (status == ARCHBIRD_OK && parsed)
    edge_count = normalize_links(&analysis);
  if (status == ARCHBIRD_OK && parsed)
    status = add_analysis_diagnostics(&analysis);
  if (status == ARCHBIRD_OK)
    status = add_summary_fact(&analysis, edge_count,
                              analysis_evidence_state(&analysis, parsed));
  if (status == ARCHBIRD_OK && !parsed) {
    char message[sizeof(parse_error) + 96];
    if (parse_offset == ARCHBIRD_NO_OFFSET)
      (void)snprintf(message, sizeof(message), "index rejected: %s",
                     parse_error);
    else
      (void)snprintf(message, sizeof(message), "index rejected at byte %zu: %s",
                     parse_offset, parse_error);
    status = add_index_diagnostic_fact(&analysis,
                                       spec->required ? "error" : "warning",
                                       "invalid-index", message, &spec->path);
  }
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_finish(&analysis.builder, &bundle);
  if (status == ARCHBIRD_OK)
    status = ab_project_take_provider_bundle(engine, project,
                                             mode == ARCHBIRD_PROVIDER_AUDIT
                                                 ? ARCHBIRD_PROVIDER_AUDIT
                                                 : ARCHBIRD_PROVIDER_AUGMENT,
                                             &bundle);
  if (status != ARCHBIRD_OK)
    ab_provider_bundle_free(engine, &bundle);
done:
  analysis_free(&analysis);
  return status;
}

ArchbirdStatus ab_scan_scip_indexes(ArchbirdEngine *engine,
                                    ArchbirdProject *project,
                                    ArchbirdProviderMode mode) {
  const AbMapConfig *config;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!engine || !project)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (ab_project_providers_finalized(project))
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "providers were already finalized");
  config = ab_project_config(project);
  if (!config)
    return ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < config->index_count;
       index++) {
    const AbConfigIndex *spec = &config->indexes[index];
    if (spec->format.length == 4 && memcmp(spec->format.data, "scip", 4) == 0)
      status = scan_index(engine, project, spec, mode);
  }
  return status;
}
