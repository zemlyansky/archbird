#include "map_internal.h"

#include "archbird_internal.h"
#include "lexical/tokenizer.h"
#include "package_json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct MakeVariable {
  AbString name;
  AbString value;
  int simple;
} MakeVariable;

typedef struct BuildWork {
  AbString name;
  AbStringArray deps;
  AbStringArray recipes;
  AbStringArray conditions;
} BuildWork;

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static int string_compare(const void *left_raw, const void *right_raw) {
  return ab_string_compare((const AbString *)left_raw,
                           (const AbString *)right_raw);
}

static int build_compare(const void *left_raw, const void *right_raw) {
  const AbMapBuildRoute *left = (const AbMapBuildRoute *)left_raw;
  const AbMapBuildRoute *right = (const AbMapBuildRoute *)right_raw;
  int compared = ab_string_compare(&left->source, &right->source);
  if (compared)
    return compared;
  compared = ab_string_compare(&left->name, &right->name);
  if (compared)
    return compared;
  return ab_string_compare(&left->command, &right->command);
}

static ArchbirdStatus append_unique(ArchbirdEngine *engine,
                                    AbStringArray *array, const char *data,
                                    size_t length) {
  AbString *resized;
  size_t index;
  for (index = 0; index < array->count; index++) {
    if (array->items[index].length == length &&
        (!length || memcmp(array->items[index].data, data, length) == 0))
      return ARCHBIRD_OK;
  }
  if (array->count == SIZE_MAX / sizeof(*array->items))
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "too many build-route values");
  resized = (AbString *)ab_realloc(engine, array->items,
                                   (array->count + 1) * sizeof(*array->items));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting build routes");
  array->items = resized;
  memset(&array->items[array->count], 0, sizeof(*array->items));
  if (ab_string_copy(engine, &array->items[array->count], data, length) !=
      ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  array->count++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus append_value(ArchbirdEngine *engine, AbStringArray *array,
                                   const char *data, size_t length) {
  AbString *resized;
  if (array->count == SIZE_MAX / sizeof(*array->items))
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "too many build parser values");
  resized = (AbString *)ab_realloc(engine, array->items,
                                   (array->count + 1) * sizeof(*array->items));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory parsing build file");
  array->items = resized;
  memset(&array->items[array->count], 0, sizeof(*array->items));
  if (ab_string_copy(engine, &array->items[array->count], data, length) !=
      ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  array->count++;
  return ARCHBIRD_OK;
}

static void string_array_free(ArchbirdEngine *engine, AbStringArray *array) {
  size_t index;
  for (index = 0; index < array->count; index++)
    ab_string_free(engine, &array->items[index]);
  ab_free(engine, array->items);
  memset(array, 0, sizeof(*array));
}

static ArchbirdStatus logical_lines(ArchbirdEngine *engine, const uint8_t *text,
                                    size_t length, AbStringArray *out) {
  AbBuffer current;
  size_t line_start = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_buffer_init(&current, engine);
  while (status == ARCHBIRD_OK && line_start <= length) {
    size_t line_end = line_start;
    size_t start;
    size_t end;
    while (line_end < length && text[line_end] != '\n')
      line_end++;
    end = line_end;
    if (end > line_start && text[end - 1] == '\r')
      end--;
    while (end > line_start && isspace((unsigned char)text[end - 1]))
      end--;
    start = line_start;
    if (current.length) {
      while (start < end && isspace((unsigned char)text[start]))
        start++;
      status = ab_buffer_literal(&current, " ");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&current, text + start, end - start);
    while (current.length &&
           isspace((unsigned char)current.data[current.length - 1]))
      current.length--;
    if (current.length && current.data[current.length - 1] == '\\') {
      current.length--;
      while (current.length &&
             isspace((unsigned char)current.data[current.length - 1]))
        current.length--;
    } else {
      status =
          append_value(engine, out, (const char *)current.data, current.length);
      current.length = 0;
    }
    if (line_end == length)
      break;
    line_start = line_end + 1;
  }
  if (status == ARCHBIRD_OK && current.length)
    status =
        append_value(engine, out, (const char *)current.data, current.length);
  ab_buffer_free(&current);
  return status;
}

static MakeVariable *variable(MakeVariable *variables, size_t count,
                              const char *name, size_t length) {
  size_t index;
  for (index = 0; index < count; index++) {
    if (variables[index].name.length == length &&
        memcmp(variables[index].name.data, name, length) == 0)
      return &variables[index];
  }
  return NULL;
}

static ArchbirdStatus expand_make(ArchbirdEngine *engine, const char *value,
                                  size_t length, MakeVariable *variables,
                                  size_t variable_count, size_t depth,
                                  const AbStringArray *expanding,
                                  AbString *out) {
  AbBuffer buffer;
  size_t index = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (depth > 12)
    return ab_string_copy(engine, out, value, length);
  ab_buffer_init(&buffer, engine);
  while (status == ARCHBIRD_OK && index < length) {
    size_t start = index;
    if (value[index] == '$' && index + 3 < length && value[index + 1] == '(') {
      size_t name_start = index + 2;
      size_t name_end = name_start;
      MakeVariable *found;
      int cycle = 0;
      size_t active;
      while (name_end < length && (isalnum((unsigned char)value[name_end]) ||
                                   value[name_end] == '_'))
        name_end++;
      if (name_end > name_start && name_end < length &&
          value[name_end] == ')') {
        found = variable(variables, variable_count, value + name_start,
                         name_end - name_start);
        for (active = 0; active < expanding->count; active++) {
          if (expanding->items[active].length == name_end - name_start &&
              memcmp(expanding->items[active].data, value + name_start,
                     name_end - name_start) == 0)
            cycle = 1;
        }
        if (found && !cycle) {
          AbStringArray next = {0};
          AbString expanded = {0};
          for (active = 0; status == ARCHBIRD_OK && active < expanding->count;
               active++)
            status = append_unique(engine, &next, expanding->items[active].data,
                                   expanding->items[active].length);
          if (status == ARCHBIRD_OK)
            status = append_unique(engine, &next, found->name.data,
                                   found->name.length);
          if (status == ARCHBIRD_OK)
            status = expand_make(engine, found->value.data, found->value.length,
                                 variables, variable_count, depth + 1, &next,
                                 &expanded);
          if (status == ARCHBIRD_OK)
            status = ab_buffer_append(&buffer, (const uint8_t *)expanded.data,
                                      expanded.length);
          ab_string_free(engine, &expanded);
          string_array_free(engine, &next);
          index = name_end + 1;
          continue;
        }
      }
    }
    while (index < length && !(value[index] == '$' && index + 1 < length &&
                               value[index + 1] == '('))
      index++;
    if (index == start)
      index++;
    status = ab_buffer_append(&buffer, (const uint8_t *)value + start,
                              index - start);
  }
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static int assignment(const AbString *line, size_t *name_start,
                      size_t *name_end, size_t *operator_start,
                      size_t *operator_length, size_t *value_start) {
  size_t index = 0;
  while (index < line->length && isspace((unsigned char)line->data[index]))
    index++;
  *name_start = index;
  if (index == line->length ||
      !(isalpha((unsigned char)line->data[index]) || line->data[index] == '_'))
    return 0;
  index++;
  while (index < line->length && (isalnum((unsigned char)line->data[index]) ||
                                  line->data[index] == '_'))
    index++;
  *name_end = index;
  while (index < line->length && isspace((unsigned char)line->data[index]))
    index++;
  *operator_start = index;
  if (index + 1 < line->length &&
      (line->data[index] == '?' || line->data[index] == ':' ||
       line->data[index] == '+') &&
      line->data[index + 1] == '=') {
    *operator_length = 2;
    index += 2;
  } else if (index < line->length && line->data[index] == '=') {
    *operator_length = 1;
    index++;
  } else {
    return 0;
  }
  while (index < line->length && isspace((unsigned char)line->data[index]))
    index++;
  *value_start = index;
  return 1;
}

static ArchbirdStatus strip_make_comment(ArchbirdEngine *engine,
                                         const char *value, size_t length,
                                         AbString *out) {
  char *copy = (char *)ab_malloc(engine, length + 1);
  size_t index;
  size_t write = 0;
  int escaped = 0;
  ArchbirdStatus status;
  if (!copy)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory parsing Make variable");
  for (index = 0; index < length; index++) {
    char character = value[index];
    if (character == '#' && !escaped)
      break;
    if (character == '#' && escaped && write && copy[write - 1] == '\\')
      copy[write - 1] = '#';
    else
      copy[write++] = character;
    escaped = character == '\\' && !escaped;
    if (character != '\\')
      escaped = 0;
  }
  while (write && isspace((unsigned char)copy[write - 1]))
    write--;
  status = ab_string_copy(engine, out, copy, write);
  ab_free(engine, copy);
  return status;
}

static void variables_free(ArchbirdEngine *engine, MakeVariable *variables,
                           size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    ab_string_free(engine, &variables[index].name);
    ab_string_free(engine, &variables[index].value);
  }
  ab_free(engine, variables);
}

static ArchbirdStatus parse_variables(ArchbirdEngine *engine,
                                      const AbStringArray *lines,
                                      MakeVariable **out_variables,
                                      size_t *out_count) {
  MakeVariable *variables = NULL;
  size_t count = 0;
  size_t line_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out_variables = NULL;
  *out_count = 0;
  for (line_index = 0; status == ARCHBIRD_OK && line_index < lines->count;
       line_index++) {
    const AbString *line = &lines->items[line_index];
    size_t name_start;
    size_t name_end;
    size_t operator_start;
    size_t operator_length;
    size_t value_start;
    MakeVariable *item;
    int new_item = 0;
    AbString value = {0};
    if (line->length && line->data[0] == '\t')
      continue;
    if (!assignment(line, &name_start, &name_end, &operator_start,
                    &operator_length, &value_start))
      continue;
    status = strip_make_comment(engine, line->data + value_start,
                                line->length - value_start, &value);
    if (status != ARCHBIRD_OK)
      break;
    item = variable(variables, count, line->data + name_start,
                    name_end - name_start);
    if (!item) {
      MakeVariable *resized;
      if (count == SIZE_MAX / sizeof(*variables)) {
        status =
            archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                               ARCHBIRD_NO_OFFSET, "too many Make variables");
        ab_string_free(engine, &value);
        break;
      }
      resized = (MakeVariable *)ab_realloc(engine, variables,
                                           (count + 1) * sizeof(*variables));
      if (!resized) {
        status = archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory collecting Make variables");
        ab_string_free(engine, &value);
        break;
      }
      variables = resized;
      item = &variables[count++];
      new_item = 1;
      memset(item, 0, sizeof(*item));
      status = ab_string_copy(engine, &item->name, line->data + name_start,
                              name_end - name_start);
    }
    if (status != ARCHBIRD_OK) {
      ab_string_free(engine, &value);
      break;
    }
    if (line->data[operator_start] == '?' && item->value.data) {
      ab_string_free(engine, &value);
      continue;
    }
    if (line->data[operator_start] == '+' && item->value.data) {
      AbString expanded = {0};
      AbString joined = {0};
      AbBuffer buffer;
      AbStringArray none = {0};
      if (item->simple)
        status = expand_make(engine, value.data, value.length, variables, count,
                             0, &none, &expanded);
      else
        status = ab_string_copy(engine, &expanded, value.data, value.length);
      ab_buffer_init(&buffer, engine);
      if (status == ARCHBIRD_OK && item->value.length)
        status = ab_buffer_append(&buffer, (const uint8_t *)item->value.data,
                                  item->value.length);
      if (status == ARCHBIRD_OK && item->value.length && expanded.length)
        status = ab_buffer_literal(&buffer, " ");
      if (status == ARCHBIRD_OK && expanded.length)
        status = ab_buffer_append(&buffer, (const uint8_t *)expanded.data,
                                  expanded.length);
      if (status == ARCHBIRD_OK)
        status = ab_string_copy(engine, &joined, (const char *)buffer.data,
                                buffer.length);
      ab_buffer_free(&buffer);
      ab_string_free(engine, &expanded);
      ab_string_free(engine, &value);
      if (status == ARCHBIRD_OK) {
        ab_string_free(engine, &item->value);
        item->value = joined;
      } else {
        ab_string_free(engine, &joined);
      }
    } else {
      if (line->data[operator_start] == ':') {
        AbString expanded = {0};
        AbStringArray none = {0};
        status = expand_make(engine, value.data, value.length, variables,
                             new_item ? count - 1 : count, 0, &none, &expanded);
        ab_string_free(engine, &value);
        value = expanded;
        item->simple = 1;
      } else {
        item->simple = 0;
      }
      if (status == ARCHBIRD_OK) {
        ab_string_free(engine, &item->value);
        item->value = value;
      } else {
        ab_string_free(engine, &value);
      }
    }
  }
  *out_variables = variables;
  *out_count = count;
  return status;
}

