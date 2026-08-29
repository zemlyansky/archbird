#ifndef ARCHBIRD_PROJECTION_COMPARISON_H
#define ARCHBIRD_PROJECTION_COMPARISON_H

#include "base/comparison.h"
#include "projection/projection_model.h"

typedef struct AbProjectionComparisonInput {
  AbComparisonItem *left;
  AbComparisonItem *right;
  AbComparisonGroup *groups;
  const AbString **targets;
} AbProjectionComparisonInput;

ArchbirdStatus ab_projection_comparison_prepare(
    ArchbirdEngine *engine, const AbProjectionData *left,
    const AbProjectionData *right, const AbValue *mapping,
    int require_left_coverage, int require_right_coverage,
    AbProjectionComparisonInput *out, AbComparisonPlan *plan);
void ab_projection_comparison_free(ArchbirdEngine *engine,
                                   AbProjectionComparisonInput *input);

#endif
