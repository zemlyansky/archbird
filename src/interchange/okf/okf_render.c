#include "okf_internal.h"

#include <string.h>

#define OKF_RENDER_TRY(expression)                                             \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

static ArchbirdStatus render_text(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value ? value->data : "",
                               value ? value->length : 0);
}

static size_t concept_count(const AbOkfIndex *index) {
  size_t document;
  size_t count = 0;
  for (document = 0; document < index->document_count; document++)
    if (index->documents[document].kind.length == 7 &&
        !memcmp(index->documents[document].kind.data, "concept", 7))
      count++;
  return count;
}

static size_t requirement_identity_count(const AbOkfIndex *index) {
  size_t item;
  size_t count = 0;
  const AbString *previous = NULL;
  for (item = 0; item < index->requirement_count; item++) {
    const AbString *value = &index->requirements[item].requirement_id;
    if (!previous || !ab_string_equal(previous, value)) {
      count++;
      previous = value;
    }
  }
  return count;
}

static size_t broken_link_count(const AbOkfIndex *index) {
  size_t document;
  size_t count = 0;
  for (document = 0; document < index->document_count; document++) {
    size_t link;
    for (link = 0; link < index->documents[document].link_count; link++)
      if (index->documents[document].links[link].state.length == 6 &&
          !memcmp(index->documents[document].links[link].state.data, "broken",
                  6))
        count++;
  }
  return count;
}

static size_t diagnostic_count(const AbOkfIndex *index, const char *severity) {
  size_t item;
  size_t count = 0;
  size_t length = strlen(severity);
  for (item = 0; item < index->diagnostic_count; item++)
    if (index->diagnostics[item].severity.length == length &&
        !memcmp(index->diagnostics[item].severity.data, severity, length))
      count++;
  return count;
}

