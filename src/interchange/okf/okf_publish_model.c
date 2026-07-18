#include "okf_publish_internal.h"

#include <stdlib.h>
#include <string.h>

#define MODEL_TRY(expression)                                                  \
  do {                                                                         \
    ArchbirdStatus model_status_ = (expression);                               \
    if (model_status_ != ARCHBIRD_OK)                                          \
      return model_status_;                                                    \
  } while (0)

static int field_compare(const void *left, const void *right) {
  return strcmp(((const AbOkfPubField *)left)->name,
                ((const AbOkfPubField *)right)->name);
}

static int string_compare(const void *left, const void *right) {
  return ab_string_compare((const AbString *)left, (const AbString *)right);
}

static int concept_compare(const void *left, const void *right) {
  return ab_string_compare(&((const AbOkfPubConcept *)left)->path,
                           &((const AbOkfPubConcept *)right)->path);
}

static int file_compare(const void *left, const void *right) {
  return ab_string_compare(&((const AbOkfPubFile *)left)->path,
                           &((const AbOkfPubFile *)right)->path);
}

static int valid_path(const char *path, int concept) {
  const char *cursor;
  const char *last;
  if (!path || !*path || *path == '/' || strchr(path, '\\'))
    return 0;
  cursor = path;
  while (*cursor) {
    const char *end = strchr(cursor, '/');
    size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
    if (!length || (length == 1 && cursor[0] == '.') ||
        (length == 2 && cursor[0] == '.' && cursor[1] == '.'))
      return 0;
    cursor = end ? end + 1 : cursor + length;
  }
  last = strrchr(path, '/');
  last = last ? last + 1 : path;
  if (strlen(last) < 3 || strcmp(last + strlen(last) - 3, ".md"))
    return 0;
  if (concept && (!strcmp(last, "index.md") || !strcmp(last, "log.md")))
    return 0;
  return 1;
}

static ArchbirdStatus render_object(AbOkfPublication *pub,
                                    const AbOkfPubField *fields,
                                    size_t field_count, AbString *out) {
  AbOkfPubField *ordered = NULL;
  AbBuffer buffer;
  size_t index;
  ArchbirdStatus status;
  if (field_count) {
    ordered =
        (AbOkfPubField *)ab_calloc(pub->engine, field_count, sizeof(*ordered));
    if (!ordered)
      return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory rendering OKF metadata");
    memcpy(ordered, fields, field_count * sizeof(*ordered));
    if (field_count > 1)
      qsort(ordered, field_count, sizeof(*ordered), field_compare);
    for (index = 1; index < field_count; index++) {
      if (!strcmp(ordered[index - 1].name, ordered[index].name)) {
        ab_free(pub->engine, ordered);
        return ab_okf_pub_error(pub, "duplicate OKF metadata field");
      }
    }
  }
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_literal(&buffer, "{");
  for (index = 0; status == ARCHBIRD_OK && index < field_count; index++) {
    if (index)
      status = ab_buffer_literal(&buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&buffer, ordered[index].name,
                                     strlen(ordered[index].name));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, ":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&buffer, ordered[index].json.data,
                                ordered[index].json.length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "}");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  ab_free(pub->engine, ordered);
  return status;
}

