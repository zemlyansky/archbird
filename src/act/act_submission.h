#ifndef ARCHBIRD_ACT_SUBMISSION_H
#define ARCHBIRD_ACT_SUBMISSION_H

#include "base/json_value.h"

typedef struct AbActSubmission {
  const AbString *item_id;
  uint8_t *replacement;
  size_t replacement_length;
  int consumed;
} AbActSubmission;

typedef struct AbActSubmissions {
  AbValue document;
  AbActSubmission *items;
  size_t count;
} AbActSubmissions;

ArchbirdStatus ab_act_submissions_load(ArchbirdEngine *engine,
                                       const uint8_t *json, size_t length,
                                       AbActSubmissions *out);
const AbValue *ab_act_submission_path(const AbValue *operation);
AbActSubmission *ab_act_submission_take(AbActSubmissions *submissions,
                                        const AbString *item_id);
ArchbirdStatus
ab_act_submissions_require_consumed(ArchbirdEngine *engine,
                                    const AbActSubmissions *submissions);
void ab_act_submissions_free(ArchbirdEngine *engine,
                             AbActSubmissions *submissions);

#endif
