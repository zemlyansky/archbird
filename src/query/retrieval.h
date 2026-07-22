#ifndef ARCHBIRD_RETRIEVAL_H
#define ARCHBIRD_RETRIEVAL_H

#include "json_value.h"
#include "projection_model.h"

#define AB_RETRIEVAL_MAX_TERMS 16

typedef enum AbRetrievalKind {
  AB_RETRIEVAL_FILE = 0,
  AB_RETRIEVAL_SYMBOL = 1,
  AB_RETRIEVAL_COMPONENT = 2,
  AB_RETRIEVAL_PACKAGE = 3,
  AB_RETRIEVAL_ARTIFACT = 4
} AbRetrievalKind;

typedef struct AbRetrievalReason {
  const char *field;
  const AbString *value;
  const char *match;
  size_t term_index;
  unsigned strength;
  unsigned weight;
} AbRetrievalReason;

typedef struct AbRetrievalHit {
  AbRetrievalKind kind;
  const AbValue *row;
  const AbString *path;
  const AbString *name;
  const AbString *symbol_scope;
  const AbString *symbol_signature;
  const AbString *symbol_kind;
  size_t line;
  size_t source_index;
  uint64_t score;
  AbRetrievalReason reasons[AB_RETRIEVAL_MAX_TERMS];
  size_t reason_count;
} AbRetrievalHit;

typedef struct AbRetrievalResult {
  AbString terms[AB_RETRIEVAL_MAX_TERMS];
  size_t document_frequency[AB_RETRIEVAL_MAX_TERMS];
  size_t term_count;
  AbRetrievalHit *hits;
  size_t hit_count;
  size_t matched_count;
  size_t candidate_count;
  size_t limit;
} AbRetrievalResult;

ArchbirdStatus ab_query_retrieve(ArchbirdEngine *engine, const AbValue *map,
                                 const AbProjectionData *domain,
                                 const AbValue *queries, size_t limit,
                                 AbRetrievalResult *out);
void ab_query_retrieval_free(ArchbirdEngine *engine, AbRetrievalResult *result);
const char *ab_query_retrieval_kind_name(AbRetrievalKind kind);
uint64_t ab_query_retrieval_text_score(const AbRetrievalResult *result,
                                       const AbString *primary,
                                       unsigned primary_weight,
                                       const AbString *secondary,
                                       unsigned secondary_weight);

#endif
