#ifndef ARCHBIRD_PROJECTION_MODEL_H
#define ARCHBIRD_PROJECTION_MODEL_H

#include "json_value.h"
#include "render_internal.h"

int ab_projection_value_is(const AbValue *value, const char *literal);
int ab_projection_nonblank(const AbValue *value);
int ab_projection_path_is_repository(const AbValue *value);

typedef struct AbProjectionEvidence {
  AbString provenance;
  AbString project;
  AbString path;
  uint64_t line;
  AbString sha256;
  AbString detail;
} AbProjectionEvidence;

typedef struct AbProjectionItem {
  AbString key;
  AbString label;
  AbValue value;
  AbObjectField *attributes;
  size_t attribute_count;
  AbProjectionEvidence *evidence;
  size_t evidence_count;
  size_t evidence_capacity;
  AbString state;
  AbString message;
} AbProjectionItem;

typedef struct AbProjectionCompleteness {
  AbString unit;
  uint64_t universe;
  uint64_t selected;
  uint64_t evaluated;
  uint64_t excluded;
  uint64_t unsupported;
  uint64_t unknown;
  int has_universe;
  int has_selected;
  int has_evaluated;
  int has_excluded;
  int has_unsupported;
  int has_unknown;
  int has_truncated;
  int truncated;
} AbProjectionCompleteness;

typedef struct AbProjectionData {
  AbString name;
  AbString shape;
  AbString provenance;
  AbString project;
  AbProjectionItem *items;
  size_t item_count;
  size_t item_capacity;
  size_t *item_slots;
  size_t item_slot_count;
  AbString state;
  AbString message;
  AbProjectionCompleteness selection;
  char sha256[65];
} AbProjectionData;

void ab_projection_data_free(ArchbirdEngine *engine, AbProjectionData *fact);

ArchbirdStatus ab_projection_data_init(ArchbirdEngine *engine,
                                       AbProjectionData *fact,
                                       const AbString *name, const char *shape,
                                       const char *provenance,
                                       const AbString *project);

ArchbirdStatus ab_projection_data_add_item(ArchbirdEngine *engine,
                                           AbProjectionData *fact,
                                           AbProjectionItem *item);

ArchbirdStatus ab_projection_data_find_item(ArchbirdEngine *engine,
                                            AbProjectionData *fact,
                                            const AbString *key,
                                            AbProjectionItem **out);

ArchbirdStatus ab_projection_data_finish(ArchbirdEngine *engine,
                                         AbProjectionData *fact);

ArchbirdStatus ab_projection_data_completeness_exact(
    ArchbirdEngine *engine, AbProjectionData *fact, const char *unit,
    uint64_t universe, uint64_t selected, uint64_t excluded,
    uint64_t unsupported, int truncated);

const char *ab_projection_data_classification(const AbProjectionData *fact);

ArchbirdStatus ab_projection_data_render(AbBuffer *buffer,
                                         const AbProjectionData *fact,
                                         int include_sha256);

/* Render semantic fact content without its plan-local operand name or digest.
 */
ArchbirdStatus ab_projection_data_render_content(AbBuffer *buffer,
                                                 const AbProjectionData *fact);

ArchbirdStatus
ab_projection_data_unknown(ArchbirdEngine *engine, AbProjectionData *fact,
                           const AbString *name, const AbString *project,
                           const char *shape, const char *message);

ArchbirdStatus ab_projection_evidence_init(
    ArchbirdEngine *engine, AbProjectionEvidence *evidence,
    const char *provenance, const AbString *project, const AbString *path,
    uint64_t line, const char *sha256, const char *detail,
    size_t detail_length);
void ab_projection_evidence_free(ArchbirdEngine *engine,
                                 AbProjectionEvidence *evidence);
int ab_projection_evidence_compare(const void *left, const void *right);
ArchbirdStatus
ab_projection_evidence_render(AbBuffer *buffer,
                              const AbProjectionEvidence *evidence);

void ab_projection_item_free(ArchbirdEngine *engine, AbProjectionItem *item);
ArchbirdStatus ab_projection_item_init(ArchbirdEngine *engine,
                                       AbProjectionItem *item,
                                       const AbString *key,
                                       const AbString *label,
                                       const AbValue *value);
ArchbirdStatus
ab_projection_item_add_evidence(ArchbirdEngine *engine, AbProjectionItem *item,
                                const AbProjectionEvidence *source);
ArchbirdStatus ab_projection_item_set_state(ArchbirdEngine *engine,
                                            AbProjectionItem *item,
                                            const char *state,
                                            const char *message);
ArchbirdStatus ab_projection_data_decode_artifact(ArchbirdEngine *engine,
                                                  const AbValue *value,
                                                  AbProjectionData *out);
ArchbirdStatus ab_projection_evidence_decode_artifact(
    ArchbirdEngine *engine, const AbValue *value, AbProjectionEvidence *out);

#endif
