#ifndef ARCHBIRD_PATTERN_H
#define ARCHBIRD_PATTERN_H

#include "model.h"

typedef struct AbPattern AbPattern;

typedef struct AbPatternMatch {
  size_t start;
  size_t end;
  int capture_present;
  size_t capture_start;
  size_t capture_end;
} AbPatternMatch;

typedef ArchbirdStatus (*AbPatternMatchFn)(void *user_data,
                                           const AbPatternMatch *match);

ArchbirdStatus ab_pattern_compile(ArchbirdEngine *engine,
                                  const AbString *source,
                                  size_t required_capture_count,
                                  AbPattern **out_pattern);

void ab_pattern_free(AbPattern *pattern);

ArchbirdStatus ab_pattern_scan(ArchbirdEngine *engine, const AbPattern *pattern,
                               const uint8_t *subject, size_t subject_length,
                               size_t capture_index, AbPatternMatchFn match_fn,
                               void *user_data, size_t *out_match_count);

#endif
