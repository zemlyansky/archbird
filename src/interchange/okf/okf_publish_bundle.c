#include "okf_publish_internal.h"

#include <stdlib.h>
#include <string.h>

#define BUNDLE_TRY(expression)                                                 \
  do {                                                                         \
    ArchbirdStatus bundle_status_ = (expression);                              \
    if (bundle_status_ != ARCHBIRD_OK)                                         \
      return bundle_status_;                                                   \
  } while (0)

typedef struct DirectoryList {
  AbString *items;
  size_t count;
  size_t capacity;
} DirectoryList;

static int string_compare(const void *left, const void *right) {
  return ab_string_compare((const AbString *)left, (const AbString *)right);
}

static int concept_index_compare(const void *left, const void *right) {
  const AbOkfPubConcept *const *a = (const AbOkfPubConcept *const *)left;
  const AbOkfPubConcept *const *b = (const AbOkfPubConcept *const *)right;
  int compared = ab_string_compare(&(*a)->type_name, &(*b)->type_name);
  if (!compared)
    compared = ab_string_compare(&(*a)->sort_key, &(*b)->sort_key);
  if (!compared)
    compared = ab_string_compare(&(*a)->path, &(*b)->path);
  return compared;
}

static const AbOkfPubSource *sources(const AbOkfPublication *pub,
                                     size_t index) {
  if (index == 0)
    return &pub->map_source;
  if (pub->has_verification) {
    if (index == 1)
      return &pub->verification_source;
    index--;
  }
  if (pub->has_proposal) {
    if (index == 1)
      return &pub->proposal_source;
    index--;
  }
  if (pub->has_contract) {
    if (index == 1)
      return &pub->contract_source;
    index--;
  }
  if (pub->has_result && index == 1)
    return &pub->result_source;
  return NULL;
}

static size_t source_count(const AbOkfPublication *pub) {
  return 1 + (size_t)pub->has_verification + (size_t)pub->has_proposal +
         (size_t)pub->has_contract + (size_t)pub->has_result;
}

