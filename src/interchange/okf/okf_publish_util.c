#include "okf_publish_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PUB_TRY(expression)                                                    \
  do {                                                                         \
    ArchbirdStatus pub_status_ = (expression);                                 \
    if (pub_status_ != ARCHBIRD_OK)                                            \
      return pub_status_;                                                      \
  } while (0)

ArchbirdStatus ab_okf_pub_error(AbOkfPublication *pub, const char *message) {
  return archbird_error_set(pub->engine, ARCHBIRD_INVALID_SCHEMA,
                            ARCHBIRD_NO_OFFSET, "%s", message);
}

const AbValue *ab_okf_pub_member(const AbValue *object, const char *name,
                                 AbValueKind kind) {
  const AbValue *value = ab_value_member(object, name);
  return value && value->kind == kind ? value : NULL;
}

const AbString *ab_okf_pub_text(const AbValue *object, const char *name) {
  const AbValue *value = ab_okf_pub_member(object, name, AB_VALUE_STRING);
  return value ? &value->as.text : NULL;
}

const AbValue *ab_okf_pub_array(const AbValue *object, const char *name) {
  return ab_okf_pub_member(object, name, AB_VALUE_ARRAY);
}

int ab_okf_pub_u64(const AbValue *object, const char *name, uint64_t *out) {
  return ab_value_u64(ab_value_member(object, name), out);
}

ArchbirdStatus ab_okf_pub_copy(AbOkfPublication *pub, AbString *out,
                               const char *data, size_t length) {
  return ab_string_copy(pub->engine, out, data, length);
}

ArchbirdStatus ab_okf_pub_literal(AbOkfPublication *pub, AbString *out,
                                  const char *value) {
  return ab_string_copy(pub->engine, out, value, strlen(value));
}

ArchbirdStatus ab_okf_pub_buffer_string(AbOkfPublication *pub,
                                        const AbBuffer *buffer, AbString *out) {
  return ab_okf_pub_copy(pub, out, (const char *)buffer->data, buffer->length);
}

ArchbirdStatus ab_okf_pub_sha256(const uint8_t *data, size_t length,
                                 char output[65]) {
  uint8_t digest[32];
  ArchbirdStatus status = archbird_sha256(data, length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, output);
  return status;
}

ArchbirdStatus ab_okf_pub_value_digest(AbOkfPublication *pub,
                                       const AbValue *value, char output[65]) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_value_render(&buffer, value);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_sha256(buffer.data, buffer.length, output);
  ab_buffer_free(&buffer);
  return status;
}

static int utf8_next(const unsigned char *data, size_t length, size_t *index,
                     uint32_t *codepoint, size_t *start) {
  unsigned char first;
  size_t at = *index;
  uint32_t value;
  size_t count;
  size_t offset;
  if (at >= length)
    return 0;
  first = data[at];
  *start = at;
  if (first < 0x80) {
    *codepoint = first;
    *index = at + 1;
    return 1;
  }
  if ((first & 0xe0) == 0xc0) {
    value = first & 0x1f;
    count = 2;
  } else if ((first & 0xf0) == 0xe0) {
    value = first & 0x0f;
    count = 3;
  } else if ((first & 0xf8) == 0xf0) {
    value = first & 0x07;
    count = 4;
  } else {
    return 0;
  }
  if (at + count > length)
    return 0;
  for (offset = 1; offset < count; offset++) {
    unsigned char byte = data[at + offset];
    if ((byte & 0xc0) != 0x80)
      return 0;
    value = (value << 6) | (uint32_t)(byte & 0x3f);
  }
  if ((count == 2 && value < 0x80) || (count == 3 && value < 0x800) ||
      (count == 4 && value < 0x10000) || value > 0x10ffff ||
      (value >= 0xd800 && value <= 0xdfff))
    return 0;
  *codepoint = value;
  *index = at + count;
  return 1;
}

static int python_space(uint32_t value) {
  return (value >= 0x09 && value <= 0x0d) || (value >= 0x1c && value <= 0x20) ||
         value == 0x85 || value == 0xa0 || value == 0x1680 ||
         (value >= 0x2000 && value <= 0x200a) || value == 0x2028 ||
         value == 0x2029 || value == 0x202f || value == 0x205f ||
         value == 0x3000;
}

