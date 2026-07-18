#ifndef ARCHBIRD_VERIFY_INTERNAL_H
#define ARCHBIRD_VERIFY_INTERNAL_H

#include "json_value.h"

typedef struct AbVerifySuiteView {
  ArchbirdEngine *engine;
  const AbValue *root;
  const AbValue *name;
  const AbValue *description;
  const AbValue *policy_date;
  const AbValue *projects;
  const AbValue *extractors;
  const AbValue *mappings;
  const AbValue *attestations;
  const AbValue *checks;
  const AbValue *waivers;
  int candidate;
  char sha256[65];
} AbVerifySuiteView;

ArchbirdStatus ab_verify_suite_validate(ArchbirdEngine *engine,
                                        const AbValue *root,
                                        AbVerifySuiteView *out);

int ab_verify_string_is(const AbValue *value, const char *literal);
int ab_verify_nonblank(const AbValue *value);
int ab_verify_path_is_repository(const AbValue *value);
ArchbirdStatus ab_verify_render_normalized_path(AbBuffer *buffer,
                                                const AbValue *value);

#endif