static ArchbirdStatus render_tags(AbBuffer *buffer,
                                  const AbOkfDocument *document) {
  size_t item;
  OKF_RENDER_TRY(ab_buffer_literal(buffer, "["));
  for (item = 0; item < document->tag_count; item++) {
    if (item)
      OKF_RENDER_TRY(ab_buffer_literal(buffer, ","));
    OKF_RENDER_TRY(render_text(buffer, &document->tags[item]));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus
render_document_requirements(AbBuffer *buffer, const AbOkfIndex *index,
                             const AbOkfDocument *document) {
  size_t item;
  size_t emitted = 0;
  const AbString *previous = NULL;
  OKF_RENDER_TRY(ab_buffer_literal(buffer, "["));
  for (item = 0; item < index->requirement_count; item++) {
    const AbOkfRequirementLink *row = &index->requirements[item];
    if (!ab_string_equal(&row->concept_id, &document->concept_id) ||
        (previous && ab_string_equal(previous, &row->requirement_id)))
      continue;
    if (emitted++)
      OKF_RENDER_TRY(ab_buffer_literal(buffer, ","));
    OKF_RENDER_TRY(render_text(buffer, &row->requirement_id));
    previous = &row->requirement_id;
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_links(AbBuffer *buffer,
                                   const AbOkfDocument *document) {
  size_t item;
  OKF_RENDER_TRY(ab_buffer_literal(buffer, "["));
  for (item = 0; item < document->link_count; item++) {
    const AbOkfLink *row = &document->links[item];
    if (item)
      OKF_RENDER_TRY(ab_buffer_literal(buffer, ","));
    OKF_RENDER_TRY(ab_buffer_literal(buffer, "{\"href\":"));
    OKF_RENDER_TRY(render_text(buffer, &row->href));
    OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"kind\":"));
    OKF_RENDER_TRY(render_text(buffer, &row->kind));
    OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"label\":"));
    OKF_RENDER_TRY(render_text(buffer, &row->label));
    OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"state\":"));
    OKF_RENDER_TRY(render_text(buffer, &row->state));
    OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"target\":"));
    OKF_RENDER_TRY(render_text(buffer, &row->target));
    OKF_RENDER_TRY(ab_buffer_literal(buffer, "}"));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_document(AbBuffer *buffer, const AbOkfIndex *index,
                                      const AbOkfDocument *document,
                                      int include_body) {
  OKF_RENDER_TRY(ab_buffer_literal(buffer, "{\"concept_id\":"));
  OKF_RENDER_TRY(render_text(buffer, &document->concept_id));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"description\":"));
  OKF_RENDER_TRY(render_text(buffer, &document->description));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"frontmatter\":"));
  OKF_RENDER_TRY(ab_value_render(buffer, document->frontmatter));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"kind\":"));
  OKF_RENDER_TRY(render_text(buffer, &document->kind));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"links\":"));
  OKF_RENDER_TRY(render_links(buffer, document));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
  OKF_RENDER_TRY(render_text(buffer, &document->path));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"requirements\":"));
  OKF_RENDER_TRY(render_document_requirements(buffer, index, document));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"resource\":"));
  OKF_RENDER_TRY(render_text(buffer, &document->resource));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
  OKF_RENDER_TRY(render_text(buffer, &document->sha256));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"tags\":"));
  OKF_RENDER_TRY(render_tags(buffer, document));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"title\":"));
  OKF_RENDER_TRY(render_text(buffer, &document->title));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"type\":"));
  OKF_RENDER_TRY(render_text(buffer, &document->type_name));
  if (include_body) {
    OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"body\":"));
    OKF_RENDER_TRY(render_text(buffer, document->body));
  }
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_documents(AbBuffer *buffer,
                                       const AbOkfIndex *index,
                                       const AbOkfQuery *query,
                                       int include_body) {
  size_t item;
  OKF_RENDER_TRY(ab_buffer_literal(buffer, "["));
  if (query) {
    for (item = 0; item < query->selected_count; item++) {
      if (item)
        OKF_RENDER_TRY(ab_buffer_literal(buffer, ","));
      OKF_RENDER_TRY(render_document(buffer, index,
                                     &index->documents[query->selected[item]],
                                     include_body));
    }
  } else {
    for (item = 0; item < index->document_count; item++) {
      if (item)
        OKF_RENDER_TRY(ab_buffer_literal(buffer, ","));
      OKF_RENDER_TRY(render_document(buffer, index, &index->documents[item],
                                     include_body));
    }
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_requirement_links(AbBuffer *buffer,
                                               const AbOkfIndex *index) {
  size_t item;
  OKF_RENDER_TRY(ab_buffer_literal(buffer, "["));
  for (item = 0; item < index->requirement_count; item++) {
    const AbOkfRequirementLink *row = &index->requirements[item];
    if (item)
      OKF_RENDER_TRY(ab_buffer_literal(buffer, ","));
    OKF_RENDER_TRY(ab_buffer_literal(buffer, "{\"concept_id\":"));
    OKF_RENDER_TRY(render_text(buffer, &row->concept_id));
    OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"requirement_id\":"));
    OKF_RENDER_TRY(render_text(buffer, &row->requirement_id));
    OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"source\":"));
    OKF_RENDER_TRY(render_text(buffer, &row->source));
    OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"target_concept\":"));
    OKF_RENDER_TRY(render_text(buffer, &row->target_concept));
    OKF_RENDER_TRY(ab_buffer_literal(buffer, "}"));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_diagnostics(AbBuffer *buffer,
                                         const AbOkfIndex *index) {
  size_t item;
  OKF_RENDER_TRY(ab_buffer_literal(buffer, "["));
  for (item = 0; item < index->diagnostic_count; item++) {
    const AbOkfDiagnostic *row = &index->diagnostics[item];
    if (item)
      OKF_RENDER_TRY(ab_buffer_literal(buffer, ","));
    OKF_RENDER_TRY(ab_buffer_literal(buffer, "{\"code\":"));
    OKF_RENDER_TRY(render_text(buffer, &row->code));
    OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"message\":"));
    OKF_RENDER_TRY(render_text(buffer, &row->message));
    OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
    OKF_RENDER_TRY(render_text(buffer, &row->path));
    OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"severity\":"));
    OKF_RENDER_TRY(render_text(buffer, &row->severity));
    OKF_RENDER_TRY(ab_buffer_literal(buffer, "}"));
  }
  return ab_buffer_literal(buffer, "]");
}

