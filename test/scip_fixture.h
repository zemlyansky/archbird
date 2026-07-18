#ifndef ARCHBIRD_TEST_SCIP_FIXTURE_H
#define ARCHBIRD_TEST_SCIP_FIXTURE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct ScipTestBuffer {
  uint8_t bytes[32768];
  size_t length;
  int ok;
} ScipTestBuffer;

static inline void scip_test_init(ScipTestBuffer *buffer) {
  memset(buffer, 0, sizeof(*buffer));
  buffer->ok = 1;
}

static inline void scip_test_append(ScipTestBuffer *buffer, const void *bytes,
                                    size_t length) {
  if (!buffer->ok || length > sizeof(buffer->bytes) - buffer->length) {
    buffer->ok = 0;
    return;
  }
  memcpy(buffer->bytes + buffer->length, bytes, length);
  buffer->length += length;
}

static inline void scip_test_varint(ScipTestBuffer *buffer, uint64_t value) {
  uint8_t encoded[10];
  size_t length = 0;
  do {
    uint8_t byte = (uint8_t)(value & 0x7fu);
    value >>= 7;
    encoded[length++] = value ? (uint8_t)(byte | 0x80u) : byte;
  } while (value && length < sizeof(encoded));
  scip_test_append(buffer, encoded, length);
}

static inline void scip_test_key(ScipTestBuffer *buffer, unsigned field,
                                 unsigned wire) {
  scip_test_varint(buffer, ((uint64_t)field << 3) | wire);
}

static inline void scip_test_integer(ScipTestBuffer *buffer, unsigned field,
                                     uint64_t value) {
  scip_test_key(buffer, field, 0);
  scip_test_varint(buffer, value);
}

static inline void scip_test_bytes(ScipTestBuffer *buffer, unsigned field,
                                   const void *bytes, size_t length) {
  scip_test_key(buffer, field, 2);
  scip_test_varint(buffer, length);
  scip_test_append(buffer, bytes, length);
}

static inline void scip_test_string(ScipTestBuffer *buffer, unsigned field,
                                    const char *value) {
  scip_test_bytes(buffer, field, value, strlen(value));
}

static inline void scip_test_message(ScipTestBuffer *buffer, unsigned field,
                                     const ScipTestBuffer *message) {
  scip_test_bytes(buffer, field, message->bytes, message->length);
  if (!message->ok)
    buffer->ok = 0;
}

static inline void scip_test_metadata(ScipTestBuffer *out) {
  ScipTestBuffer tool;
  scip_test_init(&tool);
  scip_test_string(&tool, 1, "archbird-scip-fixture");
  scip_test_string(&tool, 2, "1.0");
  scip_test_string(&tool, 3, "--deterministic");
  scip_test_message(out, 2, &tool);
  scip_test_integer(out, 4, 1);
}

static inline void scip_test_single_range(ScipTestBuffer *out, uint32_t line,
                                          uint32_t start, uint32_t end) {
  scip_test_integer(out, 1, line);
  scip_test_integer(out, 2, start);
  scip_test_integer(out, 3, end);
}

static inline void scip_test_multi_range(ScipTestBuffer *out,
                                         uint32_t start_line, uint32_t start,
                                         uint32_t end_line, uint32_t end) {
  scip_test_integer(out, 1, start_line);
  scip_test_integer(out, 2, start);
  scip_test_integer(out, 3, end_line);
  scip_test_integer(out, 4, end);
}

static inline void scip_test_occurrence(ScipTestBuffer *out, const char *symbol,
                                        unsigned roles, unsigned range_field,
                                        const ScipTestBuffer *range) {
  scip_test_string(out, 2, symbol);
  if (roles)
    scip_test_integer(out, 3, roles);
  scip_test_message(out, range_field, range);
}

static inline void
scip_test_deprecated_occurrence(ScipTestBuffer *out, const char *symbol,
                                unsigned roles, uint32_t line, uint32_t start,
                                uint32_t end) {
  ScipTestBuffer packed;
  scip_test_init(&packed);
  scip_test_varint(&packed, line);
  scip_test_varint(&packed, start);
  scip_test_varint(&packed, end);
  scip_test_bytes(out, 1, packed.bytes, packed.length);
  scip_test_string(out, 2, symbol);
  if (roles)
    scip_test_integer(out, 3, roles);
}

static inline void scip_test_document(ScipTestBuffer *out, const char *path,
                                      unsigned encoding,
                                      const ScipTestBuffer *occurrence,
                                      const ScipTestBuffer *symbol) {
  scip_test_string(out, 1, path);
  if (occurrence)
    scip_test_message(out, 2, occurrence);
  if (symbol)
    scip_test_message(out, 3, symbol);
  scip_test_string(out, 4, "fixture");
  scip_test_integer(out, 6, encoding);
}

