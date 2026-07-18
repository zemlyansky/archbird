#include "verify_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct VerifyConfigContext {
  ArchbirdEngine *engine;
  AbVerifySuiteView *suite;
} VerifyConfigContext;

static ArchbirdStatus invalid(VerifyConfigContext *context, const char *where,
                              const char *message) {
  return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                            ARCHBIRD_NO_OFFSET, "%s: %s", where, message);
}

static int string_equal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

int ab_verify_string_is(const AbValue *value, const char *literal) {
  return value && value->kind == AB_VALUE_STRING &&
         string_equal(&value->as.text, literal);
}

static int unicode_space(const unsigned char *bytes, size_t length,
                         size_t *width) {
  unsigned codepoint;
  if (!length) {
    *width = 0;
    return 0;
  }
  if (bytes[0] < 0x80) {
    *width = 1;
    return isspace(bytes[0]) != 0;
  }
  if (bytes[0] < 0xe0 && length >= 2) {
    codepoint =
        ((unsigned)(bytes[0] & 0x1f) << 6) | (unsigned)(bytes[1] & 0x3f);
    *width = 2;
  } else if (bytes[0] < 0xf0 && length >= 3) {
    codepoint = ((unsigned)(bytes[0] & 0x0f) << 12) |
                ((unsigned)(bytes[1] & 0x3f) << 6) |
                (unsigned)(bytes[2] & 0x3f);
    *width = 3;
  } else if (length >= 4) {
    codepoint = ((unsigned)(bytes[0] & 0x07) << 18) |
                ((unsigned)(bytes[1] & 0x3f) << 12) |
                ((unsigned)(bytes[2] & 0x3f) << 6) |
                (unsigned)(bytes[3] & 0x3f);
    *width = 4;
  } else {
    *width = 1;
    return 0;
  }
  return codepoint == 0x0085 || codepoint == 0x00a0 || codepoint == 0x1680 ||
         (codepoint >= 0x2000 && codepoint <= 0x200a) || codepoint == 0x2028 ||
         codepoint == 0x2029 || codepoint == 0x202f || codepoint == 0x205f ||
         codepoint == 0x3000;
}

int ab_verify_nonblank(const AbValue *value) {
  size_t offset = 0;
  if (!value || value->kind != AB_VALUE_STRING || !value->as.text.length)
    return 0;
  while (offset < value->as.text.length) {
    size_t width;
    if (!unicode_space((const unsigned char *)value->as.text.data + offset,
                       value->as.text.length - offset, &width))
      return 1;
    offset += width;
  }
  return 0;
}

static int stable_id(const AbString *value) {
  size_t index;
  if (!value || !value->length)
    return 0;
  for (index = 0; index < value->length; index++) {
    unsigned char byte = (unsigned char)value->data[index];
    if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
          (byte >= '0' && byte <= '9') ||
          (index &&
           (byte == '_' || byte == '.' || byte == ':' || byte == '-'))))
      return 0;
  }
  return 1;
}

static int lowercase_sha256(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || value->as.text.length != 64)
    return 0;
  for (index = 0; index < 64; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return 0;
  }
  return 1;
}

static int name_in(const AbString *name, const char *const *allowed,
                   size_t allowed_count) {
  size_t index;
  for (index = 0; index < allowed_count; index++)
    if (string_equal(name, allowed[index]))
      return 1;
  return 0;
}

