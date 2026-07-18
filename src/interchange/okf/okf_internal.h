#ifndef ARCHBIRD_INTERCHANGE_OKF_INTERNAL_H
#define ARCHBIRD_INTERCHANGE_OKF_INTERNAL_H

#include "json_value.h"
#include "path_match.h"
#include "sha256.h"

typedef struct AbOkfDiagnostic {
  AbString severity;
  AbString code;
  AbString message;
  AbString path;
} AbOkfDiagnostic;

typedef struct AbOkfLink {
  AbString label;
  AbString href;
  AbString target;
  AbString kind;
  AbString state;
} AbOkfLink;

typedef struct AbOkfRequirementLink {
  AbString requirement_id;
  AbString concept_id;
  AbString source;
  AbString target_concept;
} AbOkfRequirementLink;

typedef struct AbOkfDocument {
  const AbValue *source;
  const AbValue *frontmatter;
  const AbString *body;
  const AbString *folded_type;
  const AbString *folded_text;
  const AbValue *folded_tags;
  AbString path;
  AbString concept_id;
  AbString kind;
  AbString sha256;
  AbString type_name;
  AbString title;
  AbString description;
  AbString resource;
  AbString *tags;
  size_t tag_count;
  AbString *explicit_requirements;
  size_t explicit_requirement_count;
  AbOkfLink *links;
  size_t link_count;
} AbOkfDocument;

typedef struct AbOkfIndex {
  ArchbirdEngine *engine;
  AbValue input;
  AbOkfDocument *documents;
  size_t document_count;
  AbString *known_paths;
  size_t known_path_count;
  AbOkfRequirementLink *requirements;
  size_t requirement_count;
  AbOkfDiagnostic *diagnostics;
  size_t diagnostic_count;
  size_t diagnostic_capacity;
  AbString okf_version;
  char bundle_sha256[65];
} AbOkfIndex;

typedef struct AbOkfQuery {
  ArchbirdEngine *engine;
  AbValue input;
  const AbValue *concepts;
  const AbValue *types;
  const AbValue *tags;
  const AbValue *text;
  const AbValue *requirements;
  size_t *selected;
  size_t selected_count;
  int active;
} AbOkfQuery;

ArchbirdStatus ab_okf_index_load(ArchbirdEngine *engine, const uint8_t *json,
                                 size_t json_length, AbOkfIndex *out);
void ab_okf_index_free(AbOkfIndex *index);

ArchbirdStatus ab_okf_query_load(ArchbirdEngine *engine,
                                 const uint8_t *query_json, size_t query_length,
                                 const AbOkfIndex *index, AbOkfQuery *out);
void ab_okf_query_free(AbOkfQuery *query);

ArchbirdStatus ab_okf_render_json(const AbOkfIndex *index,
                                  const AbOkfQuery *query, int include_body,
                                  AbBuffer *buffer);
ArchbirdStatus ab_okf_render_markdown(const AbOkfIndex *index,
                                      const AbOkfQuery *query,
                                      AbBuffer *buffer);

ArchbirdStatus ab_okf_error(ArchbirdEngine *engine, const char *message);
int ab_okf_text_is(const AbValue *value, const char *literal);
const AbString *ab_okf_optional_text(const AbValue *object, const char *name);
ArchbirdStatus ab_okf_copy_text(ArchbirdEngine *engine, AbString *out,
                                const AbString *source);
ArchbirdStatus ab_okf_copy_literal(ArchbirdEngine *engine, AbString *out,
                                   const char *literal);

#endif