ArchbirdStatus ab_make_variable_value(ArchbirdEngine *engine,
                                      const uint8_t *text, size_t text_length,
                                      const AbString *name, AbString *out,
                                      int *found) {
  AbStringArray lines = {0};
  MakeVariable *variables = NULL;
  size_t variable_count = 0;
  MakeVariable *selected;
  AbStringArray none = {0};
  ArchbirdStatus status;
  if (!engine || (!text && text_length) || !name || !out || !found)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  *found = 0;
  status = logical_lines(engine, text, text_length, &lines);
  if (status == ARCHBIRD_OK)
    status = parse_variables(engine, &lines, &variables, &variable_count);
  selected = status == ARCHBIRD_OK
                 ? variable(variables, variable_count, name->data, name->length)
                 : NULL;
  if (selected) {
    status = expand_make(engine, selected->value.data, selected->value.length,
                         variables, variable_count, 0, &none, out);
    if (status == ARCHBIRD_OK)
      *found = 1;
  }
  variables_free(engine, variables, variable_count);
  string_array_free(engine, &lines);
  return status;
}

static BuildWork *work_route(BuildWork **works, size_t *count,
                             ArchbirdEngine *engine, const char *name,
                             size_t name_length, ArchbirdStatus *status) {
  BuildWork *resized;
  size_t index;
  for (index = 0; index < *count; index++) {
    if ((*works)[index].name.length == name_length &&
        memcmp((*works)[index].name.data, name, name_length) == 0)
      return &(*works)[index];
  }
  if (*count == SIZE_MAX / sizeof(**works)) {
    *status = archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                 ARCHBIRD_NO_OFFSET, "too many Make targets");
    return NULL;
  }
  resized =
      (BuildWork *)ab_realloc(engine, *works, (*count + 1) * sizeof(**works));
  if (!resized) {
    *status =
        archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                           "out of memory collecting Make targets");
    return NULL;
  }
  *works = resized;
  memset(&(*works)[*count], 0, sizeof(**works));
  *status = ab_string_copy(engine, &(*works)[*count].name, name, name_length);
  if (*status != ARCHBIRD_OK)
    return NULL;
  (*count)++;
  return &(*works)[*count - 1];
}

