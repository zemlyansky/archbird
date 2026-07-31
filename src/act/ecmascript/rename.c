#include "rename_internal.h"

#include <string.h>

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
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

static int providers_have(const AbValue *providers, const char *name) {
  size_t index;
  if (!providers || providers->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < providers->as.array.count; index++)
    if (text_is(&providers->as.array.items[index], name))
      return 1;
  return 0;
}

int ab_act_ecmascript_rename_evidence_supported(
    const AbActRenameEvidence *evidence, const char **out_reason) {
  const AbValue *language = evidence ? field(evidence->file, "language") : NULL;
  *out_reason = NULL;
  if (!text_is(language, "javascript") && !text_is(language, "typescript") &&
      !text_is(language, "tsx")) {
    *out_reason =
        "the occurrence is not mapped as JavaScript, TypeScript, or TSX";
    return 0;
  }
  if (role_is(evidence->role, "declaration") && !evidence->providers)
    return 1;
  if (!role_is(evidence->role, "binding") &&
      !role_is(evidence->role, "declaration") &&
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
