#include "transformation/rename_capability.h"

#include <string.h>

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static const AbValue *attribute(const AbProjectionItem *item,
                                const char *name) {
  size_t index;
  size_t length = strlen(name);
  for (index = 0; item && index < item->attribute_count; index++)
    if (item->attributes[index].name.length == length &&
        memcmp(item->attributes[index].name.data, name, length) == 0)
      return &item->attributes[index].value;
  return NULL;
}

static int text_is(const AbValue *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->kind == AB_VALUE_STRING &&
         value->as.text.length == length &&
         memcmp(value->as.text.data, literal, length) == 0;
}

static int role_is(const AbString *role, const char *literal) {
  size_t length = strlen(literal);
  return role && role->length == length &&
         memcmp(role->data, literal, length) == 0;
}

static int string_array(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < value->as.array.count; index++)
    if (value->as.array.items[index].kind != AB_VALUE_STRING ||
        !value->as.array.items[index].as.text.length)
      return 0;
  return 1;
}

static int providers_have(const AbValue *providers, const char *name) {
  size_t index;
  if (!providers || providers->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < providers->as.array.count; index++)
    if (text_is(&providers->as.array.items[index], name))
      return 1;
  return 0;
}

static int python_supported(const AbRenameEvidence *evidence,
                            const char **out_reason) {
  if (!text_is(field(evidence->file, "language"), "python")) {
    *out_reason = "the occurrence is not mapped as Python";
    return 0;
  }
  if (role_is(evidence->role, "declaration"))
    return 1;
  if (!role_is(evidence->role, "binding") &&
      !role_is(evidence->role, "export") &&
      !role_is(evidence->role, "import") &&
      !role_is(evidence->role, "reference")) {
    *out_reason = "the Python occurrence has an unsupported semantic role";
    return 0;
  }
  if (!providers_have(evidence->providers, "archbird-python-ast")) {
    *out_reason =
        "the Python occurrence is not established by the CPython AST provider";
    return 0;
  }
  return 1;
}

static int ecmascript_supported(const AbRenameEvidence *evidence,
                                const char **out_reason) {
  const AbValue *language = field(evidence->file, "language");
  if (!text_is(language, "javascript") && !text_is(language, "typescript") &&
      !text_is(language, "tsx")) {
    *out_reason =
        "the occurrence is not mapped as JavaScript, TypeScript, or TSX";
    return 0;
  }
  if (role_is(evidence->role, "declaration"))
    return 1;
  if (!role_is(evidence->role, "binding") &&
      !role_is(evidence->role, "export") &&
      !role_is(evidence->role, "import") &&
      !role_is(evidence->role, "reference")) {
    *out_reason = "the ECMAScript occurrence has an unsupported semantic role";
    return 0;
  }
  if (role_is(evidence->role, "reference") &&
      !providers_have(evidence->providers, "archbird-typescript")) {
    *out_reason = "the ECMAScript reference is not established by the "
                  "TypeScript compiler";
    return 0;
  }
  if (!role_is(evidence->role, "reference") &&
      !providers_have(evidence->providers, "archbird-typescript") &&
      !providers_have(evidence->providers, "archbird-tree-sitter-javascript") &&
      !providers_have(evidence->providers, "archbird-tree-sitter-typescript") &&
      !providers_have(evidence->providers, "archbird-tree-sitter-tsx")) {
    *out_reason =
        "the ECMAScript declaration or binding is not established by a syntax "
        "provider";
    return 0;
  }
  return 1;
}

int ab_rename_evidence_supported(const AbProjectionItem *item,
                                 const AbValue *file, const char **out_reason) {
  const AbValue *role = attribute(item, "role");
  const AbValue *providers = attribute(item, "providers");
  const AbValue *language = field(file, "language");
  AbRenameEvidence evidence;
  *out_reason = NULL;
  if (!role || role->kind != AB_VALUE_STRING ||
      (providers && !string_array(providers))) {
    *out_reason = "the occurrence has no typed role/provider evidence";
    return 0;
  }
  evidence.file = file;
  evidence.providers = providers;
  evidence.role = &role->as.text;
  if (text_is(language, "python"))
    return python_supported(&evidence, out_reason);
  if (text_is(language, "javascript") || text_is(language, "typescript") ||
      text_is(language, "tsx"))
    return ecmascript_supported(&evidence, out_reason);
  *out_reason = "no language executor supports the mapped source language";
  return 0;
}