static void works_free(ArchbirdEngine *engine, BuildWork *works, size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    ab_string_free(engine, &works[index].name);
    string_array_free(engine, &works[index].deps);
    string_array_free(engine, &works[index].recipes);
    string_array_free(engine, &works[index].conditions);
  }
  ab_free(engine, works);
}

static int starts_condition(const AbString *line, const char *word) {
  size_t length = strlen(word);
  return line->length >= length && memcmp(line->data, word, length) == 0 &&
         (line->length == length || isspace((unsigned char)line->data[length]));
}

static ArchbirdStatus joined_conditions(ArchbirdEngine *engine,
                                        const AbStringArray *stack,
                                        AbString *out) {
  AbBuffer buffer;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_buffer_init(&buffer, engine);
  for (index = 0; status == ARCHBIRD_OK && index < stack->count; index++) {
    if (index)
      status = ab_buffer_literal(&buffer, " && ");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(&buffer, (const uint8_t *)stack->items[index].data,
                           stack->items[index].length);
  }
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static int contains_byte(const char *data, size_t length, char value) {
  return memchr(data, value, length) != NULL;
}

static ArchbirdStatus
parse_make_works(ArchbirdEngine *engine, const AbStringArray *lines,
                 MakeVariable *variables, size_t variable_count,
                 BuildWork **out_works, size_t *out_count) {
  BuildWork *works = NULL;
  size_t work_count = 0;
  size_t *current = NULL;
  size_t current_count = 0;
  AbStringArray conditions = {0};
  size_t line_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out_works = NULL;
  *out_count = 0;
  for (line_index = 0; status == ARCHBIRD_OK && line_index < lines->count;
       line_index++) {
    const AbString *line = &lines->items[line_index];
    size_t start = 0;
    size_t end = line->length;
    AbString stripped;
    size_t name_start;
    size_t name_end;
    size_t operator_start;
    size_t operator_length;
    size_t value_start;
    while (start < end && isspace((unsigned char)line->data[start]))
      start++;
    while (end > start && isspace((unsigned char)line->data[end - 1]))
      end--;
    stripped.data = line->data + start;
    stripped.length = end - start;
    if (line->length && line->data[0] == '\t') {
      size_t route_index;
      for (route_index = 0; route_index < current_count; route_index++) {
        status = append_value(engine, &works[current[route_index]].recipes,
                              stripped.data, stripped.length);
        if (status != ARCHBIRD_OK)
          break;
      }
      continue;
    }
    if (starts_condition(&stripped, "ifeq") ||
        starts_condition(&stripped, "ifneq") ||
        starts_condition(&stripped, "ifdef") ||
        starts_condition(&stripped, "ifndef")) {
      status =
          append_value(engine, &conditions, stripped.data, stripped.length);
      current_count = 0;
      continue;
    }
    if (starts_condition(&stripped, "else")) {
      if (conditions.count) {
        AbString replacement = {0};
        AbBuffer buffer;
        size_t alternate_start = 4;
        while (alternate_start < stripped.length &&
               isspace((unsigned char)stripped.data[alternate_start]))
          alternate_start++;
        ab_buffer_init(&buffer, engine);
        status = ab_buffer_literal(&buffer, "else(");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(
              &buffer,
              (const uint8_t *)conditions.items[conditions.count - 1].data,
              conditions.items[conditions.count - 1].length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&buffer, ")");
        if (status == ARCHBIRD_OK && alternate_start < stripped.length)
          status = ab_buffer_literal(&buffer, " && ");
        if (status == ARCHBIRD_OK && alternate_start < stripped.length)
          status = ab_buffer_append(
              &buffer, (const uint8_t *)stripped.data + alternate_start,
              stripped.length - alternate_start);
        if (status == ARCHBIRD_OK)
          status = ab_string_copy(engine, &replacement,
                                  (const char *)buffer.data, buffer.length);
        ab_buffer_free(&buffer);
        if (status == ARCHBIRD_OK) {
          ab_string_free(engine, &conditions.items[conditions.count - 1]);
          conditions.items[conditions.count - 1] = replacement;
        }
      }
      current_count = 0;
      continue;
    }
    if (string_literal(&stripped, "endif")) {
      if (conditions.count)
        ab_string_free(engine, &conditions.items[--conditions.count]);
      current_count = 0;
      continue;
    }
    if (assignment(&stripped, &name_start, &name_end, &operator_start,
                   &operator_length, &value_start)) {
      current_count = 0;
      continue;
    }
    {
      size_t colon = 0;
      size_t target_end;
      AbString expanded = {0};
      AbStringArray none = {0};
      while (colon < line->length && line->data[colon] != ':' &&
             line->data[colon] != '=')
        colon++;
      if (!line->length || line->data[0] == '#' || line->data[0] == '\t' ||
          colon == line->length || line->data[colon] != ':') {
        current_count = 0;
        continue;
      }
      target_end = colon;
      while (target_end && isspace((unsigned char)line->data[target_end - 1]))
        target_end--;
      start = colon + 1;
      while (start < line->length && isspace((unsigned char)line->data[start]))
        start++;
      status = expand_make(engine, line->data + start, line->length - start,
                           variables, variable_count, 0, &none, &expanded);
      ab_free(engine, current);
      current = NULL;
      current_count = 0;
      if (status != ARCHBIRD_OK) {
        ab_string_free(engine, &expanded);
        break;
      }
      start = 0;
      while (status == ARCHBIRD_OK && start < target_end) {
        size_t target_start;
        size_t route_index;
        BuildWork *route;
        size_t *resized;
        while (start < target_end && isspace((unsigned char)line->data[start]))
          start++;
        target_start = start;
        while (start < target_end && !isspace((unsigned char)line->data[start]))
          start++;
        if (start == target_start ||
            contains_byte(line->data + target_start, start - target_start, '%'))
          continue;
        if (start - target_start == 6 &&
            memcmp(line->data + target_start, ".PHONY", 6) == 0) {
          current_count = 0;
          continue;
        }
        route =
            work_route(&works, &work_count, engine, line->data + target_start,
                       start - target_start, &status);
        if (status != ARCHBIRD_OK)
          break;
        route_index = (size_t)(route - works);
        resized = (size_t *)ab_realloc(engine, current,
                                       (current_count + 1) * sizeof(*current));
        if (!resized) {
          status = archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                      ARCHBIRD_NO_OFFSET,
                                      "out of memory tracking Make targets");
          break;
        }
        current = resized;
        current[current_count++] = route_index;
      }
      if (status == ARCHBIRD_OK && current_count) {
        size_t dep_start = 0;
        while (dep_start < expanded.length) {
          size_t dep_end;
          while (dep_start < expanded.length &&
                 isspace((unsigned char)expanded.data[dep_start]))
            dep_start++;
          dep_end = dep_start;
          while (dep_end < expanded.length &&
                 !isspace((unsigned char)expanded.data[dep_end]))
            dep_end++;
          if (dep_end > dep_start && expanded.data[dep_start] != '|') {
            size_t route;
            for (route = 0; route < current_count; route++) {
              status =
                  append_unique(engine, &works[current[route]].deps,
                                expanded.data + dep_start, dep_end - dep_start);
              if (status != ARCHBIRD_OK)
                break;
            }
          }
          dep_start = dep_end;
        }
        if (status == ARCHBIRD_OK && conditions.count) {
          AbString joined = {0};
          size_t route;
          status = joined_conditions(engine, &conditions, &joined);
          for (route = 0; status == ARCHBIRD_OK && route < current_count;
               route++)
            status = append_unique(engine, &works[current[route]].conditions,
                                   joined.data, joined.length);
          ab_string_free(engine, &joined);
        }
      }
      ab_string_free(engine, &expanded);
    }
  }
  ab_free(engine, current);
  string_array_free(engine, &conditions);
  if (status != ARCHBIRD_OK) {
    works_free(engine, works, work_count);
    return status;
  }
  *out_works = works;
  *out_count = work_count;
  return ARCHBIRD_OK;
}

