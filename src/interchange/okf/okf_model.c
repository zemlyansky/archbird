#include "okf_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OKF_TRY(expression)                                                    \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

ArchbirdStatus ab_okf_error(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "OKF input: %s", message);
}

int ab_okf_text_is(const AbValue *value, const char *literal) {
  return value && value->kind == AB_VALUE_STRING &&
         value->as.text.length == strlen(literal) &&
         !memcmp(value->as.text.data, literal, value->as.text.length);
}

const AbString *ab_okf_optional_text(const AbValue *object, const char *name) {
  const AbValue *value = ab_value_member(object, name);
  return value && value->kind == AB_VALUE_STRING ? &value->as.text : NULL;
}

ArchbirdStatus ab_okf_copy_text(ArchbirdEngine *engine, AbString *out,
                                const AbString *source) {
  return ab_string_copy(engine, out, source ? source->data : "",
                        source ? source->length : 0);
}

ArchbirdStatus ab_okf_copy_literal(ArchbirdEngine *engine, AbString *out,
                                   const char *literal) {
  return ab_string_copy(engine, out, literal, strlen(literal));
}

static int lowercase_sha256(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || value->as.text.length != 64)
    return 0;
  for (index = 0; index < 64; index++) {
    char byte = value->as.text.data[index];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return 0;
  }
  return 1;
}

static int path_valid(const AbString *path) {
  size_t start = 0;
  size_t index;
  if (!path || !path->length || path->data[0] == '/' ||
      (path->length >= 2 && isalpha((unsigned char)path->data[0]) &&
       path->data[1] == ':'))
    return 0;
  for (index = 0; index <= path->length; index++) {
    if (index < path->length && path->data[index] == '\\')
      return 0;
    if (index == path->length || path->data[index] == '/') {
      size_t length = index - start;
      if (!length || (length == 1 && path->data[start] == '.') ||
          (length == 2 && path->data[start] == '.' &&
           path->data[start + 1] == '.'))
        return 0;
      start = index + 1;
    }
  }
  return path->length >= 3 && path->data[path->length - 3] == '.' &&
         (path->data[path->length - 2] == 'm' ||
          path->data[path->length - 2] == 'M') &&
         (path->data[path->length - 1] == 'd' ||
          path->data[path->length - 1] == 'D');
}

static int string_qsort(const void *left, const void *right) {
  return ab_string_compare((const AbString *)left, (const AbString *)right);
}

static int document_compare(const void *left, const void *right) {
  return ab_string_compare(&((const AbOkfDocument *)left)->path,
                           &((const AbOkfDocument *)right)->path);
}

static int diagnostic_compare(const void *left, const void *right) {
  const AbOkfDiagnostic *a = (const AbOkfDiagnostic *)left;
  const AbOkfDiagnostic *b = (const AbOkfDiagnostic *)right;
  int compared = ab_string_compare(&a->severity, &b->severity);
  if (!compared)
    compared = ab_string_compare(&a->code, &b->code);
  if (!compared)
    compared = ab_string_compare(&a->message, &b->message);
  return compared ? compared : ab_string_compare(&a->path, &b->path);
}

static int link_compare(const void *left, const void *right) {
  const AbOkfLink *a = (const AbOkfLink *)left;
  const AbOkfLink *b = (const AbOkfLink *)right;
  int compared = ab_string_compare(&a->label, &b->label);
  if (!compared)
    compared = ab_string_compare(&a->href, &b->href);
  if (!compared)
    compared = ab_string_compare(&a->target, &b->target);
  if (!compared)
    compared = ab_string_compare(&a->kind, &b->kind);
  return compared ? compared : ab_string_compare(&a->state, &b->state);
}

static int requirement_compare(const void *left, const void *right) {
  const AbOkfRequirementLink *a = (const AbOkfRequirementLink *)left;
  const AbOkfRequirementLink *b = (const AbOkfRequirementLink *)right;
  int compared = ab_string_compare(&a->requirement_id, &b->requirement_id);
  if (!compared)
    compared = ab_string_compare(&a->concept_id, &b->concept_id);
  if (!compared)
    compared = ab_string_compare(&a->source, &b->source);
  return compared ? compared
                  : ab_string_compare(&a->target_concept, &b->target_concept);
}

static void diagnostic_free(ArchbirdEngine *engine, AbOkfDiagnostic *row) {
  ab_string_free(engine, &row->severity);
  ab_string_free(engine, &row->code);
  ab_string_free(engine, &row->message);
  ab_string_free(engine, &row->path);
}

