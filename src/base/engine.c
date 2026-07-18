#include "archbird_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARCHBIRD_DEFAULT_MAX_INPUT_BYTES ((size_t)64 * 1024 * 1024)
#define ARCHBIRD_DEFAULT_MAX_DEPTH ((size_t)64)
#define ARCHBIRD_DEFAULT_MAX_VALUES ((size_t)4000000)
#define ARCHBIRD_DEFAULT_MAX_STRING_BYTES ((size_t)16 * 1024 * 1024)
#define ARCHBIRD_DEFAULT_MAX_FILES ((size_t)1000000)
#define ARCHBIRD_DEFAULT_MAX_FILE_BYTES ((size_t)64 * 1024 * 1024)
#define ARCHBIRD_DEFAULT_MAX_INDEX_BYTES ((size_t)512 * 1024 * 1024)
#define ARCHBIRD_DEFAULT_MAX_SOURCE_BYTES ((size_t)2 * 1024 * 1024 * 1024)
#define ARCHBIRD_DEFAULT_MAX_SYNTAX_BYTES ((size_t)256 * 1024 * 1024)
#define ARCHBIRD_DEFAULT_MAX_PROVIDER_BUNDLES ((size_t)100000)
#define ARCHBIRD_DEFAULT_MAX_FACTS ((size_t)10000000)
#define ARCHBIRD_DEFAULT_MAX_PATTERN_MATCHES ((size_t)1000000)
#define ARCHBIRD_DEFAULT_REGEX_MATCH_LIMIT ((uint32_t)1000000)
#define ARCHBIRD_DEFAULT_REGEX_DEPTH_LIMIT ((uint32_t)1000)
#define ARCHBIRD_DEFAULT_REGEX_HEAP_LIMIT_KIB ((uint32_t)65536)

const char *archbird_implementation_sha256(void) {
  return ARCHBIRD_IMPLEMENTATION_SHA256;
}

static void *default_allocate(void *user_data, size_t size) {
  (void)user_data;
  return malloc(size);
}

static void *default_reallocate(void *user_data, void *pointer, size_t size) {
  (void)user_data;
  return realloc(pointer, size);
}

static void default_deallocate(void *user_data, void *pointer) {
  (void)user_data;
  free(pointer);
}

void *ab_malloc(ArchbirdEngine *engine, size_t size) {
  if (!engine)
    return NULL;
  return engine->options.allocate(engine->options.allocator_user_data,
                                  size ? size : 1);
}

void *ab_calloc(ArchbirdEngine *engine, size_t count, size_t size) {
  void *pointer;
  size_t total;
  if (!engine || (size && count > SIZE_MAX / size))
    return NULL;
  total = count * size;
  pointer = ab_malloc(engine, total);
  if (pointer)
    memset(pointer, 0, total ? total : 1);
  return pointer;
}

void *ab_realloc(ArchbirdEngine *engine, void *pointer, size_t size) {
  if (!engine)
    return NULL;
  if (!pointer)
    return ab_malloc(engine, size);
  if (!size) {
    ab_free(engine, pointer);
    return NULL;
  }
  return engine->options.reallocate(engine->options.allocator_user_data,
                                    pointer, size);
}

void ab_free(ArchbirdEngine *engine, void *pointer) {
  if (engine && pointer)
    engine->options.deallocate(engine->options.allocator_user_data, pointer);
}

int ab_sha256_literal_valid(const char *value) {
  size_t index;
  if (!value)
    return 0;
  for (index = 0; index < 64; index++) {
    char current = value[index];
    if (!((current >= '0' && current <= '9') ||
          (current >= 'a' && current <= 'f')))
      return 0;
  }
  return value[64] == '\0';
}