ArchbirdStatus ab_okf_pub_one_line(AbOkfPublication *pub, const AbString *value,
                                   AbString *out) {
  AbBuffer buffer;
  size_t index = 0;
  int pending_space = 0;
  int written = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!value || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&buffer, pub->engine);
  while (status == ARCHBIRD_OK && index < value->length) {
    size_t start;
    size_t next;
    uint32_t codepoint;
    next = index;
    if (!utf8_next((const unsigned char *)value->data, value->length, &next,
                   &codepoint, &start)) {
      status = ab_okf_pub_error(pub, "invalid UTF-8 in OKF publication text");
      break;
    }
    if (python_space(codepoint)) {
      if (written)
        pending_space = 1;
    } else {
      if (pending_space) {
        status = ab_buffer_literal(&buffer, " ");
        pending_space = 0;
      }
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&buffer, value->data + start, next - start);
      written = 1;
    }
    index = next;
  }
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

ArchbirdStatus ab_okf_pub_body(AbOkfPublication *pub, const AbBuffer *value,
                               AbString *out) {
  AbBuffer buffer;
  size_t index = 0;
  size_t keep = 0;
  ArchbirdStatus status;
  if (!value || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  while (index < value->length) {
    size_t start;
    size_t next = index;
    uint32_t codepoint;
    if (!utf8_next(value->data, value->length, &next, &codepoint, &start))
      return ab_okf_pub_error(pub, "invalid UTF-8 in OKF concept body");
    if (!python_space(codepoint))
      keep = next;
    index = next;
  }
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_append(&buffer, value->data, keep);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "\n");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus html_text(AbBuffer *buffer, const char *data,
                                size_t length, int markdown) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < length; index++) {
    switch ((unsigned char)data[index]) {
    case '&':
      status = ab_buffer_literal(buffer, "&amp;");
      break;
    case '<':
      status = ab_buffer_literal(buffer, "&lt;");
      break;
    case '>':
      status = ab_buffer_literal(buffer, "&gt;");
      break;
    case '\\':
      status = markdown ? ab_buffer_literal(buffer, "\\\\")
                        : ab_buffer_append(buffer, data + index, 1);
      break;
    case '[':
      status = markdown ? ab_buffer_literal(buffer, "\\[")
                        : ab_buffer_append(buffer, data + index, 1);
      break;
    case ']':
      status = markdown ? ab_buffer_literal(buffer, "\\]")
                        : ab_buffer_append(buffer, data + index, 1);
      break;
    case '|':
      status = markdown ? ab_buffer_literal(buffer, "\\|")
                        : ab_buffer_append(buffer, data + index, 1);
      break;
    default:
      status = ab_buffer_append(buffer, data + index, 1);
      break;
    }
  }
  return status;
}

ArchbirdStatus ab_okf_pub_plain(AbBuffer *buffer, const AbString *value) {
  AbString line = {0};
  ArchbirdStatus status;
  AbOkfPublication pub = {0};
  if (!buffer || !value)
    return ARCHBIRD_INVALID_ARGUMENT;
  pub.engine = buffer->engine;
  status = ab_okf_pub_one_line(&pub, value, &line);
  if (status == ARCHBIRD_OK)
    status = html_text(buffer, line.data, line.length, 1);
  ab_string_free(buffer->engine, &line);
  return status;
}

ArchbirdStatus ab_okf_pub_code(AbBuffer *buffer, const AbString *value) {
  AbString line = {0};
  ArchbirdStatus status;
  AbOkfPublication pub = {0};
  if (!buffer || !value)
    return ARCHBIRD_INVALID_ARGUMENT;
  pub.engine = buffer->engine;
  status = ab_okf_pub_one_line(&pub, value, &line);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "<code>");
  if (status == ARCHBIRD_OK)
    status = html_text(buffer, line.data, line.length, 0);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "</code>");
  ab_string_free(buffer->engine, &line);
  return status;
}

ArchbirdStatus ab_okf_pub_json_code(AbBuffer *buffer, const AbValue *value) {
  AbBuffer json;
  AbString text;
  ArchbirdStatus status;
  if (!buffer || !value)
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&json, buffer->engine);
  status = ab_value_render(&json, value);
  text.data = (char *)json.data;
  text.length = json.length;
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_code(buffer, &text);
  ab_buffer_free(&json);
  return status;
}

