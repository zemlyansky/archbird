#include "query_context.h"

#include <stdint.h>
#include <string.h>

static int string_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static int string_allowed(const AbString *value, const char *const *allowed,
                          size_t allowed_count) {
  size_t index;
  for (index = 0; index < allowed_count; index++)
    if (string_is(value, allowed[index]))
      return 1;
  return 0;
}

static int enum_array_valid(const AbValue *value, const char *const *allowed,
                            size_t allowed_count) {
  size_t index;
  if (!value || value->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < value->as.array.count; index++)
    if (value->as.array.items[index].kind != AB_VALUE_STRING ||
        !string_allowed(&value->as.array.items[index].as.text, allowed,
                        allowed_count))
      return 0;
  return 1;
}

static ArchbirdStatus validate_counts(ArchbirdEngine *engine,
                                      const AbValue *value, const char *field) {
  static const char *const allowed[] = {"files", "symbol_calls",
                                        "symbol_references", "test_matches"};
  size_t index;
  uint64_t count;
  if (!value || value->kind != AB_VALUE_OBJECT)
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "query.context.%s must be an object", field);
  for (index = 0; index < value->as.object.count; index++) {
    const AbObjectField *row = &value->as.object.fields[index];
    if (!string_allowed(&row->name, allowed,
                        sizeof(allowed) / sizeof(allowed[0])) ||
        !ab_value_u64(&row->value, &count) || count > SIZE_MAX)
      return archbird_error_set(
          engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "query.context.%s values must be nonnegative integers for known "
          "fact kinds",
          field);
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_query_context_validate(ArchbirdEngine *engine,
                                         const AbValue *context) {
  static const char *const profiles[] = {"exact", "change", "architecture",
                                         "audit"};
  static const char *const provenances[] = {"derived", "asserted", "observed"};
  static const char *const confidences[] = {"exact", "candidate",
                                            "conservative", "unresolved"};
  static const char *const route_modes[] = {"collapse", "expand", "exclude"};
  size_t index;
  uint64_t distance;
  if (!engine || !context)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (context->kind != AB_VALUE_OBJECT)
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "query.context must be an object");
  for (index = 0; index < context->as.object.count; index++) {
    const AbObjectField *field = &context->as.object.fields[index];
    if (string_is(&field->name, "profile")) {
      if (field->value.kind != AB_VALUE_STRING ||
          !string_allowed(&field->value.as.text, profiles,
                          sizeof(profiles) / sizeof(profiles[0])))
        return archbird_error_set(
            engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
            "query.context.profile must be exact, change, architecture, or "
            "audit");
    } else if (string_is(&field->name, "provenance")) {
      if (!enum_array_valid(&field->value, provenances,
                            sizeof(provenances) / sizeof(provenances[0])))
        return archbird_error_set(
            engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
            "query.context.provenance contains an invalid value");
    } else if (string_is(&field->name, "confidence")) {
      if (!enum_array_valid(&field->value, confidences,
                            sizeof(confidences) / sizeof(confidences[0])))
        return archbird_error_set(
            engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
            "query.context.confidence contains an invalid value");
    } else if (string_is(&field->name, "max_seed_distance")) {
      if (!ab_value_u64(&field->value, &distance) || distance > SIZE_MAX)
        return archbird_error_set(
            engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
            "query.context.max_seed_distance must be a nonnegative integer");
    } else if (string_is(&field->name, "candidate") ||
               string_is(&field->name, "conservative")) {
      if (field->value.kind != AB_VALUE_STRING ||
          !string_allowed(&field->value.as.text, route_modes,
                          sizeof(route_modes) / sizeof(route_modes[0])))
        return archbird_error_set(
            engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
            "query context candidate/conservative policy must be collapse, "
            "expand, or exclude");
    } else if (string_is(&field->name, "quotas") ||
               string_is(&field->name, "offsets")) {
      ArchbirdStatus status = validate_counts(
          engine, &field->value,
          string_is(&field->name, "quotas") ? "quotas" : "offsets");
      if (status != ARCHBIRD_OK)
        return status;
    } else {
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET,
                                "query.context contains an unknown field");
    }
  }
  return ARCHBIRD_OK;
}