static int path_character(uint8_t value) {
  return isalnum(value) || value == '_' || value == '@' || value == '.' ||
         value == '/' || value == '*' || value == '+' || value == ':' ||
         value == '-';
}

static ArchbirdStatus
recipe_paths(ArchbirdEngine *engine, const AbStringArray *recipes,
             MakeVariable *variables, size_t variable_count,
             AbStringArray *out_paths, AbString *out_command) {
  AbBuffer command;
  size_t recipe_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_buffer_init(&command, engine);
  for (recipe_index = 0; status == ARCHBIRD_OK && recipe_index < recipes->count;
       recipe_index++) {
    const AbString *recipe = &recipes->items[recipe_index];
    AbString expanded = {0};
    AbStringArray none = {0};
    size_t index = 0;
    if (recipe_index)
      status = ab_buffer_literal(&command, " && ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&command, (const uint8_t *)recipe->data,
                                recipe->length);
    if (status == ARCHBIRD_OK)
      status = expand_make(engine, recipe->data, recipe->length, variables,
                           variable_count, 0, &none, &expanded);
    while (status == ARCHBIRD_OK && index < expanded.length) {
      int boundary =
          index == 0 || isspace((unsigned char)expanded.data[index - 1]) ||
          expanded.data[index - 1] == '=' || expanded.data[index - 1] == '\'' ||
          expanded.data[index - 1] == '"';
      size_t start;
      size_t end;
      if (!boundary || !path_character((uint8_t)expanded.data[index])) {
        index++;
        continue;
      }
      start = index;
      while (index < expanded.length &&
             path_character((uint8_t)expanded.data[index]))
        index++;
      end = index;
      if (end - start >= 2 && expanded.data[start] == '.' &&
          expanded.data[start + 1] == '/')
        start += 2;
      while (end > start &&
             (expanded.data[end - 1] == '\\' || expanded.data[end - 1] == ',' ||
              expanded.data[end - 1] == ';' || expanded.data[end - 1] == ':'))
        end--;
      if (end > start && (memchr(expanded.data + start, '/', end - start) ||
                          (end - start >= 5 &&
                           memcmp(expanded.data + start, "build", 5) == 0)))
        status = append_unique(engine, out_paths, expanded.data + start,
                               end - start);
    }
    ab_string_free(engine, &expanded);
  }
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(engine, out_command, (const char *)command.data,
                            command.length);
  if (status == ARCHBIRD_OK && out_paths->count > 1)
    qsort(out_paths->items, out_paths->count, sizeof(*out_paths->items),
          string_compare);
  ab_buffer_free(&command);
  return status;
}

