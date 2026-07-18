#include "sha256.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

/* FIPS 180-4, sections 4.1.2, 4.2.2, 5.3.3, and 6.2. */

static const uint32_t SHA256_CONSTANTS[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
    UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
    UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
    UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
    UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
    UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
    UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
    UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
    UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
    UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
    UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
    UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
    UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
    UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
    UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
    UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
    UINT32_C(0xc67178f2)};

static uint32_t rotate_right(uint32_t value, unsigned count) {
  return (value >> count) | (value << (32u - count));
}

static uint32_t load_big_endian_u32(const uint8_t *bytes) {
  return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
         ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static void store_big_endian_u32(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)(value >> 24);
  bytes[1] = (uint8_t)(value >> 16);
  bytes[2] = (uint8_t)(value >> 8);
  bytes[3] = (uint8_t)value;
}

static void sha256_transform(ArchbirdSha256Context *context,
                             const uint8_t block[64]) {
  uint32_t words[64];
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
  uint32_t e;
  uint32_t f;
  uint32_t g;
  uint32_t h;
  size_t index;

  for (index = 0; index < 16; index++)
    words[index] = load_big_endian_u32(block + index * 4);
  for (; index < 64; index++) {
    uint32_t sigma0 = rotate_right(words[index - 15], 7) ^
                      rotate_right(words[index - 15], 18) ^
                      (words[index - 15] >> 3);
    uint32_t sigma1 = rotate_right(words[index - 2], 17) ^
                      rotate_right(words[index - 2], 19) ^
                      (words[index - 2] >> 10);
    words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
  }

  a = context->state[0];
  b = context->state[1];
  c = context->state[2];
  d = context->state[3];
  e = context->state[4];
  f = context->state[5];
  g = context->state[6];
  h = context->state[7];
  for (index = 0; index < 64; index++) {
    uint32_t choice = (e & f) ^ (~e & g);
    uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    uint32_t sum0 =
        rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    uint32_t sum1 =
        rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    uint32_t temporary1 =
        h + sum1 + choice + SHA256_CONSTANTS[index] + words[index];
    uint32_t temporary2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }
  context->state[0] += a;
  context->state[1] += b;
  context->state[2] += c;
  context->state[3] += d;
  context->state[4] += e;
  context->state[5] += f;
  context->state[6] += g;
  context->state[7] += h;
}

void archbird_sha256_init(ArchbirdSha256Context *context) {
  context->state[0] = UINT32_C(0x6a09e667);
  context->state[1] = UINT32_C(0xbb67ae85);
  context->state[2] = UINT32_C(0x3c6ef372);
  context->state[3] = UINT32_C(0xa54ff53a);
  context->state[4] = UINT32_C(0x510e527f);
  context->state[5] = UINT32_C(0x9b05688c);
  context->state[6] = UINT32_C(0x1f83d9ab);
  context->state[7] = UINT32_C(0x5be0cd19);
  context->block_length = 0;
  context->total_bytes = 0;
}

ArchbirdStatus archbird_sha256_update(ArchbirdSha256Context *context,
                                      const uint8_t *input,
                                      size_t input_length) {
  if (!context || (!input && input_length != 0))
    return ARCHBIRD_INVALID_ARGUMENT;
  if ((uint64_t)input_length > UINT64_MAX / UINT64_C(8) - context->total_bytes)
    return ARCHBIRD_LIMIT_EXCEEDED;
  context->total_bytes += (uint64_t)input_length;
  while (input_length != 0) {
    size_t available = sizeof(context->block) - context->block_length;
    size_t chunk = input_length < available ? input_length : available;
    memcpy(context->block + context->block_length, input, chunk);
    context->block_length += chunk;
    input += chunk;
    input_length -= chunk;
    if (context->block_length == sizeof(context->block)) {
      sha256_transform(context, context->block);
      context->block_length = 0;
    }
  }
  return ARCHBIRD_OK;
}

void archbird_sha256_final(ArchbirdSha256Context *context, uint8_t digest[32]) {
  uint64_t bit_length = context->total_bytes * UINT64_C(8);
  size_t index;
  context->block[context->block_length++] = UINT8_C(0x80);
  if (context->block_length > 56) {
    memset(context->block + context->block_length, 0,
           sizeof(context->block) - context->block_length);
    sha256_transform(context, context->block);
    context->block_length = 0;
  }
  memset(context->block + context->block_length, 0, 56 - context->block_length);
  for (index = 0; index < 8; index++)
    context->block[63 - index] = (uint8_t)(bit_length >> (index * 8));
  sha256_transform(context, context->block);
  for (index = 0; index < 8; index++)
    store_big_endian_u32(digest + index * 4, context->state[index]);
}

ArchbirdStatus archbird_sha256(const uint8_t *input, size_t input_length,
                               uint8_t digest[32]) {
  ArchbirdSha256Context context;
  ArchbirdStatus status;
  if (!digest || (!input && input_length != 0))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (input_length > (size_t)(UINT64_MAX / UINT64_C(8)))
    return ARCHBIRD_LIMIT_EXCEEDED;
  archbird_sha256_init(&context);
  status = archbird_sha256_update(&context, input, input_length);
  if (status != ARCHBIRD_OK)
    return status;
  archbird_sha256_final(&context, digest);
  return ARCHBIRD_OK;
}

void archbird_sha256_hex(const uint8_t digest[32], char hex[65]) {
  static const char digits[] = "0123456789abcdef";
  size_t index;
  for (index = 0; index < 32; index++) {
    hex[index * 2] = digits[digest[index] >> 4];
    hex[index * 2 + 1] = digits[digest[index] & UINT8_C(0x0f)];
  }
  hex[64] = '\0';
}
