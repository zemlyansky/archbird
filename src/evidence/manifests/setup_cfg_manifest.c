#include "evidence/manifests/setup_cfg_manifest.h"

#include "base/archbird_internal.h"
#include "base/utf8.h"

#include <string.h>

typedef enum SetupSection {
  SETUP_SECTION_OTHER,
  SETUP_SECTION_METADATA,
  SETUP_SECTION_OPTIONS,
  SETUP_SECTION_PACKAGE_FIND
} SetupSection;

typedef enum SetupField {
  SETUP_FIELD_NONE,
  SETUP_FIELD_NAME,
  SETUP_FIELD_VERSION,
  SETUP_FIELD_PACKAGES,
  SETUP_FIELD_PY_MODULES,
  SETUP_FIELD_PACKAGE_DIR,
  SETUP_FIELD_FIND_INCLUDE,
  SETUP_FIELD_FIND_WHERE,
  SETUP_FIELD_FIND_EXCLUDE
} SetupField;

typedef struct SetupParser {
  ArchbirdEngine *engine;
  AbPythonPackageMetadata *metadata;
  SetupSection section;
  SetupField field;
  unsigned int section_seen;
  unsigned int field_seen;
  unsigned int field_value_seen;
  int metadata_conflict;
  int layout_conflict;
  int find_enabled;
  int find_include_seen;
  int layout_seen;
  int continuation_allowed;
  int syntax_invalid;
} SetupParser;

static void trim(const uint8_t *text, size_t *start, size_t *end) {
  while (*start < *end &&
         (text[*start] == ' ' || text[*start] == '\t' || text[*start] == '\r'))
    (*start)++;
  while (*end > *start && (text[*end - 1] == ' ' || text[*end - 1] == '\t' ||
                           text[*end - 1] == '\r'))
    (*end)--;
}

static int span_equal(const uint8_t *text, size_t start, size_t end,
                      const char *literal) {
  size_t length = strlen(literal);
  return end - start == length && !memcmp(text + start, literal, length);
}

static int ascii_prefix(const uint8_t *text, size_t start, size_t end,
                        const char *literal) {
  size_t index;
  size_t length = strlen(literal);
  if (end - start < length)
    return 0;
  for (index = 0; index < length; index++) {
    unsigned char actual = text[start + index];
    unsigned char wanted = (unsigned char)literal[index];
    if (actual >= 'A' && actual <= 'Z')
      actual = (unsigned char)(actual - 'A' + 'a');
    if (actual != wanted)
      return 0;
  }
  return 1;
}

static int dynamic_scalar(const uint8_t *text, size_t start, size_t end) {
  size_t index;
  if (ascii_prefix(text, start, end, "attr:") ||
      ascii_prefix(text, start, end, "file:"))
    return 1;
  for (index = start; index < end; index++)
    if (text[index] == '$' || text[index] == '%')
      return 1;
  return 0;
}

static int nonblank_scalar(const uint8_t *text, size_t start, size_t end) {
  size_t index;
  for (index = start; index < end; index++)
    if (text[index] != ' ' && text[index] != '\t' && text[index] != '\r' &&
        text[index] != '\n')
      return 1;
  return 0;
}

static ArchbirdStatus invalidate_module_hints(SetupParser *parser) {
  parser->metadata->module_hints_present = 1;
  parser->metadata->module_hints_supported = 0;
  ab_string_free(parser->engine, &parser->metadata->module);
  return ARCHBIRD_OK;
}

static ArchbirdStatus invalidate_source_root(SetupParser *parser) {
  parser->metadata->source_root_present = 1;
  parser->metadata->source_root_supported = 0;
  ab_string_free(parser->engine, &parser->metadata->source_root);
  return ARCHBIRD_OK;
}

static ArchbirdStatus merge_module_span(SetupParser *parser,
                                        const uint8_t *text, size_t start,
                                        size_t end, int pattern) {
  AbString value;
  const char *candidate = NULL;
  size_t candidate_length = 0;
  int supported;
  value.data = (char *)text + start;
  value.length = end - start;
  supported = ab_python_module_candidate(&value, pattern, &candidate,
                                         &candidate_length);
  return ab_python_module_hint_merge(parser->engine, &parser->metadata->module,
                                     &parser->metadata->module_hints_present,
                                     &parser->metadata->module_hints_supported,
                                     candidate, candidate_length, supported);
}

