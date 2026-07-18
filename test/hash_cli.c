#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
  uint8_t *bytes = NULL;
  size_t length = 0;
  size_t capacity = 0;
  uint8_t digest[32];
  char hex[65];
  for (;;) {
    size_t read_count;
    if (capacity - length < 4096) {
      size_t next_capacity = capacity ? capacity * 2 : 8192;
      uint8_t *next;
      if (next_capacity < capacity || next_capacity > SIZE_MAX - 1) {
        fputs("input too large\n", stderr);
        free(bytes);
        return 2;
      }
      next = (uint8_t *)realloc(bytes, next_capacity);
      if (!next) {
        fputs("out of memory\n", stderr);
        free(bytes);
        return 2;
      }
      bytes = next;
      capacity = next_capacity;
    }
    read_count = fread(bytes + length, 1, capacity - length, stdin);
    length += read_count;
    if (read_count == 0) {
      if (ferror(stdin)) {
        fputs("read failed\n", stderr);
        free(bytes);
        return 2;
      }
      break;
    }
  }
  if (archbird_sha256(bytes, length, digest) != ARCHBIRD_OK) {
    fputs("hash failed\n", stderr);
    free(bytes);
    return 2;
  }
  archbird_sha256_hex(digest, hex);
  puts(hex);
  free(bytes);
  return 0;
}
