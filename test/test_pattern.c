#include "archbird_internal.h"
#include "pattern.h"

#include <stdio.h>
#include <string.h>

typedef struct Matches {
  AbPatternMatch rows[16];
  size_t count;
} Matches;

static int fail(const char *message) {
  fprintf(stderr, "pattern test failed: %s\n", message);
  return 1;
}

static ArchbirdStatus collect(void *user_data, const AbPatternMatch *match) {
  Matches *matches = (Matches *)user_data;
  if (matches->count >= sizeof(matches->rows) / sizeof(matches->rows[0]))
    return ARCHBIRD_LIMIT_EXCEEDED;
  matches->rows[matches->count++] = *match;
  return ARCHBIRD_OK;
}

static ArchbirdStatus compile_literal(ArchbirdEngine *engine,
                                      const char *source,
                                      size_t required_capture_count,
                                      AbPattern **out) {
  AbString pattern = {(char *)source, strlen(source)};
  return ab_pattern_compile(engine, &pattern, required_capture_count, out);
}

int main(void) {
  ArchbirdEngine *engine = NULL;
  ArchbirdEngine *limited_engine = NULL;
  ArchbirdEngineOptions options;
  AbPattern *pattern = NULL;
  Matches matches = {0};
  size_t count = 0;
  ArchbirdStatus status = archbird_engine_create(NULL, &engine);
  if (status != ARCHBIRD_OK)
    return fail("cannot create engine");
  if (ab_build_identity_validate(engine) != ARCHBIRD_OK ||
      !ab_sha256_literal_valid(
          "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") ||
      ab_sha256_literal_valid(
          "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde") ||
      ab_sha256_literal_valid(
          "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeF") ||
      ab_sha256_literal_valid(
          "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeq"))
    return fail("build identity validation is not strict lowercase SHA-256");
  if (ARCHBIRD_PATTERN_CONTRACT_VERSION != 1u ||
      strcmp(ARCHBIRD_PATTERN_CONTRACT, "archbird-pcre2-v1") != 0 ||
      strcmp(ARCHBIRD_PATTERN_ENGINE, "PCRE2 10.47") != 0 ||
      strcmp(ARCHBIRD_PATTERN_UNICODE, "UCD 16.0.0") != 0 ||
      strcmp(ARCHBIRD_PATTERN_OPTIONS,
             "UTF,UCP,NEWLINE_LF,BSR_UNICODE,NEVER_BACKSLASH_C,"
             "NEVER_CALLOUT,"
             "JIT_DISABLED") != 0)
    return fail("public pattern-contract metadata changed");

  status = compile_literal(engine, "[", SIZE_MAX, &pattern);
  if (status != ARCHBIRD_INVALID_SCHEMA || pattern)
    return fail("invalid pattern was accepted");
  status = compile_literal(engine, "(?C)a", SIZE_MAX, &pattern);
  if (status != ARCHBIRD_INVALID_SCHEMA || pattern ||
      !strstr(archbird_engine_error(engine), "callouts is disabled"))
    return fail("explicit callout was accepted");
  status = compile_literal(engine, "\\C", SIZE_MAX, &pattern);
  if (status != ARCHBIRD_INVALID_SCHEMA || pattern ||
      !strstr(archbird_engine_error(engine), "using \\C is disabled"))
    return fail("byte-unit escape was accepted");
  status = compile_literal(engine, "\\p{L}+", 0, &pattern);
  if (status != ARCHBIRD_OK)
    return fail("PCRE2 Unicode-property syntax was rejected");
  ab_pattern_free(pattern);
  pattern = NULL;
  status = compile_literal(engine, "(a+)", 1, &pattern);
  if (status != ARCHBIRD_OK)
    return fail("one-capture pattern was rejected");
  ab_pattern_free(pattern);
  pattern = NULL;
  status = compile_literal(engine, "(a+)", 0, &pattern);
  if (status != ARCHBIRD_INVALID_SCHEMA || pattern)
    return fail("capture cardinality was not enforced");

  status = compile_literal(engine, "\\b\\w+\\b", 0, &pattern);
  if (status != ARCHBIRD_OK)
    return fail("Unicode word pattern did not compile");
  status = ab_pattern_scan(
      engine, pattern,
      (const uint8_t
           *)"caf\xc3\xa9 \xce\xba\xcf\x8c\xcf\x83\xce\xbc\xce\xbf\xcf\x82",
      strlen("caf\xc3\xa9 \xce\xba\xcf\x8c\xcf\x83\xce\xbc\xce\xbf\xcf\x82"),
      SIZE_MAX, collect, &matches, &count);
  if (status != ARCHBIRD_OK || count != 2 || matches.count != 2 ||
      matches.rows[0].start != 0 || matches.rows[0].end != 5 ||
      matches.rows[1].start != 6 || matches.rows[1].end != 18)
    return fail("Unicode finditer offsets disagree");
  ab_pattern_free(pattern);
  pattern = NULL;

  memset(&matches, 0, sizeof(matches));
  status = compile_literal(engine, "(?=a)", 0, &pattern);
  if (status != ARCHBIRD_OK)
    return fail("empty-match pattern did not compile");
  status = ab_pattern_scan(engine, pattern, (const uint8_t *)"aa", 2, SIZE_MAX,
                           collect, &matches, &count);
  if (status != ARCHBIRD_OK || count != 2 || matches.rows[0].start != 0 ||
      matches.rows[0].end != 0 || matches.rows[1].start != 1 ||
      matches.rows[1].end != 1)
    return fail("empty matches did not make progress like finditer");
  ab_pattern_free(pattern);
  pattern = NULL;

  memset(&matches, 0, sizeof(matches));
  status = compile_literal(engine, "(a)?b", 1, &pattern);
  if (status != ARCHBIRD_OK)
    return fail("optional-capture pattern did not compile");
  status = ab_pattern_scan(engine, pattern, (const uint8_t *)"b ab", 4, 1,
                           collect, &matches, &count);
  if (status != ARCHBIRD_OK || count != 2 || matches.rows[0].capture_present ||
      !matches.rows[1].capture_present || matches.rows[1].capture_start != 2 ||
      matches.rows[1].capture_end != 3)
    return fail("optional capture offsets are wrong");
  ab_pattern_free(pattern);
  pattern = NULL;

  archbird_engine_options_init(&options);
  options.max_pattern_matches = 2;
  status = archbird_engine_create(&options, &limited_engine);
  if (status != ARCHBIRD_OK)
    return fail("cannot create limited engine");
  status = compile_literal(limited_engine, ".", 0, &pattern);
  if (status != ARCHBIRD_OK)
    return fail("limit test pattern did not compile");
  status = ab_pattern_scan(limited_engine, pattern, (const uint8_t *)"abc", 3,
                           SIZE_MAX, NULL, NULL, &count);
  if (status != ARCHBIRD_LIMIT_EXCEEDED)
    return fail("match-count limit was not enforced");

  ab_pattern_free(pattern);
  archbird_engine_destroy(limited_engine);
  archbird_engine_destroy(engine);
  puts("native pattern tests passed");
  return 0;
}