static ArchbirdStatus reserve_build(AbMapState *state, AbMapBuildRoute **out) {
  AbMapBuildRoute *resized;
  if (state->build_count == SIZE_MAX / sizeof(*state->builds))
    return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET, "too many build routes");
  resized = (AbMapBuildRoute *)ab_realloc(state->engine, state->builds,
                                          (state->build_count + 1) *
                                              sizeof(*state->builds));
  if (!resized)
    return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory storing build routes");
  state->builds = resized;
  *out = &state->builds[state->build_count];
  memset(*out, 0, sizeof(**out));
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_make_routes(AbMapState *state,
                                      const AbConfigBuild *config,
                                      const uint8_t *text, size_t length) {
  AbStringArray lines = {0};
  MakeVariable *variables = NULL;
  size_t variable_count = 0;
  BuildWork *works = NULL;
  size_t work_count = 0;
  size_t index;
  ArchbirdStatus status = logical_lines(state->engine, text, length, &lines);
  if (status == ARCHBIRD_OK)
    status =
        parse_variables(state->engine, &lines, &variables, &variable_count);
  if (status == ARCHBIRD_OK)
    status = parse_make_works(state->engine, &lines, variables, variable_count,
                              &works, &work_count);
  for (index = 0; status == ARCHBIRD_OK && index < work_count; index++) {
    BuildWork *work = &works[index];
    AbMapBuildRoute *route = NULL;
    if (!work->name.length || work->name.data[0] == '.' ||
        contains_byte(work->name.data, work->name.length, '%') ||
        contains_byte(work->name.data, work->name.length, '$'))
      continue;
    status = reserve_build(state, &route);
    if (status == ARCHBIRD_OK)
      state->build_count++;
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(state->engine, &route->source, config->path.data,
                              config->path.length);
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(state->engine, &route->name, work->name.data,
                              work->name.length);
    if (status == ARCHBIRD_OK) {
      route->deps = work->deps;
      memset(&work->deps, 0, sizeof(work->deps));
      route->conditions = work->conditions;
      memset(&work->conditions, 0, sizeof(work->conditions));
      if (route->deps.count > 1)
        qsort(route->deps.items, route->deps.count, sizeof(*route->deps.items),
              string_compare);
      if (route->conditions.count > 1)
        qsort(route->conditions.items, route->conditions.count,
              sizeof(*route->conditions.items), string_compare);
      status = recipe_paths(state->engine, &work->recipes, variables,
                            variable_count, &route->paths, &route->command);
    }
  }
  works_free(state->engine, works, work_count);
  variables_free(state->engine, variables, variable_count);
  string_array_free(state->engine, &lines);
  return status;
}

