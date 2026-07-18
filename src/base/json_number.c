#include "json_number.h"

#include "json_internal.h"

#include "yyjson.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int token_is_real(const char *token, size_t length) {
  size_t index;
  for (index = 0; index < length; index++) {
    if (token[index] == '.' || token[index] == 'e' || token[index] == 'E')
      return 1;
  }
  return 0;
}

ArchbirdStatus ab_json_real_parse(ArchbirdEngine *engine, const char *token,
                                  size_t token_length, double *out_value) {
  char local[128];
  char *copy = local;
  yyjson_val parsed;
  yyjson_read_err error;
  yyjson_alc allocator;
  const char *end;
  double value;
  if (!engine || !token || !out_value || !token_length)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!token_is_real(token, token_length))
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "expected a JSON real-number token");
  if (token_length >= sizeof(local)) {
    if (token_length == SIZE_MAX)
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "JSON number token is too large");
    copy = (char *)ab_malloc(engine, token_length + 1);
    if (!copy)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory parsing JSON real number");
  }
  memcpy(copy, token, token_length);
  copy[token_length] = '\0';
  ab_yyjson_allocator(engine, &allocator);
  end = yyjson_read_number(copy, &parsed, 0, &allocator, &error);
  if (!end || end != copy + token_length || !yyjson_is_real(&parsed)) {
    if (copy != local)
      ab_free(engine, copy);
    if (error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory parsing JSON real number");
    return archbird_error_set(
        engine, ARCHBIRD_UNSUPPORTED_NUMBER, ARCHBIRD_NO_OFFSET,
        "JSON real number is outside the finite binary64 domain");
  }
  value = yyjson_get_real(&parsed);
  if (copy != local)
    ab_free(engine, copy);
  if (!isfinite(value))
    return archbird_error_set(
        engine, ARCHBIRD_UNSUPPORTED_NUMBER, ARCHBIRD_NO_OFFSET,
        "JSON real number is outside the finite binary64 domain");
  *out_value = value;
  return ARCHBIRD_OK;
}

static size_t unsigned_decimal(char *output, unsigned value) {
  char reverse[16];
  size_t count = 0;
  size_t index;
  do {
    reverse[count++] = (char)('0' + value % 10u);
    value /= 10u;
  } while (value);
  for (index = 0; index < count; index++)
    output[index] = reverse[count - index - 1];
  return count;
}

ArchbirdStatus ab_json_real_format(ArchbirdEngine *engine, double value,
                                   char output[AB_JSON_REAL_BUFFER_SIZE],
                                   size_t *out_length) {
  yyjson_val number = {0};
  char shortest[AB_JSON_REAL_BUFFER_SIZE];
  char digits[AB_JSON_REAL_BUFFER_SIZE];
  char *shortest_end;
  const char *cursor;
  const char *exponent_mark;
  const char *mantissa_end;
  const char *dot;
  size_t shortest_length;
  size_t before_length;
  size_t digit_count = 0;
  size_t first = SIZE_MAX;
  size_t significant_count;
  size_t index;
  size_t written = 0;
  int negative;
  int exponent = 0;
  int decimal_exponent;
  if (!engine || !output || !out_length)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!isfinite(value))
    return archbird_error_set(engine, ARCHBIRD_UNSUPPORTED_NUMBER,
                              ARCHBIRD_NO_OFFSET,
                              "cannot render a non-finite JSON real number");
  (void)yyjson_set_real(&number, value);
  shortest_end = yyjson_write_number(&number, shortest);
  if (!shortest_end)
    return archbird_error_set(engine, ARCHBIRD_UNSUPPORTED_NUMBER,
                              ARCHBIRD_NO_OFFSET,
                              "failed to render JSON real number");
  shortest_length = (size_t)(shortest_end - shortest);
  if (shortest_length >= sizeof(shortest))
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "JSON real formatter exceeded its bound");
  shortest[shortest_length] = '\0';

  negative = shortest[0] == '-';
  cursor = shortest + negative;
  exponent_mark = strchr(cursor, 'e');
  if (!exponent_mark)
    exponent_mark = strchr(cursor, 'E');
  mantissa_end = exponent_mark ? exponent_mark : shortest_end;
  dot = memchr(cursor, '.', (size_t)(mantissa_end - cursor));
  before_length = (size_t)((dot ? dot : mantissa_end) - cursor);
  if (exponent_mark) {
    const char *part = exponent_mark + 1;
    int exponent_negative = 0;
    if (*part == '+' || *part == '-') {
      exponent_negative = *part == '-';
      part++;
    }
    while (part < shortest_end) {
      if (exponent > 10000)
        return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                                  "JSON real exponent exceeded its bound");
      exponent = exponent * 10 + (*part++ - '0');
    }
    if (exponent_negative)
      exponent = -exponent;
  }
  for (; cursor < mantissa_end; cursor++) {
    if (*cursor == '.')
      continue;
    if (digit_count >= sizeof(digits))
      return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                                "JSON real significand exceeded its bound");
    digits[digit_count] = *cursor;
    if (first == SIZE_MAX && *cursor != '0')
      first = digit_count;
    digit_count++;
  }
  if (first == SIZE_MAX) {
    const char *zero = negative ? "-0.0" : "0.0";
    size_t zero_length = negative ? 4 : 3;
    memcpy(output, zero, zero_length + 1);
    *out_length = zero_length;
    return ARCHBIRD_OK;
  }
  decimal_exponent = exponent + (int)before_length - 1 - (int)first;
  while (digit_count > first + 1 && digits[digit_count - 1] == '0')
    digit_count--;
  significant_count = digit_count - first;
  if (negative)
    output[written++] = '-';
  if (decimal_exponent < -4 || decimal_exponent >= 16) {
    unsigned magnitude =
        (unsigned)(decimal_exponent < 0 ? -decimal_exponent : decimal_exponent);
    char exponent_digits[16];
    size_t exponent_length;
    output[written++] = digits[first];
    if (significant_count > 1) {
      output[written++] = '.';
      memcpy(output + written, digits + first + 1, significant_count - 1);
      written += significant_count - 1;
    }
    output[written++] = 'e';
    output[written++] = decimal_exponent < 0 ? '-' : '+';
    exponent_length = unsigned_decimal(exponent_digits, magnitude);
    if (exponent_length < 2)
      output[written++] = '0';
    memcpy(output + written, exponent_digits, exponent_length);
    written += exponent_length;
  } else {
    int point = decimal_exponent + 1;
    if (point <= 0) {
      output[written++] = '0';
      output[written++] = '.';
      for (index = 0; index < (size_t)(-point); index++)
        output[written++] = '0';
      memcpy(output + written, digits + first, significant_count);
      written += significant_count;
    } else if ((size_t)point >= significant_count) {
      memcpy(output + written, digits + first, significant_count);
      written += significant_count;
      for (index = significant_count; index < (size_t)point; index++)
        output[written++] = '0';
      output[written++] = '.';
      output[written++] = '0';
    } else {
      memcpy(output + written, digits + first, (size_t)point);
      written += (size_t)point;
      output[written++] = '.';
      memcpy(output + written, digits + first + point,
             significant_count - (size_t)point);
      written += significant_count - (size_t)point;
    }
  }
  if (written >= AB_JSON_REAL_BUFFER_SIZE)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "Python JSON real formatter exceeded its bound");
  output[written] = '\0';
  *out_length = written;
  return ARCHBIRD_OK;
}
