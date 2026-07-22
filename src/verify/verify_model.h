#ifndef ARCHBIRD_VERIFY_MODEL_H
#define ARCHBIRD_VERIFY_MODEL_H

#include "verify_internal.h"

typedef struct AbVerifyEvidence {
  AbString provenance;
  AbString project;
  AbString path;
  uint64_t line;
  AbString sha256;
  AbString detail;
} AbVerifyEvidence;

typedef struct AbVerifyFactItem {
  AbString key;
  AbString label;
  AbValue value;
  AbObjectField *attributes;
  size_t attribute_count;
  AbVerifyEvidence *evidence;
  size_t evidence_count;
  size_t evidence_capacity;
  AbString state;
  AbString message;
} AbVerifyFactItem;

typedef struct AbVerifySelection {
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
} AbVerifySelection;

typedef struct AbVerifyFactSet {
  AbString name;
  AbString shape;
  AbString provenance;
  AbString project;
  AbVerifyFactItem *items;
  size_t item_count;
  size_t item_capacity;
  size_t *item_slots;
  size_t item_slot_count;
  AbString state;
  AbString message;
  AbVerifySelection selection;
  char sha256[65];
} AbVerifyFactSet;

void ab_verify_fact_free(ArchbirdEngine *engine, AbVerifyFactSet *fact);

ArchbirdStatus ab_verify_fact_init(ArchbirdEngine *engine,
                                   AbVerifyFactSet *fact, const AbString *name,
                                   const char *shape, const char *provenance,
                                   const AbString *project);

ArchbirdStatus ab_verify_fact_add_item(ArchbirdEngine *engine,
                                       AbVerifyFactSet *fact,
                                       AbVerifyFactItem *item);

ArchbirdStatus ab_verify_fact_find_item(ArchbirdEngine *engine,
                                        AbVerifyFactSet *fact,
                                        const AbString *key,
                                        AbVerifyFactItem **out);

ArchbirdStatus ab_verify_fact_finish(ArchbirdEngine *engine,
                                     AbVerifyFactSet *fact);

ArchbirdStatus
ab_verify_fact_selection_exact(ArchbirdEngine *engine, AbVerifyFactSet *fact,
                               const char *unit, uint64_t universe,
                               uint64_t selected, uint64_t excluded,
                               uint64_t unsupported, int truncated);

const char *
ab_verify_fact_selection_classification(const AbVerifyFactSet *fact);

ArchbirdStatus ab_verify_fact_render(AbBuffer *buffer,
                                     const AbVerifyFactSet *fact,
                                     int include_sha256);

/* Render semantic fact content without its plan-local operand name or digest.
 */
ArchbirdStatus ab_verify_fact_render_content(AbBuffer *buffer,
                                             const AbVerifyFactSet *fact);

ArchbirdStatus ab_verify_fact_unknown(ArchbirdEngine *engine,
                                      AbVerifyFactSet *fact,
                                      const AbString *name,
                                      const AbString *project,
                                      const char *shape, const char *message);

ArchbirdStatus
ab_verify_evidence_init(ArchbirdEngine *engine, AbVerifyEvidence *evidence,
                        const char *provenance, const AbString *project,
                        const AbString *path, uint64_t line, const char *sha256,
                        const char *detail, size_t detail_length);
void ab_verify_evidence_free(ArchbirdEngine *engine,
                             AbVerifyEvidence *evidence);
int ab_verify_evidence_compare(const void *left, const void *right);
ArchbirdStatus ab_verify_evidence_render(AbBuffer *buffer,
                                         const AbVerifyEvidence *evidence);

void ab_verify_fact_item_free(ArchbirdEngine *engine, AbVerifyFactItem *item);

#endif