static const AbMapPackage *package_for_manifest(const AbMapState *state,
                                                const AbString *path) {
  size_t index;
  for (index = 0; index < state->package_count; index++) {
    if (ab_string_equal(&state->packages[index].manifest, path))
      return &state->packages[index];
  }
  return NULL;
}

static ArchbirdStatus add_npm_routes(AbMapState *state,
                                     const AbConfigBuild *config,
                                     const uint8_t *text, size_t length) {
  const AbMapPackage *existing = package_for_manifest(state, &config->path);
  AbMapPackage temporary = {0};
  const AbMapPackage *package = existing;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!package) {
    status = ab_decode_npm_manifest(state->engine, text, length, &temporary);
    if (status == ARCHBIRD_INVALID_JSON || status == ARCHBIRD_DUPLICATE_KEY ||
        status == ARCHBIRD_INVALID_SCHEMA) {
      const char *message = archbird_engine_error(state->engine);
      status = ab_map_add_diagnostic(
          state, "error", "invalid-package-json",
          message && message[0] ? message : "invalid npm build manifest",
          &config->path);
      archbird_error_clear(state->engine);
      ab_map_package_clear(state->engine, &temporary);
      return status;
    }
    if (status != ARCHBIRD_OK) {
      ab_map_package_clear(state->engine, &temporary);
      return status;
    }
    package = &temporary;
  }
  for (index = 0; status == ARCHBIRD_OK && index < package->script_count;
       index++) {
    AbMapBuildRoute *route = NULL;
    status = reserve_build(state, &route);
    if (status == ARCHBIRD_OK)
      state->build_count++;
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(state->engine, &route->source, config->path.data,
                              config->path.length);
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(state->engine, &route->name,
                              package->scripts[index].key.data,
                              package->scripts[index].key.length);
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(state->engine, &route->command,
                              package->scripts[index].value.data,
                              package->scripts[index].value.length);
  }
  ab_map_package_clear(state->engine, &temporary);
  return status;
}