static inline void scip_test_valid_index(ScipTestBuffer *index) {
  static const char symbol[] = "scip c demo 1.0 demo/add().";
  static const char contract[] = "scip c demo 1.0 demo/contract().";
  static const char external[] = "scip c runtime 1.0 runtime/print().";
  ScipTestBuffer metadata;
  ScipTestBuffer range;
  ScipTestBuffer occurrence;
  ScipTestBuffer relationship;
  ScipTestBuffer implementation;
  ScipTestBuffer symbol_info;
  ScipTestBuffer document;
  ScipTestBuffer external_info;
  scip_test_init(index);
  scip_test_init(&metadata);
  scip_test_metadata(&metadata);
  scip_test_message(index, 1, &metadata);

  scip_test_init(&range);
  scip_test_single_range(&range, 0, 4, 7);
  scip_test_init(&occurrence);
  scip_test_occurrence(&occurrence, symbol, 1, 8, &range);
  scip_test_init(&relationship);
  scip_test_string(&relationship, 1, external);
  scip_test_integer(&relationship, 2, 1);
  scip_test_init(&symbol_info);
  scip_test_string(&symbol_info, 1, symbol);
  scip_test_message(&symbol_info, 4, &relationship);
  scip_test_init(&implementation);
  scip_test_string(&implementation, 1, contract);
  scip_test_integer(&implementation, 3, 1);
  scip_test_message(&symbol_info, 4, &implementation);
  scip_test_integer(&symbol_info, 5, 17);
  scip_test_string(&symbol_info, 6, "add");
  scip_test_init(&document);
  scip_test_document(&document, "src/core.c", 1, &occurrence, &symbol_info);
  scip_test_string(&document, 5, "int add(void) { return 1; }\n");
  scip_test_message(index, 2, &document);

  scip_test_init(&range);
  scip_test_single_range(&range, 0, 4, 12);
  scip_test_init(&occurrence);
  scip_test_occurrence(&occurrence, contract, 1, 8, &range);
  scip_test_init(&symbol_info);
  scip_test_string(&symbol_info, 1, contract);
  scip_test_integer(&symbol_info, 5, 17);
  scip_test_init(&document);
  scip_test_document(&document, "src/contract.c", 1, &occurrence, &symbol_info);
  scip_test_string(&document, 5, "int contract(void) { return 1; }\n");
  scip_test_message(index, 2, &document);

  scip_test_init(&range);
  scip_test_single_range(&range, 0, 4, 7);
  scip_test_init(&occurrence);
  scip_test_occurrence(&occurrence, symbol, 0, 8, &range);
  scip_test_init(&document);
  scip_test_string(&document, 1, "py/api.py");
  scip_test_message(&document, 2, &occurrence);
  scip_test_init(&range);
  scip_test_single_range(&range, 0, 14, 17);
  scip_test_init(&occurrence);
  scip_test_occurrence(&occurrence, symbol, 0, 8, &range);
  scip_test_message(&document, 2, &occurrence);
  scip_test_string(&document, 4, "fixture");
  scip_test_string(&document, 5, "use(add); use(add);\n");
  scip_test_message(index, 2, &document);

  scip_test_init(&range);
  scip_test_single_range(&range, 2, 4, 7);
  scip_test_init(&occurrence);
  scip_test_occurrence(&occurrence, symbol, 0, 8, &range);
  scip_test_init(&document);
  scip_test_document(&document, "py/multiline.py", 1, &occurrence, NULL);
  scip_test_string(&document, 5, "zero\none\ntwo add\n");
  scip_test_message(index, 2, &document);

  scip_test_init(&range);
  scip_test_multi_range(&range, 0, 4, 0, 7);
  scip_test_init(&occurrence);
  scip_test_occurrence(&occurrence, symbol, 0, 9, &range);
  scip_test_init(&document);
  scip_test_document(&document, "py/utf16.py", 2, &occurrence, NULL);
  scip_test_string(&document, 5, "🚀x=add\n");
  scip_test_message(index, 2, &document);

  scip_test_init(&occurrence);
  scip_test_deprecated_occurrence(&occurrence, symbol, 0, 0, 3, 6);
  scip_test_init(&document);
  scip_test_document(&document, "py/utf32.py", 3, &occurrence, NULL);
  scip_test_string(&document, 5, "🚀x=add\n");
  scip_test_message(index, 2, &document);

  scip_test_init(&external_info);
  scip_test_string(&external_info, 1, external);
  scip_test_message(index, 3, &external_info);
}