static ArchbirdStatus render_relations(AbOkfPublication *pub,
                                       const AbOkfPubRelationList *relations,
                                       AbString *out) {
  AbBuffer buffer;
  size_t index;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_literal(&buffer, "[");
  for (index = 0;
       status == ARCHBIRD_OK && relations && index < relations->count;
       index++) {
    if (index)
      status = ab_buffer_literal(&buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&buffer, relations->items[index].json.data,
                                relations->items[index].json.length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "]");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus render_entity(AbOkfPublication *pub, const char *kind,
                                    const AbString *id, AbString *out) {
  AbOkfPubField fields[2] = {{"id", {0}}, {"kind", {0}}};
  ArchbirdStatus status = ab_okf_pub_json_text(pub, &fields[0].json, id);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_literal(pub, &fields[1].json, kind);
  if (status == ARCHBIRD_OK)
    status = render_object(pub, fields, 2, out);
  ab_okf_pub_fields_free(pub, fields, 2);
  return status;
}

static ArchbirdStatus render_producer(AbOkfPublication *pub, AbString *out) {
  AbOkfPubField fields[3] = {
      {"implementation_sha256", {0}}, {"name", {0}}, {"version", {0}}};
  ArchbirdStatus status = ab_okf_pub_json_literal(
      pub, &fields[0].json, ARCHBIRD_IMPLEMENTATION_SHA256);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_literal(pub, &fields[1].json, "archbird");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_literal(pub, &fields[2].json, ARCHBIRD_VERSION);
  if (status == ARCHBIRD_OK)
    status = render_object(pub, fields, 3, out);
  ab_okf_pub_fields_free(pub, fields, 3);
  return status;
}

static ArchbirdStatus render_source(AbOkfPublication *pub,
                                    const AbOkfPubSource *source,
                                    AbString *out) {
  AbOkfPubField fields[6] = {{"artifact", {0}},       {"evidence_sha256", {0}},
                             {"file_sha256", {0}},    {"producer", {0}},
                             {"schema_version", {0}}, {NULL, {0}}};
  ArchbirdStatus status =
      ab_okf_pub_json_literal(pub, &fields[0].json, source->artifact);
  if (status == ARCHBIRD_OK)
    status =
        ab_okf_pub_json_text(pub, &fields[1].json, &source->evidence_sha256);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_literal(pub, &fields[2].json, source->file_sha256);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_value(pub, &fields[3].json, source->tool);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_u64(pub, &fields[4].json, source->schema_version);
  if (status == ARCHBIRD_OK)
    status = render_object(pub, fields, 5, out);
  ab_okf_pub_fields_free(pub, fields, 5);
  return status;
}

static ArchbirdStatus render_metadata(AbOkfPublication *pub,
                                      const AbOkfConceptSpec *spec,
                                      AbString *out) {
  AbOkfPubField *fields;
  size_t base_count = 6;
  size_t count = base_count + spec->extra_count;
  ArchbirdStatus status;
  fields = (AbOkfPubField *)ab_calloc(pub->engine, count, sizeof(*fields));
  if (!fields)
    return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory rendering OKF extension");
  fields[0].name = "entity";
  fields[1].name = "extension_version";
  fields[2].name = "producer";
  fields[3].name = "provenance";
  fields[4].name = "relations";
  fields[5].name = "source";
  status =
      render_entity(pub, spec->entity_kind, spec->entity_id, &fields[0].json);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_u64(pub, &fields[1].json, 1);
  if (status == ARCHBIRD_OK)
    status = render_producer(pub, &fields[2].json);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_literal(pub, &fields[3].json, spec->provenance);
  if (status == ARCHBIRD_OK)
    status = render_relations(pub, spec->relations, &fields[4].json);
  if (status == ARCHBIRD_OK)
    status = render_source(pub, spec->source, &fields[5].json);
  if (status == ARCHBIRD_OK && spec->extra_count)
    memcpy(fields + base_count, spec->extra,
           spec->extra_count * sizeof(*fields));
  if (status == ARCHBIRD_OK)
    status = render_object(pub, fields, count, out);
  ab_okf_pub_fields_free(pub, fields, base_count);
  ab_free(pub->engine, fields);
  return status;
}

static ArchbirdStatus render_tags(AbOkfPublication *pub,
                                  const AbOkfConceptSpec *spec, AbString *out) {
  AbString *tags;
  size_t count = spec->tag_count + 2;
  size_t index;
  size_t written = 0;
  AbBuffer buffer;
  ArchbirdStatus status = ARCHBIRD_OK;
  tags = (AbString *)ab_calloc(pub->engine, count, sizeof(*tags));
  if (!tags)
    return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory rendering OKF tags");
  tags[written++] = (AbString){(char *)"archbird", 8};
  tags[written++] =
      (AbString){(char *)spec->provenance, strlen(spec->provenance)};
  for (index = 0; index < spec->tag_count; index++)
    if (spec->tags[index].length)
      tags[written++] = spec->tags[index];
  if (written > 1)
    qsort(tags, written, sizeof(*tags), string_compare);
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_literal(&buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < written; index++) {
    if (index && ab_string_equal(&tags[index - 1], &tags[index]))
      continue;
    if (buffer.length > 1)
      status = ab_buffer_literal(&buffer, ",");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(&buffer, tags[index].data, tags[index].length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "]");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  ab_free(pub->engine, tags);
  return status;
}

static ArchbirdStatus render_resource(AbOkfPublication *pub,
                                      const AbOkfConceptSpec *spec,
                                      AbString *out) {
  char digest[65];
  AbBuffer buffer;
  ArchbirdStatus status = ab_okf_pub_sha256(
      (const uint8_t *)spec->entity_id->data, spec->entity_id->length, digest);
  ab_buffer_init(&buffer, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "urn:archbird:");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, spec->entity_kind);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, spec->source->evidence_sha256.data,
                              spec->source->evidence_sha256.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, digest, 24);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus reserve_concepts(AbOkfPublication *pub, size_t count) {
  size_t capacity = pub->concept_capacity ? pub->concept_capacity : 32;
  AbOkfPubConcept *resized;
  if (count <= pub->concept_capacity)
    return ARCHBIRD_OK;
  while (capacity < count) {
    if (capacity > SIZE_MAX / 2)
      return archbird_error_set(pub->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET, "too many OKF concepts");
    capacity *= 2;
  }
  if (capacity > pub->engine->options.max_values ||
      capacity > SIZE_MAX / sizeof(*resized))
    return archbird_error_set(pub->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET, "too many OKF concepts");
  resized = (AbOkfPubConcept *)ab_realloc(pub->engine, pub->concepts,
                                          capacity * sizeof(*resized));
  if (!resized)
    return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory growing OKF concepts");
  memset(resized + pub->concept_capacity, 0,
         (capacity - pub->concept_capacity) * sizeof(*resized));
  pub->concepts = resized;
  pub->concept_capacity = capacity;
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_okf_pub_add_concept(AbOkfPublication *pub,
                                      const AbOkfConceptSpec *spec) {
  AbOkfPubConcept *concept;
  AbString metadata = {0};
  AbString tags = {0};
  AbString resource = {0};
  AbString body = {0};
  AbBuffer text;
  size_t index;
  ArchbirdStatus status;
  if (!pub || !spec || !spec->source || !spec->path || !spec->type_name ||
      !spec->title || !spec->description || !spec->provenance ||
      !spec->entity_kind || !spec->entity_id || !spec->body ||
      !valid_path(spec->path, 1))
    return ARCHBIRD_INVALID_ARGUMENT;
  for (index = 0; index < pub->concept_count; index++)
    if (pub->concepts[index].path.length == strlen(spec->path) &&
        !memcmp(pub->concepts[index].path.data, spec->path,
                pub->concepts[index].path.length))
      return ab_okf_pub_error(pub, "OKF concept path collision");
  status = reserve_concepts(pub, pub->concept_count + 1);
  if (status != ARCHBIRD_OK)
    return status;
  concept = &pub->concepts[pub->concept_count];
  status = ab_okf_pub_literal(pub, &concept->path, spec->path);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_literal(pub, &concept->type_name, spec->type_name);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_one_line(pub, spec->title, &concept->title);
  if (status == ARCHBIRD_OK)
    status =
        spec->sort_key
            ? ab_okf_pub_copy(pub, &concept->sort_key, spec->sort_key->data,
                              spec->sort_key->length)
            : ab_okf_pub_sort_key(pub, &concept->title, &concept->sort_key);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_one_line(pub, spec->description, &concept->description);
  if (status == ARCHBIRD_OK)
    status = render_metadata(pub, spec, &metadata);
  if (status == ARCHBIRD_OK)
    status = render_tags(pub, spec, &tags);
  if (status == ARCHBIRD_OK)
    status = render_resource(pub, spec, &resource);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_body(pub, spec->body, &body);
  ab_buffer_init(&text, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&text, "---\ntype: ");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_json_string(&text, spec->type_name, strlen(spec->type_name));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&text, "\ntitle: ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&text, concept->title.data,
                                   concept->title.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&text, "\ndescription: ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&text, concept->description.data,
                                   concept->description.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&text, "\nresource: ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&text, resource.data, resource.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&text, "\ntags: ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&text, tags.data, tags.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&text, "\narchbird: ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&text, metadata.data, metadata.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&text, "\n---\n");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&text, body.data, body.length);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &text, &concept->text);
  ab_buffer_free(&text);
  ab_string_free(pub->engine, &metadata);
  ab_string_free(pub->engine, &tags);
  ab_string_free(pub->engine, &resource);
  ab_string_free(pub->engine, &body);
  if (status != ARCHBIRD_OK) {
    ab_string_free(pub->engine, &concept->path);
    ab_string_free(pub->engine, &concept->type_name);
    ab_string_free(pub->engine, &concept->title);
    ab_string_free(pub->engine, &concept->sort_key);
    ab_string_free(pub->engine, &concept->description);
    ab_string_free(pub->engine, &concept->text);
    memset(concept, 0, sizeof(*concept));
    return status;
  }
  pub->concept_count++;
  if (pub->concept_count > 1)
    qsort(pub->concepts, pub->concept_count, sizeof(*pub->concepts),
          concept_compare);
  return ARCHBIRD_OK;
}

const AbOkfPubConcept *ab_okf_pub_find_concept(const AbOkfPublication *pub,
                                               const char *path) {
  AbOkfPubConcept key = {0};
  if (!pub || !pub->concept_count || !path)
    return NULL;
  key.path.data = (char *)path;
  key.path.length = strlen(path);
  return (const AbOkfPubConcept *)bsearch(
      &key, pub->concepts, pub->concept_count, sizeof(*pub->concepts),
      concept_compare);
}

static ArchbirdStatus reserve_files(AbOkfPublication *pub, size_t count) {
  size_t capacity = pub->file_capacity ? pub->file_capacity : 32;
  AbOkfPubFile *resized;
  if (count <= pub->file_capacity)
    return ARCHBIRD_OK;
  while (capacity < count) {
    if (capacity > SIZE_MAX / 2)
      return archbird_error_set(pub->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET, "too many OKF files");
    capacity *= 2;
  }
  if (capacity > pub->engine->options.max_values ||
      capacity > SIZE_MAX / sizeof(*resized))
    return archbird_error_set(pub->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET, "too many OKF files");
  resized = (AbOkfPubFile *)ab_realloc(pub->engine, pub->files,
                                       capacity * sizeof(*resized));
  if (!resized)
    return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory growing OKF files");
  memset(resized + pub->file_capacity, 0,
         (capacity - pub->file_capacity) * sizeof(*resized));
  pub->files = resized;
  pub->file_capacity = capacity;
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_okf_pub_add_file(AbOkfPublication *pub, const char *path,
                                   const uint8_t *text, size_t length) {
  AbOkfPubFile *file;
  size_t index;
  ArchbirdStatus status;
  if (!pub || !valid_path(path, 0) || (!text && length))
    return ARCHBIRD_INVALID_ARGUMENT;
  for (index = 0; index < pub->file_count; index++)
    if (pub->files[index].path.length == strlen(path) &&
        !memcmp(pub->files[index].path.data, path,
                pub->files[index].path.length))
      return ab_okf_pub_error(pub, "duplicate OKF output path");
  status = reserve_files(pub, pub->file_count + 1);
  if (status != ARCHBIRD_OK)
    return status;
  file = &pub->files[pub->file_count];
  status = ab_okf_pub_literal(pub, &file->path, path);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_copy(pub, &file->text, (const char *)text, length);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_sha256(text, length, file->sha256);
  if (status != ARCHBIRD_OK) {
    ab_string_free(pub->engine, &file->path);
    ab_string_free(pub->engine, &file->text);
    memset(file, 0, sizeof(*file));
    return status;
  }
  pub->file_count++;
  if (pub->file_count > 1)
    qsort(pub->files, pub->file_count, sizeof(*pub->files), file_compare);
  return ARCHBIRD_OK;
}
