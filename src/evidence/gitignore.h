#ifndef ARCHBIRD_GITIGNORE_H
#define ARCHBIRD_GITIGNORE_H

#include "model.h"

typedef struct AbIgnoreToken AbIgnoreToken;

typedef struct AbIgnoreRule {
  AbString source_path;
  AbString base;
  AbString pattern;
  size_t line;
  int negative;
  int directory_only;
  int basename_only;
  AbIgnoreToken *tokens;
  size_t token_count;
} AbIgnoreRule;

typedef struct AbIgnoreSource {
  AbString path;
  char sha256[65];
  size_t rule_count;
} AbIgnoreSource;

typedef struct AbIgnoreSet {
  ArchbirdEngine *engine;
  AbIgnoreRule *rules;
  size_t rule_count;
  AbIgnoreSource *sources;
  size_t source_count;
  size_t max_tokens;
  uint8_t *scratch_left;
  uint8_t *scratch_right;
} AbIgnoreSet;

void ab_ignore_set_init(AbIgnoreSet *set, ArchbirdEngine *engine);

ArchbirdStatus ab_ignore_set_add(AbIgnoreSet *set, const char *path,
                                 size_t path_length, const uint8_t *bytes,
                                 size_t byte_length);

ArchbirdStatus ab_ignore_set_finalize(AbIgnoreSet *set);

int ab_ignore_set_matches(AbIgnoreSet *set, const AbString *path,
                          int directory);

void ab_ignore_set_free(AbIgnoreSet *set);

#endif
