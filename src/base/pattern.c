#include "pattern.h"

#include "archbird_internal.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include "pcre2.h"

#include <stdlib.h>
#include <string.h>

#if PCRE2_MAJOR != 10 || PCRE2_MINOR != 47
#error "archbird-pcre2-v1 requires PCRE2 10.47"
#endif

#define ARCHBIRD_PCRE2_RUNTIME_VERSION "10.47 2025-10-21"
#define ARCHBIRD_PCRE2_RUNTIME_UNICODE "16.0.0"
/* Pinned PCRE2 reports compile-time heap exhaustion as compile error 121
 * (COMPILE_ERROR_BASE + ERR21), distinct from the runtime -48 status. */
#define ARCHBIRD_PCRE2_COMPILE_ERROR_NOMEMORY 121

struct AbPattern {
  ArchbirdEngine *engine;
  pcre2_general_context *general_context;
  pcre2_code *code;
  uint32_t capture_count;
};

static void *pcre2_allocate(size_t size, void *context) {
  return ab_malloc((ArchbirdEngine *)context, size);
}

static void pcre2_deallocate(void *pointer, void *context) {
  ab_free((ArchbirdEngine *)context, pointer);
}

static ArchbirdStatus pattern_error(ArchbirdEngine *engine, int code,
                                    size_t offset) {
  PCRE2_UCHAR message[192];
  int length = pcre2_get_error_message(code, message, sizeof(message));
  if (code == PCRE2_ERROR_NOMEMORY ||
      code == ARCHBIRD_PCRE2_COMPILE_ERROR_NOMEMORY)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory compiling configured pattern");
  if (length < 0)
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "%s compile error at pattern byte %zu",
                              ARCHBIRD_PATTERN_CONTRACT, offset);
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "%s compile error at pattern byte %zu: %.*s",
                            ARCHBIRD_PATTERN_CONTRACT, offset, length,
                            (const char *)message);
}

static ArchbirdStatus validate_pattern_runtime(ArchbirdEngine *engine) {
  PCRE2_UCHAR version[64];
  PCRE2_UCHAR unicode_version[32];
  uint32_t compiled_widths = 0;
  uint32_t jit = 0;
  uint32_t unicode = 0;
  int result;

  result = pcre2_config(PCRE2_CONFIG_VERSION, NULL);
  if (result <= 0 || (size_t)result > sizeof(version) ||
      pcre2_config(PCRE2_CONFIG_VERSION, version) != result ||
      strcmp((const char *)version, ARCHBIRD_PCRE2_RUNTIME_VERSION) != 0)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "%s requires the pinned PCRE2 runtime %s",
                              ARCHBIRD_PATTERN_CONTRACT,
                              ARCHBIRD_PCRE2_RUNTIME_VERSION);

  result = pcre2_config(PCRE2_CONFIG_UNICODE_VERSION, NULL);
  if (result <= 0 || (size_t)result > sizeof(unicode_version) ||
      pcre2_config(PCRE2_CONFIG_UNICODE_VERSION, unicode_version) != result ||
      strcmp((const char *)unicode_version, ARCHBIRD_PCRE2_RUNTIME_UNICODE) !=
          0)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "%s requires Unicode data %s",
                              ARCHBIRD_PATTERN_CONTRACT,
                              ARCHBIRD_PCRE2_RUNTIME_UNICODE);

  if (pcre2_config(PCRE2_CONFIG_COMPILED_WIDTHS, &compiled_widths) < 0 ||
      pcre2_config(PCRE2_CONFIG_JIT, &jit) < 0 ||
      pcre2_config(PCRE2_CONFIG_UNICODE, &unicode) < 0 ||
      compiled_widths != 1u || jit != 0u || unicode != 1u)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "%s requires 8-bit Unicode PCRE2 without JIT",
                              ARCHBIRD_PATTERN_CONTRACT);
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_pattern_compile(ArchbirdEngine *engine,
                                  const AbString *source,
                                  size_t required_capture_count,
                                  AbPattern **out_pattern) {
  AbPattern *pattern = NULL;
  pcre2_compile_context *compile_context = NULL;
  int error_code = 0;
  PCRE2_SIZE error_offset = 0;
  int info_status;
  ArchbirdStatus status;
  if (!engine || !source || !out_pattern)
    return ARCHBIRD_INVALID_ARGUMENT;
  *out_pattern = NULL;
  if (source->length > engine->options.max_string_bytes)
    return archbird_error_set(
        engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
        "configured pattern exceeds max_string_bytes: %zu > %zu",
        source->length, engine->options.max_string_bytes);
  status = validate_pattern_runtime(engine);
  if (status != ARCHBIRD_OK)
    return status;
  pattern = (AbPattern *)ab_calloc(engine, 1, sizeof(*pattern));
  if (!pattern)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory compiling configured pattern");
  pattern->engine = engine;
  pattern->general_context =
      pcre2_general_context_create(pcre2_allocate, pcre2_deallocate, engine);
  if (!pattern->general_context) {
    ab_free(engine, pattern);
    return archbird_error_set(
        engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
        "out of memory creating pattern allocator context");
  }
  compile_context = pcre2_compile_context_create(pattern->general_context);
  if (!compile_context) {
    pcre2_general_context_free(pattern->general_context);
    ab_free(engine, pattern);
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory creating pattern compile context");
  }
  if (pcre2_set_compile_extra_options(compile_context,
                                      PCRE2_EXTRA_NEVER_CALLOUT) != 0 ||
      pcre2_set_newline(compile_context, PCRE2_NEWLINE_LF) != 0 ||
      pcre2_set_bsr(compile_context, PCRE2_BSR_UNICODE) != 0 ||
      pcre2_set_max_pattern_length(compile_context,
                                   engine->options.max_string_bytes) != 0 ||
      pcre2_set_max_pattern_compiled_length(
          compile_context, engine->options.max_string_bytes) != 0) {
    pcre2_compile_context_free(compile_context);
    pcre2_general_context_free(pattern->general_context);
    ab_free(engine, pattern);
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "cannot configure the %s compiler",
                              ARCHBIRD_PATTERN_CONTRACT);
  }
  pattern->code = pcre2_compile((PCRE2_SPTR)(source->data ? source->data : ""),
                                (PCRE2_SIZE)source->length,
                                PCRE2_UTF | PCRE2_UCP | PCRE2_NEVER_BACKSLASH_C,
                                &error_code, &error_offset, compile_context);
  pcre2_compile_context_free(compile_context);
  if (!pattern->code) {
    pcre2_general_context_free(pattern->general_context);
    ab_free(engine, pattern);
    return pattern_error(engine, error_code, (size_t)error_offset);
  }
  info_status = pcre2_pattern_info(pattern->code, PCRE2_INFO_CAPTURECOUNT,
                                   &pattern->capture_count);
  if (info_status != 0) {
    pcre2_code_free(pattern->code);
    pcre2_general_context_free(pattern->general_context);
    ab_free(engine, pattern);
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "cannot inspect compiled regular expression");
  }
  if (required_capture_count != SIZE_MAX &&
      pattern->capture_count != required_capture_count) {
    pcre2_code_free(pattern->code);
    pcre2_general_context_free(pattern->general_context);
    ab_free(engine, pattern);
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "configured regular expression must have exactly %zu capture groups",
        required_capture_count);
  }
  *out_pattern = pattern;
  return ARCHBIRD_OK;
}