static ArchbirdStatus source_list_json(AbOkfPublication *pub, AbString *out,
                                       int full) {
  AbBuffer buffer;
  size_t index;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_literal(&buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < source_count(pub); index++) {
    const AbOkfPubSource *source = sources(pub, index);
    if (index)
      status = ab_buffer_literal(&buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, "{\"artifact\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&buffer, source->artifact,
                                     strlen(source->artifact));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, ",\"evidence_sha256\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&buffer, source->evidence_sha256.data,
                                     source->evidence_sha256.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, ",\"file_sha256\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&buffer, source->file_sha256, 64);
    if (status == ARCHBIRD_OK && full)
      status = ab_buffer_literal(&buffer, ",\"producer\":");
    if (status == ARCHBIRD_OK && full)
      status = ab_value_render(&buffer, source->tool);
    if (status == ARCHBIRD_OK && full)
      status = ab_buffer_literal(&buffer, ",\"schema_version\":");
    if (status == ARCHBIRD_OK && full)
      status = ab_buffer_u64(&buffer, source->schema_version);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "]");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus source_entity_digest(AbOkfPublication *pub,
                                           AbString *out) {
  AbBuffer buffer;
  char digest[65];
  size_t index;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_literal(&buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < source_count(pub); index++) {
    const AbOkfPubSource *source = sources(pub, index);
    if (index)
      status = ab_buffer_literal(&buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, "[");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&buffer, source->artifact,
                                     strlen(source->artifact));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&buffer, source->file_sha256, 64);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&buffer, source->evidence_sha256.data,
                                     source->evidence_sha256.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, "]");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "]");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_sha256(buffer.data, buffer.length, digest);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_copy(pub, out, digest, 64);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus add_source_page(AbOkfPublication *pub) {
  AbOkfPubRelationList relations = {0};
  AbOkfPubField extra[1] = {{"sources", {0}}};
  AbString title = {(char *)"Canonical source artifacts", 26};
  AbString description = {0};
  AbString entity = {0};
  AbString tags[] = {{(char *)"provenance", 10}, {(char *)"sources", 7}};
  AbBuffer body;
  size_t index;
  ArchbirdStatus status = ab_okf_pub_relation_simple(
      pub, &relations, "publishes", "architecture/project");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_relation_simple(pub, &relations, "has_integrity",
                                        "provenance/integrity");
  if (status == ARCHBIRD_OK)
    status = source_list_json(pub, &extra[0].json, 0);
  if (status == ARCHBIRD_OK)
    status = source_entity_digest(pub, &entity);
  if (status == ARCHBIRD_OK) {
    AbBuffer value;
    ab_buffer_init(&value, pub->engine);
    status = ab_buffer_literal(&value, "Exact identities for ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&value, source_count(pub));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&value, " Archbird source artifacts.");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_buffer_string(pub, &value, &description);
    ab_buffer_free(&value);
  }
  ab_buffer_init(&body, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        &body,
        "# Canonical source artifacts\n\n"
        "Every concept in this generated bundle identifies its exact source "
        "artifact. These saved Archbird JSON artifacts remain "
        "authoritative.\n\n"
        "| Artifact | Schema | Evidence SHA-256 | File SHA-256 | Producer |\n"
        "| --- | --- | --- | --- | --- |\n");
  for (index = 0; status == ARCHBIRD_OK && index < source_count(pub); index++) {
    const AbOkfPubSource *source = sources(pub, index);
    const AbString *name = ab_okf_pub_text(source->tool, "name");
    const AbString *version = ab_okf_pub_text(source->tool, "version");
    status = ab_buffer_literal(&body, "| <code>");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, source->artifact);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "</code> | ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&body, source->schema_version);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, " | <code>");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&body, source->evidence_sha256.data,
                                source->evidence_sha256.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "</code> | <code>");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&body, source->file_sha256, 64);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "</code> | <code>");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&body, name->data, name->length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, " ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&body, version->data, version->length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "</code> |\n");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        &body,
        "\nNo source pathname, host root, timestamp, Git state, or model "
        "output "
        "is part of the generated knowledge content.\n\n"
        "# Bundle links\n\n"
        "* [Published project architecture](../architecture/project.md)\n"
        "* [Generated bundle integrity](integrity.md)\n");
  if (status == ARCHBIRD_OK) {
    AbOkfConceptSpec spec = {&pub->map_source,
                             "provenance/sources.md",
                             "Archbird Source Provenance",
                             &title,
                             NULL,
                             &description,
                             "derived",
                             "source_provenance",
                             &entity,
                             tags,
                             2,
                             &relations,
                             extra,
                             1,
                             &body};
    status = ab_okf_pub_add_concept(pub, &spec);
  }
  ab_buffer_free(&body);
  ab_string_free(pub->engine, &description);
  ab_string_free(pub->engine, &entity);
  ab_okf_pub_fields_free(pub, extra, 1);
  ab_okf_pub_relations_free(pub, &relations);
  return status;
}

static ArchbirdStatus directory_add(AbOkfPublication *pub, DirectoryList *list,
                                    const char *data, size_t length) {
  size_t index;
  size_t capacity;
  AbString *resized;
  for (index = 0; index < list->count; index++)
    if (list->items[index].length == length &&
        (!length || !memcmp(list->items[index].data, data, length)))
      return ARCHBIRD_OK;
  if (list->count == list->capacity) {
    capacity = list->capacity ? list->capacity * 2 : 16;
    if (capacity > SIZE_MAX / sizeof(*resized) ||
        capacity > pub->engine->options.max_values)
      return archbird_error_set(pub->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET, "too many OKF directories");
    resized = (AbString *)ab_realloc(pub->engine, list->items,
                                     capacity * sizeof(*resized));
    if (!resized)
      return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory growing OKF directories");
    memset(resized + list->capacity, 0,
           (capacity - list->capacity) * sizeof(*resized));
    list->items = resized;
    list->capacity = capacity;
  }
  BUNDLE_TRY(ab_okf_pub_copy(pub, &list->items[list->count], data, length));
  list->count++;
  if (list->count > 1)
    qsort(list->items, list->count, sizeof(*list->items), string_compare);
  return ARCHBIRD_OK;
}