static inline void scip_test_no_text_index(ScipTestBuffer *index) {
  static const char symbol[] = "scip c demo 1.0 demo/unknown_add().";
  ScipTestBuffer metadata;
  ScipTestBuffer range;
  ScipTestBuffer occurrence;
  ScipTestBuffer document;
  scip_test_init(index);
  scip_test_init(&metadata);
  scip_test_metadata(&metadata);
  scip_test_message(index, 1, &metadata);

  scip_test_init(&range);
  /* SCIP explicitly permits empty occurrence ranges.  scip-clang uses them
     for file symbols, so a zero-width definition remains a valid target. */
  scip_test_single_range(&range, 0, 0, 0);
  scip_test_init(&occurrence);
  scip_test_occurrence(&occurrence, symbol, 1, 8, &range);
  scip_test_init(&document);
  scip_test_document(&document, "src/unknown_core.c", 1, &occurrence, NULL);
  scip_test_message(index, 2, &document);

  scip_test_init(&range);
  scip_test_single_range(&range, 0, 4, 15);
  scip_test_init(&occurrence);
  scip_test_occurrence(&occurrence, symbol, 0, 8, &range);
  scip_test_init(&document);
  scip_test_document(&document, "py/unknown.py", 1, &occurrence, NULL);
  scip_test_message(index, 2, &document);
}

static inline void scip_test_invalid_path_index(ScipTestBuffer *index) {
  ScipTestBuffer metadata;
  ScipTestBuffer document;
  scip_test_init(index);
  scip_test_init(&metadata);
  scip_test_metadata(&metadata);
  scip_test_message(index, 1, &metadata);
  scip_test_init(&document);
  scip_test_document(&document, "../escape.py", 1, NULL, NULL);
  scip_test_message(index, 2, &document);
}

static inline void scip_test_stale_index(ScipTestBuffer *index) {
  static const char symbol[] = "scip c demo 1.0 demo/add().";
  ScipTestBuffer metadata;
  ScipTestBuffer range;
  ScipTestBuffer occurrence;
  ScipTestBuffer document;
  scip_test_init(index);
  scip_test_init(&metadata);
  scip_test_metadata(&metadata);
  scip_test_message(index, 1, &metadata);
  scip_test_init(&range);
  scip_test_single_range(&range, 0, 0, 100);
  scip_test_init(&occurrence);
  scip_test_occurrence(&occurrence, symbol, 0, 8, &range);
  scip_test_init(&document);
  scip_test_document(&document, "py/stale.py", 1, &occurrence, NULL);
  scip_test_message(index, 2, &document);
}

static inline void scip_test_source_mismatch_index(ScipTestBuffer *index) {
  static const char symbol[] = "scip c demo 1.0 demo/add().";
  ScipTestBuffer metadata;
  ScipTestBuffer range;
  ScipTestBuffer occurrence;
  ScipTestBuffer document;
  scip_test_init(index);
  scip_test_init(&metadata);
  scip_test_metadata(&metadata);
  scip_test_message(index, 1, &metadata);
  scip_test_init(&range);
  scip_test_single_range(&range, 0, 0, 3);
  scip_test_init(&occurrence);
  scip_test_occurrence(&occurrence, symbol, 0, 8, &range);
  scip_test_init(&document);
  scip_test_document(&document, "py/mismatch.py", 1, &occurrence, NULL);
  scip_test_string(&document, 5, "old\n");
  scip_test_message(index, 2, &document);
}

static inline void scip_test_malformed_index(ScipTestBuffer *index) {
  static const uint8_t malformed[] = {0x0a, 0x01, 0x00};
  scip_test_init(index);
  scip_test_append(index, malformed, sizeof(malformed));
}

static inline void scip_test_late_metadata_index(ScipTestBuffer *index) {
  ScipTestBuffer external_info;
  ScipTestBuffer metadata;
  scip_test_init(index);
  scip_test_init(&external_info);
  scip_test_string(&external_info, 1, "scip c runtime 1.0 runtime/print().");
  scip_test_message(index, 3, &external_info);
  scip_test_init(&metadata);
  scip_test_metadata(&metadata);
  scip_test_message(index, 1, &metadata);
}

static inline void scip_test_invalid_utf8_index(ScipTestBuffer *index) {
  static const uint8_t invalid_utf8[] = {0xc0, 0x80};
  ScipTestBuffer tool;
  ScipTestBuffer metadata;
  scip_test_init(index);
  scip_test_init(&tool);
  scip_test_bytes(&tool, 1, invalid_utf8, sizeof(invalid_utf8));
  scip_test_string(&tool, 2, "1.0");
  scip_test_init(&metadata);
  scip_test_message(&metadata, 2, &tool);
  scip_test_integer(&metadata, 4, 1);
  scip_test_message(index, 1, &metadata);
}

#endif