ArchbirdStatus ab_okf_render_json(const AbOkfIndex *index,
                                  const AbOkfQuery *query, int include_body,
                                  AbBuffer *buffer) {
  if (!index || !index->engine || !buffer)
    return ARCHBIRD_INVALID_ARGUMENT;
  OKF_RENDER_TRY(ab_build_identity_validate(index->engine));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, "{\"artifact\":"));
  OKF_RENDER_TRY(ab_buffer_json_string(
      buffer, query ? "okf-query" : "okf-index", query ? 9 : 9));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"diagnostics\":"));
  OKF_RENDER_TRY(render_diagnostics(buffer, index));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"documents\":"));
  OKF_RENDER_TRY(render_documents(buffer, index, query, include_body));
  OKF_RENDER_TRY(
      ab_buffer_literal(buffer, ",\"evidence\":{\"bundle_sha256\":"));
  OKF_RENDER_TRY(ab_buffer_json_string(buffer, index->bundle_sha256, 64));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"okf_version\":"));
  OKF_RENDER_TRY(render_text(buffer, &index->okf_version));
  OKF_RENDER_TRY(ab_buffer_literal(
      buffer,
      ",\"prose_interpreted_as_checks\":false},\"requirement_links\":"));
  OKF_RENDER_TRY(render_requirement_links(buffer, index));
  OKF_RENDER_TRY(ab_buffer_literal(
      buffer, ",\"schema_version\":1,\"summary\":{\"broken_links\":"));
  OKF_RENDER_TRY(ab_buffer_u64(buffer, broken_link_count(index)));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"concepts\":"));
  OKF_RENDER_TRY(ab_buffer_u64(buffer, concept_count(index)));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"documents\":"));
  OKF_RENDER_TRY(ab_buffer_u64(buffer, index->document_count));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"errors\":"));
  OKF_RENDER_TRY(ab_buffer_u64(buffer, diagnostic_count(index, "error")));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"requirements\":"));
  OKF_RENDER_TRY(ab_buffer_u64(buffer, requirement_identity_count(index)));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, ",\"warnings\":"));
  OKF_RENDER_TRY(ab_buffer_u64(buffer, diagnostic_count(index, "warning")));
  OKF_RENDER_TRY(ab_buffer_literal(
      buffer,
      "},\"tool\":{\"implementation_sha256\":\"" ARCHBIRD_IMPLEMENTATION_SHA256
      "\",\"name\":\"archbird\",\"version\":\"" ARCHBIRD_VERSION "\"}}"));
  return ARCHBIRD_OK;
}

static ArchbirdStatus markdown_text(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_append(buffer, value ? value->data : "",
                          value ? value->length : 0);
}