static void directory_free(AbOkfPublication *pub, DirectoryList *list) {
  size_t index;
  for (index = 0; index < list->count; index++)
    ab_string_free(pub->engine, &list->items[index]);
  ab_free(pub->engine, list->items);
  memset(list, 0, sizeof(*list));
}

static ArchbirdStatus collect_directories(AbOkfPublication *pub,
                                          DirectoryList *list) {
  size_t concept;
  ArchbirdStatus status = directory_add(pub, list, "", 0);
  for (concept = 0; status == ARCHBIRD_OK && concept < pub->concept_count;
       ++concept) {
    const AbString *path = &pub->concepts[concept].path;
    size_t index;
    for (index = 0; index < path->length; index++)
      if (path->data[index] == '/')
        status = directory_add(pub, list, path->data, index);
  }
  return status;
}

static const char *directory_description(const AbString *path) {
  if (!path->length)
    return "Progressive index for the deterministic Archbird knowledge "
           "projection.";
  if (path->length == 12 && !memcmp(path->data, "architecture", 12))
    return "Derived source architecture concepts.";
  if (path->length == 12 && !memcmp(path->data, "verification", 12))
    return "Typed constraints, operands, findings, requirements, and "
           "observations.";
  if (path->length == 7 && !memcmp(path->data, "changes", 7))
    return "Derived proposals, asserted contracts, and derived results.";
  if (path->length == 10 && !memcmp(path->data, "provenance", 10))
    return "Canonical source identities and generated-content integrity.";
  return NULL;
}

static int direct_concept(const AbString *directory,
                          const AbOkfPubConcept *concept) {
  const char *slash = strrchr(concept->path.data, '/');
  size_t parent = slash ? (size_t)(slash - concept->path.data) : 0;
  return parent == directory->length &&
         (!parent || !memcmp(concept->path.data, directory->data, parent));
}

static ArchbirdStatus child_directories(AbOkfPublication *pub,
                                        const DirectoryList *all,
                                        const AbString *parent,
                                        DirectoryList *children) {
  size_t index;
  for (index = 0; index < all->count; index++) {
    const AbString *row = &all->items[index];
    size_t offset;
    const char *slash;
    if (row->length <= parent->length)
      continue;
    if (parent->length && (memcmp(row->data, parent->data, parent->length) ||
                           row->data[parent->length] != '/'))
      continue;
    offset = parent->length ? parent->length + 1 : 0;
    slash = memchr(row->data + offset, '/', row->length - offset);
    if (!slash)
      BUNDLE_TRY(directory_add(pub, children, row->data, row->length));
  }
  return ARCHBIRD_OK;
}

static const char *basename_text(const AbString *path, size_t *length) {
  const char *slash = NULL;
  size_t index = path->length;
  while (index) {
    index--;
    if (path->data[index] == '/') {
      slash = path->data + index;
      break;
    }
  }
  const char *result = slash ? slash + 1 : path->data;
  *length = path->length - (size_t)(result - path->data);
  return result;
}

static ArchbirdStatus index_link(AbBuffer *body, const AbString *source,
                                 const AbString *target,
                                 const AbString *label) {
  return ab_okf_pub_relative_link(body, source->data, target->data, label);
}

