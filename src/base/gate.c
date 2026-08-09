#include "base/gate.h"

#include "base/artifact_validation.h"
#include "base/render_internal.h"

#include <string.h>

static int buffer_write(void *user_data, const uint8_t *bytes, size_t length) {
  return ab_buffer_append((AbBuffer *)user_data, bytes, length) == ARCHBIRD_OK
             ? 0
             : 1;
}

static int stable_id(const AbString *value) {
  size_t index;
  if (!value || !value->length || value->length > 256)
    return 0;
  for (index = 0; index < value->length; index++) {
    unsigned char byte = (unsigned char)value->data[index];
    if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
          (byte >= '0' && byte <= '9') || (index && strchr("_.:-", byte))))
      return 0;
  }
  return 1;
}

static int fields_allowed(const AbValue *value, const char *const *fields,
                          size_t count) {
  size_t index;
  size_t field;
  for (index = 0; index < value->as.object.count; index++) {
    const AbString *name = &value->as.object.fields[index].name;
    int found = 0;
    for (field = 0; field < count; field++) {
      size_t length = strlen(fields[field]);
      if (name->length == length &&
          !memcmp(name->data, fields[field], length)) {
        found = 1;
        break;
      }
    }
    if (!found)
      return 0;
  }
  return 1;
}

static int safe_cwd(const AbValue *value) {
  const char *data;
  size_t length;
  size_t index;
  if (!value)
    return 1;
  if (value->kind != AB_VALUE_STRING || !value->as.text.length ||
      value->as.text.length > AB_GATE_MAX_ARGUMENT_BYTES)
    return 0;
  data = value->as.text.data;
  length = value->as.text.length;
  if (length == 1 && data[0] == '.')
    return 1;
  if (data[0] == '/' || data[0] == '\\' || data[length - 1] == '/' ||
      data[length - 1] == '\\' || (length >= 2 && data[1] == ':'))
    return 0;
  for (index = 0; index < length; index++) {
    size_t start = index;
    if (data[index] == '\\')
      return 0;
    while (index < length && data[index] != '/')
      index++;
    if (index == start || (index - start == 1 && data[start] == '.') ||
        (index - start == 2 && data[start] == '.' && data[start + 1] == '.'))
      return 0;
  }
  return 1;
}

static int definition_valid(const AbValue *gate) {
  static const char *const fields[] = {"argv", "cwd", "depends_on",
                                       "max_output_bytes", "timeout_seconds"};
  const AbValue *argv;
  const AbValue *depends_on;
  const AbValue *timeout;
  const AbValue *max_output;
  size_t argument;
  uint64_t number;
  if (!gate || gate->kind != AB_VALUE_OBJECT ||
      !fields_allowed(gate, fields, sizeof(fields) / sizeof(fields[0])))
    return 0;
  argv = ab_value_member(gate, "argv");
  depends_on = ab_value_member(gate, "depends_on");
  timeout = ab_value_member(gate, "timeout_seconds");
  max_output = ab_value_member(gate, "max_output_bytes");
  if (!argv || argv->kind != AB_VALUE_ARRAY || !argv->as.array.count ||
      argv->as.array.count > AB_GATE_MAX_ARGUMENTS)
    return 0;
  for (argument = 0; argument < argv->as.array.count; argument++)
    if (!ab_artifact_bounded_text(&argv->as.array.items[argument],
                                  AB_GATE_MAX_ARGUMENT_BYTES, 1))
      return 0;
  if (depends_on) {
    if (depends_on->kind != AB_VALUE_ARRAY ||
        depends_on->as.array.count > AB_GATE_MAX_COUNT)
      return 0;
    for (argument = 0; argument < depends_on->as.array.count; argument++) {
      const AbValue *id = &depends_on->as.array.items[argument];
      if (id->kind != AB_VALUE_STRING || !stable_id(&id->as.text) ||
          (argument &&
           ab_string_compare(&depends_on->as.array.items[argument - 1].as.text,
                             &id->as.text) >= 0))
        return 0;
    }
  }
  if (!ab_artifact_safe_integer(timeout, &number) || !number ||
      number > AB_GATE_MAX_TIMEOUT_SECONDS)
    return 0;
  if (max_output && (!ab_artifact_safe_integer(max_output, &number) ||
                     number > AB_GATE_MAX_OUTPUT_BYTES))
    return 0;
  return safe_cwd(ab_value_member(gate, "cwd"));
}