ArchbirdStatus ab_okf_render_markdown(const AbOkfIndex *index,
                                      const AbOkfQuery *query,
                                      AbBuffer *buffer) {
  size_t item;
  OKF_RENDER_TRY(ab_buffer_literal(buffer, query ? "# OKF query\n\n"
                                                 : "# OKF bundle index\n\n"));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, "Bundle SHA-256: `"));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, index->bundle_sha256));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, "`\n\nDocuments: "));
  OKF_RENDER_TRY(ab_buffer_u64(buffer, index->document_count));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, "; concepts: "));
  OKF_RENDER_TRY(ab_buffer_u64(buffer, concept_count(index)));
  OKF_RENDER_TRY(ab_buffer_literal(buffer, "; requirements: "));
  OKF_RENDER_TRY(ab_buffer_u64(buffer, requirement_identity_count(index)));
  OKF_RENDER_TRY(ab_buffer_literal(
      buffer,
      ".\n\nRequirement links come only from explicit `archbird` metadata and "
      "typed relations. Prose is never translated into constraints."));
  if (!query) {
    OKF_RENDER_TRY(ab_buffer_literal(buffer, "\n\n## Concepts\n"));
    for (item = 0; item < index->document_count; item++) {
      const AbOkfDocument *row = &index->documents[item];
      if (!(row->kind.length == 7 && !memcmp(row->kind.data, "concept", 7)))
        continue;
      OKF_RENDER_TRY(ab_buffer_literal(buffer, "\n* `"));
      OKF_RENDER_TRY(markdown_text(buffer, &row->concept_id));
      OKF_RENDER_TRY(ab_buffer_literal(buffer, "` — "));
      OKF_RENDER_TRY(markdown_text(buffer, &row->type_name));
      if (row->title.length) {
        OKF_RENDER_TRY(ab_buffer_literal(buffer, ": "));
        OKF_RENDER_TRY(markdown_text(buffer, &row->title));
      }
    }
  } else {
    for (item = 0; item < query->selected_count; item++) {
      const AbOkfDocument *row = &index->documents[query->selected[item]];
      OKF_RENDER_TRY(ab_buffer_literal(buffer, "\n\n## "));
      OKF_RENDER_TRY(markdown_text(
          buffer, row->title.length ? &row->title : &row->concept_id));
      OKF_RENDER_TRY(ab_buffer_literal(buffer, "\n\nConcept: `"));
      OKF_RENDER_TRY(markdown_text(buffer, &row->concept_id));
      OKF_RENDER_TRY(ab_buffer_literal(buffer, "`; type: `"));
      OKF_RENDER_TRY(markdown_text(buffer, &row->type_name));
      OKF_RENDER_TRY(ab_buffer_literal(buffer, "`; SHA-256: `"));
      OKF_RENDER_TRY(markdown_text(buffer, &row->sha256));
      OKF_RENDER_TRY(ab_buffer_literal(buffer, "`.\n\n"));
      OKF_RENDER_TRY(markdown_text(buffer, row->body));
      while (buffer->length && buffer->data[buffer->length - 1] == '\n')
        buffer->length--;
    }
  }
  OKF_RENDER_TRY(ab_buffer_literal(buffer, "\n\n## Diagnostics\n\n"));
  if (!index->diagnostic_count) {
    OKF_RENDER_TRY(ab_buffer_literal(buffer, "None."));
  } else {
    for (item = 0; item < index->diagnostic_count; item++) {
      const AbOkfDiagnostic *row = &index->diagnostics[item];
      OKF_RENDER_TRY(ab_buffer_literal(buffer, "* "));
      OKF_RENDER_TRY(markdown_text(buffer, &row->severity));
      OKF_RENDER_TRY(ab_buffer_literal(buffer, " `"));
      OKF_RENDER_TRY(markdown_text(buffer, &row->code));
      OKF_RENDER_TRY(ab_buffer_literal(buffer, "`"));
      if (row->path.length) {
        OKF_RENDER_TRY(ab_buffer_literal(buffer, " `"));
        OKF_RENDER_TRY(markdown_text(buffer, &row->path));
        OKF_RENDER_TRY(ab_buffer_literal(buffer, "`"));
      }
      OKF_RENDER_TRY(ab_buffer_literal(buffer, ": "));
      OKF_RENDER_TRY(markdown_text(buffer, &row->message));
      OKF_RENDER_TRY(ab_buffer_literal(buffer, "\n"));
    }
    while (buffer->length && buffer->data[buffer->length - 1] == '\n')
      buffer->length--;
  }
  return ab_buffer_literal(buffer, "\n");
}