static ArchbirdStatus render_index(AbOkfPublication *pub,
                                   const DirectoryList *all,
                                   const AbString *directory,
                                   AbString *path_out, AbString *text_out) {
  DirectoryList children = {0};
  AbOkfPubConcept **concepts = NULL;
  size_t direct_count = 0;
  size_t concept;
  size_t index;
  AbBuffer path;
  AbBuffer body;
  ArchbirdStatus status = child_directories(pub, all, directory, &children);
  ab_buffer_init(&path, pub->engine);
  if (status == ARCHBIRD_OK && directory->length) {
    status = ab_buffer_append(&path, directory->data, directory->length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&path, "/");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&path, "index.md");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &path, path_out);
  for (concept = 0; concept < pub->concept_count; ++concept)
    if (direct_concept(directory, &pub->concepts[concept]))
      direct_count++;
  if (status == ARCHBIRD_OK && direct_count) {
    concepts = (AbOkfPubConcept **)ab_calloc(pub->engine, direct_count,
                                             sizeof(*concepts));
    if (!concepts)
      status = archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory building OKF index");
  }
  direct_count = 0;
  for (concept = 0; status == ARCHBIRD_OK && concept < pub->concept_count;
       ++concept)
    if (direct_concept(directory, &pub->concepts[concept]))
      concepts[direct_count++] = &pub->concepts[concept];
  if (direct_count > 1)
    qsort(concepts, direct_count, sizeof(*concepts), concept_index_compare);
  ab_buffer_init(&body, pub->engine);
  if (status == ARCHBIRD_OK && !directory->length)
    status = ab_buffer_literal(&body, "---\nokf_version: \"0.1\"\n---\n\n");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "# ");
  if (status == ARCHBIRD_OK && !directory->length) {
    status = ab_okf_pub_plain(&body, &pub->project);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, " Archbird knowledge");
  } else if (status == ARCHBIRD_OK) {
    status = ab_okf_pub_plain(&body, directory);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n\n");
  if (status == ARCHBIRD_OK && directory_description(directory))
    status = ab_buffer_literal(&body, directory_description(directory));
  else if (status == ARCHBIRD_OK) {
    status = ab_buffer_literal(&body, "Generated concepts under ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&body, directory->data, directory->length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, ".");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n\n");
  if (status == ARCHBIRD_OK && children.count)
    status = ab_buffer_literal(&body, "# Subdirectories\n\n");
  for (index = 0; status == ARCHBIRD_OK && index < children.count; index++) {
    const AbString *child = &children.items[index];
    size_t label_length;
    const char *label_data = basename_text(child, &label_length);
    AbString label = {(char *)label_data, label_length};
    AbBuffer target;
    AbString target_text;
    ab_buffer_init(&target, pub->engine);
    status = ab_buffer_append(&target, child->data, child->length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&target, "/index.md");
    target_text = (AbString){(char *)target.data, target.length};
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "* ");
    if (status == ARCHBIRD_OK)
      status = index_link(&body, path_out, &target_text, &label);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, " - ");
    if (status == ARCHBIRD_OK && directory_description(child))
      status = ab_buffer_literal(&body, directory_description(child));
    else if (status == ARCHBIRD_OK) {
      status = ab_buffer_literal(&body, "Generated concepts under ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&body, child->data, child->length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, ".");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n");
    ab_buffer_free(&target);
  }
  if (status == ARCHBIRD_OK && children.count)
    status = ab_buffer_literal(&body, "\n");
  for (index = 0; status == ARCHBIRD_OK && index < direct_count;) {
    size_t end = index + 1;
    while (end < direct_count && ab_string_equal(&concepts[index]->type_name,
                                                 &concepts[end]->type_name))
      end++;
    status = ab_buffer_literal(&body, "# ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, &concepts[index]->type_name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\n");
    for (; status == ARCHBIRD_OK && index < end; index++) {
      status = ab_buffer_literal(&body, "* ");
      if (status == ARCHBIRD_OK)
        status = index_link(&body, path_out, &concepts[index]->path,
                            &concepts[index]->title);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, " - ");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_plain(&body, &concepts[index]->description);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "\n");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n");
  }
  while (body.length && (body.data[body.length - 1] == '\n' ||
                         body.data[body.length - 1] == ' ' ||
                         body.data[body.length - 1] == '\t'))
    body.length--;
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &body, text_out);
  ab_buffer_free(&body);
  ab_buffer_free(&path);
  ab_free(pub->engine, concepts);
  directory_free(pub, &children);
  return status;
}

static ArchbirdStatus add_indexes(AbOkfPublication *pub) {
  DirectoryList directories = {0};
  size_t index;
  ArchbirdStatus status = collect_directories(pub, &directories);
  for (index = 0; status == ARCHBIRD_OK && index < directories.count; index++) {
    AbString path = {0};
    AbString text = {0};
    status = render_index(pub, &directories, &directories.items[index], &path,
                          &text);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_add_file(pub, path.data, (const uint8_t *)text.data,
                                   text.length);
    ab_string_free(pub->engine, &path);
    ab_string_free(pub->engine, &text);
  }
  directory_free(pub, &directories);
  return status;
}

