#include "fuzz_common.h"

#include "pattern.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const uint8_t *separator;
  AbPattern *pattern = NULL;
  AbString source;
  ArchbirdEngine *engine;
  size_t pattern_length;
  size_t match_count = 0;
  if (!data && size)
    return 0;
  separator = size ? (const uint8_t *)memchr(data, '\n', size) : NULL;
  pattern_length = separator ? (size_t)(separator - data) : size;
  source.data = (char *)data;
  source.length = pattern_length;
  engine = fuzz_engine();
  if (!engine)
    return 0;
  if (ab_pattern_compile(engine, &source, SIZE_MAX, &pattern) == ARCHBIRD_OK) {
    const uint8_t *subject = separator ? separator + 1 : data + size;
    size_t subject_length = separator ? size - pattern_length - 1 : 0;
    (void)ab_pattern_scan(engine, pattern, subject, subject_length, SIZE_MAX,
                          NULL, NULL, &match_count);
  }
  ab_pattern_free(pattern);
  archbird_engine_destroy(engine);
  return 0;
}
