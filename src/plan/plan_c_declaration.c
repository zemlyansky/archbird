#include "plan_compile_internal.h"

#include "artifact_validation.h"

#include <ctype.h>
#include <string.h>

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static int path_has_suffix(const AbString *path, const char *suffix) {
  size_t length = strlen(suffix);
  return path && path->length >= length &&
         memcmp(path->data + path->length - length, suffix, length) == 0;
}

static int signature_token_count(const AbString *signature,
                                 const AbString *symbol) {
  size_t index;
  int count = 0;
  if (!signature || !symbol || !signature->length || !symbol->length ||
      isspace((unsigned char)signature->data[0]) ||
      isspace((unsigned char)signature->data[signature->length - 1]) ||
      memchr(signature->data, '\n', signature->length) ||
      memchr(signature->data, '\r', signature->length) ||
      memchr(signature->data, ';', signature->length) ||
      memchr(signature->data, '{', signature->length) ||
      memchr(signature->data, '}', signature->length))
    return 0;
  for (index = 0; index + symbol->length <= signature->length; index++) {
    unsigned char before;
    unsigned char after;
    if (memcmp(signature->data + index, symbol->data, symbol->length) != 0)
      continue;
    before = index ? (unsigned char)signature->data[index - 1] : 0;
    after = index + symbol->length < signature->length
                ? (unsigned char)signature->data[index + symbol->length]
                : 0;
    if ((before && (isalnum(before) || before == '_')) ||
        (after && (isalnum(after) || after == '_')))
      continue;
    count++;
  }
  return count;
}

static int signature_has_word(const AbString *signature, const char *word) {
  size_t length = strlen(word);
  size_t index;
  for (index = 0; index + length <= signature->length; index++) {
    unsigned char before;
    unsigned char after;
    if (memcmp(signature->data + index, word, length) != 0)
      continue;
    before = index ? (unsigned char)signature->data[index - 1] : 0;
    after = index + length < signature->length
                ? (unsigned char)signature->data[index + length]
                : 0;
    if ((!before || (!isalnum(before) && before != '_')) &&
        (!after || (!isalnum(after) && after != '_')))
      return 1;
  }
  return 0;
}

static int bytes_contain(const char *bytes, size_t length, const char *needle) {
  size_t needle_length = strlen(needle);
  size_t index;
  if (!needle_length || length < needle_length)
    return 0;
  for (index = 0; index + needle_length <= length; index++)
    if (memcmp(bytes + index, needle, needle_length) == 0)
      return 1;
  return 0;
}

static const AbValue *map_file(const AbValue *map, const AbString *path) {
  const AbValue *files = field(map, "files");
  const AbValue *match = NULL;
  size_t index;
  if (!files || files->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < files->as.array.count; index++) {
    const AbValue *candidate = &files->as.array.items[index];
    const AbValue *candidate_path = field(candidate, "path");
    if (!candidate_path || candidate_path->kind != AB_VALUE_STRING ||
        !ab_string_equal(&candidate_path->as.text, path))
      continue;
    if (match)
      return NULL;
    match = candidate;
  }
  return match;
}

static const AbValue *unique_function_symbol(const AbValue *map,
                                             const AbString *name,
                                             const AbString *signature,
                                             const AbString *excluded_path,
                                             const AbValue **out_file) {
  const AbValue *files = field(map, "files");
  const AbValue *match = NULL;
  size_t file_index;
  if (out_file)
    *out_file = NULL;
  if (!files || files->kind != AB_VALUE_ARRAY)
    return NULL;
  for (file_index = 0; file_index < files->as.array.count; file_index++) {
    const AbValue *file = &files->as.array.items[file_index];
    const AbValue *path = field(file, "path");
    const AbValue *language = field(file, "language");
    const AbValue *symbols = field(file, "symbols");
    size_t symbol_index;
    if (!path || path->kind != AB_VALUE_STRING ||
        !ab_artifact_text_is(language, "c") ||
        (excluded_path && ab_string_equal(&path->as.text, excluded_path)) ||
        !symbols || symbols->kind != AB_VALUE_ARRAY)
      continue;
    for (symbol_index = 0; symbol_index < symbols->as.array.count;
         symbol_index++) {
      const AbValue *symbol = &symbols->as.array.items[symbol_index];
      const AbValue *symbol_name = field(symbol, "name");
      const AbValue *kind = field(symbol, "kind");
      const AbValue *symbol_signature = field(symbol, "signature");
      if (!symbol_name || symbol_name->kind != AB_VALUE_STRING ||
          !ab_string_equal(&symbol_name->as.text, name) ||
          !ab_artifact_text_is(kind, "function") || !symbol_signature ||
          symbol_signature->kind != AB_VALUE_STRING ||
          (signature &&
           !ab_string_equal(&symbol_signature->as.text, signature)) ||
          field(symbol, "syntax_recovery"))
        continue;
      if (match) {
        if (out_file)
          *out_file = NULL;
        return NULL;
      }
      match = symbol;
      if (out_file)
        *out_file = file;
    }
  }
  return match;
}

