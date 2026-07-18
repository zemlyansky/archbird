#ifndef ARCHBIRD_SHA256_H
#define ARCHBIRD_SHA256_H

#include <archbird/archbird.h>

typedef struct ArchbirdSha256Context {
  uint32_t state[8];
  uint8_t block[64];
  size_t block_length;
  uint64_t total_bytes;
} ArchbirdSha256Context;

void archbird_sha256_init(ArchbirdSha256Context *context);

ArchbirdStatus archbird_sha256_update(ArchbirdSha256Context *context,
                                      const uint8_t *input,
                                      size_t input_length);

void archbird_sha256_final(ArchbirdSha256Context *context, uint8_t digest[32]);

ArchbirdStatus archbird_sha256(const uint8_t *input, size_t input_length,
                               uint8_t digest[32]);

void archbird_sha256_hex(const uint8_t digest[32], char hex[65]);

#endif