static ArchbirdStatus reject_unknown(VerifyConfigContext *context,
                                     const AbValue *object, const char *where,
                                     const char *const *allowed,
                                     size_t allowed_count) {
  size_t index;
  if (!object || object->kind != AB_VALUE_OBJECT)
    return invalid(context, where, "expected object");
  for (index = 0; index < object->as.object.count; index++) {
    const AbString *name = &object->as.object.fields[index].name;
    if (!name_in(name, allowed, allowed_count))
      return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET, "%s: unknown field '%.*s'",
                                where, (int)name->length, name->data);
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_nonblank(VerifyConfigContext *context,
                                        const AbValue *value, const char *where,
                                        int allow_empty) {
  if (!value || value->kind != AB_VALUE_STRING ||
      (!allow_empty && !ab_verify_nonblank(value)))
    return invalid(context, where,
                   allow_empty ? "expected string"
                               : "expected non-empty string");
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_id_value(VerifyConfigContext *context,
                                        const AbValue *value,
                                        const char *where) {
  if (!value || value->kind != AB_VALUE_STRING || !stable_id(&value->as.text))
    return invalid(context, where, "expected stable identifier");
  return ARCHBIRD_OK;
}

static int scalar(const AbValue *value) {
  return value &&
         (value->kind == AB_VALUE_NULL || value->kind == AB_VALUE_BOOL ||
          value->kind == AB_VALUE_INTEGER || value->kind == AB_VALUE_REAL ||
          value->kind == AB_VALUE_STRING);
}

static int string_array_contains(const AbValue *array, size_t before,
                                 const AbString *value) {
  size_t index;
  for (index = 0; index < before; index++)
    if (ab_string_equal(&array->as.array.items[index].as.text, value))
      return 1;
  return 0;
}

static ArchbirdStatus validate_strings(VerifyConfigContext *context,
                                       const AbValue *value, const char *where,
                                       int required) {
  size_t index;
  if (!value)
    return required ? invalid(context, where, "field is required")
                    : ARCHBIRD_OK;
  if (value->kind != AB_VALUE_ARRAY)
    return invalid(context, where, "expected array");
  if (required && !value->as.array.count)
    return invalid(context, where, "expected at least one value");
  for (index = 0; index < value->as.array.count; index++) {
    const AbValue *item = &value->as.array.items[index];
    if (!ab_verify_nonblank(item))
      return invalid(context, where, "expected non-empty string values");
    if (string_array_contains(value, index, &item->as.text))
      return invalid(context, where, "duplicate values");
  }
  return ARCHBIRD_OK;
}

static int path_has_parent(const char *data, size_t length) {
  size_t start = 0;
  size_t index;
  for (index = 0; index <= length; index++) {
    if (index == length || data[index] == '/' || data[index] == '\\') {
      if (index - start == 2 && data[start] == '.' && data[start + 1] == '.')
        return 1;
      start = index + 1;
    }
  }
  return 0;
}

static int path_is_dot(const char *data, size_t length) {
  size_t index;
  int any = 0;
  for (index = 0; index < length; index++) {
    if (data[index] == '/' || data[index] == '\\')
      continue;
    if (data[index] != '.')
      return 0;
    any = 1;
  }
  return any;
}

static int portable_path(const AbValue *value, int repository) {
  const char *data;
  size_t length;
  if (!ab_verify_nonblank(value))
    return 0;
  data = value->as.text.data;
  length = value->as.text.length;
  if (data[0] == '/' || data[0] == '\\')
    return 0;
  if (repository && path_has_parent(data, length))
    return 0;
  if (!repository && path_is_dot(data, length))
    return 0;
  return 1;
}

int ab_verify_path_is_repository(const AbValue *value) {
  return portable_path(value, 1);
}

static ArchbirdStatus validate_path(VerifyConfigContext *context,
                                    const AbValue *value, const char *where,
                                    int repository) {
  return portable_path(value, repository)
             ? ARCHBIRD_OK
             : invalid(context, where,
                       repository ? "expected repository-relative file path"
                                  : "expected portable file path");
}

static ArchbirdStatus validate_pattern_strings(VerifyConfigContext *context,
                                               const AbValue *value,
                                               const char *where) {
  size_t index;
  ArchbirdStatus status = validate_strings(context, value, where, 0);
  if (status != ARCHBIRD_OK || !value)
    return status;
  for (index = 0; index < value->as.array.count; index++) {
    const AbString *item = &value->as.array.items[index].as.text;
    if ((item->length && item->data[0] == '/') ||
        memchr(item->data, '\\', item->length))
      return invalid(context, where, "expected repository-relative patterns");
  }
  return ARCHBIRD_OK;
}

static int leap_year(unsigned year) {
  return year % 4u == 0u && (year % 100u != 0u || year % 400u == 0u);
}

static int valid_date(const AbValue *value) {
  static const unsigned days[] = {0,  31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
  const char *text;
  unsigned year;
  unsigned month;
  unsigned day;
  unsigned maximum;
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || value->as.text.length != 10)
    return 0;
  text = value->as.text.data;
  for (index = 0; index < 10; index++) {
    if (index == 4 || index == 7) {
      if (text[index] != '-')
        return 0;
    } else if (text[index] < '0' || text[index] > '9') {
      return 0;
    }
  }
  year = (unsigned)(text[0] - '0') * 1000u + (unsigned)(text[1] - '0') * 100u +
         (unsigned)(text[2] - '0') * 10u + (unsigned)(text[3] - '0');
  month = (unsigned)(text[5] - '0') * 10u + (unsigned)(text[6] - '0');
  day = (unsigned)(text[8] - '0') * 10u + (unsigned)(text[9] - '0');
  if (!year || month < 1 || month > 12)
    return 0;
  maximum = days[month] + (month == 2 && leap_year(year));
  return day >= 1 && day <= maximum;
}

static int uint_value(const AbValue *value, uint64_t *out) {
  return value && value->kind == AB_VALUE_INTEGER && ab_value_u64(value, out);
}

static int object_has(const AbValue *object, const AbString *name) {
  size_t low = 0;
  size_t high = object->as.object.count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared =
        ab_string_compare(&object->as.object.fields[middle].name, name);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return 1;
  }
  return 0;
}

static ArchbirdStatus validate_parameters(VerifyConfigContext *context,
                                          const AbValue *value,
                                          const char *where) {
  size_t index;
  if (!value)
    return ARCHBIRD_OK;
  if (value->kind != AB_VALUE_OBJECT)
    return invalid(context, where, "expected object");
  for (index = 0; index < value->as.object.count; index++) {
    const AbObjectField *field = &value->as.object.fields[index];
    if (!field->name.length || !scalar(&field->value))
      return invalid(context, where,
                     "expected non-empty keys and finite scalar values");
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_source_lock(VerifyConfigContext *context,
                                           const AbValue *value,
                                           const char *where) {
  size_t index;
  if (!value)
    return ARCHBIRD_OK;
  if (value->kind != AB_VALUE_OBJECT || !value->as.object.count)
    return invalid(context, where,
                   "expected a non-empty path-to-SHA-256 object");
  for (index = 0; index < value->as.object.count; index++) {
    const AbObjectField *field = &value->as.object.fields[index];
    AbValue path = {0};
    size_t cursor;
    path.kind = AB_VALUE_STRING;
    path.as.text = field->name;
    if (!ab_verify_path_is_repository(&path) ||
        field->name.data[field->name.length - 1] == '/' ||
        memchr(field->name.data, '\\', field->name.length))
      return invalid(context, where,
                     "keys must be exact repository file paths");
    for (cursor = 0; cursor < field->name.length; cursor++)
      if (field->name.data[cursor] == '*' || field->name.data[cursor] == '?' ||
          field->name.data[cursor] == '[')
        return invalid(context, where,
                       "keys must not contain path-pattern metacharacters");
    if (!lowercase_sha256(&field->value))
      return invalid(context, where,
                     "values must be lowercase SHA-256 digests");
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_projects(VerifyConfigContext *context) {
  static const char *const allowed[] = {
      "capabilities", "config",   "map",  "parameters",
      "profile",      "revision", "root", "source_lock",
  };
  const AbValue *projects = context->suite->projects;
  size_t index;
  if (!projects || projects->kind != AB_VALUE_OBJECT ||
      !projects->as.object.count)
    return invalid(context, "projects", "expected at least one project");
  for (index = 0; index < projects->as.object.count; index++) {
    const AbObjectField *field = &projects->as.object.fields[index];
    const AbValue *row = &field->value;
    const AbValue *config;
    const AbValue *map;
    const AbValue *root;
    char where[256];
    if (!stable_id(&field->name))
      return invalid(context, "projects.key", "expected stable identifier");
    (void)snprintf(where, sizeof(where), "projects.%.*s",
                   (int)field->name.length, field->name.data);
    if (reject_unknown(context, row, where, allowed,
                       sizeof(allowed) / sizeof(allowed[0])) != ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
    config = ab_value_member(row, "config");
    map = ab_value_member(row, "map");
    if (!!config == !!map)
      return invalid(context, where,
                     "exactly one of config or map is required");
    if (validate_path(context, config ? config : map,
                      config ? "projects.config" : "projects.map",
                      0) != ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
    root = ab_value_member(row, "root");
    if (root && (!ab_verify_nonblank(root) || root->as.text.data[0] == '/' ||
                 root->as.text.data[0] == '\\'))
      return invalid(context, where, "root must be a portable relative path");
    if (ab_value_member(row, "revision") &&
        validate_nonblank(context, ab_value_member(row, "revision"), where,
                          1) != ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
    if (ab_value_member(row, "profile") &&
        validate_nonblank(context, ab_value_member(row, "profile"), where, 1) !=
            ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
    if (validate_strings(context, ab_value_member(row, "capabilities"), where,
                         0) != ARCHBIRD_OK ||
        validate_parameters(context, ab_value_member(row, "parameters"),
                            where) != ARCHBIRD_OK ||
        validate_source_lock(context, ab_value_member(row, "source_lock"),
                             where) != ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
  }
  return ARCHBIRD_OK;
}

static int source_extractor(const AbValue *kind) {
  return ab_verify_string_is(kind, "python_enum") ||
         ab_verify_string_is(kind, "python_set") ||
         ab_verify_string_is(kind, "c_enum") ||
         ab_verify_string_is(kind, "c_designated_initializer") ||
         ab_verify_string_is(kind, "c_macro_set");
}

static int literal_extractor(const AbValue *kind) {
  return ab_verify_string_is(kind, "literal_set") ||
         ab_verify_string_is(kind, "literal_values") ||
         ab_verify_string_is(kind, "literal_relation");
}

static int supported_extractor(const AbValue *kind) {
  static const char *const names[] = {
      "python_enum",      "python_set",
      "c_enum",           "c_designated_initializer",
      "c_macro_set",      "symbols",
      "file_edges",       "component_edges",
      "test_routes",      "provider_surface",
      "literal_set",      "literal_values",
      "literal_relation",
  };
  size_t index;
  for (index = 0; index < sizeof(names) / sizeof(names[0]); index++)
    if (ab_verify_string_is(kind, names[index]))
      return 1;
  return 0;
}

static int extractor_field_allowed(const AbValue *kind, const AbString *name) {
  static const char *const common[] = {
      "exclude", "include",      "kind",         "path",
      "project", "strip_prefix", "strip_suffix",
  };
  static const char *const python_enum[] = {"auto_start", "class"};
  static const char *const python_set[] = {"attribute", "class"};
  static const char *const named[] = {"name"};
  static const char *const macro[] = {
      "call",
      "selector",
      "selector_argument",
      "values_from_argument",
  };
  static const char *const symbols[] = {"kinds", "layer", "paths",
                                        "public_only"};
  static const char *const file_edges[] = {"from_paths", "kinds", "to_paths"};
  static const char *const component_edges[] = {"kinds"};
  static const char *const test_routes[] = {"configured_only", "group",
                                            "selectors"};
  static const char *const literal_values[] = {"values"};
  static const char *const literal_relation[] = {"rows"};
  if (name_in(name, common, sizeof(common) / sizeof(common[0])))
    return 1;
  if (ab_verify_string_is(kind, "python_enum"))
    return name_in(name, python_enum,
                   sizeof(python_enum) / sizeof(python_enum[0]));
  if (ab_verify_string_is(kind, "python_set"))
    return name_in(name, python_set,
                   sizeof(python_set) / sizeof(python_set[0]));
  if (ab_verify_string_is(kind, "c_enum") ||
      ab_verify_string_is(kind, "c_designated_initializer") ||
      ab_verify_string_is(kind, "provider_surface"))
    return name_in(name, named, sizeof(named) / sizeof(named[0]));
  if (ab_verify_string_is(kind, "c_macro_set"))
    return name_in(name, macro, sizeof(macro) / sizeof(macro[0]));
  if (ab_verify_string_is(kind, "symbols"))
    return name_in(name, symbols, sizeof(symbols) / sizeof(symbols[0]));
  if (ab_verify_string_is(kind, "file_edges"))
    return name_in(name, file_edges,
                   sizeof(file_edges) / sizeof(file_edges[0]));
  if (ab_verify_string_is(kind, "component_edges"))
    return name_in(name, component_edges,
                   sizeof(component_edges) / sizeof(component_edges[0]));
  if (ab_verify_string_is(kind, "test_routes"))
    return name_in(name, test_routes,
                   sizeof(test_routes) / sizeof(test_routes[0]));
  if (ab_verify_string_is(kind, "literal_set") ||
      ab_verify_string_is(kind, "literal_values"))
    return name_in(name, literal_values,
                   sizeof(literal_values) / sizeof(literal_values[0]));
  if (ab_verify_string_is(kind, "literal_relation"))
    return name_in(name, literal_relation,
                   sizeof(literal_relation) / sizeof(literal_relation[0]));
  return 0;
}

static ArchbirdStatus validate_relation_rows(VerifyConfigContext *context,
                                             const AbValue *rows,
                                             const char *where) {
  static const char *const allowed[] = {"kind", "source", "target"};
  size_t index;
  size_t previous;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return invalid(context, where, "expected array");
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *source;
    const AbValue *target;
    const AbValue *kind;
    if (reject_unknown(context, row, where, allowed,
                       sizeof(allowed) / sizeof(allowed[0])) != ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
    source = ab_value_member(row, "source");
    target = ab_value_member(row, "target");
    kind = ab_value_member(row, "kind");
    if (!ab_verify_nonblank(source) || !ab_verify_nonblank(target) ||
        (kind && !ab_verify_nonblank(kind)))
      return invalid(context, where,
                     "relation rows require non-empty source/target/kind");
    for (previous = 0; previous < index; previous++) {
      const AbValue *old = &rows->as.array.items[previous];
      const AbValue *old_source = ab_value_member(old, "source");
      const AbValue *old_target = ab_value_member(old, "target");
      const AbValue *old_kind = ab_value_member(old, "kind");
      if (ab_string_equal(&source->as.text, &old_source->as.text) &&
          ab_string_equal(&target->as.text, &old_target->as.text) &&
          ((!kind && !old_kind) ||
           (kind && old_kind &&
            ab_string_equal(&kind->as.text, &old_kind->as.text))))
        return invalid(context, where, "duplicate relation rows");
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_extractors(VerifyConfigContext *context) {
  const AbValue *extractors = context->suite->extractors;
  size_t index;
  if (!extractors || extractors->kind != AB_VALUE_OBJECT)
    return invalid(context, "extractors", "expected object");
  for (index = 0; index < extractors->as.object.count; index++) {
    const AbObjectField *field = &extractors->as.object.fields[index];
    const AbValue *row = &field->value;
    const AbValue *kind;
    const AbValue *project;
    const AbValue *value;
    uint64_t selector_argument = 0;
    uint64_t value_argument = 1;
    char where[256];
    if (!stable_id(&field->name))
      return invalid(context, "extractors.key", "expected stable identifier");
    (void)snprintf(where, sizeof(where), "extractors.%.*s",
                   (int)field->name.length, field->name.data);
    if (!row || row->kind != AB_VALUE_OBJECT)
      return invalid(context, where, "expected object");
    kind = ab_value_member(row, "kind");
    if (!supported_extractor(kind))
      return invalid(context, where, "unsupported extractor kind");
    {
      size_t field_index;
      for (field_index = 0; field_index < row->as.object.count; field_index++)
        if (!extractor_field_allowed(kind,
                                     &row->as.object.fields[field_index].name))
          return archbird_error_set(
              context->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
              "%s: unknown field '%.*s'", where,
              (int)row->as.object.fields[field_index].name.length,
              row->as.object.fields[field_index].name.data);
    }
    project = ab_value_member(row, "project");
    if (!literal_extractor(kind)) {
      if (!project || project->kind != AB_VALUE_STRING ||
          !object_has(context->suite->projects, &project->as.text))
        return invalid(context, where, "unknown or missing project");
    }
    if (source_extractor(kind) &&
        validate_path(context, ab_value_member(row, "path"), where, 1) !=
            ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
    if ((ab_verify_string_is(kind, "python_enum") &&
         !ab_verify_nonblank(ab_value_member(row, "class"))) ||
        (ab_verify_string_is(kind, "python_set") &&
         (!ab_verify_nonblank(ab_value_member(row, "class")) ||
          !ab_verify_nonblank(ab_value_member(row, "attribute")))) ||
        ((ab_verify_string_is(kind, "c_enum") ||
          ab_verify_string_is(kind, "c_designated_initializer") ||
          ab_verify_string_is(kind, "provider_surface")) &&
         !ab_verify_nonblank(ab_value_member(row, "name"))) ||
        (ab_verify_string_is(kind, "c_macro_set") &&
         (!ab_verify_nonblank(ab_value_member(row, "call")) ||
          !ab_verify_nonblank(ab_value_member(row, "selector")))))
      return invalid(context, where, "required extractor field is missing");
    if (ab_value_member(row, "auto_start") &&
        !uint_value(ab_value_member(row, "auto_start"), &selector_argument))
      return invalid(context, where, "auto_start must be an integer >= 0");
    if (ab_verify_string_is(kind, "c_macro_set")) {
      if (ab_value_member(row, "selector_argument") &&
          !uint_value(ab_value_member(row, "selector_argument"),
                      &selector_argument))
        return invalid(context, where,
                       "selector_argument must be an integer >= 0");
      if (ab_value_member(row, "values_from_argument") &&
          !uint_value(ab_value_member(row, "values_from_argument"),
                      &value_argument))
        return invalid(context, where,
                       "values_from_argument must be an integer >= 0");
      if (selector_argument == value_argument)
        return invalid(context, where,
                       "selector and value argument indexes must differ");
    }
    if (validate_strings(context, ab_value_member(row, "include"), where, 0) !=
            ARCHBIRD_OK ||
        validate_strings(context, ab_value_member(row, "exclude"), where, 0) !=
            ARCHBIRD_OK ||
        validate_strings(context, ab_value_member(row, "kinds"), where, 0) !=
            ARCHBIRD_OK ||
        validate_strings(context, ab_value_member(row, "selectors"), where,
                         0) != ARCHBIRD_OK ||
        validate_pattern_strings(context, ab_value_member(row, "paths"),
                                 where) != ARCHBIRD_OK ||
        validate_pattern_strings(context, ab_value_member(row, "from_paths"),
                                 where) != ARCHBIRD_OK ||
        validate_pattern_strings(context, ab_value_member(row, "to_paths"),
                                 where) != ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
    value = ab_value_member(row, "configured_only");
    if (value && value->kind != AB_VALUE_BOOL)
      return invalid(context, where, "configured_only must be boolean");
    value = ab_value_member(row, "public_only");
    if (value && value->kind != AB_VALUE_BOOL)
      return invalid(context, where, "public_only must be boolean");
    value = ab_value_member(row, "values");
    if (ab_verify_string_is(kind, "literal_set")) {
      if (validate_strings(context, value, where, 0) != ARCHBIRD_OK)
        return ARCHBIRD_INVALID_SCHEMA;
    } else if (ab_verify_string_is(kind, "literal_values")) {
      if (!value || value->kind != AB_VALUE_OBJECT)
        return invalid(context, where, "literal values must be an object");
      {
        size_t value_index;
        for (value_index = 0; value_index < value->as.object.count;
             value_index++)
          if (!value->as.object.fields[value_index].name.length ||
              !scalar(&value->as.object.fields[value_index].value))
            return invalid(context, where,
                           "literal values require scalar values");
      }
    } else if (ab_verify_string_is(kind, "literal_relation")) {
      if (validate_relation_rows(context, ab_value_member(row, "rows"),
                                 where) != ARCHBIRD_OK)
        return ARCHBIRD_INVALID_SCHEMA;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_mappings(VerifyConfigContext *context) {
  static const char *const allowed[] = {"actual_to_expected"};
  const AbValue *mappings = context->suite->mappings;
  size_t index;
  if (!mappings)
    return ARCHBIRD_OK;
  if (mappings->kind != AB_VALUE_OBJECT)
    return invalid(context, "mappings", "expected object");
  for (index = 0; index < mappings->as.object.count; index++) {
    const AbObjectField *field = &mappings->as.object.fields[index];
    const AbValue *aliases;
    size_t alias_index;
    if (!stable_id(&field->name) ||
        reject_unknown(context, &field->value, "mappings", allowed, 1) !=
            ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
    aliases = ab_value_member(&field->value, "actual_to_expected");
    if (!aliases)
      continue;
    if (aliases->kind != AB_VALUE_OBJECT)
      return invalid(context, "mappings",
                     "actual_to_expected must be an object");
    for (alias_index = 0; alias_index < aliases->as.object.count;
         alias_index++) {
      const AbObjectField *alias = &aliases->as.object.fields[alias_index];
      size_t previous;
      if (!alias->name.length || !ab_verify_nonblank(&alias->value))
        return invalid(context, "mappings", "mapping names must be non-empty");
      for (previous = 0; previous < alias_index; previous++)
        if (ab_string_equal(&alias->value.as.text,
                            &aliases->as.object.fields[previous].value.as.text))
          return invalid(context, "mappings", "mapping targets must be unique");
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_attestations(VerifyConfigContext *context) {
  static const char *const allowed[] = {"path", "project"};
  const AbValue *rows = context->suite->attestations;
  size_t index;
  if (!rows)
    return ARCHBIRD_OK;
  if (rows->kind != AB_VALUE_OBJECT)
    return invalid(context, "attestations", "expected object");
  for (index = 0; index < rows->as.object.count; index++) {
    const AbObjectField *field = &rows->as.object.fields[index];
    const AbValue *project;
    if (!stable_id(&field->name) ||
        reject_unknown(context, &field->value, "attestations", allowed, 2) !=
            ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
    project = ab_value_member(&field->value, "project");
    if (!project || project->kind != AB_VALUE_STRING ||
        !object_has(context->suite->projects, &project->as.text))
      return invalid(context, "attestations", "unknown or missing project");
    if (validate_path(context, ab_value_member(&field->value, "path"),
                      "attestations.path", 0) != ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
  }
  return ARCHBIRD_OK;
}

static int supported_assertion(const AbValue *value) {
  static const char *const names[] = {
      "set_equal",       "mapped_set_equal",
      "values_equal",    "mapped_values_equal",
      "subset",          "required_subset",
      "required_values", "cardinality",
      "required_edges",  "forbidden_edges",
      "allowed_edges",   "acyclic",
      "min_test_routes", "attestation_equal",
  };
  size_t index;
  for (index = 0; index < sizeof(names) / sizeof(names[0]); index++)
    if (ab_verify_string_is(value, names[index]))
      return 1;
  return 0;
}

static int two_operand_assertion(const AbValue *value) {
  return ab_verify_string_is(value, "set_equal") ||
         ab_verify_string_is(value, "mapped_set_equal") ||
         ab_verify_string_is(value, "values_equal") ||
         ab_verify_string_is(value, "mapped_values_equal") ||
         ab_verify_string_is(value, "subset") ||
         ab_verify_string_is(value, "required_subset") ||
         ab_verify_string_is(value, "required_values") ||
         ab_verify_string_is(value, "required_edges") ||
         ab_verify_string_is(value, "forbidden_edges") ||
         ab_verify_string_is(value, "allowed_edges") ||
         ab_verify_string_is(value, "attestation_equal");
}

static int check_field_allowed(const AbValue *assertion, const AbString *name) {
  static const char *const common[] = {
      "assert", "id", "owner", "rationale", "requirement", "severity", "tags",
  };
  static const char *const operands[] = {"actual", "expected"};
  static const char *const mapped[] = {"actual", "expected", "mapping"};
  static const char *const cardinality[] = {"actual", "exact", "max", "min"};
  static const char *const actual_only[] = {"actual"};
  static const char *const routes[] = {"actual", "min", "required_routes"};
  static const char *const attestation[] = {
      "actual",
      "expected",
      "reference_route",
      "required_routes",
  };
  if (name_in(name, common, sizeof(common) / sizeof(common[0])))
    return 1;
  if (ab_verify_string_is(assertion, "mapped_set_equal") ||
      ab_verify_string_is(assertion, "mapped_values_equal") ||
      ab_verify_string_is(assertion, "subset") ||
      ab_verify_string_is(assertion, "required_subset") ||
      ab_verify_string_is(assertion, "required_values"))
    return name_in(name, mapped, sizeof(mapped) / sizeof(mapped[0]));
  if (ab_verify_string_is(assertion, "set_equal") ||
      ab_verify_string_is(assertion, "values_equal") ||
      ab_verify_string_is(assertion, "required_edges") ||
      ab_verify_string_is(assertion, "forbidden_edges") ||
      ab_verify_string_is(assertion, "allowed_edges"))
    return name_in(name, operands, sizeof(operands) / sizeof(operands[0]));
  if (ab_verify_string_is(assertion, "cardinality"))
    return name_in(name, cardinality,
                   sizeof(cardinality) / sizeof(cardinality[0]));
  if (ab_verify_string_is(assertion, "acyclic"))
    return name_in(name, actual_only,
                   sizeof(actual_only) / sizeof(actual_only[0]));
  if (ab_verify_string_is(assertion, "min_test_routes"))
    return name_in(name, routes, sizeof(routes) / sizeof(routes[0]));
  if (ab_verify_string_is(assertion, "attestation_equal"))
    return name_in(name, attestation,
                   sizeof(attestation) / sizeof(attestation[0]));
  return 0;
}

static ArchbirdStatus validate_requirement(VerifyConfigContext *context,
                                           const AbValue *value,
                                           const char *where) {
  if (!value)
    return ARCHBIRD_OK;
  if (value->kind == AB_VALUE_STRING)
    return ab_verify_nonblank(value)
               ? ARCHBIRD_OK
               : invalid(context, where, "expected non-empty requirement");
  return validate_strings(context, value, where, 1);
}

static ArchbirdStatus validate_checks(VerifyConfigContext *context) {
  const AbValue *checks = context->suite->checks;
  size_t index;
  if (!checks || checks->kind != AB_VALUE_ARRAY || !checks->as.array.count)
    return invalid(context, "checks", "expected at least one check");
  for (index = 0; index < checks->as.array.count; index++) {
    const AbValue *row = &checks->as.array.items[index];
    const AbValue *id;
    const AbValue *assertion;
    const AbValue *expected;
    const AbValue *actual;
    const AbValue *mapping;
    const AbValue *severity;
    uint64_t minimum;
    uint64_t maximum;
    uint64_t exact;
    int has_min;
    int has_max;
    int has_exact;
    size_t previous;
    if (!row || row->kind != AB_VALUE_OBJECT)
      return invalid(context, "checks", "expected object");
    id = ab_value_member(row, "id");
    if (validate_id_value(context, id, "checks.id") != ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
    for (previous = 0; previous < index; previous++) {
      const AbValue *old =
          ab_value_member(&checks->as.array.items[previous], "id");
      if (old && ab_string_equal(&id->as.text, &old->as.text))
        return invalid(context, "checks", "duplicate check IDs");
    }
    assertion = ab_value_member(row, "assert");
    if (!supported_assertion(assertion))
      return invalid(context, "checks.assert", "unsupported assertion");
    {
      size_t field_index;
      for (field_index = 0; field_index < row->as.object.count; field_index++)
        if (!check_field_allowed(assertion,
                                 &row->as.object.fields[field_index].name))
          return archbird_error_set(
              context->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
              "checks: unknown field '%.*s'",
              (int)row->as.object.fields[field_index].name.length,
              row->as.object.fields[field_index].name.data);
    }
    if (!ab_verify_nonblank(ab_value_member(row, "owner")) ||
        !ab_verify_nonblank(ab_value_member(row, "rationale")))
      return invalid(context, "checks", "owner and rationale are required");
    severity = ab_value_member(row, "severity");
    if (severity && !ab_verify_string_is(severity, "error") &&
        !ab_verify_string_is(severity, "warning") &&
        !ab_verify_string_is(severity, "note"))
      return invalid(context, "checks.severity", "unsupported severity");
    if (validate_requirement(context, ab_value_member(row, "requirement"),
                             "checks.requirement") != ARCHBIRD_OK ||
        validate_strings(context, ab_value_member(row, "tags"), "checks.tags",
                         0) != ARCHBIRD_OK ||
        validate_strings(context, ab_value_member(row, "required_routes"),
                         "checks.required_routes", 0) != ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
    expected = ab_value_member(row, "expected");
    actual = ab_value_member(row, "actual");
    mapping = ab_value_member(row, "mapping");
    if (two_operand_assertion(assertion) && !ab_verify_nonblank(expected))
      return invalid(context, "checks.expected", "field is required");
    if (!ab_verify_nonblank(actual))
      return invalid(context, "checks.actual", "field is required");
    if ((ab_verify_string_is(assertion, "mapped_set_equal") ||
         ab_verify_string_is(assertion, "mapped_values_equal")) &&
        !ab_verify_nonblank(mapping))
      return invalid(context, "checks.mapping", "field is required");
    if (ab_verify_string_is(assertion, "attestation_equal")) {
      if (!object_has(context->suite->attestations, &expected->as.text) ||
          !object_has(context->suite->attestations, &actual->as.text))
        return invalid(context, "checks", "unknown attestation operand");
    } else {
      if ((expected &&
           !object_has(context->suite->extractors, &expected->as.text)) ||
          !object_has(context->suite->extractors, &actual->as.text))
        return invalid(context, "checks", "unknown extractor operand");
    }
    if (mapping && (!context->suite->mappings ||
                    !object_has(context->suite->mappings, &mapping->as.text)))
      return invalid(context, "checks.mapping", "unknown mapping");
    has_min = ab_value_member(row, "min") != NULL;
    has_max = ab_value_member(row, "max") != NULL;
    has_exact = ab_value_member(row, "exact") != NULL;
    if ((has_min && !uint_value(ab_value_member(row, "min"), &minimum)) ||
        (has_max && !uint_value(ab_value_member(row, "max"), &maximum)) ||
        (has_exact && !uint_value(ab_value_member(row, "exact"), &exact)))
      return invalid(context, "checks",
                     "cardinality values must be integers >= 0");
    if (ab_verify_string_is(assertion, "cardinality")) {
      if (!has_min && !has_max && !has_exact)
        return invalid(context, "checks",
                       "cardinality requires exact, min, or max");
      if (has_exact && (has_min || has_max))
        return invalid(context, "checks",
                       "exact cannot be combined with min/max");
      if (has_min && has_max && minimum > maximum)
        return invalid(context, "checks", "min must be <= max");
    }
    if (ab_verify_string_is(assertion, "min_test_routes") && !has_min)
      return invalid(context, "checks.min", "field is required");
  }
  return ARCHBIRD_OK;
}

static int date_after(const AbValue *left, const AbValue *right) {
  return memcmp(left->as.text.data, right->as.text.data, 10) > 0;
}

static ArchbirdStatus validate_waivers(VerifyConfigContext *context) {
  static const char *const allowed[] = {
      "check", "comparison", "expires_on", "fingerprint",  "id",
      "key",   "owner",      "rationale",  "until_inputs",
  };
  const AbValue *waivers = context->suite->waivers;
  size_t index;
  if (!waivers)
    return ARCHBIRD_OK;
  if (waivers->kind != AB_VALUE_ARRAY)
    return invalid(context, "waivers", "expected array");
  for (index = 0; index < waivers->as.array.count; index++) {
    const AbValue *row = &waivers->as.array.items[index];
    const AbValue *id;
    const AbValue *fingerprint;
    const AbValue *check;
    const AbValue *comparison;
    const AbValue *key;
    const AbValue *expires;
    const AbValue *until_inputs;
    size_t previous;
    if (reject_unknown(context, row, "waivers", allowed,
                       sizeof(allowed) / sizeof(allowed[0])) != ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
    id = ab_value_member(row, "id");
    if (validate_id_value(context, id, "waivers.id") != ARCHBIRD_OK ||
        !ab_verify_nonblank(ab_value_member(row, "owner")) ||
        !ab_verify_nonblank(ab_value_member(row, "rationale")))
      return invalid(context, "waivers",
                     "id, owner, and rationale are required");
    for (previous = 0; previous < index; previous++) {
      const AbValue *old =
          ab_value_member(&waivers->as.array.items[previous], "id");
      if (old && ab_string_equal(&id->as.text, &old->as.text))
        return invalid(context, "waivers", "duplicate waiver IDs");
    }
    fingerprint = ab_value_member(row, "fingerprint");
    check = ab_value_member(row, "check");
    comparison = ab_value_member(row, "comparison");
    key = ab_value_member(row, "key");
    if (fingerprint) {
      if (!lowercase_sha256(fingerprint))
        return invalid(context, "waivers.fingerprint",
                       "expected lowercase SHA-256");
      if (check || comparison || key)
        return invalid(
            context, "waivers",
            "fingerprint cannot be combined with check/comparison/key");
    } else {
      if (!ab_verify_nonblank(check) || !ab_verify_nonblank(comparison) ||
          !ab_verify_nonblank(key))
        return invalid(
            context, "waivers",
            "require fingerprint or exact check+comparison+key matcher");
      if (!ab_verify_string_is(comparison, "equal") &&
          !ab_verify_string_is(comparison, "missing") &&
          !ab_verify_string_is(comparison, "extra") &&
          !ab_verify_string_is(comparison, "different"))
        return invalid(context, "waivers.comparison", "unsupported comparison");
      {
        size_t check_index;
        int found = 0;
        for (check_index = 0;
             check_index < context->suite->checks->as.array.count;
             check_index++) {
          const AbValue *check_id = ab_value_member(
              &context->suite->checks->as.array.items[check_index], "id");
          if (check_id &&
              ab_string_equal(&check_id->as.text, &check->as.text)) {
            found = 1;
            break;
          }
        }
        if (!found)
          return invalid(context, "waivers.check", "unknown check");
      }
    }
    expires = ab_value_member(row, "expires_on");
    until_inputs = ab_value_member(row, "until_inputs");
    if (!expires && !until_inputs)
      return invalid(context, "waivers",
                     "waiver requires expires_on or until_inputs");
    if (expires) {
      if (!valid_date(expires))
        return invalid(context, "waivers.expires_on", "expected YYYY-MM-DD");
      if (!context->suite->policy_date)
        return invalid(context, "waivers.expires_on",
                       "suite.policy_date is required");
      (void)date_after(context->suite->policy_date, expires);
    }
    if (until_inputs) {
      size_t input_index;
      if (until_inputs->kind != AB_VALUE_OBJECT)
        return invalid(context, "waivers.until_inputs", "expected object");
      for (input_index = 0; input_index < until_inputs->as.object.count;
           input_index++) {
        const AbObjectField *input =
            &until_inputs->as.object.fields[input_index];
        if (!object_has(context->suite->projects, &input->name) ||
            !lowercase_sha256(&input->value))
          return invalid(context, "waivers.until_inputs",
                         "unknown project or invalid SHA-256");
      }
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_root(VerifyConfigContext *context,
                                    const AbValue *root) {
  static const char *const allowed[] = {
      "attestations",   "candidate", "checks",      "description",
      "extractors",     "mappings",  "policy_date", "projects",
      "schema_version", "suite",     "waivers",
  };
  const AbValue *schema;
  const AbValue *candidate;
  uint64_t version;
  if (reject_unknown(context, root, "suite", allowed,
                     sizeof(allowed) / sizeof(allowed[0])) != ARCHBIRD_OK)
    return ARCHBIRD_INVALID_SCHEMA;
  schema = ab_value_member(root, "schema_version");
  if (!uint_value(schema, &version) || version != 1)
    return invalid(context, "schema_version", "expected 1");
  context->suite->name = ab_value_member(root, "suite");
  if (validate_id_value(context, context->suite->name, "suite.suite") !=
      ARCHBIRD_OK)
    return ARCHBIRD_INVALID_SCHEMA;
  context->suite->description = ab_value_member(root, "description");
  if (context->suite->description &&
      context->suite->description->kind != AB_VALUE_STRING)
    return invalid(context, "suite.description", "expected string");
  candidate = ab_value_member(root, "candidate");
  if (candidate && candidate->kind != AB_VALUE_BOOL)
    return invalid(context, "suite.candidate", "expected boolean");
  context->suite->candidate = candidate && candidate->as.boolean;
  context->suite->policy_date = ab_value_member(root, "policy_date");
  if (context->suite->policy_date && !valid_date(context->suite->policy_date))
    return invalid(context, "suite.policy_date", "expected YYYY-MM-DD");
  context->suite->projects = ab_value_member(root, "projects");
  context->suite->extractors = ab_value_member(root, "extractors");
  context->suite->mappings = ab_value_member(root, "mappings");
  context->suite->attestations = ab_value_member(root, "attestations");
  context->suite->checks = ab_value_member(root, "checks");
  context->suite->waivers = ab_value_member(root, "waivers");
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_verify_suite_validate(ArchbirdEngine *engine,
                                        const AbValue *root,
                                        AbVerifySuiteView *out) {
  VerifyConfigContext context;
  ArchbirdStatus status;
  if (!engine || !root || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  out->engine = engine;
  out->root = root;
  context.engine = engine;
  context.suite = out;
  status = validate_root(&context, root);
  if (status == ARCHBIRD_OK)
    status = validate_projects(&context);
  if (status == ARCHBIRD_OK)
    status = validate_extractors(&context);
  if (status == ARCHBIRD_OK)
    status = validate_mappings(&context);
  if (status == ARCHBIRD_OK)
    status = validate_attestations(&context);
  if (status == ARCHBIRD_OK)
    status = validate_checks(&context);
  if (status == ARCHBIRD_OK)
    status = validate_waivers(&context);
  return status;
}

ArchbirdStatus ab_verify_render_normalized_path(AbBuffer *buffer,
                                                const AbValue *value) {
  AbBuffer normalized;
  size_t index;
  size_t segment_start = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!buffer || !value || value->kind != AB_VALUE_STRING)
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&normalized, buffer->engine);
  for (index = 0; status == ARCHBIRD_OK && index <= value->as.text.length;
       index++) {
    int separator = index == value->as.text.length ||
                    value->as.text.data[index] == '/' ||
                    value->as.text.data[index] == '\\';
    if (!separator)
      continue;
    if (index > segment_start && !(index - segment_start == 1 &&
                                   value->as.text.data[segment_start] == '.')) {
      if (normalized.length)
        status = ab_buffer_literal(&normalized, "/");
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_append(&normalized, value->as.text.data + segment_start,
                             index - segment_start);
    }
    segment_start = index + 1;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, (const char *)normalized.data,
                                   normalized.length);
  ab_buffer_free(&normalized);
  return status;
}