int ab_plan_c_declaration_analyze(const AbValue *map, const AbString *path,
                                  const AbString *symbol,
                                  AbPlanCDeclarationProof *out,
                                  const char **out_reason) {
  const AbValue *target;
  const AbValue *target_symbols;
  const AbValue *implementation;
  const AbValue *implementation_file;
  const AbValue *signature;
  const AbValue *anchor = NULL;
  const AbValue *anchor_fact_id = NULL;
  uint64_t anchor_start = 0;
  uint64_t anchor_end = 0;
  size_t index;
  memset(out, 0, sizeof(*out));
  *out_reason = NULL;
  target = map_file(map, path);
  if (!target || !path_has_suffix(path, ".h") ||
      !ab_artifact_text_is(field(target, "language"), "c")) {
    *out_reason = "The reviewed destination is not one exact mapped C header.";
    return 0;
  }
  target_symbols = field(target, "symbols");
  if (!target_symbols || target_symbols->kind != AB_VALUE_ARRAY) {
    *out_reason = "The reviewed C destination has no complete symbol ledger.";
    return 0;
  }
  for (index = 0; index < target_symbols->as.array.count; index++) {
    const AbValue *candidate = &target_symbols->as.array.items[index];
    const AbValue *name = field(candidate, "name");
    if (name && name->kind == AB_VALUE_STRING &&
        ab_string_equal(&name->as.text, symbol)) {
      *out_reason = "The reviewed destination already contains the symbol.";
      return 0;
    }
  }
  implementation =
      unique_function_symbol(map, symbol, NULL, path, &implementation_file);
  signature = field(implementation, "signature");
  if (!implementation || !signature || signature->kind != AB_VALUE_STRING ||
      signature_token_count(&signature->as.text, symbol) != 1) {
    *out_reason =
        "No unique exact C implementation signature proves the declaration.";
    return 0;
  }
  for (index = 0; index < target_symbols->as.array.count; index++) {
    const AbValue *candidate = &target_symbols->as.array.items[index];
    const AbValue *name = field(candidate, "name");
    const AbValue *candidate_signature = field(candidate, "signature");
    const AbValue *fact_id = field(candidate, "fact_id");
    const AbValue *extent = field(candidate, "extent");
    uint64_t start;
    uint64_t end;
    if (!ab_artifact_text_is(field(candidate, "kind"), "declaration") ||
        !name || name->kind != AB_VALUE_STRING || !candidate_signature ||
        candidate_signature->kind != AB_VALUE_STRING || !fact_id ||
        fact_id->kind != AB_VALUE_STRING || fact_id->as.text.length < 3 ||
        memcmp(fact_id->as.text.data, "f:", 2) != 0 || !extent ||
        extent->kind != AB_VALUE_OBJECT ||
        !ab_artifact_safe_integer(field(extent, "start"), &start) ||
        !ab_artifact_safe_integer(field(extent, "end"), &end) || start >= end ||
        !unique_function_symbol(map, &name->as.text,
                                &candidate_signature->as.text, path, NULL))
      continue;
    if (!anchor || end > anchor_end) {
      anchor = candidate;
      anchor_fact_id = fact_id;
      anchor_start = start;
      anchor_end = end;
    }
  }
  if (!anchor) {
    *out_reason =
        "No peer declaration proves the destination's C signature style.";
    return 0;
  }
  out->target_file = target;
  out->implementation_file = implementation_file;
  out->implementation_symbol = implementation;
  out->anchor_symbol = anchor;
  out->anchor_fact_id = anchor_fact_id;
  out->anchor_start = anchor_start;
  out->anchor_end = anchor_end;
  return 1;
}