ArchbirdStatus ab_build_identity_validate(ArchbirdEngine *engine) {
  static const struct {
    const char *name;
    const char *value;
  } identities[] = {
      {"ARCHBIRD_IMPLEMENTATION_SHA256", ARCHBIRD_IMPLEMENTATION_SHA256},
      {"ARCHBIRD_LEXICAL_C_IMPLEMENTATION_SHA256",
       ARCHBIRD_LEXICAL_C_IMPLEMENTATION_SHA256},
      {"ARCHBIRD_LEXICAL_JAVASCRIPT_IMPLEMENTATION_SHA256",
       ARCHBIRD_LEXICAL_JAVASCRIPT_IMPLEMENTATION_SHA256},
      {"ARCHBIRD_LEXICAL_PYTHON_IMPLEMENTATION_SHA256",
       ARCHBIRD_LEXICAL_PYTHON_IMPLEMENTATION_SHA256},
      {"ARCHBIRD_LEXICAL_R_IMPLEMENTATION_SHA256",
       ARCHBIRD_LEXICAL_R_IMPLEMENTATION_SHA256},
      {"ARCHBIRD_SCIP_IMPLEMENTATION_SHA256",
       ARCHBIRD_SCIP_IMPLEMENTATION_SHA256},
#ifdef ARCHBIRD_HAVE_TREE_SITTER_C
      {"ARCHBIRD_TREE_SITTER_C_IMPLEMENTATION_SHA256",
       ARCHBIRD_TREE_SITTER_C_IMPLEMENTATION_SHA256},
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_CPP
      {"ARCHBIRD_TREE_SITTER_CPP_IMPLEMENTATION_SHA256",
       ARCHBIRD_TREE_SITTER_CPP_IMPLEMENTATION_SHA256},
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_PYTHON
      {"ARCHBIRD_TREE_SITTER_PYTHON_IMPLEMENTATION_SHA256",
       ARCHBIRD_TREE_SITTER_PYTHON_IMPLEMENTATION_SHA256},
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_JAVASCRIPT
      {"ARCHBIRD_TREE_SITTER_JAVASCRIPT_IMPLEMENTATION_SHA256",
       ARCHBIRD_TREE_SITTER_JAVASCRIPT_IMPLEMENTATION_SHA256},
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_TYPESCRIPT
      {"ARCHBIRD_TREE_SITTER_TYPESCRIPT_IMPLEMENTATION_SHA256",
       ARCHBIRD_TREE_SITTER_TYPESCRIPT_IMPLEMENTATION_SHA256},
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_TSX
      {"ARCHBIRD_TREE_SITTER_TSX_IMPLEMENTATION_SHA256",
       ARCHBIRD_TREE_SITTER_TSX_IMPLEMENTATION_SHA256},
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_R
      {"ARCHBIRD_TREE_SITTER_R_IMPLEMENTATION_SHA256",
       ARCHBIRD_TREE_SITTER_R_IMPLEMENTATION_SHA256},
#endif
  };
  size_t index;
  if (!engine)
    return ARCHBIRD_INVALID_ARGUMENT;
  for (index = 0; index < sizeof(identities) / sizeof(identities[0]); index++) {
    if (!ab_sha256_literal_valid(identities[index].value))
      return archbird_error_set(
          engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
          "invalid build identity %s: expected 64 lowercase hexadecimal "
          "characters",
          identities[index].name);
  }
  return ARCHBIRD_OK;
}

void archbird_error_clear(ArchbirdEngine *engine) {
  if (!engine)
    return;
  engine->error_status = ARCHBIRD_OK;
  engine->error_offset = ARCHBIRD_NO_OFFSET;
  engine->error[0] = '\0';
}

ArchbirdStatus archbird_error_set(ArchbirdEngine *engine, ArchbirdStatus status,
                                  size_t offset, const char *format, ...) {
  va_list args;
  if (!engine)
    return status;
  engine->error_status = status;
  engine->error_offset = offset;
  va_start(args, format);
  (void)vsnprintf(engine->error, sizeof(engine->error), format, args);
  va_end(args);
  return status;
}