ArchbirdStatus ab_okf_pub_url(AbBuffer *buffer, const AbString *value) {
  static const char hex[] = "0123456789ABCDEF";
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < value->length; index++) {
    unsigned char byte = (unsigned char)value->data[index];
    int safe = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') ||
               strchr("_.-~/:#?[]@!$&'*+,;=%", byte);
    if (safe)
      status = ab_buffer_append(buffer, value->data + index, 1);
    else {
      char encoded[3] = {'%', hex[byte >> 4], hex[byte & 15]};
      status = ab_buffer_append(buffer, encoded, sizeof(encoded));
    }
  }
  return status;
}

static const AbValue *normalization_row(const AbOkfPublication *pub,
                                        const AbString *value) {
  size_t index;
  const AbValue *rows = pub->normalization.rows;
  if (!rows)
    return NULL;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbString *text = ab_okf_pub_text(row, "text");
    if (text && ab_string_equal(text, value))
      return row;
  }
  return NULL;
}

static int has_non_ascii(const AbString *value) {
  size_t index;
  for (index = 0; index < value->length; index++)
    if ((unsigned char)value->data[index] >= 0x80)
      return 1;
  return 0;
}

static ArchbirdStatus slug_ascii(AbOkfPublication *pub, const AbString *input,
                                 AbString *out) {
  AbBuffer buffer;
  size_t index;
  int dash = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_buffer_init(&buffer, pub->engine);
  for (index = 0; status == ARCHBIRD_OK && index < input->length; index++) {
    unsigned char byte = (unsigned char)input->data[index];
    int allowed = (byte >= 'A' && byte <= 'Z') ||
                  (byte >= 'a' && byte <= 'z') ||
                  (byte >= '0' && byte <= '9') || byte == '_' || byte == '.' ||
                  byte == '-';
    if (allowed) {
      if (byte == '-' && (buffer.length == 0 || dash))
        continue;
      if (buffer.length >= 80)
        break;
      byte = (unsigned char)tolower(byte);
      status = ab_buffer_append(&buffer, &byte, 1);
      dash = byte == '-';
    } else if (buffer.length && !dash) {
      if (buffer.length < 80)
        status = ab_buffer_literal(&buffer, "-");
      dash = 1;
    }
  }
  while (buffer.length && (buffer.data[buffer.length - 1] == '-' ||
                           buffer.data[buffer.length - 1] == '.'))
    buffer.length--;
  while (buffer.length && (buffer.data[0] == '-' || buffer.data[0] == '.')) {
    memmove(buffer.data, buffer.data + 1, --buffer.length);
  }
  if (!buffer.length)
    status = ab_buffer_literal(&buffer, "item");
  if (status == ARCHBIRD_OK &&
      ((buffer.length == 5 && !memcmp(buffer.data, "index", 5)) ||
       (buffer.length == 3 && !memcmp(buffer.data, "log", 3)))) {
    AbBuffer prefixed;
    ab_buffer_init(&prefixed, pub->engine);
    status = ab_buffer_literal(&prefixed, "item-");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&prefixed, buffer.data, buffer.length);
    if (status == ARCHBIRD_OK) {
      ab_buffer_free(&buffer);
      buffer = prefixed;
      memset(&prefixed, 0, sizeof(prefixed));
    }
    ab_buffer_free(&prefixed);
  }
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