static ArchbirdStatus merge_module_list(SetupParser *parser,
                                        const uint8_t *text, size_t start,
                                        size_t end, int pattern) {
  size_t item_start = start;
  size_t index;
  size_t values = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = start; status == ARCHBIRD_OK && index <= end; index++) {
    size_t item_end;
    if (index < end && text[index] != ',')
      continue;
    item_end = index;
    trim(text, &item_start, &item_end);
    if (item_start < item_end) {
      values++;
      status = merge_module_span(parser, text, item_start, item_end, pattern);
    }
    item_start = index + 1;
  }
  return values ? status : invalidate_module_hints(parser);
}

static ArchbirdStatus merge_root_span(SetupParser *parser, const uint8_t *text,
                                      size_t start, size_t end) {
  AbString value;
  value.data = (char *)text + start;
  value.length = end - start;
  return ab_python_source_root_merge(
      parser->engine, &parser->metadata->source_root,
      &parser->metadata->source_root_present,
      &parser->metadata->source_root_supported, &value,
      start < end && !memchr(text + start, ',', end - start));
}

static ArchbirdStatus parse_package_dir(SetupParser *parser,
                                        const uint8_t *text, size_t start,
                                        size_t end) {
  size_t equal = start;
  size_t key_start = start;
  size_t key_end;
  size_t value_start;
  size_t value_end = end;
  while (equal < end && text[equal] != '=')
    equal++;
  if (equal == end)
    return invalidate_source_root(parser);
  key_end = equal;
  value_start = equal + 1;
  trim(text, &key_start, &key_end);
  trim(text, &value_start, &value_end);
  if (key_start != key_end || value_start == value_end)
    return invalidate_source_root(parser);
  return merge_root_span(parser, text, value_start, value_end);
}

static ArchbirdStatus set_scalar(SetupParser *parser, AbString *target,
                                 const uint8_t *text, size_t start, size_t end,
                                 int dynamic_forbidden) {
  if (target->length || start == end ||
      (dynamic_forbidden && dynamic_scalar(text, start, end))) {
    ab_string_free(parser->engine, target);
    if (target == &parser->metadata->name)
      parser->metadata_conflict = 1;
    return ARCHBIRD_OK;
  }
  return ab_string_copy(parser->engine, target, (const char *)text + start,
                        end - start);
}

static ArchbirdStatus process_field_value(SetupParser *parser,
                                          const uint8_t *text, size_t start,
                                          size_t end, int continuation) {
  trim(text, &start, &end);
  if (start == end)
    return ARCHBIRD_OK;
  parser->field_value_seen |= 1U << (unsigned int)parser->field;
  switch (parser->field) {
  case SETUP_FIELD_NAME:
    if (continuation) {
      ab_string_free(parser->engine, &parser->metadata->name);
      parser->metadata_conflict = 1;
      return ARCHBIRD_OK;
    }
    return set_scalar(parser, &parser->metadata->name, text, start, end, 1);
  case SETUP_FIELD_VERSION:
    if (continuation) {
      ab_string_free(parser->engine, &parser->metadata->version);
      return ARCHBIRD_OK;
    }
    return set_scalar(parser, &parser->metadata->version, text, start, end, 1);
  case SETUP_FIELD_PACKAGES:
    parser->layout_seen = 1;
    if (span_equal(text, start, end, "find:") ||
        span_equal(text, start, end, "find_namespace:")) {
      if (continuation || parser->find_enabled)
        return invalidate_module_hints(parser);
      parser->find_enabled = 1;
      parser->metadata->source_shape = span_equal(text, start, end, "find:")
                                           ? AB_PYTHON_SOURCE_PACKAGE
                                           : AB_PYTHON_SOURCE_ANY;
      return ARCHBIRD_OK;
    }
    if (parser->find_enabled)
      return invalidate_module_hints(parser);
    return merge_module_list(parser, text, start, end, 0);
  case SETUP_FIELD_PY_MODULES:
    parser->layout_seen = 1;
    parser->metadata->source_shape = AB_PYTHON_SOURCE_MODULE;
    return merge_module_list(parser, text, start, end, 0);
  case SETUP_FIELD_PACKAGE_DIR:
    parser->layout_seen = 1;
    return parse_package_dir(parser, text, start, end);
  case SETUP_FIELD_FIND_INCLUDE:
    parser->layout_seen = 1;
    parser->find_include_seen = 1;
    return merge_module_list(parser, text, start, end, 1);
  case SETUP_FIELD_FIND_WHERE:
    parser->layout_seen = 1;
    if (continuation && parser->metadata->source_root.length)
      return invalidate_source_root(parser);
    return merge_root_span(parser, text, start, end);
  case SETUP_FIELD_FIND_EXCLUDE:
    parser->layout_seen = 1;
    /* Proving that a selected alias survives an arbitrary exclusion pattern
     * requires evaluating setuptools discovery. Keep identity, but suppress
     * package/import-root inference instead of ignoring contrary evidence. */
    return invalidate_module_hints(parser);
  case SETUP_FIELD_NONE:
    return ARCHBIRD_OK;
  }
  return ARCHBIRD_OK;
}