ArchbirdStatus ab_plan_c_declaration_signature(
    ArchbirdEngine *engine, const ArchbirdProject *project, const AbValue *map,
    const AbString *symbol, const AbPlanCDeclarationProof *proof,
    AbBuffer *signature, const char **out_reason) {
  const AbValue *path = field(proof->implementation_file, "path");
  const AbValue *extent = field(proof->implementation_symbol, "extent");
  AbPlanSourceLock lock;
  AbString exact;
  uint64_t start;
  uint64_t end;
  size_t cursor;
  size_t signature_start;
  size_t signature_end;
  ArchbirdStatus status;
  *out_reason = NULL;
  if (!path || path->kind != AB_VALUE_STRING || !extent ||
      extent->kind != AB_VALUE_OBJECT ||
      !ab_artifact_safe_integer(field(extent, "start"), &start) ||
      !ab_artifact_safe_integer(field(extent, "end"), &end) || start >= end) {
    *out_reason = "The C implementation has no exact source extent.";
    return ARCHBIRD_OK;
  }
  status = ab_plan_source_lock(engine, project, map, &path->as.text, &lock);
  if (status != ARCHBIRD_OK)
    return status;
  if (end > lock.source.byte_length) {
    *out_reason = "The C implementation extent exceeds its source.";
    return ARCHBIRD_OK;
  }
  signature_start = (size_t)start;
  while (signature_start < (size_t)end &&
         (lock.source.bytes[signature_start] == ' ' ||
          lock.source.bytes[signature_start] == '\t'))
    signature_start++;
  signature_end = signature_start;
  for (cursor = signature_start; cursor < (size_t)end; cursor++) {
    uint8_t byte = lock.source.bytes[cursor];
    if (byte == '\n' || byte == '\r') {
      *out_reason =
          "The C implementation signature is not one exact source line.";
      return ARCHBIRD_OK;
    }
    if (byte == '{') {
      signature_end = cursor;
      break;
    }
  }
  if (cursor == (size_t)end) {
    *out_reason = "The C implementation has no exact opening brace.";
    return ARCHBIRD_OK;
  }
  while (signature_end > signature_start &&
         (lock.source.bytes[signature_end - 1] == ' ' ||
          lock.source.bytes[signature_end - 1] == '\t'))
    signature_end--;
  exact.data = (char *)lock.source.bytes + signature_start;
  exact.length = signature_end - signature_start;
  if (signature_token_count(&exact, symbol) != 1 ||
      signature_has_word(&exact, "static") ||
      signature_has_word(&exact, "inline") ||
      memchr(exact.data, '#', exact.length) ||
      bytes_contain(exact.data, exact.length, "/*") ||
      bytes_contain(exact.data, exact.length, "//")) {
    *out_reason =
        "The C implementation signature is not a safe external declaration.";
    return ARCHBIRD_OK;
  }
  return ab_buffer_append(signature, exact.data, exact.length);
}

int ab_plan_c_declaration_place(const AbPlanCDeclarationProof *proof,
                                const ArchbirdSourceView *source,
                                AbPlanCDeclarationPlacement *out,
                                const char **out_reason) {
  size_t line_start;
  size_t index;
  memset(out, 0, sizeof(*out));
  *out_reason = NULL;
  if (!source || !source->bytes || proof->anchor_start >= proof->anchor_end ||
      proof->anchor_end >= source->byte_length) {
    *out_reason = "The peer declaration has no exact source extent.";
    return 0;
  }
  if (source->bytes[proof->anchor_end] == '\r') {
    if (proof->anchor_end + 1 >= source->byte_length ||
        source->bytes[proof->anchor_end + 1] != '\n') {
      *out_reason = "The C header has malformed line endings.";
      return 0;
    }
    out->newline_length = 2;
  } else if (source->bytes[proof->anchor_end] == '\n') {
    out->newline_length = 1;
  } else {
    *out_reason =
        "The peer declaration does not end before a source line boundary.";
    return 0;
  }
  line_start = (size_t)proof->anchor_start;
  while (line_start && source->bytes[line_start - 1] != '\n' &&
         source->bytes[line_start - 1] != '\r')
    line_start--;
  for (index = line_start; index < (size_t)proof->anchor_start; index++)
    if (source->bytes[index] != ' ' && source->bytes[index] != '\t') {
      *out_reason = "The peer declaration has a non-whitespace source prefix.";
      return 0;
    }
  out->line_start = line_start;
  return 1;
}