ArchbirdStatus ab_okf_pub_slug(AbOkfPublication *pub, const AbString *value,
                               AbString *out) {
  const AbString *input = value;
  const AbValue *row;
  if (!value || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (has_non_ascii(value)) {
    row = normalization_row(pub, value);
    input = row ? ab_okf_pub_text(row, "slug_ascii") : NULL;
    if (!input)
      return ab_okf_pub_error(pub,
                              "missing Unicode slug normalization evidence");
  }
  return slug_ascii(pub, input, out);
}

ArchbirdStatus ab_okf_pub_sort_key(AbOkfPublication *pub, const AbString *value,
                                   AbString *out) {
  size_t index;
  const AbValue *row;
  if (!value || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (has_non_ascii(value)) {
    row = normalization_row(pub, value);
    if (!row || !ab_okf_pub_text(row, "casefold"))
      return ab_okf_pub_error(
          pub, "missing Unicode case-fold normalization evidence");
    return ab_okf_pub_copy(pub, out, ab_okf_pub_text(row, "casefold")->data,
                           ab_okf_pub_text(row, "casefold")->length);
  }
  out->data = (char *)ab_malloc(pub->engine, value->length + 1);
  if (!out->data)
    return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory building OKF sort key");
  out->length = value->length;
  for (index = 0; index < value->length; index++)
    out->data[index] = (char)tolower((unsigned char)value->data[index]);
  out->data[out->length] = '\0';
  return ARCHBIRD_OK;
}

static ArchbirdStatus normalize_relative(AbBuffer *buffer, const char *source,
                                         const char *target) {
  const char *source_slash = strrchr(source, '/');
  const char *target_cursor = target;
  const char *source_cursor = source;
  size_t source_dir_length = source_slash ? (size_t)(source_slash - source) : 0;
  size_t common = 0;
  size_t index;
  size_t ups = 0;
  while (common < source_dir_length && target_cursor[common] &&
         source_cursor[common] == target_cursor[common]) {
    common++;
  }
  if (common == source_dir_length && target_cursor[common] == '/')
    common++;
  else
    while (common && source_cursor[common - 1] != '/')
      common--;
  for (index = common; index < source_dir_length; index++)
    if (source[index] == '/')
      ups++;
  if (source_dir_length > common)
    ups++;
  for (index = 0; index < ups; index++)
    PUB_TRY(ab_buffer_literal(buffer, "../"));
  target_cursor = target + common;
  if (!ups && !*target_cursor)
    return ab_buffer_literal(buffer, ".");
  return ab_buffer_literal(buffer, target_cursor);
}

ArchbirdStatus ab_okf_pub_relative_link(AbBuffer *buffer,
                                        const char *source_path,
                                        const char *target_path,
                                        const AbString *label) {
  PUB_TRY(ab_buffer_literal(buffer, "["));
  PUB_TRY(ab_okf_pub_plain(buffer, label));
  PUB_TRY(ab_buffer_literal(buffer, "]("));
  PUB_TRY(normalize_relative(buffer, source_path, target_path));
  return ab_buffer_literal(buffer, ")");
}

ArchbirdStatus ab_okf_pub_external_link(AbBuffer *buffer,
                                        const AbString *target,
                                        const AbString *label) {
  PUB_TRY(ab_buffer_literal(buffer, "["));
  PUB_TRY(ab_okf_pub_plain(buffer, label));
  PUB_TRY(ab_buffer_literal(buffer, "]("));
  PUB_TRY(ab_okf_pub_url(buffer, target));
  return ab_buffer_literal(buffer, ")");
}

static int field_compare(const void *left, const void *right) {
  return strcmp(((const AbOkfPubField *)left)->name,
                ((const AbOkfPubField *)right)->name);
}

static int relation_compare(const void *left, const void *right) {
  return ab_string_compare(&((const AbOkfPubRelation *)left)->json,
                           &((const AbOkfPubRelation *)right)->json);
}

void ab_okf_pub_fields_free(AbOkfPublication *pub, AbOkfPubField *fields,
                            size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    ab_string_free(pub->engine, &fields[index].json);
}

void ab_okf_pub_relations_free(AbOkfPublication *pub,
                               AbOkfPubRelationList *relations) {
  size_t index;
  if (!relations)
    return;
  for (index = 0; index < relations->count; index++)
    ab_string_free(pub->engine, &relations->items[index].json);
  ab_free(pub->engine, relations->items);
  memset(relations, 0, sizeof(*relations));
}

ArchbirdStatus ab_okf_pub_relation(AbOkfPublication *pub,
                                   AbOkfPubRelationList *relations,
                                   const AbOkfPubField *fields,
                                   size_t field_count) {
  AbOkfPubField *ordered = NULL;
  AbBuffer buffer;
  AbOkfPubRelation *resized;
  size_t index;
  size_t capacity;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!relations || (!fields && field_count))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (field_count) {
    ordered =
        (AbOkfPubField *)ab_calloc(pub->engine, field_count, sizeof(*ordered));
    if (!ordered)
      return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory building OKF relation");
    memcpy(ordered, fields, field_count * sizeof(*ordered));
    if (field_count > 1)
      qsort(ordered, field_count, sizeof(*ordered), field_compare);
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
  ab_free(pub->engine, ordered);
  if (status != ARCHBIRD_OK) {
    ab_buffer_free(&buffer);
    return status;
  }
  for (index = 0; index < relations->count; index++) {
    if (relations->items[index].json.length == buffer.length &&
        !memcmp(relations->items[index].json.data, buffer.data,
                buffer.length)) {
      ab_buffer_free(&buffer);
      return ARCHBIRD_OK;
    }
  }
  if (relations->count == relations->capacity) {
    capacity = relations->capacity ? relations->capacity * 2 : 8;
    if (capacity > pub->engine->options.max_values ||
        capacity > SIZE_MAX / sizeof(*resized)) {
      ab_buffer_free(&buffer);
      return archbird_error_set(pub->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too many OKF typed relations");
    }
    resized = (AbOkfPubRelation *)ab_realloc(
        pub->engine, relations->items, capacity * sizeof(*relations->items));
    if (!resized) {
      ab_buffer_free(&buffer);
      return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory growing OKF relations");
    }
    memset(resized + relations->capacity, 0,
           (capacity - relations->capacity) * sizeof(*resized));
    relations->items = resized;
    relations->capacity = capacity;
  }
  status = ab_okf_pub_buffer_string(pub, &buffer,
                                    &relations->items[relations->count].json);
  ab_buffer_free(&buffer);
  if (status == ARCHBIRD_OK) {
    relations->count++;
    if (relations->count > 1)
      qsort(relations->items, relations->count, sizeof(*relations->items),
            relation_compare);
  }
  return status;
}

ArchbirdStatus ab_okf_pub_json_text(AbOkfPublication *pub, AbString *out,
                                    const AbString *value) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_json_string(&buffer, value->data, value->length);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

ArchbirdStatus ab_okf_pub_json_literal(AbOkfPublication *pub, AbString *out,
                                       const char *value) {
  AbString string = {(char *)value, strlen(value)};
  return ab_okf_pub_json_text(pub, out, &string);
}

ArchbirdStatus ab_okf_pub_json_u64(AbOkfPublication *pub, AbString *out,
                                   uint64_t value) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_u64(&buffer, value);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

ArchbirdStatus ab_okf_pub_json_bool(AbOkfPublication *pub, AbString *out,
                                    int value) {
  return ab_okf_pub_literal(pub, out, value ? "true" : "false");
}

ArchbirdStatus ab_okf_pub_json_value(AbOkfPublication *pub, AbString *out,
                                     const AbValue *value) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_value_render(&buffer, value);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

ArchbirdStatus ab_okf_pub_json_string_array(AbOkfPublication *pub,
                                            AbString *out,
                                            const AbValue *values) {
  size_t index;
  AbBuffer buffer;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!values || values->kind != AB_VALUE_ARRAY)
    return ab_okf_pub_error(pub, "expected string array in OKF publication");
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_literal(&buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < values->as.array.count;
       index++) {
    const AbValue *value = &values->as.array.items[index];
    if (value->kind != AB_VALUE_STRING) {
      status =
          ab_okf_pub_error(pub, "expected string in OKF publication array");
      break;
    }
    if (index)
      status = ab_buffer_literal(&buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&buffer, value->as.text.data,
                                     value->as.text.length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "]");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

ArchbirdStatus ab_okf_pub_relation_simple(AbOkfPublication *pub,
                                          AbOkfPubRelationList *relations,
                                          const char *kind,
                                          const char *target) {
  AbOkfPubField fields[2] = {{"kind", {0}}, {"target", {0}}};
  ArchbirdStatus status = ab_okf_pub_json_literal(pub, &fields[0].json, kind);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_literal(pub, &fields[1].json, target);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_relation(pub, relations, fields, 2);
  ab_okf_pub_fields_free(pub, fields, 2);
  return status;
}