ArchbirdStatus ab_map_analyze_builds(AbMapState *state) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!state || !state->engine || !state->project || !state->manifest ||
      !state->config)
    return ARCHBIRD_INVALID_ARGUMENT;
  for (index = 0; status == ARCHBIRD_OK && index < state->config->build_count;
       index++) {
    const AbConfigBuild *config = &state->config->builds[index];
    const AbManifestFile *file = ab_map_manifest_file(
        state->manifest, config->path.data, config->path.length);
    const uint8_t *text;
    size_t file_index;
    if (!file) {
      char message[256];
      snprintf(message, sizeof(message), "build %.*s", (int)config->name.length,
               config->name.data);
      status = ab_map_add_diagnostic(state, "error", "missing-build-file",
                                     message, &config->path);
      continue;
    }
    file_index = (size_t)(file - state->manifest->files);
    text = ab_project_source_bytes(state->project, file_index);
    if (!text) {
      status =
          ab_map_add_diagnostic(state, "error", "read-failed",
                                "build file bytes are absent", &config->path);
      continue;
    }
    status = ab_utf8_validate(state->engine, text, file->byte_length);
    if (status != ARCHBIRD_OK) {
      const char *message = archbird_engine_error(state->engine);
      status = ab_map_add_diagnostic(
          state, "error", "read-failed",
          message && message[0] ? message : "build file is not valid UTF-8",
          &config->path);
      archbird_error_clear(state->engine);
      continue;
    }
    if (string_literal(&config->kind, "make"))
      status = add_make_routes(state, config, text, file->byte_length);
    else
      status = add_npm_routes(state, config, text, file->byte_length);
  }
  if (status == ARCHBIRD_OK && state->build_count > 1)
    qsort(state->builds, state->build_count, sizeof(*state->builds),
          build_compare);
  return status;
}

static ArchbirdStatus json_string(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value->data, value->length);
}

static ArchbirdStatus render_strings(AbBuffer *buffer,
                                     const AbStringArray *values) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < values->count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &values->items[index]);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

ArchbirdStatus ab_map_render_builds(AbBuffer *buffer, const AbMapState *state) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < state->build_count;
       index++) {
    const AbMapBuildRoute *route = &state->builds[index];
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"command\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &route->command);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"conditions\":");
    if (status == ARCHBIRD_OK)
      status = render_strings(buffer, &route->conditions);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"deps\":");
    if (status == ARCHBIRD_OK)
      status = render_strings(buffer, &route->deps);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"name\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &route->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"paths\":");
    if (status == ARCHBIRD_OK)
      status = render_strings(buffer, &route->paths);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"source\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &route->source);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}
