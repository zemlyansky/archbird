#include "path_match.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int path_glob_class(const char *pattern, size_t length, uint8_t value,
                           size_t *consumed) {
  size_t index = 1;
  int negate = 0;
  int matched = 0;
  if (index < length && (pattern[index] == '!' || pattern[index] == '^')) {
    negate = 1;
    index++;
  }
  while (index < length && pattern[index] != ']') {
    uint8_t start = (uint8_t)pattern[index++];
    uint8_t end = start;
    if (index + 1 < length && pattern[index] == '-' &&
        pattern[index + 1] != ']') {
      index++;
      end = (uint8_t)pattern[index++];
    }
    if (value >= start && value <= end)
      matched = 1;
  }
  if (index >= length || pattern[index] != ']') {
    *consumed = 1;
    return value == '[';
  }
  *consumed = index + 1;
  return negate ? !matched : matched;
}

static size_t path_pattern_advance(const char *pattern, size_t pattern_length,
                                   size_t index, int collapse_globstar) {
  if (collapse_globstar && index + 3 < pattern_length &&
      pattern[index] == '/' && pattern[index + 1] == '*' &&
      pattern[index + 2] == '*' && pattern[index + 3] == '/')
    return index + 4;
  return index + 1;
}

static int path_glob_match_mode(const char *pattern, size_t pattern_length,
                                const char *path, size_t path_length,
                                int collapse_globstar) {
  size_t p = 0;
  size_t s = 0;
  size_t star_pattern = SIZE_MAX;
  size_t star_path = 0;
  while (s < path_length) {
    if (p < pattern_length && pattern[p] == '*') {
      while (p < pattern_length && pattern[p] == '*')
        p++;
      star_pattern = p;
      star_path = s;
      if (p == pattern_length)
        return 1;
      continue;
    }
    if (p < pattern_length && pattern[p] == '?') {
      p = path_pattern_advance(pattern, pattern_length, p, collapse_globstar);
      s++;
      continue;
    }
    if (p < pattern_length && pattern[p] == '[') {
      size_t consumed = 0;
      if (path_glob_class(pattern + p, pattern_length - p, (uint8_t)path[s],
                          &consumed)) {
        p += consumed;
        s++;
        continue;
      }
    } else if (p < pattern_length && pattern[p] == path[s]) {
      p = path_pattern_advance(pattern, pattern_length, p, collapse_globstar);
      s++;
      continue;
    }
    if (star_pattern != SIZE_MAX) {
      p = star_pattern;
      s = ++star_path;
      continue;
    }
    return 0;
  }
  while (p < pattern_length && pattern[p] == '*')
    p++;
  return p == pattern_length;
}

static int path_glob_match(const char *pattern, size_t pattern_length,
                           const char *path, size_t path_length) {
  return path_glob_match_mode(pattern, pattern_length, path, path_length, 0);
}

int ab_map_path_match(const AbString *path, const AbString *pattern) {
  const char *start = pattern->data;
  size_t length = pattern->length;
  if (length >= 2 && start[0] == '.' && start[1] == '/') {
    start += 2;
    length -= 2;
  }
  if (path_glob_match(start, length, path->data, path->length))
    return 1;
  if (path_glob_match_mode(start, length, path->data, path->length, 1))
    return 1;
  if (length >= 3 && memcmp(start + length - 3, "/**", 3) == 0) {
    size_t prefix = length - 3;
    while (prefix && start[prefix - 1] == '/')
      prefix--;
    if ((path->length == prefix && !memcmp(path->data, start, prefix)) ||
        (path->length > prefix && path->data[prefix] == '/' &&
         !memcmp(path->data, start, prefix)))
      return 1;
  }
  return 0;
}