static ArchbirdStatus digest_entries(AbOkfPublication *pub,
                                     const AbOkfPubFile *files, size_t count,
                                     AbString *out) {
  AbBuffer buffer;
  char digest[65];
  size_t index;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_literal(&buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    if (index)
      status = ab_buffer_literal(&buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, "[");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&buffer, files[index].path.data,
                                     files[index].path.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&buffer, files[index].sha256, 64);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, "]");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "]");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_sha256(buffer.data, buffer.length, digest);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_copy(pub, out, digest, 64);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus add_integrity_concept(AbOkfPublication *pub,
                                            size_t display_count) {
  AbOkfPubRelationList relations = {0};
  AbOkfPubField extra[2] = {{"concepts", {0}}, {"content_sha256", {0}}};
  AbString title = {(char *)"Generated bundle integrity", 26};
  AbString description = {0};
  AbString entity = {0};
  AbString tags[] = {{(char *)"provenance", 10}, {(char *)"integrity", 9}};
  AbBuffer body;
  size_t index;
  ArchbirdStatus status =
      digest_entries(pub, pub->files, pub->file_count, &entity);
  if (status == ARCHBIRD_OK)
    status =
        ab_okf_pub_copy(pub, &pub->content_sha256, entity.data, entity.length);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_relation_simple(pub, &relations, "describes",
                                        "provenance/sources");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_u64(pub, &extra[0].json, pub->file_count);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_text(pub, &extra[1].json, &entity);
  if (status == ARCHBIRD_OK) {
    AbBuffer value;
    ab_buffer_init(&value, pub->engine);
    status = ab_buffer_literal(&value, "Content digest for ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&value, display_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&value, " generated files.");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_buffer_string(pub, &value, &description);
    ab_buffer_free(&value);
  }
  ab_buffer_init(&body, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        &body,
        "# Generated bundle integrity\n\n"
        "This digest covers every generated file below, excluding only this "
        "self-describing integrity concept.\n\n"
        "| Path | SHA-256 |\n| --- | --- |\n");
  for (index = 0; status == ARCHBIRD_OK && index < pub->file_count; index++) {
    status = ab_buffer_literal(&body, "| <code>");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&body, pub->files[index].path.data,
                                pub->files[index].path.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "</code> | <code>");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&body, pub->files[index].sha256, 64);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "</code> |\n");
  }
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_literal(&body, "\n# Source provenance\n\n"
                                 "[Canonical source artifacts](sources.md)\n");
  if (status == ARCHBIRD_OK) {
    AbOkfConceptSpec spec = {&pub->map_source,
                             "provenance/integrity.md",
                             "Archbird Bundle Integrity",
                             &title,
                             NULL,
                             &description,
                             "derived",
                             "bundle_integrity",
                             &entity,
                             tags,
                             2,
                             &relations,
                             extra,
                             2,
                             &body};
    status = ab_okf_pub_add_concept(pub, &spec);
  }
  ab_buffer_free(&body);
  ab_string_free(pub->engine, &description);
  ab_string_free(pub->engine, &entity);
  ab_okf_pub_fields_free(pub, extra, 2);
  ab_okf_pub_relations_free(pub, &relations);
  return status;
}

static void clear_files(AbOkfPublication *pub) {
  size_t index;
  for (index = 0; index < pub->file_count; index++) {
    ab_string_free(pub->engine, &pub->files[index].path);
    ab_string_free(pub->engine, &pub->files[index].text);
  }
  pub->file_count = 0;
}

static ArchbirdStatus materialize_without_integrity(AbOkfPublication *pub) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  clear_files(pub);
  for (index = 0; status == ARCHBIRD_OK && index < pub->concept_count;
       index++) {
    const AbOkfPubConcept *concept = &pub->concepts[index];
    if (concept->path.length == 23 &&
        !memcmp(concept->path.data, "provenance/integrity.md", 23))
      continue;
    status = ab_okf_pub_add_file(pub, concept->path.data,
                                 (const uint8_t *)concept->text.data,
                                 concept->text.length);
  }
  if (status == ARCHBIRD_OK)
    status = add_indexes(pub);
  return status;
}

