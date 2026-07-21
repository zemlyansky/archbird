#include <archbird/archbird.h>

#include "json_value.h"
#include "verify_internal.h"

#include <string.h>

#define RECIPE_TRY(expression)                                                 \
  do {                                                                         \
    ArchbirdStatus recipe_status__ = (expression);                             \
    if (recipe_status__ != ARCHBIRD_OK)                                        \
      return recipe_status__;                                                  \
  } while (0)

static int text_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static int member_is(const AbValue *object, const char *name,
                     const char *literal) {
  return ab_verify_string_is(ab_value_member(object, name), literal);
}

static int field_allowed(const AbString *name, const char *const *allowed,
                         size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    if (text_is(name, allowed[index]))
      return 1;
  return 0;
}

static ArchbirdStatus fields_allowed(ArchbirdEngine *engine,
                                     const AbValue *object, const char *where,
                                     const char *const *allowed,
                                     size_t allowed_count) {
  size_t index;
  if (!object || object->kind != AB_VALUE_OBJECT)
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET, "%s: expected object", where);
  for (index = 0; index < object->as.object.count; index++) {
    const AbString *name = &object->as.object.fields[index].name;
    if (!field_allowed(name, allowed, allowed_count))
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET, "%s: unknown field '%.*s'",
                                where, (int)name->length, name->data);
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "%s", message);
}

static int strings(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < value->as.array.count; index++)
    if (!ab_verify_nonblank(&value->as.array.items[index]))
      return 0;
  return 1;
}

static ArchbirdStatus render_recipe(AbBuffer *buffer) {
  return ab_buffer_literal(
      buffer,
      "{\"arguments\":[{\"default\":false,\"description\":\"Permit an "
      "empty file selection only when explicitly intended.\",\"name\":"
      "\"allow-empty\",\"required\":false,\"type\":\"boolean\"},{\"default\":"
      "[],\"description\":\"Exclude repository-relative file globs after "
      "include selection.\",\"name\":\"exclude\",\"repeatable\":true,"
      "\"required\":false,\"type\":\"path-glob\"},{\"default\":[],"
      "\"description\":\"Select repository-relative file globs; an empty list "
      "selects every mapped source file.\",\"name\":\"include\","
      "\"repeatable\":true,\"required\":false,\"type\":\"path-glob\"},{"
      "\"description\":\"Maximum permitted UTF-8 source bytes per selected "
      "file.\",\"name\":\"max\",\"required\":true,\"type\":\"byte-size\"}],"
      "\"description\":\"Require every selected mapped source file to remain "
      "within an explicit byte limit.\",\"evidence\":\"map.files.bytes\","
      "\"name\":\"max-file-bytes\",\"nonempty_by_default\":true,"
      "\"policy_source\":\"explicit-arguments\"}");
}

ArchbirdStatus archbird_verification_recipe_catalog(
    ArchbirdEngine *engine, const char *recipe, size_t recipe_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data) {
  AbBuffer catalog;
  ArchbirdStatus status;
  int selected;
  if (!engine || (!recipe && recipe_length) || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  selected =
      !recipe_length || (recipe_length == strlen("max-file-bytes") &&
                         memcmp(recipe, "max-file-bytes", recipe_length) == 0);
  if (!selected)
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_ARGUMENT, ARCHBIRD_NO_OFFSET,
        "unknown verification recipe '%.*s'", (int)recipe_length, recipe);
  ab_buffer_init(&catalog, engine);
  status = ab_buffer_literal(
      &catalog, "{\"artifact\":\"verification-recipe-catalog\",\"recipes\":[");
  if (status == ARCHBIRD_OK)
    status = render_recipe(&catalog);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&catalog, "],\"schema_version\":1}");
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, catalog.data, catalog.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&catalog);
  return status;
}

static ArchbirdStatus render_optional(AbBuffer *buffer, const AbValue *object,
                                      const char *name) {
  const AbValue *value = ab_value_member(object, name);
  if (!value)
    return ARCHBIRD_OK;
  RECIPE_TRY(ab_buffer_literal(buffer, ","));
  RECIPE_TRY(ab_buffer_json_string(buffer, name, strlen(name)));
  RECIPE_TRY(ab_buffer_literal(buffer, ":"));
  return ab_value_render(buffer, value);
}