static int collection_match(const char *pattern, size_t pattern_length,
                            size_t pattern_index, const char *path,
                            size_t path_length, size_t path_index) {
  size_t pattern_end = pattern_index;
  size_t path_end = path_index;
  while (pattern_end < pattern_length && pattern[pattern_end] != '/')
    pattern_end++;
  while (path_end < path_length && path[path_end] != '/')
    path_end++;
  if (pattern_index == pattern_length)
    return path_index == path_length;
  if (pattern_end == pattern_index + 2 && pattern[pattern_index] == '*' &&
      pattern[pattern_index + 1] == '*') {
    size_t next_pattern =
        pattern_end < pattern_length ? pattern_end + 1 : pattern_end;
    if (next_pattern == pattern_length)
      return 1;
    if (collection_match(pattern, pattern_length, next_pattern, path,
                         path_length, path_index))
      return 1;
    if (path_index < path_length)
      return collection_match(pattern, pattern_length, pattern_index, path,
                              path_length,
                              path_end < path_length ? path_end + 1 : path_end);
    return 0;
  }
  if (path_index == path_length ||
      !path_glob_match(pattern + pattern_index, pattern_end - pattern_index,
                       path + path_index, path_end - path_index))
    return 0;
  return collection_match(
      pattern, pattern_length,
      pattern_end < pattern_length ? pattern_end + 1 : pattern_end, path,
      path_length, path_end < path_length ? path_end + 1 : path_end);
}

int ab_map_collection_match(const AbString *path, const AbString *pattern) {
  const char *start = pattern->data;
  size_t length = pattern->length;
  if (length >= 2 && start[0] == '.' && start[1] == '/') {
    start += 2;
    length -= 2;
  }
  return collection_match(start, length, 0, path->data, path->length, 0);
}

static size_t utf8_next(const char *text, size_t length, size_t index) {
  unsigned char byte;
  size_t width;
  if (index >= length)
    return length;
  byte = (unsigned char)text[index];
  width = byte < 0x80 ? 1 : byte < 0xe0 ? 2 : byte < 0xf0 ? 3 : 4;
  return width <= length - index ? index + width : index + 1;
}

static int class_match(const char *pattern, size_t length, size_t *index,
                       unsigned char value) {
  size_t cursor = *index + 1;
  int negate = 0;
  int matched = 0;
  int closed = 0;
  if (cursor < length && (pattern[cursor] == '!' || pattern[cursor] == '^')) {
    negate = 1;
    cursor++;
  }
  if (cursor < length && pattern[cursor] == ']') {
    matched = value == ']';
    cursor++;
  }
  while (cursor < length) {
    unsigned char first = (unsigned char)pattern[cursor++];
    if (first == ']') {
      closed = 1;
      break;
    }
    if (cursor + 1 < length && pattern[cursor] == '-' &&
        pattern[cursor + 1] != ']') {
      unsigned char last = (unsigned char)pattern[cursor + 1];
      if (first <= value && value <= last)
        matched = 1;
      cursor += 2;
    } else if (first == value) {
      matched = 1;
    }
  }
  if (!closed)
    return -1;
  *index = cursor;
  return negate ? !matched : matched;
}

int ab_map_glob_match(const AbString *pattern, const AbString *value) {
  size_t p = 0, v = 0;
  size_t star_p = SIZE_MAX, star_v = 0;
  while (v < value->length) {
    size_t next_v = utf8_next(value->data, value->length, v);
    int matched = 0;
    if (p < pattern->length && pattern->data[p] == '*') {
      while (p < pattern->length && pattern->data[p] == '*')
        p++;
      star_p = p;
      star_v = v;
      if (p == pattern->length)
        return 1;
      continue;
    }
    if (p < pattern->length && pattern->data[p] == '?') {
      p++;
      matched = 1;
    } else if (p < pattern->length && pattern->data[p] == '[' &&
               next_v == v + 1) {
      int result = class_match(pattern->data, pattern->length, &p,
                               (unsigned char)value->data[v]);
      matched = result >= 0 ? result : value->data[v] == pattern->data[p++];
    } else if (p < pattern->length) {
      size_t next_p = utf8_next(pattern->data, pattern->length, p);
      matched = next_p - p == next_v - v &&
                !memcmp(pattern->data + p, value->data + v, next_p - p);
      if (matched)
        p = next_p;
    }
    if (matched) {
      v = next_v;
      continue;
    }
    if (star_p != SIZE_MAX) {
      star_v = utf8_next(value->data, value->length, star_v);
      v = star_v;
      p = star_p;
      continue;
    }
    return 0;
  }
  while (p < pattern->length && pattern->data[p] == '*')
    p++;
  return p == pattern->length;
}
