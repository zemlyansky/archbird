#include "c/declaration.h"

#include "artifact_validation.h"

#include <ctype.h>
#include <string.h>

typedef struct AbActCDeclarationProof {
  const AbValue *implementation_file;
  const AbValue *implementation_symbol;
  AbString declaration_prefix;
  AbString declaration_suffix;
  uint64_t anchor_start;
  uint64_t anchor_end;
} AbActCDeclarationProof;

typedef struct AbActCDeclarationPlacement {
  size_t line_start;
  size_t newline_length;
} AbActCDeclarationPlacement;

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static ArchbirdStatus reject(AbActContext *context, ArchbirdStatus status,
                             const char *message) {
  return archbird_error_set(ab_act_executor_engine(context), status,
                            ARCHBIRD_NO_OFFSET,
                            "act C declaration executor: %s", message);
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

static int path_has_suffix(const AbString *path, const char *suffix) {
  size_t length = strlen(suffix);
  return path && path->length >= length &&
         memcmp(path->data + path->length - length, suffix, length) == 0;
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

static int declaration_wrapper(const AbString *declaration,
                               const AbString *implementation,
                               AbString *out_prefix, AbString *out_suffix) {
  size_t index;
  size_t match = 0;
  int found = 0;
  if (!declaration || !implementation || !implementation->length ||
      declaration->length < implementation->length)
    return 0;
  for (index = 0; index + implementation->length <= declaration->length;
       index++) {
    if (memcmp(declaration->data + index, implementation->data,
               implementation->length) != 0)
      continue;
    if (found)
      return 0;
    found = 1;
    match = index;
  }
  if (!found)
    return 0;
  out_prefix->data = declaration->data;
  out_prefix->length = match;
  out_suffix->data = declaration->data + match + implementation->length;
  out_suffix->length = declaration->length - match - implementation->length;
  return 1;
}

static int same_wrapper(const AbString *left_prefix,
                        const AbString *left_suffix,
                        const AbString *right_prefix,
                        const AbString *right_suffix) {
  return ab_string_equal(left_prefix, right_prefix) &&
         ab_string_equal(left_suffix, right_suffix);
}

static int analyze_declaration(const AbValue *map, const AbString *path,
                               const AbString *symbol,
                               AbActCDeclarationProof *out,
                               const char **out_reason) {
  const AbValue *target;
  const AbValue *target_symbols;
  const AbValue *implementation;
  const AbValue *implementation_file;
  const AbValue *signature;
  const AbValue *anchor = NULL;
  int style_found = 0;
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
    const AbValue *peer_implementation;
    const AbValue *peer_signature;
    const AbValue *fact_id = field(candidate, "fact_id");
    const AbValue *extent = field(candidate, "extent");
    AbString prefix = {0};
    AbString suffix = {0};
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
        end - start != candidate_signature->as.text.length + 1)
      continue;
    peer_implementation =
        unique_function_symbol(map, &name->as.text, NULL, path, NULL);
    peer_signature = field(peer_implementation, "signature");
    if (!peer_implementation || !peer_signature ||
        peer_signature->kind != AB_VALUE_STRING ||
        !declaration_wrapper(&candidate_signature->as.text,
                             &peer_signature->as.text, &prefix, &suffix))
      continue;
    if (style_found &&
        !same_wrapper(&out->declaration_prefix, &out->declaration_suffix,
                      &prefix, &suffix)) {
      *out_reason = "Peer declarations prove conflicting C signature styles.";
      return 0;
    }
    if (!style_found) {
      out->declaration_prefix = prefix;
      out->declaration_suffix = suffix;
      style_found = 1;
    }
    if (!anchor || end > anchor_end) {
      anchor = candidate;
      anchor_start = start;
      anchor_end = end;
    }
  }
  if (!anchor || !style_found) {
    *out_reason =
        "No peer declaration proves the destination's C signature style.";
    return 0;
  }
  out->implementation_file = implementation_file;
  out->implementation_symbol = implementation;
  out->anchor_start = anchor_start;
  out->anchor_end = anchor_end;
  return 1;
}