static SetupSection section_kind(const uint8_t *text, size_t start,
                                 size_t end) {
  if (span_equal(text, start, end, "metadata"))
    return SETUP_SECTION_METADATA;
  if (span_equal(text, start, end, "options"))
    return SETUP_SECTION_OPTIONS;
  if (span_equal(text, start, end, "options.packages.find"))
    return SETUP_SECTION_PACKAGE_FIND;
  return SETUP_SECTION_OTHER;
}

static unsigned int section_bit(SetupSection section) {
  return section == SETUP_SECTION_METADATA       ? 1U
         : section == SETUP_SECTION_OPTIONS      ? 2U
         : section == SETUP_SECTION_PACKAGE_FIND ? 4U
                                                 : 0U;
}

static SetupField field_kind(SetupSection section, const uint8_t *text,
                             size_t start, size_t end) {
  if (section == SETUP_SECTION_METADATA && span_equal(text, start, end, "name"))
    return SETUP_FIELD_NAME;
  if (section == SETUP_SECTION_METADATA &&
      span_equal(text, start, end, "version"))
    return SETUP_FIELD_VERSION;
  if (section == SETUP_SECTION_OPTIONS &&
      span_equal(text, start, end, "packages"))
    return SETUP_FIELD_PACKAGES;
  if (section == SETUP_SECTION_OPTIONS &&
      span_equal(text, start, end, "py_modules"))
    return SETUP_FIELD_PY_MODULES;
  if (section == SETUP_SECTION_OPTIONS &&
      span_equal(text, start, end, "package_dir"))
    return SETUP_FIELD_PACKAGE_DIR;
  if (section == SETUP_SECTION_PACKAGE_FIND &&
      span_equal(text, start, end, "include"))
    return SETUP_FIELD_FIND_INCLUDE;
  if (section == SETUP_SECTION_PACKAGE_FIND &&
      span_equal(text, start, end, "where"))
    return SETUP_FIELD_FIND_WHERE;
  if (section == SETUP_SECTION_PACKAGE_FIND &&
      span_equal(text, start, end, "exclude"))
    return SETUP_FIELD_FIND_EXCLUDE;
  return SETUP_FIELD_NONE;
}

static unsigned int field_bit(SetupField field) {
  return field == SETUP_FIELD_NONE ? 0U : 1U << (unsigned int)field;
}