int ab_gate_definitions_valid(const AbValue *gates) {
  unsigned indegrees[AB_GATE_MAX_COUNT];
  uint8_t emitted[AB_GATE_MAX_COUNT];
  size_t index;
  size_t emitted_count = 0;
  if (!gates || gates->kind != AB_VALUE_OBJECT ||
      gates->as.object.count > AB_GATE_MAX_COUNT)
    return 0;
  memset(indegrees, 0, sizeof(indegrees));
  memset(emitted, 0, sizeof(emitted));
  for (index = 0; index < gates->as.object.count; index++) {
    const AbValue *depends_on =
        ab_value_member(&gates->as.object.fields[index].value, "depends_on");
    size_t dependency;
    if (!stable_id(&gates->as.object.fields[index].name) ||
        !definition_valid(&gates->as.object.fields[index].value))
      return 0;
    if (!depends_on)
      continue;
    for (dependency = 0; dependency < depends_on->as.array.count;
         dependency++) {
      size_t candidate;
      int found = 0;
      if (ab_string_equal(&gates->as.object.fields[index].name,
                          &depends_on->as.array.items[dependency].as.text))
        return 0;
      for (candidate = 0; candidate < gates->as.object.count; candidate++)
        if (ab_string_equal(&gates->as.object.fields[candidate].name,
                            &depends_on->as.array.items[dependency].as.text)) {
          found = 1;
          break;
        }
      if (!found)
        return 0;
      indegrees[index]++;
    }
  }
  while (emitted_count < gates->as.object.count) {
    size_t selected = SIZE_MAX;
    size_t candidate;
    for (candidate = 0; candidate < gates->as.object.count; candidate++)
      if (!emitted[candidate] && !indegrees[candidate] &&
          (selected == SIZE_MAX ||
           ab_string_compare(&gates->as.object.fields[candidate].name,
                             &gates->as.object.fields[selected].name) < 0))
        selected = candidate;
    if (selected == SIZE_MAX)
      return 0;
    emitted[selected] = 1;
    emitted_count++;
    for (candidate = 0; candidate < gates->as.object.count; candidate++) {
      const AbValue *depends_on = ab_value_member(
          &gates->as.object.fields[candidate].value, "depends_on");
      size_t dependency;
      if (emitted[candidate] || !depends_on)
        continue;
      for (dependency = 0; dependency < depends_on->as.array.count;
           dependency++)
        if (ab_string_equal(&gates->as.object.fields[selected].name,
                            &depends_on->as.array.items[dependency].as.text)) {
          indegrees[candidate]--;
          break;
        }
    }
  }
  return 1;
}

ArchbirdStatus ab_gate_definition_sha256(ArchbirdEngine *engine,
                                         const AbValue *gate, char out[65]) {
  AbBuffer rendered;
  AbBuffer canonical;
  ArchbirdStatus status;
  if (!engine || !gate || !out || !definition_valid(gate))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&rendered, engine);
  ab_buffer_init(&canonical, engine);
  status = ab_value_render(&rendered, gate);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, rendered.data, rendered.length,
                                        0, buffer_write, &canonical);
  if (status == ARCHBIRD_OK)
    status =
        ab_artifact_json_sha256(engine, canonical.data, canonical.length, out);
  ab_buffer_free(&canonical);
  ab_buffer_free(&rendered);
  return status;
}