static ArchbirdStatus extract_signature(AbActContext *context,
                                        const AbString *symbol,
                                        const AbActCDeclarationProof *proof,
                                        AbBuffer *signature) {
  const AbValue *path = field(proof->implementation_file, "path");
  const AbValue *extent = field(proof->implementation_symbol, "extent");
  ArchbirdSourceView source;
  AbString exact;
  uint64_t start;
  uint64_t end;
  size_t cursor;
  size_t signature_start;
  size_t signature_end;
  ArchbirdStatus status;
  if (!path || path->kind != AB_VALUE_STRING || !extent ||
      extent->kind != AB_VALUE_OBJECT ||
      !ab_artifact_safe_integer(field(extent, "start"), &start) ||
      !ab_artifact_safe_integer(field(extent, "end"), &end) || start >= end)
    return reject(context, ARCHBIRD_POLICY_REJECTED,
                  "the C implementation has no exact source extent");
  status = ab_act_executor_source(context, &path->as.text, &source);
  if (status != ARCHBIRD_OK)
    return status;
  if (end > source.byte_length)
    return reject(context, ARCHBIRD_CONFLICT,
                  "the C implementation extent exceeds its source");
  signature_start = (size_t)start;
  while (signature_start < (size_t)end &&
         (source.bytes[signature_start] == ' ' ||
          source.bytes[signature_start] == '\t'))
    signature_start++;
  signature_end = signature_start;
  for (cursor = signature_start; cursor < (size_t)end; cursor++) {
    uint8_t byte = source.bytes[cursor];
    if (byte == '\n' || byte == '\r')
      return reject(
          context, ARCHBIRD_POLICY_REJECTED,
          "the C implementation signature is not one exact source line");
    if (byte == '{') {
      signature_end = cursor;
      break;
    }
  }
  if (cursor == (size_t)end)
    return reject(context, ARCHBIRD_POLICY_REJECTED,
                  "the C implementation has no exact opening brace");
  while (signature_end > signature_start &&
         (source.bytes[signature_end - 1] == ' ' ||
          source.bytes[signature_end - 1] == '\t'))
    signature_end--;
  exact.data = (char *)source.bytes + signature_start;
  exact.length = signature_end - signature_start;
  if (signature_token_count(&exact, symbol) != 1 ||
      signature_has_word(&exact, "static") ||
      signature_has_word(&exact, "inline") ||
      memchr(exact.data, '#', exact.length) ||
      bytes_contain(exact.data, exact.length, "/*") ||
      bytes_contain(exact.data, exact.length, "//"))
    return reject(
        context, ARCHBIRD_POLICY_REJECTED,
        "the C implementation signature is not a safe external declaration");
  status = ab_buffer_append(signature, proof->declaration_prefix.data,
                            proof->declaration_prefix.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(signature, exact.data, exact.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(signature, proof->declaration_suffix.data,
                              proof->declaration_suffix.length);
  return status;
}

static ArchbirdStatus place_declaration(AbActContext *context,
                                        const AbActCDeclarationProof *proof,
                                        const ArchbirdSourceView *source,
                                        AbActCDeclarationPlacement *out) {
  size_t line_start;
  size_t index;
  memset(out, 0, sizeof(*out));
  if (!source || !source->bytes || proof->anchor_start >= proof->anchor_end ||
      proof->anchor_end >= source->byte_length)
    return reject(context, ARCHBIRD_CONFLICT,
                  "the peer declaration has no exact source extent");
  if (source->bytes[proof->anchor_end] == '\r') {
    if (proof->anchor_end + 1 >= source->byte_length ||
        source->bytes[proof->anchor_end + 1] != '\n')
      return reject(context, ARCHBIRD_CONFLICT,
                    "the C header has malformed line endings");
    out->newline_length = 2;
  } else if (source->bytes[proof->anchor_end] == '\n') {
    out->newline_length = 1;
  } else {
    return reject(
        context, ARCHBIRD_CONFLICT,
        "the peer declaration does not end before a source line boundary");
  }
  line_start = (size_t)proof->anchor_start;
  while (line_start && source->bytes[line_start - 1] != '\n' &&
         source->bytes[line_start - 1] != '\r')
    line_start--;
  for (index = line_start; index < (size_t)proof->anchor_start; index++)
    if (source->bytes[index] != ' ' && source->bytes[index] != '\t')
      return reject(context, ARCHBIRD_CONFLICT,
                    "the peer declaration has a non-whitespace source prefix");
  out->line_start = line_start;
  return ARCHBIRD_OK;
}

static int source_paths_match(const AbValue *paths, const AbString *target,
                              const AbString *implementation) {
  int target_seen = 0;
  int implementation_seen = 0;
  size_t index;
  if (!paths || paths->kind != AB_VALUE_ARRAY || paths->as.array.count != 2)
    return 0;
  for (index = 0; index < paths->as.array.count; index++) {
    const AbValue *path = &paths->as.array.items[index];
    if (path->kind != AB_VALUE_STRING)
      return 0;
    if (ab_string_equal(&path->as.text, target))
      target_seen++;
    if (ab_string_equal(&path->as.text, implementation))
      implementation_seen++;
  }
  return target_seen == 1 && implementation_seen == 1;
}

static ArchbirdStatus
ground_declaration(AbActContext *context, const AbString *path,
                   const AbString *symbol, const AbValue *source_paths,
                   const AbString *item_id, const char *executor_capability) {
  ArchbirdEngine *engine = ab_act_executor_engine(context);
  const AbValue *implementation_path;
  AbActCDeclarationProof proof;
  AbActCDeclarationPlacement placement = {0};
  ArchbirdSourceView source = {0};
  const char *reason = NULL;
  AbBuffer signature;
  AbBuffer replacement;
  ArchbirdStatus status;
  memset(&proof, 0, sizeof(proof));
  ab_buffer_init(&signature, engine);
  ab_buffer_init(&replacement, engine);
  status = ab_act_executor_begin(context, item_id, executor_capability);
  if (status == ARCHBIRD_OK)
    status = ab_act_executor_source(context, path, &source);
  if (status == ARCHBIRD_OK &&
      !analyze_declaration(ab_act_executor_map(context), path, symbol, &proof,
                           &reason))
    status = reject(context, ARCHBIRD_POLICY_REJECTED,
                    reason ? reason
                           : "the mapped declaration objective is unsupported");
  implementation_path =
      status == ARCHBIRD_OK ? field(proof.implementation_file, "path") : NULL;
  if (status == ARCHBIRD_OK && source_paths &&
      (!implementation_path || implementation_path->kind != AB_VALUE_STRING ||
       !source_paths_match(source_paths, path, &implementation_path->as.text)))
    status = reject(context, ARCHBIRD_CONFLICT,
                    "the declared source closure differs from the Map proof");
  if (status == ARCHBIRD_OK)
    status = extract_signature(context, symbol, &proof, &signature);
  if (status == ARCHBIRD_OK)
    status = place_declaration(context, &proof, &source, &placement);
  if (status == ARCHBIRD_OK && placement.newline_length == 2)
    status = ab_buffer_append(&replacement, "\r\n", 2);
  else if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&replacement, "\n", 1);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&replacement, source.bytes + placement.line_start,
                              proof.anchor_start - placement.line_start);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&replacement, signature.data, signature.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&replacement, ";", 1);
  if (status == ARCHBIRD_OK)
    status = ab_act_executor_replace_exact(
        context, item_id, path, (size_t)proof.anchor_end,
        (size_t)proof.anchor_end, source.bytes + proof.anchor_end, 0,
        replacement.data, replacement.length);
  ab_buffer_free(&signature);
  ab_buffer_free(&replacement);
  return status;
}

ArchbirdStatus ab_act_c_declare_symbol(AbActContext *context,
                                       const AbValue *operation,
                                       const AbString *item_id) {
  const AbValue *path = field(operation, "path");
  const AbValue *symbol = field(operation, "symbol");
  return ground_declaration(context, &path->as.text, &symbol->as.text,
                            field(operation, "source_paths"), item_id,
                            "archbird.native.c.declare-symbol@1");
}

ArchbirdStatus ab_act_c_file_provider_capability(
    AbActContext *context, const AbActCProviderCapability *provider,
    const AbValue *operation, const AbString *item_id) {
  return ground_declaration(context, provider->path, provider->capability,
                            field(operation, "source_paths"), item_id,
                            "archbird.native.c.provider-capability@1");
}
