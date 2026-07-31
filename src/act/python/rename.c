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

int ab_act_python_rename_evidence_supported(const AbActRenameEvidence *evidence,
                                            const char **out_reason) {
  *out_reason = NULL;
  if (!evidence || !text_is(field(evidence->file, "language"), "python")) {
    *out_reason = "the occurrence is not mapped as Python";
    return 0;
  }
  if (role_is(evidence->role, "declaration") && !evidence->providers)
    return 1;
  if (!role_is(evidence->role, "binding") &&
      !role_is(evidence->role, "declaration") &&
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
