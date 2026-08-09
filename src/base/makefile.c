#include "base/makefile.h"

#include <ctype.h>

int ab_make_assignment(const char *line, size_t length, size_t *name_start,
                       size_t *name_end, size_t *operator_start,
                       size_t *operator_length, size_t *value_start) {
  size_t index = 0;
  if (!line || !name_start || !name_end || !operator_start ||
      !operator_length || !value_start)
    return 0;
  while (index < length && isspace((unsigned char)line[index]))
    index++;
  *name_start = index;
  if (index == length ||
      !(isalpha((unsigned char)line[index]) || line[index] == '_'))
    return 0;
  index++;
  while (index < length &&
         (isalnum((unsigned char)line[index]) || line[index] == '_'))
    index++;
  *name_end = index;
  while (index < length && isspace((unsigned char)line[index]))
    index++;
  *operator_start = index;
  if (index + 1 < length &&
      (line[index] == '?' || line[index] == ':' || line[index] == '+') &&
      line[index + 1] == '=') {
    *operator_length = 2;
    index += 2;
  } else if (index < length && line[index] == '=') {
    *operator_length = 1;
    index++;
  } else {
    return 0;
  }
  while (index < length && isspace((unsigned char)line[index]))
    index++;
  *value_start = index;
  return 1;
}