ArchbirdStatus ab_setup_cfg_metadata(ArchbirdEngine *engine,
                                     const uint8_t *text, size_t length,
                                     AbPythonPackageMetadata *out) {
  SetupParser parser;
  size_t line_start = 0;
  ArchbirdStatus status;
  if (!engine || (!text && length) || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_python_package_metadata_init(out);
  memset(&parser, 0, sizeof(parser));
  parser.engine = engine;
  parser.metadata = out;
  status = ab_utf8_validate(engine, text, length);
  while (status == ARCHBIRD_OK && line_start <= length) {
    size_t line_end = line_start;
    size_t start;
    size_t end;
    int indented;
    while (line_end < length && text[line_end] != '\n')
      line_end++;
    start = line_start;
    end = line_end;
    indented = start < end && (text[start] == ' ' || text[start] == '\t');
    trim(text, &start, &end);
    if (start < end && text[start] != '#' && text[start] != ';') {
      if (text[start] == '[' && end > start + 2 && text[end - 1] == ']') {
        unsigned int bit;
        size_t section_start = start + 1;
        size_t section_end = end - 1;
        trim(text, &section_start, &section_end);
        parser.section = section_kind(text, section_start, section_end);
        parser.field = SETUP_FIELD_NONE;
        parser.continuation_allowed = 0;
        bit = section_bit(parser.section);
        if (bit && (parser.section_seen & bit)) {
          if (parser.section == SETUP_SECTION_METADATA)
            parser.metadata_conflict = 1;
          else
            parser.layout_conflict = 1;
        }
        parser.section_seen |= bit;
      } else if (indented && parser.field != SETUP_FIELD_NONE) {
        status = process_field_value(&parser, text, start, end, 1);
      } else if (indented && !parser.continuation_allowed) {
        parser.syntax_invalid = 1;
      } else if (indented) {
        /* A continuation for an unmodeled option is valid INI syntax but does
         * not contribute to the bounded metadata subset. */
      } else {
        size_t equal = start;
        size_t key_start = start;
        size_t key_end;
        size_t value_start;
        size_t value_end = end;
        unsigned int bit;
        while (equal < end && text[equal] != '=')
          equal++;
        parser.field = SETUP_FIELD_NONE;
        if (equal < end) {
          key_end = equal;
          value_start = equal + 1;
          trim(text, &key_start, &key_end);
          trim(text, &value_start, &value_end);
          parser.field = field_kind(parser.section, text, key_start, key_end);
          bit = field_bit(parser.field);
          if (bit && (parser.field_seen & bit)) {
            if (parser.field == SETUP_FIELD_NAME ||
                parser.field == SETUP_FIELD_VERSION)
              parser.metadata_conflict = 1;
            else
              parser.layout_conflict = 1;
          }
          parser.field_seen |= bit;
          parser.continuation_allowed = 1;
          status =
              process_field_value(&parser, text, value_start, value_end, 0);
        } else
          parser.syntax_invalid = 1;
      }
    }
    if (line_end == length)
      break;
    line_start = line_end + 1;
  }
  if (status == ARCHBIRD_OK && parser.syntax_invalid) {
    ab_python_package_metadata_free(engine, out);
    ab_python_package_metadata_init(out);
  } else if (status == ARCHBIRD_OK && parser.metadata_conflict) {
    ab_string_free(engine, &out->name);
    ab_string_free(engine, &out->version);
  }
  if (status == ARCHBIRD_OK && out->name.length &&
      !ab_python_distribution_name_valid(&out->name))
    ab_string_free(engine, &out->name);
  if (status == ARCHBIRD_OK && out->version.length &&
      !nonblank_scalar((const uint8_t *)out->version.data, 0,
                       out->version.length))
    ab_string_free(engine, &out->version);
  if (status == ARCHBIRD_OK && parser.find_include_seen && !parser.find_enabled)
    status = invalidate_module_hints(&parser);
  if (status == ARCHBIRD_OK &&
      (parser.field_seen & field_bit(SETUP_FIELD_PACKAGES)) &&
      (parser.field_seen & field_bit(SETUP_FIELD_PY_MODULES)))
    status = invalidate_module_hints(&parser);
  if (status == ARCHBIRD_OK &&
      (parser.field_seen & field_bit(SETUP_FIELD_PACKAGES)) &&
      !(parser.field_value_seen & field_bit(SETUP_FIELD_PACKAGES)))
    status = invalidate_module_hints(&parser);
  if (status == ARCHBIRD_OK &&
      (parser.field_seen & field_bit(SETUP_FIELD_PY_MODULES)) &&
      !(parser.field_value_seen & field_bit(SETUP_FIELD_PY_MODULES)))
    status = invalidate_module_hints(&parser);
  if (status == ARCHBIRD_OK &&
      (parser.field_seen & field_bit(SETUP_FIELD_PACKAGE_DIR)) &&
      !(parser.field_value_seen & field_bit(SETUP_FIELD_PACKAGE_DIR)))
    status = invalidate_source_root(&parser);
  if (status == ARCHBIRD_OK && !out->source_root_present &&
      (parser.field_seen &
       (field_bit(SETUP_FIELD_PACKAGES) | field_bit(SETUP_FIELD_PY_MODULES))))
    status = merge_root_span(&parser, (const uint8_t *)".", 0, 1);
  if (status == ARCHBIRD_OK && parser.find_include_seen &&
      !(parser.field_value_seen & field_bit(SETUP_FIELD_FIND_INCLUDE)))
    status = invalidate_module_hints(&parser);
  if (status == ARCHBIRD_OK && parser.layout_conflict && parser.layout_seen) {
    status = invalidate_module_hints(&parser);
    if (status == ARCHBIRD_OK)
      status = invalidate_source_root(&parser);
  }
  if (status != ARCHBIRD_OK)
    ab_python_package_metadata_free(engine, out);
  return status;
}