static ArchbirdStatus remove_integrity_concept(AbOkfPublication *pub) {
  size_t index;
  for (index = 0; index < pub->concept_count; index++) {
    AbOkfPubConcept *row = &pub->concepts[index];
    if (row->path.length != 23 ||
        memcmp(row->path.data, "provenance/integrity.md", 23))
      continue;
    ab_string_free(pub->engine, &row->path);
    ab_string_free(pub->engine, &row->type_name);
    ab_string_free(pub->engine, &row->title);
    ab_string_free(pub->engine, &row->sort_key);
    ab_string_free(pub->engine, &row->description);
    ab_string_free(pub->engine, &row->text);
    if (index + 1 < pub->concept_count)
      memmove(row, row + 1, (pub->concept_count - index - 1) * sizeof(*row));
    pub->concept_count--;
    memset(&pub->concepts[pub->concept_count], 0, sizeof(*pub->concepts));
    return ARCHBIRD_OK;
  }
  return ab_okf_pub_error(pub, "missing provisional OKF integrity concept");
}

static ArchbirdStatus render_bundle(AbOkfPublication *pub, AbBuffer *out) {
  AbString sources_json = {0};
  size_t index;
  ArchbirdStatus status = source_list_json(pub, &sources_json, 1);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, "{\"artifact\":\"okf-output-bundle\","
                                    "\"content_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(out, pub->content_sha256.data,
                                   pub->content_sha256.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"files\":[");
  for (index = 0; status == ARCHBIRD_OK && index < pub->file_count; index++) {
    const AbOkfPubFile *file = &pub->files[index];
    if (index)
      status = ab_buffer_literal(out, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(out, "{\"path\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(out, file->path.data, file->path.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(out, ",\"sha256\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(out, file->sha256, 64);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(out, ",\"text\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(out, file->text.data, file->text.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(out, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, "],\"project\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(out, pub->project.data, pub->project.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"schema_version\":1,\"sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(out, pub->aggregate_sha256.data,
                                   pub->aggregate_sha256.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"sources\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(out, sources_json.data, sources_json.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        out,
        ",\"tool\":{\"implementation_sha256\":\"" ARCHBIRD_IMPLEMENTATION_SHA256
        "\",\"name\":\"archbird\",\"version\":\"" ARCHBIRD_VERSION "\"}}");
  ab_string_free(pub->engine, &sources_json);
  return status;
}

ArchbirdStatus ab_okf_pub_finish(AbOkfPublication *pub, AbBuffer *out) {
  size_t provisional_count;
  size_t index;
  const AbOkfPubConcept *integrity;
  ArchbirdStatus status;
  if (!pub || !pub->engine || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_build_identity_validate(pub->engine);
  if (status == ARCHBIRD_OK)
    status = add_source_page(pub);
  if (status == ARCHBIRD_OK)
    status = materialize_without_integrity(pub);
  provisional_count = pub->file_count;
  if (status == ARCHBIRD_OK)
    status = add_integrity_concept(pub, provisional_count);
  if (status == ARCHBIRD_OK)
    status = materialize_without_integrity(pub);
  if (status == ARCHBIRD_OK)
    status = remove_integrity_concept(pub);
  if (status == ARCHBIRD_OK) {
    ab_string_free(pub->engine, &pub->content_sha256);
    status = add_integrity_concept(pub, provisional_count);
  }
  integrity = ab_okf_pub_find_concept(pub, "provenance/integrity.md");
  if (status == ARCHBIRD_OK && !integrity)
    status = ab_okf_pub_error(pub, "missing OKF integrity concept");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_add_file(pub, integrity->path.data,
                                 (const uint8_t *)integrity->text.data,
                                 integrity->text.length);
  if (status == ARCHBIRD_OK && pub->file_count != provisional_count + 1)
    status = ab_okf_pub_error(pub, "unstable OKF integrity inventory");
  if (status == ARCHBIRD_OK)
    status = digest_entries(pub, pub->files, pub->file_count,
                            &pub->aggregate_sha256);
  if (status == ARCHBIRD_OK)
    status = render_bundle(pub, out);
  for (index = 1; status == ARCHBIRD_OK && index < pub->file_count; index++)
    if (ab_string_compare(&pub->files[index - 1].path,
                          &pub->files[index].path) >= 0)
      status = ab_okf_pub_error(pub, "unsorted or duplicate OKF output files");
  return status;
}