void ab_pattern_free(AbPattern *pattern) {
  if (!pattern)
    return;
  pcre2_code_free(pattern->code);
  pcre2_general_context_free(pattern->general_context);
  ab_free(pattern->engine, pattern);
}

static ArchbirdStatus match_error(ArchbirdEngine *engine, int code) {
  if (code == PCRE2_ERROR_NOMEMORY)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory matching regular expression");
  if (code == PCRE2_ERROR_MATCHLIMIT || code == PCRE2_ERROR_DEPTHLIMIT ||
      code == PCRE2_ERROR_HEAPLIMIT)
    return archbird_error_set(
        engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
        "configured regular expression exceeded its resource limit");
  return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                            "regular-expression matching failed with code %d",
                            code);
}

ArchbirdStatus ab_pattern_scan(ArchbirdEngine *engine, const AbPattern *pattern,
                               const uint8_t *subject, size_t subject_length,
                               size_t capture_index, AbPatternMatchFn match_fn,
                               void *user_data, size_t *out_match_count) {
  pcre2_match_data *match_data = NULL;
  pcre2_match_context *match_context = NULL;
  PCRE2_SIZE start_offset = 0;
  uint32_t options = 0;
  size_t match_count = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  int result;
  if (!engine || !pattern || (!subject && subject_length) || !out_match_count)
    return ARCHBIRD_INVALID_ARGUMENT;
  *out_match_count = 0;
  match_data = pcre2_match_data_create_from_pattern(pattern->code,
                                                    pattern->general_context);
  match_context = pcre2_match_context_create(pattern->general_context);
  if (!match_data || !match_context) {
    status =
        archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                           "out of memory matching regular expression");
    goto done;
  }
  if (pcre2_set_match_limit(match_context, engine->options.regex_match_limit) !=
          0 ||
      pcre2_set_depth_limit(match_context, engine->options.regex_depth_limit) !=
          0 ||
      pcre2_set_heap_limit(match_context,
                           engine->options.regex_heap_limit_kib) != 0) {
    status = archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                                "cannot configure regular-expression limits");
    goto done;
  }
  for (;;) {
    PCRE2_SIZE *offsets;
    AbPatternMatch match;
    result = pcre2_match(pattern->code, (PCRE2_SPTR)subject,
                         (PCRE2_SIZE)subject_length, start_offset, options,
                         match_data, match_context);
    if (result == PCRE2_ERROR_NOMATCH)
      break;
    if (result < 0) {
      status = match_error(engine, result);
      break;
    }
    offsets = pcre2_get_ovector_pointer(match_data);
    if (offsets[0] > offsets[1]) {
      status =
          archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                             "regular expression produced a reversed match");
      break;
    }
    if (match_count == engine->options.max_pattern_matches) {
      status = archbird_error_set(
          engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
          "configured regular expression produced too many matches");
      break;
    }
    match.start = (size_t)offsets[0];
    match.end = (size_t)offsets[1];
    match.capture_present = 0;
    match.capture_start = 0;
    match.capture_end = 0;
    if (capture_index != SIZE_MAX && capture_index <= pattern->capture_count &&
        capture_index < (size_t)result &&
        offsets[capture_index * 2] != PCRE2_UNSET) {
      match.capture_present = 1;
      match.capture_start = (size_t)offsets[capture_index * 2];
      match.capture_end = (size_t)offsets[capture_index * 2 + 1];
    }
    match_count++;
    if (match_fn) {
      status = match_fn(user_data, &match);
      if (status != ARCHBIRD_OK)
        break;
    }
    if (!pcre2_next_match(match_data, &start_offset, &options))
      break;
  }
done:
  pcre2_match_context_free(match_context);
  pcre2_match_data_free(match_data);
  if (status == ARCHBIRD_OK)
    *out_match_count = match_count;
  return status;
}
