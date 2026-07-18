#include "archbird_internal.h"
#include "pattern.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Output {
  int failed;
} Output;

static ArchbirdStatus write_match(void *user_data,
                                  const AbPatternMatch *match) {
  Output *output = (Output *)user_data;
  if (printf("%zu %zu %d %zu %zu\n", match->start, match->end,
             match->capture_present, match->capture_start,
             match->capture_end) < 0) {
    output->failed = 1;
    return ARCHBIRD_WRITE_FAILED;
  }
  return ARCHBIRD_OK;
}

int main(int argc, char **argv) {
  ArchbirdEngine *engine = NULL;
  AbPattern *pattern = NULL;
  AbString source;
  Output output = {0};
  size_t capture_index;
  size_t count = 0;
  char *end = NULL;
  ArchbirdStatus status;
  unsigned long parsed;
  if (argc != 4) {
    fprintf(stderr, "usage: pattern_cli CAPTURE_INDEX PATTERN SUBJECT\n");
    return 2;
  }
  errno = 0;
  parsed = strtoul(argv[1], &end, 10);
  if (errno || !end || *end || parsed > SIZE_MAX) {
    fprintf(stderr, "invalid capture index\n");
    return 2;
  }
  capture_index = parsed ? (size_t)parsed : SIZE_MAX;
  source.data = argv[2];
  source.length = strlen(argv[2]);
  status = archbird_engine_create(NULL, &engine);
  if (status == ARCHBIRD_OK)
    status = ab_pattern_compile(engine, &source, SIZE_MAX, &pattern);
  if (status == ARCHBIRD_OK)
    status = ab_pattern_scan(engine, pattern, (const uint8_t *)argv[3],
                             strlen(argv[3]), capture_index, write_match,
                             &output, &count);
  if (status != ARCHBIRD_OK) {
    fprintf(stderr, "%s\n", archbird_engine_error(engine));
    ab_pattern_free(pattern);
    archbird_engine_destroy(engine);
    return 1;
  }
  ab_pattern_free(pattern);
  archbird_engine_destroy(engine);
  return output.failed ? 1 : 0;
}