static ArchbirdStatus diagnostic_add(AbOkfIndex *index,
                                     const AbString *severity,
                                     const AbString *code,
                                     const AbString *message,
                                     const AbString *path) {
  AbOkfDiagnostic row = {0};
  AbOkfDiagnostic *resized;
  size_t capacity;
  ArchbirdStatus status =
      ab_okf_copy_text(index->engine, &row.severity, severity);
  if (status == ARCHBIRD_OK)
    status = ab_okf_copy_text(index->engine, &row.code, code);
  if (status == ARCHBIRD_OK)
    status = ab_okf_copy_text(index->engine, &row.message, message);
  if (status == ARCHBIRD_OK)
    status = ab_okf_copy_text(index->engine, &row.path, path);
  if (status != ARCHBIRD_OK) {
    diagnostic_free(index->engine, &row);
    return status;
  }
  if (index->diagnostic_count == index->diagnostic_capacity) {
    capacity = index->diagnostic_capacity ? index->diagnostic_capacity * 2 : 8;
    if (capacity < index->diagnostic_capacity ||
        capacity > SIZE_MAX / sizeof(*resized)) {
      diagnostic_free(index->engine, &row);
      return archbird_error_set(index->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing OKF diagnostics");
    }
    resized = (AbOkfDiagnostic *)ab_realloc(index->engine, index->diagnostics,
                                            capacity * sizeof(*resized));
    if (!resized) {
      diagnostic_free(index->engine, &row);
      return archbird_error_set(index->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing OKF diagnostics");
    }
    index->diagnostics = resized;
    index->diagnostic_capacity = capacity;
  }
  index->diagnostics[index->diagnostic_count++] = row;
  return ARCHBIRD_OK;
}

static ArchbirdStatus diagnostic_literal(AbOkfIndex *index,
                                         const char *severity, const char *code,
                                         const char *message,
                                         const AbString *path) {
  AbString a = {(char *)severity, strlen(severity)};
  AbString b = {(char *)code, strlen(code)};
  AbString c = {(char *)message, strlen(message)};
  AbString empty = {NULL, 0};
  return diagnostic_add(index, &a, &b, &c, path ? path : &empty);
}

static ArchbirdStatus diagnostic_join(AbOkfIndex *index, const char *severity,
                                      const char *code, const char *prefix,
                                      const AbString *suffix,
                                      const AbString *path) {
  AbBuffer buffer;
  AbString message = {0};
  ArchbirdStatus status;
  ab_buffer_init(&buffer, index->engine);
  status = ab_buffer_literal(&buffer, prefix);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, suffix->data, suffix->length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(index->engine, &message, (const char *)buffer.data,
                            buffer.length);
  if (status == ARCHBIRD_OK) {
    AbString a = {(char *)severity, strlen(severity)};
    AbString b = {(char *)code, strlen(code)};
    status = diagnostic_add(index, &a, &b, &message, path);
  }
  ab_string_free(index->engine, &message);
  ab_buffer_free(&buffer);
  return status;
}

static int value_truthy(const AbValue *value) {
  if (!value || value->kind == AB_VALUE_NULL)
    return 0;
  if (value->kind == AB_VALUE_BOOL)
    return value->as.boolean;
  if (value->kind == AB_VALUE_INTEGER)
    return !(value->as.text.length == 1 && value->as.text.data[0] == '0');
  if (value->kind == AB_VALUE_REAL)
    return value->as.real != 0.0;
  if (value->kind == AB_VALUE_STRING)
    return value->as.text.length != 0;
  if (value->kind == AB_VALUE_ARRAY)
    return value->as.array.count != 0;
  return value->as.object.count != 0;
}

static ArchbirdStatus string_array_add(ArchbirdEngine *engine, AbString **rows,
                                       size_t *count, const AbString *value) {
  AbString *resized;
  if (!value || !value->length)
    return ARCHBIRD_OK;
  if (*count == SIZE_MAX / sizeof(**rows))
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory indexing OKF strings");
  resized =
      (AbString *)ab_realloc(engine, *rows, (*count + 1) * sizeof(**rows));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory indexing OKF strings");
  *rows = resized;
  memset(&(*rows)[*count], 0, sizeof(**rows));
  OKF_TRY(ab_okf_copy_text(engine, &(*rows)[*count], value));
  (*count)++;
  return ARCHBIRD_OK;
}

static void string_array_finish(ArchbirdEngine *engine, AbString *rows,
                                size_t *count) {
  size_t read;
  size_t write = 0;
  if (*count > 1)
    qsort(rows, *count, sizeof(*rows), string_qsort);
  for (read = 0; read < *count; read++) {
    if (write && ab_string_equal(&rows[write - 1], &rows[read])) {
      ab_string_free(engine, &rows[read]);
      continue;
    }
    if (write != read) {
      rows[write] = rows[read];
      memset(&rows[read], 0, sizeof(rows[read]));
    }
    write++;
  }
  *count = write;
}

static void string_array_free(ArchbirdEngine *engine, AbString *rows,
                              size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    ab_string_free(engine, &rows[index]);
  ab_free(engine, rows);
}

static ArchbirdStatus explicit_value(AbOkfIndex *index, AbOkfDocument *document,
                                     const AbValue *value) {
  const AbString *identity = NULL;
  if (value && value->kind == AB_VALUE_STRING && value->as.text.length)
    identity = &value->as.text;
  else if (value && value->kind == AB_VALUE_OBJECT)
    identity = ab_okf_optional_text(value, "id");
  return string_array_add(index->engine, &document->explicit_requirements,
                          &document->explicit_requirement_count, identity);
}

static ArchbirdStatus document_requirements(AbOkfIndex *index,
                                            AbOkfDocument *document) {
  const AbValue *extension = ab_value_member(document->frontmatter, "archbird");
  const AbValue *entity;
  const AbValue *requirements;
  const AbValue *single;
  size_t item;
  if (!extension)
    return ARCHBIRD_OK;
  if (extension->kind != AB_VALUE_OBJECT)
    return diagnostic_literal(
        index, "warning", "invalid-archbird-extension",
        "archbird metadata is not an object; no requirements were linked",
        &document->path);
  entity = ab_value_member(extension, "entity");
  if (entity && entity->kind == AB_VALUE_OBJECT &&
      ab_okf_text_is(ab_value_member(entity, "kind"), "requirement"))
    OKF_TRY(explicit_value(index, document, entity));
  single = ab_value_member(extension, "requirement");
  if (single)
    OKF_TRY(explicit_value(index, document, single));
  requirements = ab_value_member(extension, "requirements");
  if (requirements && requirements->kind == AB_VALUE_ARRAY) {
    for (item = 0; item < requirements->as.array.count; item++)
      OKF_TRY(
          explicit_value(index, document, &requirements->as.array.items[item]));
  } else if (value_truthy(requirements)) {
    OKF_TRY(diagnostic_literal(
        index, "warning", "invalid-requirements-extension",
        "archbird.requirements must be a list", &document->path));
  }
  string_array_finish(index->engine, document->explicit_requirements,
                      &document->explicit_requirement_count);
  return ARCHBIRD_OK;
}

static ArchbirdStatus document_tags(AbOkfIndex *index,
                                    AbOkfDocument *document) {
  const AbValue *tags = ab_value_member(document->frontmatter, "tags");
  size_t item;
  if (!tags || tags->kind == AB_VALUE_NULL)
    return ARCHBIRD_OK;
  if (tags->kind != AB_VALUE_ARRAY)
    return diagnostic_literal(index, "warning", "invalid-tags",
                              "tags must be a list of strings",
                              &document->path);
  for (item = 0; item < tags->as.array.count; item++) {
    const AbValue *value = &tags->as.array.items[item];
    if (value->kind != AB_VALUE_STRING || !value->as.text.length) {
      string_array_free(index->engine, document->tags, document->tag_count);
      document->tags = NULL;
      document->tag_count = 0;
      return diagnostic_literal(index, "warning", "invalid-tags",
                                "tags must be a list of strings",
                                &document->path);
    }
    OKF_TRY(string_array_add(index->engine, &document->tags,
                             &document->tag_count, &value->as.text));
  }
  string_array_finish(index->engine, document->tags, &document->tag_count);
  return ARCHBIRD_OK;
}

static int date_valid(const char *value, size_t length) {
  int year, month, day;
  int days;
  int leap;
  if (length != 10 || value[4] != '-' || value[7] != '-')
    return 0;
  if (!isdigit((unsigned char)value[0]) || !isdigit((unsigned char)value[1]) ||
      !isdigit((unsigned char)value[2]) || !isdigit((unsigned char)value[3]) ||
      !isdigit((unsigned char)value[5]) || !isdigit((unsigned char)value[6]) ||
      !isdigit((unsigned char)value[8]) || !isdigit((unsigned char)value[9]))
    return 0;
  year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 +
         (value[2] - '0') * 10 + value[3] - '0';
  month = (value[5] - '0') * 10 + value[6] - '0';
  day = (value[8] - '0') * 10 + value[9] - '0';
  if (year < 1 || month < 1 || month > 12 || day < 1)
    return 0;
  leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  days = month == 2                                                ? 28 + leap
         : (month == 4 || month == 6 || month == 9 || month == 11) ? 30
                                                                   : 31;
  return day <= days;
}

static ArchbirdStatus document_structure(AbOkfIndex *index,
                                         AbOkfDocument *document) {
  const char *cursor = document->body->data;
  const char *end = cursor + document->body->length;
  int title = 0;
  int heading = 0;
  int dates = 0;
  char previous[11] = {0};
  int order_bad = 0;
  while (cursor < end) {
    const char *line_end = memchr(cursor, '\n', (size_t)(end - cursor));
    size_t length =
        line_end ? (size_t)(line_end - cursor) : (size_t)(end - cursor);
    if (length && cursor[length - 1] == '\r')
      length--;
    if (length >= 2 && cursor[0] == '#' && cursor[1] == ' ')
      title = 1;
    if (length && cursor[0] == '#') {
      size_t hashes = 0;
      while (hashes < length && cursor[hashes] == '#')
        hashes++;
      if (hashes < length && cursor[hashes] == ' ')
        heading = 1;
    }
    if (document->kind.length == 3 && !memcmp(document->kind.data, "log", 3) &&
        length >= 3 && cursor[0] == '#' && cursor[1] == '#' &&
        cursor[2] == ' ') {
      const char *value = cursor + 3;
      size_t value_length = length - 3;
      while (value_length && value[0] == ' ') {
        value++;
        value_length--;
      }
      while (value_length && value[value_length - 1] == ' ')
        value_length--;
      if (!date_valid(value, value_length)) {
        OKF_TRY(diagnostic_literal(index, "error", "invalid-log-date",
                                   "level-two log headings must use YYYY-MM-DD",
                                   &document->path));
      } else {
        if (dates && memcmp(previous, value, 10) < 0)
          order_bad = 1;
        memcpy(previous, value, 10);
        previous[10] = '\0';
        dates++;
      }
    }
    cursor = line_end ? line_end + 1 : end;
  }
  if (document->kind.length == 3 && !memcmp(document->kind.data, "log", 3)) {
    if (!title || !dates)
      OKF_TRY(diagnostic_literal(
          index, "error", "invalid-log-structure",
          "log.md requires a title and at least one YYYY-MM-DD section",
          &document->path));
    else if (order_bad)
      OKF_TRY(diagnostic_literal(index, "error", "invalid-log-order",
                                 "log.md date sections must be newest first",
                                 &document->path));
  } else if (document->kind.length == 5 &&
             !memcmp(document->kind.data, "index", 5) && !heading) {
    OKF_TRY(diagnostic_literal(index, "error", "invalid-index-structure",
                               "index.md requires at least one section heading",
                               &document->path));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus document_init(AbOkfIndex *index, AbOkfDocument *document,
                                    const AbValue *source) {
  const AbValue *frontmatter = ab_value_member(source, "frontmatter");
  const AbValue *body = ab_value_member(source, "body");
  const AbValue *casefold = ab_value_member(source, "casefold");
  const AbValue *links = ab_value_member(source, "links");
  const AbValue *path = ab_value_member(source, "path");
  const AbValue *sha256 = ab_value_member(source, "sha256");
  const AbValue *folded_tags;
  const AbString *value;
  const char *base;
  size_t base_length;
  if (!source || source->kind != AB_VALUE_OBJECT || !path ||
      path->kind != AB_VALUE_STRING || !path_valid(&path->as.text) ||
      !lowercase_sha256(sha256) || !frontmatter ||
      frontmatter->kind != AB_VALUE_OBJECT || !body ||
      body->kind != AB_VALUE_STRING || !casefold ||
      casefold->kind != AB_VALUE_OBJECT || !links ||
      links->kind != AB_VALUE_ARRAY)
    return ab_okf_error(index->engine, "invalid current document");
  value = ab_okf_optional_text(casefold, "type");
  document->folded_text = ab_okf_optional_text(casefold, "text");
  folded_tags = ab_value_member(casefold, "tags");
  if (!value || !document->folded_text || !folded_tags ||
      folded_tags->kind != AB_VALUE_ARRAY)
    return ab_okf_error(index->engine, "invalid document casefold evidence");
  {
    size_t folded_tag;
    for (folded_tag = 0; folded_tag < folded_tags->as.array.count; folded_tag++)
      if (folded_tags->as.array.items[folded_tag].kind != AB_VALUE_STRING)
        return ab_okf_error(index->engine,
                            "invalid document folded tag evidence");
  }
  document->source = source;
  document->frontmatter = frontmatter;
  document->body = &body->as.text;
  document->folded_type = value;
  document->folded_tags = folded_tags;
  OKF_TRY(ab_okf_copy_text(index->engine, &document->path, &path->as.text));
  OKF_TRY(ab_okf_copy_text(index->engine, &document->sha256, &sha256->as.text));
  OKF_TRY(ab_string_copy(index->engine, &document->concept_id,
                         path->as.text.data, path->as.text.length - 3));
  base = path->as.text.data;
  base_length = path->as.text.length;
  while (base_length && base[base_length - 1] != '/')
    base_length--;
  base += base_length;
  base_length = path->as.text.length - base_length;
  if ((base_length == 8 && !memcmp(base, "index.md", 8)) ||
      (base_length == 6 && !memcmp(base, "log.md", 6)))
    OKF_TRY(
        ab_string_copy(index->engine, &document->kind, base, base_length - 3));
  else
    OKF_TRY(ab_okf_copy_literal(index->engine, &document->kind, "concept"));
  value = ab_okf_optional_text(frontmatter, "type");
  if (document->kind.length == 7 &&
      !memcmp(document->kind.data, "concept", 7) &&
      (!value || !value->length)) {
    OKF_TRY(diagnostic_literal(index, "error", "missing-type",
                               "frontmatter type is required",
                               &document->path));
    value = NULL;
  }
  OKF_TRY(ab_okf_copy_text(index->engine, &document->type_name, value));
  OKF_TRY(ab_okf_copy_text(index->engine, &document->title,
                           ab_okf_optional_text(frontmatter, "title")));
  OKF_TRY(ab_okf_copy_text(index->engine, &document->description,
                           ab_okf_optional_text(frontmatter, "description")));
  OKF_TRY(ab_okf_copy_text(index->engine, &document->resource,
                           ab_okf_optional_text(frontmatter, "resource")));
  OKF_TRY(document_tags(index, document));
  OKF_TRY(document_requirements(index, document));
  OKF_TRY(document_structure(index, document));
  return ARCHBIRD_OK;
}

static int known_path(const AbOkfIndex *index, const AbString *path) {
  return index->known_path_count &&
         bsearch(path, index->known_paths, index->known_path_count,
                 sizeof(*index->known_paths), string_qsort) != NULL;
}

static ArchbirdStatus normalize_target(AbOkfIndex *index,
                                       const AbOkfDocument *document,
                                       const AbString *raw, AbString *target,
                                       int *unsafe) {
  const char *parts[512];
  size_t lengths[512];
  size_t count = 0;
  char *candidate;
  size_t candidate_length = 0;
  size_t prefix = 0;
  size_t cursor;
  size_t start;
  int trailing = raw->length && raw->data[raw->length - 1] == '/';
  AbBuffer output;
  ArchbirdStatus status = ARCHBIRD_OK;
  *unsafe = 0;
  if (memchr(raw->data, '\\', raw->length) ||
      memchr(raw->data, '\0', raw->length)) {
    *unsafe = 1;
    return ARCHBIRD_OK;
  }
  if (raw->length && raw->data[0] == '/') {
    while (prefix < raw->length && raw->data[prefix] == '/')
      prefix++;
  } else {
    const char *slash = NULL;
    size_t index_value;
    for (index_value = 0; index_value < document->path.length; index_value++)
      if (document->path.data[index_value] == '/')
        slash = document->path.data + index_value;
    if (slash)
      candidate_length = (size_t)(slash - document->path.data) + 1;
  }
  if (candidate_length > SIZE_MAX - (raw->length - prefix) - 1)
    return archbird_error_set(index->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory resolving OKF link");
  candidate = (char *)ab_malloc(index->engine,
                                candidate_length + raw->length - prefix + 1);
  if (!candidate)
    return archbird_error_set(index->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory resolving OKF link");
  if (candidate_length)
    memcpy(candidate, document->path.data, candidate_length);
  memcpy(candidate + candidate_length, raw->data + prefix,
         raw->length - prefix);
  candidate_length += raw->length - prefix;
  candidate[candidate_length] = '\0';
  cursor = 0;
  while (cursor <= candidate_length) {
    start = cursor;
    while (cursor < candidate_length && candidate[cursor] != '/')
      cursor++;
    if (cursor > start && !(cursor - start == 1 && candidate[start] == '.')) {
      if (cursor - start == 2 && candidate[start] == '.' &&
          candidate[start + 1] == '.') {
        if (!count) {
          *unsafe = 1;
          break;
        }
        count--;
      } else {
        if (count == sizeof(parts) / sizeof(parts[0])) {
          ab_free(index->engine, candidate);
          return ab_okf_error(index->engine, "link path has too many segments");
        }
        parts[count] = candidate + start;
        lengths[count++] = cursor - start;
      }
    }
    cursor++;
  }
  if (!*unsafe) {
    ab_buffer_init(&output, index->engine);
    if (!count)
      status = ab_buffer_literal(&output, "index.md");
    else {
      size_t part;
      for (part = 0; status == ARCHBIRD_OK && part < count; part++) {
        if (part)
          status = ab_buffer_literal(&output, "/");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&output, parts[part], lengths[part]);
      }
    }
    if (status == ARCHBIRD_OK && trailing)
      status = ab_buffer_literal(&output, "/index.md");
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(index->engine, target, (const char *)output.data,
                              output.length);
    ab_buffer_free(&output);
  }
  ab_free(index->engine, candidate);
  return status;
}

static void link_free(ArchbirdEngine *engine, AbOkfLink *link) {
  ab_string_free(engine, &link->label);
  ab_string_free(engine, &link->href);
  ab_string_free(engine, &link->target);
  ab_string_free(engine, &link->kind);
  ab_string_free(engine, &link->state);
}

static ArchbirdStatus document_links(AbOkfIndex *index,
                                     AbOkfDocument *document) {
  const AbValue *rows = ab_value_member(document->source, "links");
  size_t item;
  if (rows->as.array.count) {
    document->links = (AbOkfLink *)ab_calloc(
        index->engine, rows->as.array.count, sizeof(*document->links));
    if (!document->links)
      return archbird_error_set(index->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing OKF links");
  }
  for (item = 0; item < rows->as.array.count; item++) {
    const AbValue *source = &rows->as.array.items[item];
    const AbString *label = ab_okf_optional_text(source, "label");
    const AbString *href = ab_okf_optional_text(source, "href");
    const AbString *repr = ab_okf_optional_text(source, "repr");
    const AbString *raw_path = ab_okf_optional_text(source, "path");
    const AbValue *external = ab_value_member(source, "external");
    const AbValue *fragment = ab_value_member(source, "fragment_only");
    AbOkfLink *link = &document->links[document->link_count];
    int unsafe = 0;
    if (source->kind != AB_VALUE_OBJECT || !label || !href || !repr ||
        !raw_path || !external || external->kind != AB_VALUE_BOOL ||
        !fragment || fragment->kind != AB_VALUE_BOOL)
      return ab_okf_error(index->engine, "invalid decoded Markdown link");
    OKF_TRY(ab_okf_copy_text(index->engine, &link->label, label));
    OKF_TRY(ab_okf_copy_text(index->engine, &link->href, href));
    if (external->as.boolean) {
      OKF_TRY(ab_okf_copy_literal(index->engine, &link->kind, "external"));
      OKF_TRY(ab_okf_copy_literal(index->engine, &link->state, "external"));
    } else if (fragment->as.boolean || !raw_path->length) {
      OKF_TRY(ab_okf_copy_literal(index->engine, &link->kind, "anchor"));
      OKF_TRY(ab_okf_copy_literal(index->engine, &link->state, "current"));
      OKF_TRY(ab_okf_copy_text(index->engine, &link->target, &document->path));
    } else {
      OKF_TRY(
          normalize_target(index, document, raw_path, &link->target, &unsafe));
      if (unsafe) {
        OKF_TRY(ab_okf_copy_literal(index->engine, &link->kind, "unsafe"));
        OKF_TRY(ab_okf_copy_literal(index->engine, &link->state, "unresolved"));
        OKF_TRY(diagnostic_join(index, "warning", "link-outside-bundle",
                                "link target is outside or invalid: ", repr,
                                &document->path));
      } else {
        OKF_TRY(ab_okf_copy_literal(index->engine, &link->kind, "internal"));
        if (known_path(index, &link->target))
          OKF_TRY(ab_okf_copy_literal(index->engine, &link->state, "current"));
        else {
          OKF_TRY(ab_okf_copy_literal(index->engine, &link->state, "broken"));
          OKF_TRY(diagnostic_join(index, "warning", "broken-link",
                                  "target does not exist in bundle: ", repr,
                                  &document->path));
        }
      }
    }
    document->link_count++;
  }
  if (document->link_count > 1)
    qsort(document->links, document->link_count, sizeof(*document->links),
          link_compare);
  if (document->link_count > 1) {
    size_t read;
    size_t write = 0;
    for (read = 0; read < document->link_count; read++) {
      if (write &&
          !link_compare(&document->links[write - 1], &document->links[read])) {
        link_free(index->engine, &document->links[read]);
        continue;
      }
      if (write != read) {
        document->links[write] = document->links[read];
        memset(&document->links[read], 0, sizeof(document->links[read]));
      }
      write++;
    }
    document->link_count = write;
  }
  return ARCHBIRD_OK;
}

static AbOkfDocument *concept_find(AbOkfIndex *index,
                                   const AbString *concept_id) {
  size_t first = 0;
  size_t length = index->document_count;
  while (length) {
    size_t half = length / 2;
    size_t middle = first + half;
    int compared =
        ab_string_compare(&index->documents[middle].concept_id, concept_id);
    if (compared < 0) {
      first = middle + 1;
      length -= half + 1;
    } else if (compared > 0) {
      length = half;
    } else {
      return &index->documents[middle];
    }
  }
  return NULL;
}

static ArchbirdStatus requirement_add(AbOkfIndex *index,
                                      const AbString *requirement,
                                      const AbString *concept,
                                      const char *source,
                                      const AbString *target) {
  AbOkfRequirementLink *resized = (AbOkfRequirementLink *)ab_realloc(
      index->engine, index->requirements,
      (index->requirement_count + 1) * sizeof(*index->requirements));
  AbOkfRequirementLink *row;
  ArchbirdStatus status;
  if (!resized)
    return archbird_error_set(index->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory indexing OKF requirements");
  index->requirements = resized;
  row = &index->requirements[index->requirement_count];
  memset(row, 0, sizeof(*row));
  status = ab_okf_copy_text(index->engine, &row->requirement_id, requirement);
  if (status == ARCHBIRD_OK)
    status = ab_okf_copy_text(index->engine, &row->concept_id, concept);
  if (status == ARCHBIRD_OK)
    status = ab_okf_copy_literal(index->engine, &row->source, source);
  if (status == ARCHBIRD_OK)
    status = ab_okf_copy_text(index->engine, &row->target_concept, target);
  if (status == ARCHBIRD_OK)
    index->requirement_count++;
  return status;
}

static ArchbirdStatus build_requirements(AbOkfIndex *index) {
  size_t document_index;
  for (document_index = 0; document_index < index->document_count;
       document_index++) {
    AbOkfDocument *document = &index->documents[document_index];
    const AbValue *extension =
        ab_value_member(document->frontmatter, "archbird");
    size_t requirement;
    for (requirement = 0; requirement < document->explicit_requirement_count;
         requirement++)
      OKF_TRY(requirement_add(index,
                              &document->explicit_requirements[requirement],
                              &document->concept_id, "explicit", NULL));
    if (extension && extension->kind == AB_VALUE_OBJECT) {
      const AbValue *relations = ab_value_member(extension, "relations");
      size_t relation;
      if (!relations || relations->kind != AB_VALUE_ARRAY)
        continue;
      for (relation = 0; relation < relations->as.array.count; relation++) {
        const AbValue *row = &relations->as.array.items[relation];
        const AbString *target = ab_okf_optional_text(row, "target");
        AbString normalized = {0};
        AbOkfDocument *target_document;
        size_t prefix = 0;
        size_t length;
        if (row->kind != AB_VALUE_OBJECT || !target)
          continue;
        while (prefix < target->length && target->data[prefix] == '/')
          prefix++;
        length = target->length - prefix;
        if (length >= 3 &&
            !memcmp(target->data + prefix + length - 3, ".md", 3))
          length -= 3;
        OKF_TRY(ab_string_copy(index->engine, &normalized,
                               target->data + prefix, length));
        target_document = concept_find(index, &normalized);
        if (target_document)
          for (requirement = 0;
               requirement < target_document->explicit_requirement_count;
               requirement++)
            OKF_TRY(requirement_add(
                index, &target_document->explicit_requirements[requirement],
                &document->concept_id, "typed_relation", &normalized));
        ab_string_free(index->engine, &normalized);
      }
    }
  }
  if (index->requirement_count > 1)
    qsort(index->requirements, index->requirement_count,
          sizeof(*index->requirements), requirement_compare);
  if (index->requirement_count > 1) {
    size_t read;
    size_t write = 0;
    for (read = 0; read < index->requirement_count; read++) {
      AbOkfRequirementLink *row = &index->requirements[read];
      if (write && !requirement_compare(&index->requirements[write - 1], row)) {
        ab_string_free(index->engine, &row->requirement_id);
        ab_string_free(index->engine, &row->concept_id);
        ab_string_free(index->engine, &row->source);
        ab_string_free(index->engine, &row->target_concept);
        continue;
      }
      if (write != read) {
        index->requirements[write] = *row;
        memset(row, 0, sizeof(*row));
      }
      write++;
    }
    index->requirement_count = write;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus load_known_paths(AbOkfIndex *index,
                                       const AbValue *known_paths) {
  size_t item;
  if (!known_paths || known_paths->kind != AB_VALUE_ARRAY ||
      known_paths->as.array.count > index->engine->options.max_files)
    return ab_okf_error(index->engine, "invalid known_paths inventory");
  if (known_paths->as.array.count) {
    index->known_paths =
        (AbString *)ab_calloc(index->engine, known_paths->as.array.count,
                              sizeof(*index->known_paths));
    if (!index->known_paths)
      return archbird_error_set(index->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing OKF paths");
  }
  for (item = 0; item < known_paths->as.array.count; item++) {
    const AbValue *value = &known_paths->as.array.items[item];
    if (value->kind != AB_VALUE_STRING || !path_valid(&value->as.text))
      return ab_okf_error(index->engine, "invalid known OKF path");
    OKF_TRY(ab_okf_copy_text(index->engine,
                             &index->known_paths[index->known_path_count],
                             &value->as.text));
    index->known_path_count++;
  }
  if (index->known_path_count > 1)
    qsort(index->known_paths, index->known_path_count,
          sizeof(*index->known_paths), string_qsort);
  for (item = 1; item < index->known_path_count; item++)
    if (ab_string_equal(&index->known_paths[item - 1],
                        &index->known_paths[item]))
      return ab_okf_error(index->engine, "duplicate known OKF path");
  return ARCHBIRD_OK;
}

static ArchbirdStatus load_host_diagnostics(AbOkfIndex *index,
                                            const AbValue *rows) {
  size_t item;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return ab_okf_error(index->engine, "diagnostics must be an array");
  for (item = 0; item < rows->as.array.count; item++) {
    const AbValue *row = &rows->as.array.items[item];
    const AbString *severity = ab_okf_optional_text(row, "severity");
    const AbString *code = ab_okf_optional_text(row, "code");
    const AbString *message = ab_okf_optional_text(row, "message");
    const AbString *path = ab_okf_optional_text(row, "path");
    if (row->kind != AB_VALUE_OBJECT || !severity || !code || !code->length ||
        !message || !path ||
        !((severity->length == 5 && !memcmp(severity->data, "error", 5)) ||
          (severity->length == 7 && !memcmp(severity->data, "warning", 7))))
      return ab_okf_error(index->engine, "invalid host diagnostic");
    OKF_TRY(diagnostic_add(index, severity, code, message, path));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus load_documents(AbOkfIndex *index,
                                     const AbValue *documents) {
  ArchbirdSha256Context digest;
  uint8_t binary[32];
  size_t item;
  size_t current = 0;
  if (!documents || documents->kind != AB_VALUE_ARRAY ||
      documents->as.array.count > index->engine->options.max_files)
    return ab_okf_error(index->engine, "invalid documents inventory");
  for (item = 0; item < documents->as.array.count; item++) {
    const AbValue *row = &documents->as.array.items[item];
    const AbValue *state = ab_value_member(row, "state");
    const AbValue *path = ab_value_member(row, "path");
    const AbValue *sha256 = ab_value_member(row, "sha256");
    uint64_t byte_length;
    if (row->kind != AB_VALUE_OBJECT || !path ||
        path->kind != AB_VALUE_STRING || !path_valid(&path->as.text) ||
        !lowercase_sha256(sha256) || !state || state->kind != AB_VALUE_STRING ||
        !ab_value_u64(ab_value_member(row, "byte_length"), &byte_length) ||
        byte_length > 4u * 1024u * 1024u)
      return ab_okf_error(index->engine, "invalid document inventory entry");
    if (item) {
      const AbValue *previous =
          ab_value_member(&documents->as.array.items[item - 1], "path");
      if (!previous || previous->kind != AB_VALUE_STRING ||
          ab_string_compare(&previous->as.text, &path->as.text) >= 0)
        return ab_okf_error(
            index->engine,
            "document inventory paths must be sorted and unique");
    }
    if (ab_okf_text_is(state, "current"))
      current++;
    else if (!ab_okf_text_is(state, "invalid"))
      return ab_okf_error(index->engine, "invalid document state");
  }
  if (current) {
    index->documents = (AbOkfDocument *)ab_calloc(index->engine, current,
                                                  sizeof(*index->documents));
    if (!index->documents)
      return archbird_error_set(index->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing OKF documents");
  }
  for (item = 0; item < documents->as.array.count; item++) {
    const AbValue *row = &documents->as.array.items[item];
    if (ab_okf_text_is(ab_value_member(row, "state"), "current")) {
      OKF_TRY(
          document_init(index, &index->documents[index->document_count], row));
      index->document_count++;
    }
  }
  if (index->document_count > 1)
    qsort(index->documents, index->document_count, sizeof(*index->documents),
          document_compare);
  for (item = 1; item < index->document_count; item++)
    if (ab_string_equal(&index->documents[item - 1].path,
                        &index->documents[item].path))
      return ab_okf_error(index->engine, "duplicate OKF document path");
  archbird_sha256_init(&digest);
  for (item = 0; item < documents->as.array.count; item++) {
    const AbValue *row = &documents->as.array.items[item];
    const AbString *path = &ab_value_member(row, "path")->as.text;
    const AbString *sha256 = &ab_value_member(row, "sha256")->as.text;
    OKF_TRY(archbird_sha256_update(&digest, (const uint8_t *)path->data,
                                   path->length));
    OKF_TRY(archbird_sha256_update(&digest, (const uint8_t *)"\0", 1));
    OKF_TRY(archbird_sha256_update(&digest, (const uint8_t *)sha256->data,
                                   sha256->length));
    OKF_TRY(archbird_sha256_update(&digest, (const uint8_t *)"\0", 1));
  }
  archbird_sha256_final(&digest, binary);
  archbird_sha256_hex(binary, index->bundle_sha256);
  return ARCHBIRD_OK;
}

static ArchbirdStatus finish_index(AbOkfIndex *index) {
  size_t document;
  size_t read;
  size_t write = 0;
  for (document = 0; document < index->document_count; document++) {
    AbOkfDocument *row = &index->documents[document];
    if (row->path.length == 8 && !memcmp(row->path.data, "index.md", 8)) {
      const AbValue *value = ab_value_member(row->frontmatter, "okf_version");
      if (value && value->kind != AB_VALUE_STRING) {
        OKF_TRY(diagnostic_literal(index, "error", "invalid-okf-version",
                                   "okf_version must be a string", &row->path));
      } else if (value) {
        OKF_TRY(ab_okf_copy_text(index->engine, &index->okf_version,
                                 &value->as.text));
        if (value->as.text.length && !ab_okf_text_is(value, "0.1")) {
          AbBuffer message;
          AbString rendered = {0};
          ab_buffer_init(&message, index->engine);
          OKF_TRY(
              ab_buffer_literal(&message, "best-effort consumption of OKF '"));
          OKF_TRY(ab_buffer_append(&message, value->as.text.data,
                                   value->as.text.length));
          OKF_TRY(ab_buffer_literal(&message, "'"));
          OKF_TRY(ab_string_copy(index->engine, &rendered,
                                 (const char *)message.data, message.length));
          {
            AbString severity = {(char *)"warning", 7};
            AbString code = {(char *)"unknown-okf-version", 19};
            OKF_TRY(
                diagnostic_add(index, &severity, &code, &rendered, &row->path));
          }
          ab_string_free(index->engine, &rendered);
          ab_buffer_free(&message);
        }
      }
    }
    OKF_TRY(document_links(index, row));
  }
  OKF_TRY(build_requirements(index));
  if (index->diagnostic_count > 1)
    qsort(index->diagnostics, index->diagnostic_count,
          sizeof(*index->diagnostics), diagnostic_compare);
  for (read = 0; read < index->diagnostic_count; read++) {
    if (write && !diagnostic_compare(&index->diagnostics[write - 1],
                                     &index->diagnostics[read])) {
      diagnostic_free(index->engine, &index->diagnostics[read]);
      continue;
    }
    if (write != read) {
      index->diagnostics[write] = index->diagnostics[read];
      memset(&index->diagnostics[read], 0, sizeof(index->diagnostics[read]));
    }
    write++;
  }
  index->diagnostic_count = write;
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_okf_index_load(ArchbirdEngine *engine, const uint8_t *json,
                                 size_t json_length, AbOkfIndex *out) {
  const AbValue *schema;
  const AbValue *producer;
  ArchbirdStatus status;
  uint64_t version;
  if (!engine || (!json && json_length) || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  out->engine = engine;
  status = ab_json_value_decode(engine, json, json_length, &out->input);
  if (status != ARCHBIRD_OK)
    return status;
  schema = ab_value_member(&out->input, "schema_version");
  producer = ab_value_member(&out->input, "producer");
  if (out->input.kind != AB_VALUE_OBJECT || !ab_value_u64(schema, &version) ||
      version != 1 ||
      !ab_okf_text_is(ab_value_member(&out->input, "artifact"),
                      "okf-source-bundle") ||
      !producer || producer->kind != AB_VALUE_OBJECT ||
      !ab_okf_optional_text(producer, "name") ||
      !ab_okf_optional_text(producer, "version") ||
      !lowercase_sha256(ab_value_member(producer, "implementation_sha256")) ||
      !ab_okf_optional_text(producer, "runtime")) {
    status = ab_okf_error(engine, "invalid source-bundle header");
    goto fail;
  }
  status = load_known_paths(out, ab_value_member(&out->input, "known_paths"));
  if (status == ARCHBIRD_OK)
    status =
        load_host_diagnostics(out, ab_value_member(&out->input, "diagnostics"));
  if (status == ARCHBIRD_OK)
    status = load_documents(out, ab_value_member(&out->input, "documents"));
  if (status == ARCHBIRD_OK)
    status = finish_index(out);
  if (status != ARCHBIRD_OK)
    goto fail;
  return ARCHBIRD_OK;

fail:
  ab_okf_index_free(out);
  return status;
}

static void document_free(ArchbirdEngine *engine, AbOkfDocument *document) {
  size_t link;
  ab_string_free(engine, &document->path);
  ab_string_free(engine, &document->concept_id);
  ab_string_free(engine, &document->kind);
  ab_string_free(engine, &document->sha256);
  ab_string_free(engine, &document->type_name);
  ab_string_free(engine, &document->title);
  ab_string_free(engine, &document->description);
  ab_string_free(engine, &document->resource);
  string_array_free(engine, document->tags, document->tag_count);
  string_array_free(engine, document->explicit_requirements,
                    document->explicit_requirement_count);
  for (link = 0; link < document->link_count; link++)
    link_free(engine, &document->links[link]);
  ab_free(engine, document->links);
}

void ab_okf_index_free(AbOkfIndex *index) {
  size_t item;
  if (!index)
    return;
  for (item = 0; item < index->document_count; item++)
    document_free(index->engine, &index->documents[item]);
  ab_free(index->engine, index->documents);
  string_array_free(index->engine, index->known_paths, index->known_path_count);
  for (item = 0; item < index->requirement_count; item++) {
    ab_string_free(index->engine, &index->requirements[item].requirement_id);
    ab_string_free(index->engine, &index->requirements[item].concept_id);
    ab_string_free(index->engine, &index->requirements[item].source);
    ab_string_free(index->engine, &index->requirements[item].target_concept);
  }
  ab_free(index->engine, index->requirements);
  for (item = 0; item < index->diagnostic_count; item++)
    diagnostic_free(index->engine, &index->diagnostics[item]);
  ab_free(index->engine, index->diagnostics);
  ab_string_free(index->engine, &index->okf_version);
  ab_value_free(index->engine, &index->input);
  memset(index, 0, sizeof(*index));
}