void archbird_engine_options_init(ArchbirdEngineOptions *options) {
  if (!options)
    return;
  options->struct_size = sizeof(*options);
  options->max_input_bytes = ARCHBIRD_DEFAULT_MAX_INPUT_BYTES;
  options->max_depth = ARCHBIRD_DEFAULT_MAX_DEPTH;
  options->max_values = ARCHBIRD_DEFAULT_MAX_VALUES;
  options->max_string_bytes = ARCHBIRD_DEFAULT_MAX_STRING_BYTES;
  options->max_files = ARCHBIRD_DEFAULT_MAX_FILES;
  options->max_file_bytes = ARCHBIRD_DEFAULT_MAX_FILE_BYTES;
  options->max_index_bytes = ARCHBIRD_DEFAULT_MAX_INDEX_BYTES;
  options->max_source_bytes = ARCHBIRD_DEFAULT_MAX_SOURCE_BYTES;
  options->max_syntax_bytes = ARCHBIRD_DEFAULT_MAX_SYNTAX_BYTES;
  options->max_provider_bundles = ARCHBIRD_DEFAULT_MAX_PROVIDER_BUNDLES;
  options->max_facts = ARCHBIRD_DEFAULT_MAX_FACTS;
  options->max_pattern_matches = ARCHBIRD_DEFAULT_MAX_PATTERN_MATCHES;
  options->regex_match_limit = ARCHBIRD_DEFAULT_REGEX_MATCH_LIMIT;
  options->regex_depth_limit = ARCHBIRD_DEFAULT_REGEX_DEPTH_LIMIT;
  options->regex_heap_limit_kib = ARCHBIRD_DEFAULT_REGEX_HEAP_LIMIT_KIB;
  options->allocate = NULL;
  options->reallocate = NULL;
  options->deallocate = NULL;
  options->allocator_user_data = NULL;
}

ArchbirdStatus archbird_engine_create(const ArchbirdEngineOptions *options,
                                      ArchbirdEngine **out_engine) {
  ArchbirdEngineOptions resolved;
  ArchbirdEngine *engine;
  if (!out_engine)
    return ARCHBIRD_INVALID_ARGUMENT;
  *out_engine = NULL;
  archbird_engine_options_init(&resolved);
  if (options) {
    if (options->struct_size != sizeof(*options))
      return ARCHBIRD_INVALID_ARGUMENT;
    resolved = *options;
  }
  if ((resolved.allocate || resolved.reallocate || resolved.deallocate) &&
      !(resolved.allocate && resolved.reallocate && resolved.deallocate))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!resolved.allocate) {
    if (resolved.allocator_user_data)
      return ARCHBIRD_INVALID_ARGUMENT;
    resolved.allocate = default_allocate;
    resolved.reallocate = default_reallocate;
    resolved.deallocate = default_deallocate;
  }
  if (resolved.max_input_bytes == 0 || resolved.max_depth == 0 ||
      resolved.max_values == 0 || resolved.max_string_bytes == 0 ||
      resolved.max_files == 0 || resolved.max_file_bytes == 0 ||
      resolved.max_index_bytes == 0 || resolved.max_source_bytes == 0 ||
      resolved.max_syntax_bytes == 0 || resolved.max_provider_bundles == 0 ||
      resolved.max_facts == 0 || resolved.max_pattern_matches == 0 ||
      resolved.regex_match_limit == 0 || resolved.regex_depth_limit == 0 ||
      resolved.regex_heap_limit_kib == 0)
    return ARCHBIRD_INVALID_ARGUMENT;
  engine = (ArchbirdEngine *)resolved.allocate(resolved.allocator_user_data,
                                               sizeof(*engine));
  if (!engine)
    return ARCHBIRD_OUT_OF_MEMORY;
  memset(engine, 0, sizeof(*engine));
  engine->options = resolved;
  archbird_error_clear(engine);
  *out_engine = engine;
  return ARCHBIRD_OK;
}

void archbird_engine_destroy(ArchbirdEngine *engine) {
  ArchbirdDeallocateFn deallocate;
  void *user_data;
  if (!engine)
    return;
  deallocate = engine->options.deallocate;
  user_data = engine->options.allocator_user_data;
  memset(engine, 0, sizeof(*engine));
  deallocate(user_data, engine);
}

const char *archbird_engine_error(const ArchbirdEngine *engine) {
  return engine ? engine->error : "invalid Archbird engine";
}

size_t archbird_engine_error_offset(const ArchbirdEngine *engine) {
  return engine ? engine->error_offset : ARCHBIRD_NO_OFFSET;
}