static ArchbirdStatus render_suite(ArchbirdEngine *engine,
                                   const AbValue *request, AbBuffer *buffer) {
  static const char *const root_fields[] = {
      "arguments", "artifact", "check", "project", "recipe", "schema_version",
  };
  static const char *const argument_fields[] = {
      "allow_empty",
      "exclude",
      "include",
      "max",
  };
  static const char *const check_fields[] = {
      "id", "owner", "rationale", "requirement", "severity", "tags",
  };
  const AbValue *schema = ab_value_member(request, "schema_version");
  const AbValue *project = ab_value_member(request, "project");
  const AbValue *arguments = ab_value_member(request, "arguments");
  const AbValue *check = ab_value_member(request, "check");
  const AbValue *maximum;
  const AbValue *allow_empty;
  uint64_t schema_number;
  uint64_t maximum_number;
  AbValue document = {0};
  AbVerifySuiteView suite = {0};
  ArchbirdStatus status;
  if (fields_allowed(engine, request, "recipe request", root_fields,
                     sizeof(root_fields) / sizeof(root_fields[0])) !=
          ARCHBIRD_OK ||
      !member_is(request, "artifact", "verification-recipe-request") ||
      !schema || !ab_value_u64(schema, &schema_number) || schema_number != 1 ||
      !member_is(request, "recipe", "max-file-bytes"))
    return invalid(engine, "invalid verification recipe request identity");
  if (fields_allowed(engine, arguments, "recipe arguments", argument_fields,
                     sizeof(argument_fields) / sizeof(argument_fields[0])) !=
          ARCHBIRD_OK ||
      fields_allowed(engine, check, "recipe check", check_fields,
                     sizeof(check_fields) / sizeof(check_fields[0])) !=
          ARCHBIRD_OK)
    return ARCHBIRD_INVALID_SCHEMA;
  maximum = ab_value_member(arguments, "max");
  allow_empty = ab_value_member(arguments, "allow_empty");
  if (!project || project->kind != AB_VALUE_OBJECT || !maximum ||
      !ab_value_u64(maximum, &maximum_number) ||
      (allow_empty && allow_empty->kind != AB_VALUE_BOOL) ||
      (ab_value_member(arguments, "include") &&
       !strings(ab_value_member(arguments, "include"))) ||
      (ab_value_member(arguments, "exclude") &&
       !strings(ab_value_member(arguments, "exclude"))) ||
      !ab_verify_nonblank(ab_value_member(check, "id")) ||
      !ab_verify_nonblank(ab_value_member(check, "owner")) ||
      !ab_verify_nonblank(ab_value_member(check, "rationale")))
    return invalid(engine, "invalid max-file-bytes recipe arguments");

  RECIPE_TRY(ab_buffer_literal(
      buffer,
      "{\"checks\":[{\"actual\":\"recipe.file-bytes\",\"allow_empty\":"));
  RECIPE_TRY(allow_empty ? ab_value_render(buffer, allow_empty)
                         : ab_buffer_literal(buffer, "false"));
  RECIPE_TRY(
      ab_buffer_literal(buffer, ",\"assert\":\"numeric_bounds\",\"id\":"));
  RECIPE_TRY(ab_value_render(buffer, ab_value_member(check, "id")));
  RECIPE_TRY(ab_buffer_literal(buffer, ",\"max\":"));
  RECIPE_TRY(ab_value_render(buffer, maximum));
  RECIPE_TRY(ab_buffer_literal(buffer, ",\"owner\":"));
  RECIPE_TRY(ab_value_render(buffer, ab_value_member(check, "owner")));
  RECIPE_TRY(ab_buffer_literal(buffer, ",\"rationale\":"));
  RECIPE_TRY(ab_value_render(buffer, ab_value_member(check, "rationale")));
  RECIPE_TRY(render_optional(buffer, check, "requirement"));
  RECIPE_TRY(render_optional(buffer, check, "severity"));
  RECIPE_TRY(render_optional(buffer, check, "tags"));
  RECIPE_TRY(ab_buffer_literal(
      buffer,
      "}],\"description\":\"Compiled max-file-bytes verification recipe.\","
      "\"extractors\":{\"recipe.file-bytes\":{\"exclude\":"));
  RECIPE_TRY(
      ab_value_member(arguments, "exclude")
          ? ab_value_render(buffer, ab_value_member(arguments, "exclude"))
          : ab_buffer_literal(buffer, "[]"));
  RECIPE_TRY(ab_buffer_literal(buffer, ",\"include\":"));
  RECIPE_TRY(
      ab_value_member(arguments, "include")
          ? ab_value_render(buffer, ab_value_member(arguments, "include"))
          : ab_buffer_literal(buffer, "[]"));
  RECIPE_TRY(ab_buffer_literal(
      buffer, ",\"kind\":\"file_metrics\",\"metric\":\"bytes\",\"project\":"
              "\"subject\"}},\"projects\":{\"subject\":"));
  RECIPE_TRY(ab_value_render(buffer, project));
  RECIPE_TRY(ab_buffer_literal(
      buffer, "},\"schema_version\":1,\"suite\":\"recipe.max-file-bytes\"}"));

  status =
      ab_json_value_decode(engine, buffer->data, buffer->length, &document);
  if (status == ARCHBIRD_OK)
    status = ab_verify_suite_validate(engine, &document, &suite);
  ab_value_free(engine, &document);
  return status;
}

ArchbirdStatus archbird_verification_recipe_compile(
    ArchbirdEngine *engine, const uint8_t *request_json, size_t request_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data) {
  AbValue request = {0};
  AbBuffer suite;
  ArchbirdStatus status;
  if (!engine || (!request_json && request_length) || !request_length ||
      !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&suite, engine);
  status = ab_json_value_decode(engine, request_json, request_length, &request);
  if (status == ARCHBIRD_OK)
    status = render_suite(engine, &request, &suite);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, suite.data, suite.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&suite);
  ab_value_free(engine, &request);
  return status;
}

#undef RECIPE_TRY
