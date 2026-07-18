#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect_digest(const char *label, const uint8_t *input,
                          size_t input_length, const char *expected) {
  uint8_t digest[32];
  char hex[65];
  ArchbirdStatus status = archbird_sha256(input, input_length, digest);
  if (status != ARCHBIRD_OK) {
    fprintf(stderr, "FAIL %s: status %d\n", label, (int)status);
    failures++;
    return;
  }
  archbird_sha256_hex(digest, hex);
  if (strcmp(hex, expected) != 0) {
    fprintf(stderr, "FAIL %s: expected %s, got %s\n", label, expected, hex);
    failures++;
  }
}

int main(void) {
  static const uint8_t long_message[] =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  uint8_t *million_a = (uint8_t *)malloc(1000000);
  uint8_t digest[32];

  expect_digest(
      "empty", NULL, 0,
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  expect_digest(
      "abc", (const uint8_t *)"abc", 3,
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  expect_digest(
      "two-block", long_message, sizeof(long_message) - 1,
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  if (!million_a) {
    fputs("FAIL allocation\n", stderr);
    return 1;
  }
  memset(million_a, 'a', 1000000);
  expect_digest(
      "million-a", million_a, 1000000,
      "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  free(million_a);
  if (archbird_sha256(NULL, 1, digest) != ARCHBIRD_INVALID_ARGUMENT) {
    fputs("FAIL null input validation\n", stderr);
    failures++;
  }
  if (archbird_sha256((const uint8_t *)"", 0, NULL) !=
      ARCHBIRD_INVALID_ARGUMENT) {
    fputs("FAIL null digest validation\n", stderr);
    failures++;
  }
  if (failures) {
    fprintf(stderr, "%d SHA-256 test(s) failed\n", failures);
    return 1;
  }
  puts("native SHA-256 tests passed");
  return 0;
}
