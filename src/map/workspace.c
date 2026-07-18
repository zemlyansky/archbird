#include <archbird/archbird.h>

#include "archbird_internal.h"
#include "json_value.h"
#include "render_internal.h"
#include "sha256.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct WorkspaceProject {
  const AbValue *map;
  const AbValue *name;
  const AbValue *description;
  const AbValue *evidence;
  const AbValue *files;
  const AbValue *layers;
  const AbValue *components;
  const AbValue *packages;
  const AbValue *artifacts;
  const AbValue *edges;
  const AbValue *diagnostics;
} WorkspaceProject;

typedef struct WorkspaceIdentity {
  AbString manager;
  AbString normalized;
  AbString project;
  AbString package;
} WorkspaceIdentity;

typedef struct WorkspaceRoute {
  AbString kind;
  AbString source_project;
  AbString source_package;
  AbString target_project;
  AbString target_package;
  AbString dependency;
  AbStringArray evidence;
} WorkspaceRoute;

typedef struct WorkspaceExternal {
  AbString project;
  AbString manager;
  AbString dependency;
  AbStringArray evidence;
} WorkspaceExternal;

typedef struct WorkspaceDiagnostic {
  AbString severity;
  AbString code;
  AbString message;
  AbString path;
} WorkspaceDiagnostic;

typedef struct WorkspaceContext {
  ArchbirdEngine *engine;
  const AbValue *config;
  const AbValue *project_specs;
  WorkspaceProject *projects;
  size_t project_count;
  WorkspaceIdentity *identities;
  size_t identity_count;
  size_t identity_capacity;
  WorkspaceRoute *routes;
  size_t route_count;
  size_t route_capacity;
  WorkspaceExternal *externals;
  size_t external_count;
  size_t external_capacity;
  WorkspaceDiagnostic *diagnostics;
  size_t diagnostic_count;
  size_t diagnostic_capacity;
  char config_sha256[65];
  char input_sha256[65];
} WorkspaceContext;

static ArchbirdStatus workspace_error(WorkspaceContext *context,
                                      const char *message) {
  return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                            ARCHBIRD_NO_OFFSET, "%s", message);
}

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static int value_string_compare(const AbValue *left, const AbValue *right) {
  return ab_string_compare(&left->as.text, &right->as.text);
}

static const AbValue *required_member(WorkspaceContext *context,
                                      const AbValue *object, const char *name,
                                      AbValueKind kind) {
  const AbValue *value = ab_value_member(object, name);
  if (!value || value->kind != kind) {
    archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                       ARCHBIRD_NO_OFFSET,
                       "workspace field '%s' has the wrong type", name);
    return NULL;
  }
  return value;
}

static int name_in(const AbString *name, const char *const *allowed,
                   size_t allowed_count) {
  size_t index;
  for (index = 0; index < allowed_count; index++)
    if (string_literal(name, allowed[index]))
      return 1;
  return 0;
}

static ArchbirdStatus reject_unknown_fields(WorkspaceContext *context,
                                            const AbValue *object,
                                            const char *where,
                                            const char *const *allowed,
                                            size_t allowed_count) {
  size_t index;
  if (!object || object->kind != AB_VALUE_OBJECT)
    return workspace_error(context, "workspace expected an object");
  for (index = 0; index < object->as.object.count; index++) {
    const AbString *name = &object->as.object.fields[index].name;
    if (!name_in(name, allowed, allowed_count))
      return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET, "%s: unknown field '%.*s'",
                                where, (int)name->length, name->data);
  }
  return ARCHBIRD_OK;
}

static int nonblank_string(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || !value->as.text.length)
    return 0;
  for (index = 0; index < value->as.text.length; index++)
    if (!isspace((unsigned char)value->as.text.data[index]))
      return 1;
  return 0;
}

static ArchbirdStatus copy_string(WorkspaceContext *context, AbString *out,
                                  const AbString *source) {
  return ab_string_copy(context->engine, out, source->data, source->length);
}

static ArchbirdStatus copy_literal(WorkspaceContext *context, AbString *out,
                                   const char *literal) {
  return ab_string_copy(context->engine, out, literal, strlen(literal));
}

static ArchbirdStatus concat_string(WorkspaceContext *context, AbString *out,
                                    const char *prefix, const AbString *value) {
  size_t prefix_length = strlen(prefix);
  char *bytes;
  if (prefix_length > SIZE_MAX - value->length)
    return archbird_error_set(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "workspace string is too large");
  bytes = (char *)ab_malloc(context->engine, prefix_length + value->length + 1);
  if (!bytes)
    return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory constructing workspace evidence");
  memcpy(bytes, prefix, prefix_length);
  memcpy(bytes + prefix_length, value->data, value->length);
  bytes[prefix_length + value->length] = '\0';
  out->data = bytes;
  out->length = prefix_length + value->length;
  return ARCHBIRD_OK;
}

static int owned_string_compare(const void *left_raw, const void *right_raw) {
  return ab_string_compare((const AbString *)left_raw,
                           (const AbString *)right_raw);
}

static ArchbirdStatus append_unique_string(WorkspaceContext *context,
                                           AbStringArray *array,
                                           const AbString *value) {
  AbString *resized;
  size_t index;
  ArchbirdStatus status;
  for (index = 0; index < array->count; index++)
    if (ab_string_equal(&array->items[index], value))
      return ARCHBIRD_OK;
  if (array->count == SIZE_MAX / sizeof(*array->items))
    return archbird_error_set(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many workspace evidence values");
  resized = (AbString *)ab_realloc(context->engine, array->items,
                                   (array->count + 1) * sizeof(*array->items));
  if (!resized)
    return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting workspace evidence");
  array->items = resized;
  memset(&array->items[array->count], 0, sizeof(*array->items));
  status = copy_string(context, &array->items[array->count], value);
  if (status == ARCHBIRD_OK)
    array->count++;
  return status;
}

static void string_array_free(WorkspaceContext *context, AbStringArray *array) {
  size_t index;
  for (index = 0; index < array->count; index++)
    ab_string_free(context->engine, &array->items[index]);
  ab_free(context->engine, array->items);
  memset(array, 0, sizeof(*array));
}

static ArchbirdStatus normalize_name(WorkspaceContext *context,
                                     const AbString *manager,
                                     const AbString *name, AbString *out) {
  size_t start = 0;
  size_t end = name->length;
  size_t read;
  size_t written = 0;
  int normalized_separators =
      string_literal(manager, "pypi") || string_literal(manager, "cran");
  int lower = normalized_separators || string_literal(manager, "npm");
  char *bytes;
  while (start < end && isspace((unsigned char)name->data[start]))
    start++;
  while (end > start && isspace((unsigned char)name->data[end - 1]))
    end--;
  bytes = (char *)ab_malloc(context->engine, end - start + 1);
  if (!bytes)
    return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory normalizing package identity");
  for (read = start; read < end; read++) {
    unsigned char byte = (unsigned char)name->data[read];
    if (normalized_separators && (byte == '-' || byte == '_' || byte == '.')) {
      if (!written || bytes[written - 1] != '-')
        bytes[written++] = '-';
      continue;
    }
    bytes[written++] =
        (char)(lower && byte >= 'A' && byte <= 'Z' ? byte - 'A' + 'a' : byte);
  }
  bytes[written] = '\0';
  out->data = bytes;
  out->length = written;
  return ARCHBIRD_OK;
}

static const char *manager_for_package(const AbString *kind) {
  if (string_literal(kind, "npm"))
    return "npm";
  if (string_literal(kind, "python"))
    return "pypi";
  if (string_literal(kind, "r"))
    return "cran";
  return "generic";
}

static const char *manager_for_language(const AbString *language) {
  if (string_literal(language, "javascript") ||
      string_literal(language, "typescript") || string_literal(language, "vue"))
    return "npm";
  if (string_literal(language, "python"))
    return "pypi";
  if (string_literal(language, "r"))
    return "cran";
  return NULL;
}

static void identity_free(WorkspaceContext *context,
                          WorkspaceIdentity *identity) {
  ab_string_free(context->engine, &identity->manager);
  ab_string_free(context->engine, &identity->normalized);
  ab_string_free(context->engine, &identity->project);
  ab_string_free(context->engine, &identity->package);
}

static void route_free(WorkspaceContext *context, WorkspaceRoute *route) {
  ab_string_free(context->engine, &route->kind);
  ab_string_free(context->engine, &route->source_project);
  ab_string_free(context->engine, &route->source_package);
  ab_string_free(context->engine, &route->target_project);
  ab_string_free(context->engine, &route->target_package);
  ab_string_free(context->engine, &route->dependency);
  string_array_free(context, &route->evidence);
}

static void external_free(WorkspaceContext *context,
                          WorkspaceExternal *external) {
  ab_string_free(context->engine, &external->project);
  ab_string_free(context->engine, &external->manager);
  ab_string_free(context->engine, &external->dependency);
  string_array_free(context, &external->evidence);
}

static void diagnostic_free(WorkspaceContext *context,
                            WorkspaceDiagnostic *diagnostic) {
  ab_string_free(context->engine, &diagnostic->severity);
  ab_string_free(context->engine, &diagnostic->code);
  ab_string_free(context->engine, &diagnostic->message);
  ab_string_free(context->engine, &diagnostic->path);
}

static void workspace_context_free(WorkspaceContext *context) {
  size_t index;
  for (index = 0; index < context->identity_count; index++)
    identity_free(context, &context->identities[index]);
  for (index = 0; index < context->route_count; index++)
    route_free(context, &context->routes[index]);
  for (index = 0; index < context->external_count; index++)
    external_free(context, &context->externals[index]);
  for (index = 0; index < context->diagnostic_count; index++)
    diagnostic_free(context, &context->diagnostics[index]);
  ab_free(context->engine, context->projects);
  ab_free(context->engine, context->identities);
  ab_free(context->engine, context->routes);
  ab_free(context->engine, context->externals);
  ab_free(context->engine, context->diagnostics);
  memset(context, 0, sizeof(*context));
}

static ArchbirdStatus digest_value(WorkspaceContext *context,
                                   const AbValue *value, char out[65]) {
  AbBuffer buffer;
  uint8_t digest[32];
  ArchbirdStatus status;
  ab_buffer_init(&buffer, context->engine);
  status = ab_value_render(&buffer, value);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(buffer.data, buffer.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus validate_workspace_config(WorkspaceContext *context,
                                                const AbValue *root) {
  static const char *const root_fields[] = {"schema_version", "workspace",
                                            "description", "projects"};
  static const char *const project_fields[] = {"config", "root"};
  const AbValue *schema;
  const AbValue *name;
  const AbValue *description;
  const AbValue *projects;
  uint64_t version;
  size_t index;
  ArchbirdStatus status;
  if (!root || root->kind != AB_VALUE_OBJECT)
    return workspace_error(context, "workspace config: expected object");
  status = reject_unknown_fields(context, root, "workspace config", root_fields,
                                 sizeof(root_fields) / sizeof(root_fields[0]));
  if (status != ARCHBIRD_OK)
    return status;
  schema = required_member(context, root, "schema_version", AB_VALUE_INTEGER);
  name = required_member(context, root, "workspace", AB_VALUE_STRING);
  projects = required_member(context, root, "projects", AB_VALUE_ARRAY);
  description = ab_value_member(root, "description");
  if (!schema || !name || !projects)
    return ARCHBIRD_INVALID_SCHEMA;
  if (!ab_value_u64(schema, &version) || version != 1)
    return workspace_error(context, "workspace schema_version: expected 1");
  if (!nonblank_string(name))
    return workspace_error(context, "workspace: expected non-empty string");
  if (description && description->kind != AB_VALUE_STRING)
    return workspace_error(context, "workspace.description: expected string");
  if (!projects->as.array.count)
    return workspace_error(context,
                           "workspace projects: expected a non-empty array");
  for (index = 0; index < projects->as.array.count; index++) {
    const AbValue *row = &projects->as.array.items[index];
    const AbValue *config;
    const AbValue *root_value;
    size_t earlier;
    if (row->kind != AB_VALUE_OBJECT)
      return workspace_error(context,
                             "workspace project entry: expected object");
    status = reject_unknown_fields(
        context, row, "workspace project", project_fields,
        sizeof(project_fields) / sizeof(project_fields[0]));
    if (status != ARCHBIRD_OK)
      return status;
    config = required_member(context, row, "config", AB_VALUE_STRING);
    if (!config || !nonblank_string(config))
      return workspace_error(
          context, "workspace project config: expected non-empty string");
    root_value = ab_value_member(row, "root");
    if (root_value &&
        (root_value->kind != AB_VALUE_STRING || !nonblank_string(root_value)))
      return workspace_error(
          context, "workspace project root: expected non-empty string");
    for (earlier = 0; earlier < index; earlier++) {
      const AbValue *other =
          ab_value_member(&projects->as.array.items[earlier], "config");
      if (other && ab_string_equal(&config->as.text, &other->as.text))
        return workspace_error(context,
                               "workspace project config is duplicated");
    }
  }
  context->config = root;
  context->project_specs = projects;
  return digest_value(context, root, context->config_sha256);
}

static ArchbirdStatus validate_map_project(WorkspaceContext *context,
                                           const AbValue *map,
                                           WorkspaceProject *out) {
  const AbValue *artifact;
  const AbValue *schema;
  uint64_t version;
  memset(out, 0, sizeof(*out));
  if (!map || map->kind != AB_VALUE_OBJECT)
    return workspace_error(context, "workspace map input: expected map object");
  artifact = required_member(context, map, "artifact", AB_VALUE_STRING);
  schema = required_member(context, map, "schema_version", AB_VALUE_INTEGER);
  if (!artifact || !schema || !ab_value_string_is(artifact, "map") ||
      !ab_value_u64(schema, &version) || version < ARCHBIRD_MAP_SCHEMA_MIN ||
      version > ARCHBIRD_MAP_SCHEMA_CURRENT)
    return workspace_error(context,
                           "workspace input must contain Archbird maps "
                           "schema " ARCHBIRD_MAP_SCHEMA_SUPPORTED_TEXT);
  out->map = map;
  out->name = required_member(context, map, "project", AB_VALUE_STRING);
  out->description =
      required_member(context, map, "description", AB_VALUE_STRING);
  out->evidence = required_member(context, map, "evidence", AB_VALUE_OBJECT);
  out->files = required_member(context, map, "files", AB_VALUE_ARRAY);
  out->layers = required_member(context, map, "layers", AB_VALUE_ARRAY);
  out->components = required_member(context, map, "components", AB_VALUE_ARRAY);
  out->packages = required_member(context, map, "packages", AB_VALUE_ARRAY);
  out->artifacts = required_member(context, map, "artifacts", AB_VALUE_ARRAY);
  out->edges = required_member(context, map, "edges", AB_VALUE_ARRAY);
  out->diagnostics =
      required_member(context, map, "diagnostics", AB_VALUE_ARRAY);
  if (!out->name || !out->description || !out->evidence || !out->files ||
      !out->layers || !out->components || !out->packages || !out->artifacts ||
      !out->edges || !out->diagnostics)
    return ARCHBIRD_INVALID_SCHEMA;
  if (!nonblank_string(out->name) ||
      !required_member(context, out->evidence, "config_sha256",
                       AB_VALUE_STRING) ||
      !required_member(context, out->evidence, "input_sha256", AB_VALUE_STRING))
    return workspace_error(context,
                           "workspace map identity evidence is incomplete");
  return ARCHBIRD_OK;
}

static int project_compare(const void *left_raw, const void *right_raw) {
  const WorkspaceProject *left = (const WorkspaceProject *)left_raw;
  const WorkspaceProject *right = (const WorkspaceProject *)right_raw;
  return value_string_compare(left->name, right->name);
}

static ArchbirdStatus decode_projects(WorkspaceContext *context,
                                      const AbValue *maps) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!maps || maps->kind != AB_VALUE_ARRAY)
    return workspace_error(context, "workspace maps input: expected array");
  if (maps->as.array.count != context->project_specs->as.array.count)
    return workspace_error(
        context, "workspace maps count does not match configured projects");
  context->project_count = maps->as.array.count;
  if (context->project_count) {
    context->projects = (WorkspaceProject *)ab_calloc(
        context->engine, context->project_count, sizeof(*context->projects));
    if (!context->projects)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory decoding workspace projects");
  }
  for (index = 0; status == ARCHBIRD_OK && index < context->project_count;
       index++)
    status = validate_map_project(context, &maps->as.array.items[index],
                                  &context->projects[index]);
  if (status != ARCHBIRD_OK)
    return status;
  if (context->project_count > 1)
    qsort(context->projects, context->project_count, sizeof(*context->projects),
          project_compare);
  for (index = 1; index < context->project_count; index++)
    if (ab_string_equal(&context->projects[index - 1].name->as.text,
                        &context->projects[index].name->as.text))
      return archbird_error_set(
          context->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "workspace projects: duplicate project name '%.*s'",
          (int)context->projects[index].name->as.text.length,
          context->projects[index].name->as.text.data);
  return ARCHBIRD_OK;
}

static ArchbirdStatus reserve_identities(WorkspaceContext *context,
                                         size_t additional) {
  WorkspaceIdentity *resized;
  size_t needed;
  size_t capacity;
  if (additional > SIZE_MAX - context->identity_count)
    return archbird_error_set(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many workspace package identities");
  needed = context->identity_count + additional;
  if (needed <= context->identity_capacity)
    return ARCHBIRD_OK;
  capacity = context->identity_capacity ? context->identity_capacity * 2 : 32;
  while (capacity < needed) {
    if (capacity > SIZE_MAX / 2) {
      capacity = needed;
      break;
    }
    capacity *= 2;
  }
  if (capacity > SIZE_MAX / sizeof(*context->identities))
    return archbird_error_set(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many workspace package identities");
  resized =
      (WorkspaceIdentity *)ab_realloc(context->engine, context->identities,
                                      capacity * sizeof(*context->identities));
  if (!resized)
    return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting package identities");
  context->identities = resized;
  context->identity_capacity = capacity;
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_identity(WorkspaceContext *context,
                                   const AbString *manager,
                                   const AbString *identity,
                                   const AbString *project,
                                   const AbString *package) {
  WorkspaceIdentity *row;
  ArchbirdStatus status = reserve_identities(context, 1);
  if (status != ARCHBIRD_OK)
    return status;
  row = &context->identities[context->identity_count];
  memset(row, 0, sizeof(*row));
  status = copy_string(context, &row->manager, manager);
  if (status == ARCHBIRD_OK)
    status = normalize_name(context, manager, identity, &row->normalized);
  if (status == ARCHBIRD_OK)
    status = copy_string(context, &row->project, project);
  if (status == ARCHBIRD_OK)
    status = copy_string(context, &row->package, package);
  if (status == ARCHBIRD_OK && row->normalized.length)
    context->identity_count++;
  else
    identity_free(context, row);
  return status;
}

static int identity_compare(const void *left_raw, const void *right_raw) {
  const WorkspaceIdentity *left = (const WorkspaceIdentity *)left_raw;
  const WorkspaceIdentity *right = (const WorkspaceIdentity *)right_raw;
  int compared = ab_string_compare(&left->manager, &right->manager);
  if (!compared)
    compared = ab_string_compare(&left->normalized, &right->normalized);
  if (!compared)
    compared = ab_string_compare(&left->project, &right->project);
  if (!compared)
    compared = ab_string_compare(&left->package, &right->package);
  return compared;
}

static int identity_equal(const WorkspaceIdentity *left,
                          const WorkspaceIdentity *right) {
  return !identity_compare(left, right);
}

static ArchbirdStatus add_diagnostic(WorkspaceContext *context,
                                     const char *severity, const char *code,
                                     const char *message,
                                     size_t message_length) {
  WorkspaceDiagnostic *resized;
  WorkspaceDiagnostic *row;
  ArchbirdStatus status;
  if (context->diagnostic_count == context->diagnostic_capacity) {
    size_t capacity =
        context->diagnostic_capacity ? context->diagnostic_capacity * 2 : 8;
    if (capacity > SIZE_MAX / sizeof(*context->diagnostics))
      return archbird_error_set(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too many workspace diagnostics");
    resized = (WorkspaceDiagnostic *)ab_realloc(
        context->engine, context->diagnostics,
        capacity * sizeof(*context->diagnostics));
    if (!resized)
      return archbird_error_set(
          context->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
          "out of memory collecting workspace diagnostics");
    context->diagnostics = resized;
    context->diagnostic_capacity = capacity;
  }
  row = &context->diagnostics[context->diagnostic_count];
  memset(row, 0, sizeof(*row));
  status = copy_literal(context, &row->severity, severity);
  if (status == ARCHBIRD_OK)
    status = copy_literal(context, &row->code, code);
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(context->engine, &row->message, message, message_length);
  if (status == ARCHBIRD_OK)
    status = copy_literal(context, &row->path, "");
  if (status == ARCHBIRD_OK)
    context->diagnostic_count++;
  else
    diagnostic_free(context, row);
  return status;
}

static ArchbirdStatus ambiguity_diagnostic(WorkspaceContext *context,
                                           size_t start, size_t end) {
  const WorkspaceIdentity *first = &context->identities[start];
  AbBuffer message;
  size_t index;
  ArchbirdStatus status;
  ab_buffer_init(&message, context->engine);
  status =
      ab_buffer_append(&message, first->manager.data, first->manager.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&message, " identity '");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&message, first->normalized.data,
                              first->normalized.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&message, "' maps to ");
  for (index = start; status == ARCHBIRD_OK && index < end; index++) {
    if (index > start)
      status = ab_buffer_literal(&message, ", ");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(&message, context->identities[index].project.data,
                           context->identities[index].project.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&message, ":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(&message, context->identities[index].package.data,
                           context->identities[index].package.length);
  }
  if (status == ARCHBIRD_OK)
    status = add_diagnostic(context, "error", "workspace-package-ambiguous",
                            (const char *)message.data, message.length);
  ab_buffer_free(&message);
  return status;
}

static ArchbirdStatus finish_identities(WorkspaceContext *context) {
  size_t read;
  size_t written = 0;
  size_t start;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (context->identity_count > 1)
    qsort(context->identities, context->identity_count,
          sizeof(*context->identities), identity_compare);
  for (read = 0; read < context->identity_count; read++) {
    if (written && identity_equal(&context->identities[written - 1],
                                  &context->identities[read])) {
      identity_free(context, &context->identities[read]);
      continue;
    }
    if (written != read) {
      context->identities[written] = context->identities[read];
      memset(&context->identities[read], 0, sizeof(context->identities[read]));
    }
    written++;
  }
  context->identity_count = written;
  for (start = 0; status == ARCHBIRD_OK && start < context->identity_count;) {
    size_t end = start + 1;
    while (end < context->identity_count &&
           ab_string_equal(&context->identities[start].manager,
                           &context->identities[end].manager) &&
           ab_string_equal(&context->identities[start].normalized,
                           &context->identities[end].normalized))
      end++;
    if (end - start > 1)
      status = ambiguity_diagnostic(context, start, end);
    start = end;
  }
  return status;
}

static ArchbirdStatus build_identities(WorkspaceContext *context) {
  size_t project_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (project_index = 0;
       status == ARCHBIRD_OK && project_index < context->project_count;
       project_index++) {
    const WorkspaceProject *project = &context->projects[project_index];
    size_t package_index;
    for (package_index = 0; status == ARCHBIRD_OK &&
                            package_index < project->packages->as.array.count;
         package_index++) {
      const AbValue *package =
          &project->packages->as.array.items[package_index];
      const AbValue *label;
      const AbValue *kind;
      const AbValue *identity;
      const AbValue *aliases;
      AbString manager = {0};
      size_t alias_index;
      if (package->kind != AB_VALUE_OBJECT)
        return workspace_error(context, "workspace package: expected object");
      label = required_member(context, package, "name", AB_VALUE_STRING);
      kind = required_member(context, package, "kind", AB_VALUE_STRING);
      identity = required_member(context, package, "identity", AB_VALUE_STRING);
      aliases = required_member(context, package, "aliases", AB_VALUE_ARRAY);
      if (!label || !kind || !identity || !aliases)
        return ARCHBIRD_INVALID_SCHEMA;
      status =
          copy_literal(context, &manager, manager_for_package(&kind->as.text));
      if (status == ARCHBIRD_OK && identity->as.text.length)
        status = add_identity(context, &manager, &identity->as.text,
                              &project->name->as.text, &label->as.text);
      for (alias_index = 0;
           status == ARCHBIRD_OK && alias_index < aliases->as.array.count;
           alias_index++) {
        const AbValue *alias = &aliases->as.array.items[alias_index];
        if (alias->kind != AB_VALUE_STRING) {
          status = workspace_error(context,
                                   "workspace package alias: expected string");
          break;
        }
        status = add_identity(context, &manager, &alias->as.text,
                              &project->name->as.text, &label->as.text);
      }
      ab_string_free(context->engine, &manager);
    }
  }
  return status == ARCHBIRD_OK ? finish_identities(context) : status;
}

static void identity_range(const WorkspaceContext *context,
                           const AbString *manager, const AbString *normalized,
                           size_t *out_start, size_t *out_end) {
  size_t start = 0;
  while (start < context->identity_count) {
    const WorkspaceIdentity *row = &context->identities[start];
    int compared = ab_string_compare(&row->manager, manager);
    if (!compared)
      compared = ab_string_compare(&row->normalized, normalized);
    if (compared >= 0)
      break;
    start++;
  }
  *out_start = start;
  while (start < context->identity_count &&
         ab_string_equal(&context->identities[start].manager, manager) &&
         ab_string_equal(&context->identities[start].normalized, normalized))
    start++;
  *out_end = start;
}

static int route_key_equal(const WorkspaceRoute *row, const AbString *kind,
                           const AbString *source_project,
                           const AbString *source_package,
                           const AbString *target_project,
                           const AbString *target_package,
                           const AbString *dependency) {
  return ab_string_equal(&row->kind, kind) &&
         ab_string_equal(&row->source_project, source_project) &&
         ab_string_equal(&row->source_package, source_package) &&
         ab_string_equal(&row->target_project, target_project) &&
         ab_string_equal(&row->target_package, target_package) &&
         ab_string_equal(&row->dependency, dependency);
}

static int external_key_equal(const WorkspaceExternal *row,
                              const AbString *project, const AbString *manager,
                              const AbString *dependency) {
  return ab_string_equal(&row->project, project) &&
         ab_string_equal(&row->manager, manager) &&
         ab_string_equal(&row->dependency, dependency);
}

static ArchbirdStatus
ensure_route(WorkspaceContext *context, const AbString *kind,
             const AbString *source_project, const AbString *source_package,
             const AbString *target_project, const AbString *target_package,
             const AbString *dependency, WorkspaceRoute **out) {
  size_t index;
  WorkspaceRoute *resized;
  WorkspaceRoute *row;
  ArchbirdStatus status;
  for (index = 0; index < context->route_count; index++)
    if (route_key_equal(&context->routes[index], kind, source_project,
                        source_package, target_project, target_package,
                        dependency)) {
      *out = &context->routes[index];
      return ARCHBIRD_OK;
    }
  if (context->route_count == context->route_capacity) {
    size_t capacity =
        context->route_capacity ? context->route_capacity * 2 : 16;
    if (capacity > SIZE_MAX / sizeof(*context->routes))
      return archbird_error_set(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too many workspace routes");
    resized = (WorkspaceRoute *)ab_realloc(context->engine, context->routes,
                                           capacity * sizeof(*context->routes));
    if (!resized)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting workspace routes");
    context->routes = resized;
    context->route_capacity = capacity;
  }
  row = &context->routes[context->route_count];
  memset(row, 0, sizeof(*row));
  status = copy_string(context, &row->kind, kind);
  if (status == ARCHBIRD_OK)
    status = copy_string(context, &row->source_project, source_project);
  if (status == ARCHBIRD_OK)
    status = copy_string(context, &row->source_package, source_package);
  if (status == ARCHBIRD_OK)
    status = copy_string(context, &row->target_project, target_project);
  if (status == ARCHBIRD_OK)
    status = copy_string(context, &row->target_package, target_package);
  if (status == ARCHBIRD_OK)
    status = copy_string(context, &row->dependency, dependency);
  if (status == ARCHBIRD_OK) {
    context->route_count++;
    *out = row;
  } else {
    route_free(context, row);
  }
  return status;
}

static ArchbirdStatus ensure_external(WorkspaceContext *context,
                                      const AbString *project,
                                      const AbString *manager,
                                      const AbString *dependency,
                                      WorkspaceExternal **out) {
  size_t index;
  WorkspaceExternal *resized;
  WorkspaceExternal *row;
  ArchbirdStatus status;
  for (index = 0; index < context->external_count; index++)
    if (external_key_equal(&context->externals[index], project, manager,
                           dependency)) {
      *out = &context->externals[index];
      return ARCHBIRD_OK;
    }
  if (context->external_count == context->external_capacity) {
    size_t capacity =
        context->external_capacity ? context->external_capacity * 2 : 16;
    if (capacity > SIZE_MAX / sizeof(*context->externals))
      return archbird_error_set(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too many workspace externals");
    resized =
        (WorkspaceExternal *)ab_realloc(context->engine, context->externals,
                                        capacity * sizeof(*context->externals));
    if (!resized)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting workspace externals");
    context->externals = resized;
    context->external_capacity = capacity;
  }
  row = &context->externals[context->external_count];
  memset(row, 0, sizeof(*row));
  status = copy_string(context, &row->project, project);
  if (status == ARCHBIRD_OK)
    status = copy_string(context, &row->manager, manager);
  if (status == ARCHBIRD_OK)
    status = copy_string(context, &row->dependency, dependency);
  if (status == ARCHBIRD_OK) {
    context->external_count++;
    *out = row;
  } else {
    external_free(context, row);
  }
  return status;
}

static ArchbirdStatus
record_dependency(WorkspaceContext *context, const AbString *kind,
                  const AbString *source_project,
                  const AbString *source_package, const AbString *manager,
                  const AbString *dependency, const AbStringArray *evidence) {
  AbString normalized = {0};
  size_t start;
  size_t end;
  size_t index;
  ArchbirdStatus status =
      normalize_name(context, manager, dependency, &normalized);
  if (status != ARCHBIRD_OK)
    return status;
  identity_range(context, manager, &normalized, &start, &end);
  if (end - start == 1) {
    WorkspaceRoute *route = NULL;
    status =
        ensure_route(context, kind, source_project, source_package,
                     &context->identities[start].project,
                     &context->identities[start].package, dependency, &route);
    for (index = 0; status == ARCHBIRD_OK && index < evidence->count; index++)
      status = append_unique_string(context, &route->evidence,
                                    &evidence->items[index]);
  } else if (start == end) {
    WorkspaceExternal *external = NULL;
    status = ensure_external(context, source_project, manager, dependency,
                             &external);
    for (index = 0; status == ARCHBIRD_OK && index < evidence->count; index++)
      status = append_unique_string(context, &external->evidence,
                                    &evidence->items[index]);
  }
  ab_string_free(context->engine, &normalized);
  return status;
}

static const AbValue *find_source_file(const WorkspaceProject *project,
                                       const AbString *path) {
  size_t index;
  for (index = 0; index < project->files->as.array.count; index++) {
    const AbValue *file = &project->files->as.array.items[index];
    const AbValue *candidate = ab_value_member(file, "path");
    if (candidate && candidate->kind == AB_VALUE_STRING &&
        ab_string_equal(&candidate->as.text, path))
      return file;
  }
  return NULL;
}

static ArchbirdStatus source_package(WorkspaceContext *context,
                                     const WorkspaceProject *project,
                                     const AbValue *file, AbString *out) {
  const AbValue *layer =
      required_member(context, file, "layer", AB_VALUE_STRING);
  const AbValue *matched = NULL;
  size_t matches = 0;
  size_t index;
  if (!layer)
    return ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; index < project->packages->as.array.count; index++) {
    const AbValue *package = &project->packages->as.array.items[index];
    const AbValue *package_layer = ab_value_member(package, "layer");
    const AbValue *label = ab_value_member(package, "name");
    if (!package_layer || package_layer->kind != AB_VALUE_STRING || !label ||
        label->kind != AB_VALUE_STRING)
      return workspace_error(context,
                             "workspace package identity is incomplete");
    if (ab_string_equal(&package_layer->as.text, &layer->as.text)) {
      matched = label;
      matches++;
    }
  }
  if (matches == 1)
    return copy_string(context, out, &matched->as.text);
  return concat_string(context, out, "layer:", &layer->as.text);
}

static ArchbirdStatus collect_import_dependency(WorkspaceContext *context,
                                                const WorkspaceProject *project,
                                                const AbValue *edge) {
  const AbValue *source =
      required_member(context, edge, "source", AB_VALUE_STRING);
  const AbValue *target =
      required_member(context, edge, "target", AB_VALUE_STRING);
  const AbValue *names =
      required_member(context, edge, "names", AB_VALUE_ARRAY);
  const AbValue *file;
  const AbValue *language;
  const char *manager_literal;
  AbString manager = {0};
  AbString dependency = {0};
  AbString package = {0};
  AbString kind = {0};
  AbStringArray evidence = {0};
  size_t index;
  ArchbirdStatus status;
  if (!source || !target || !names)
    return ARCHBIRD_INVALID_SCHEMA;
  file = find_source_file(project, &source->as.text);
  if (!file)
    return ARCHBIRD_OK;
  language = required_member(context, file, "language", AB_VALUE_STRING);
  if (!language)
    return ARCHBIRD_INVALID_SCHEMA;
  manager_literal = manager_for_language(&language->as.text);
  if (!manager_literal)
    return ARCHBIRD_OK;
  status = copy_literal(context, &manager, manager_literal);
  if (status == ARCHBIRD_OK) {
    const char *data = target->as.text.data;
    size_t length = target->as.text.length;
    static const char prefix[] = "package:";
    if (length >= sizeof(prefix) - 1 &&
        !memcmp(data, prefix, sizeof(prefix) - 1)) {
      data += sizeof(prefix) - 1;
      length -= sizeof(prefix) - 1;
    }
    status = ab_string_copy(context->engine, &dependency, data, length);
  }
  if (status == ARCHBIRD_OK)
    status = source_package(context, project, file, &package);
  if (status == ARCHBIRD_OK)
    status = copy_literal(context, &kind, "import");
  if (status == ARCHBIRD_OK)
    status = append_unique_string(context, &evidence, &source->as.text);
  for (index = 0; status == ARCHBIRD_OK && index < names->as.array.count;
       index++) {
    const AbValue *name = &names->as.array.items[index];
    AbString rendered = {0};
    if (name->kind != AB_VALUE_STRING) {
      status = workspace_error(context, "workspace edge name: expected string");
      break;
    }
    status = concat_string(context, &rendered, "import:", &name->as.text);
    if (status == ARCHBIRD_OK)
      status = append_unique_string(context, &evidence, &rendered);
    ab_string_free(context->engine, &rendered);
  }
  if (status == ARCHBIRD_OK)
    status = record_dependency(context, &kind, &project->name->as.text,
                               &package, &manager, &dependency, &evidence);
  string_array_free(context, &evidence);
  ab_string_free(context->engine, &kind);
  ab_string_free(context->engine, &package);
  ab_string_free(context->engine, &dependency);
  ab_string_free(context->engine, &manager);
  return status;
}

static ArchbirdStatus
collect_declared_dependencies(WorkspaceContext *context,
                              const WorkspaceProject *project,
                              const AbValue *package) {
  const AbValue *label =
      required_member(context, package, "name", AB_VALUE_STRING);
  const AbValue *kind_value =
      required_member(context, package, "kind", AB_VALUE_STRING);
  const AbValue *manifest =
      required_member(context, package, "manifest", AB_VALUE_STRING);
  const AbValue *dependencies =
      required_member(context, package, "dependencies", AB_VALUE_ARRAY);
  AbString manager = {0};
  size_t index;
  ArchbirdStatus status;
  if (!label || !kind_value || !manifest || !dependencies)
    return ARCHBIRD_INVALID_SCHEMA;
  status = copy_literal(context, &manager,
                        manager_for_package(&kind_value->as.text));
  for (index = 0; status == ARCHBIRD_OK && index < dependencies->as.array.count;
       index++) {
    const AbValue *dependency = &dependencies->as.array.items[index];
    const AbValue *name;
    const AbValue *requirement;
    const AbValue *scope;
    AbString route_kind = {0};
    AbString manifest_evidence = {0};
    AbString requirement_evidence = {0};
    AbStringArray evidence = {0};
    if (dependency->kind != AB_VALUE_OBJECT) {
      status =
          workspace_error(context, "workspace dependency: expected object");
      break;
    }
    name = required_member(context, dependency, "name", AB_VALUE_STRING);
    requirement =
        required_member(context, dependency, "requirement", AB_VALUE_STRING);
    scope = required_member(context, dependency, "scope", AB_VALUE_STRING);
    if (!name || !requirement || !scope) {
      status = ARCHBIRD_INVALID_SCHEMA;
      break;
    }
    status = concat_string(context, &route_kind, "declared:", &scope->as.text);
    if (status == ARCHBIRD_OK)
      status = concat_string(context, &manifest_evidence,
                             "manifest:", &manifest->as.text);
    if (status == ARCHBIRD_OK)
      status = append_unique_string(context, &evidence, &manifest_evidence);
    if (status == ARCHBIRD_OK)
      status = concat_string(context, &requirement_evidence,
                             "requirement:", &requirement->as.text);
    if (status == ARCHBIRD_OK)
      status = append_unique_string(context, &evidence, &requirement_evidence);
    if (status == ARCHBIRD_OK)
      status = record_dependency(context, &route_kind, &project->name->as.text,
                                 &label->as.text, &manager, &name->as.text,
                                 &evidence);
    string_array_free(context, &evidence);
    ab_string_free(context->engine, &requirement_evidence);
    ab_string_free(context->engine, &manifest_evidence);
    ab_string_free(context->engine, &route_kind);
  }
  ab_string_free(context->engine, &manager);
  return status;
}

static int route_compare(const void *left_raw, const void *right_raw) {
  const WorkspaceRoute *left = (const WorkspaceRoute *)left_raw;
  const WorkspaceRoute *right = (const WorkspaceRoute *)right_raw;
  int compared = ab_string_compare(&left->kind, &right->kind);
  if (!compared)
    compared = ab_string_compare(&left->source_project, &right->source_project);
  if (!compared)
    compared = ab_string_compare(&left->source_package, &right->source_package);
  if (!compared)
    compared = ab_string_compare(&left->target_project, &right->target_project);
  if (!compared)
    compared = ab_string_compare(&left->target_package, &right->target_package);
  if (!compared)
    compared = ab_string_compare(&left->dependency, &right->dependency);
  return compared;
}

static int external_compare(const void *left_raw, const void *right_raw) {
  const WorkspaceExternal *left = (const WorkspaceExternal *)left_raw;
  const WorkspaceExternal *right = (const WorkspaceExternal *)right_raw;
  int compared = ab_string_compare(&left->project, &right->project);
  if (!compared)
    compared = ab_string_compare(&left->manager, &right->manager);
  if (!compared)
    compared = ab_string_compare(&left->dependency, &right->dependency);
  return compared;
}

static int diagnostic_compare(const void *left_raw, const void *right_raw) {
  const WorkspaceDiagnostic *left = (const WorkspaceDiagnostic *)left_raw;
  const WorkspaceDiagnostic *right = (const WorkspaceDiagnostic *)right_raw;
  int compared = ab_string_compare(&left->severity, &right->severity);
  if (!compared)
    compared = ab_string_compare(&left->code, &right->code);
  if (!compared)
    compared = ab_string_compare(&left->message, &right->message);
  if (!compared)
    compared = ab_string_compare(&left->path, &right->path);
  return compared;
}

static ArchbirdStatus build_dependencies(WorkspaceContext *context) {
  size_t project_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (project_index = 0;
       status == ARCHBIRD_OK && project_index < context->project_count;
       project_index++) {
    const WorkspaceProject *project = &context->projects[project_index];
    size_t edge_index;
    size_t package_index;
    for (edge_index = 0;
         status == ARCHBIRD_OK && edge_index < project->edges->as.array.count;
         edge_index++) {
      const AbValue *edge = &project->edges->as.array.items[edge_index];
      const AbValue *kind;
      if (edge->kind != AB_VALUE_OBJECT)
        return workspace_error(context, "workspace edge: expected object");
      kind = required_member(context, edge, "kind", AB_VALUE_STRING);
      if (!kind)
        return ARCHBIRD_INVALID_SCHEMA;
      if (ab_value_string_is(kind, "external"))
        status = collect_import_dependency(context, project, edge);
    }
    for (package_index = 0; status == ARCHBIRD_OK &&
                            package_index < project->packages->as.array.count;
         package_index++)
      status = collect_declared_dependencies(
          context, project, &project->packages->as.array.items[package_index]);
  }
  for (project_index = 0; project_index < context->route_count; project_index++)
    if (context->routes[project_index].evidence.count > 1)
      qsort(context->routes[project_index].evidence.items,
            context->routes[project_index].evidence.count,
            sizeof(*context->routes[project_index].evidence.items),
            owned_string_compare);
  for (project_index = 0; project_index < context->external_count;
       project_index++)
    if (context->externals[project_index].evidence.count > 1)
      qsort(context->externals[project_index].evidence.items,
            context->externals[project_index].evidence.count,
            sizeof(*context->externals[project_index].evidence.items),
            owned_string_compare);
  if (context->route_count > 1)
    qsort(context->routes, context->route_count, sizeof(*context->routes),
          route_compare);
  if (context->external_count > 1)
    qsort(context->externals, context->external_count,
          sizeof(*context->externals), external_compare);
  if (context->diagnostic_count > 1)
    qsort(context->diagnostics, context->diagnostic_count,
          sizeof(*context->diagnostics), diagnostic_compare);
  return status;
}

static ArchbirdStatus update_digest_row(ArchbirdSha256Context *digest,
                                        const AbString *row, int *first) {
  ArchbirdStatus status = ARCHBIRD_OK;
  static const uint8_t separator = 0;
  if (!*first)
    status = archbird_sha256_update(digest, &separator, 1);
  if (status == ARCHBIRD_OK)
    status =
        archbird_sha256_update(digest, (const uint8_t *)row->data, row->length);
  *first = 0;
  return status;
}

static ArchbirdStatus workspace_input_digest(WorkspaceContext *context) {
  ArchbirdSha256Context digest;
  uint8_t bytes[32];
  AbString config_digest = {context->config_sha256, 64};
  size_t index;
  int first = 1;
  ArchbirdStatus status;
  archbird_sha256_init(&digest);
  status = update_digest_row(&digest, &config_digest, &first);
  for (index = 0; status == ARCHBIRD_OK && index < context->project_count;
       index++) {
    const WorkspaceProject *project = &context->projects[index];
    const AbValue *config = ab_value_member(project->evidence, "config_sha256");
    const AbValue *input = ab_value_member(project->evidence, "input_sha256");
    status = update_digest_row(&digest, &project->name->as.text, &first);
    if (status == ARCHBIRD_OK)
      status = update_digest_row(&digest, &config->as.text, &first);
    if (status == ARCHBIRD_OK)
      status = update_digest_row(&digest, &input->as.text, &first);
  }
  if (status == ARCHBIRD_OK) {
    archbird_sha256_final(&digest, bytes);
    archbird_sha256_hex(bytes, context->input_sha256);
  }
  return status;
}

static ArchbirdStatus render_string(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value->data, value->length);
}

static ArchbirdStatus render_string_array(AbBuffer *buffer,
                                          const AbStringArray *values) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < values->count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, &values->items[index]);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static int value_pointer_compare(const void *left_raw, const void *right_raw) {
  const AbValue *left = *(const AbValue *const *)left_raw;
  const AbValue *right = *(const AbValue *const *)right_raw;
  return value_string_compare(left, right);
}

static ArchbirdStatus render_sorted_value_strings(WorkspaceContext *context,
                                                  AbBuffer *buffer,
                                                  const AbValue *array) {
  const AbValue **items = NULL;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!array || array->kind != AB_VALUE_ARRAY)
    return workspace_error(context, "workspace expected an array of strings");
  if (array->as.array.count) {
    items = (const AbValue **)ab_malloc(context->engine,
                                        array->as.array.count * sizeof(*items));
    if (!items)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory sorting workspace values");
  }
  for (index = 0; index < array->as.array.count; index++) {
    const AbValue *value = &array->as.array.items[index];
    if (value->kind != AB_VALUE_STRING) {
      ab_free(context->engine, items);
      return workspace_error(context, "workspace expected an array of strings");
    }
    items[index] = value;
  }
  if (array->as.array.count > 1)
    qsort(items, array->as.array.count, sizeof(*items), value_pointer_compare);
  status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < array->as.array.count;
       index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, items[index]);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  ab_free(context->engine, items);
  return status;
}

static ArchbirdStatus collect_named_rows(WorkspaceContext *context,
                                         const AbValue *array,
                                         const AbValue ***out_items,
                                         size_t *out_count) {
  const AbValue **items = NULL;
  size_t index;
  if (array->as.array.count) {
    items = (const AbValue **)ab_malloc(context->engine,
                                        array->as.array.count * sizeof(*items));
    if (!items)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory sorting workspace rows");
  }
  for (index = 0; index < array->as.array.count; index++) {
    const AbValue *row = &array->as.array.items[index];
    const AbValue *name;
    if (row->kind != AB_VALUE_OBJECT) {
      ab_free(context->engine, items);
      return workspace_error(context, "workspace row: expected object");
    }
    name = required_member(context, row, "name", AB_VALUE_STRING);
    if (!name) {
      ab_free(context->engine, items);
      return ARCHBIRD_INVALID_SCHEMA;
    }
    items[index] = name;
  }
  if (array->as.array.count > 1)
    qsort(items, array->as.array.count, sizeof(*items), value_pointer_compare);
  *out_items = items;
  *out_count = array->as.array.count;
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_named_rows(WorkspaceContext *context,
                                        AbBuffer *buffer,
                                        const AbValue *array) {
  const AbValue **items = NULL;
  size_t count = 0;
  size_t index;
  ArchbirdStatus status = collect_named_rows(context, array, &items, &count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, items[index]);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  ab_free(context->engine, items);
  return status;
}

static ArchbirdStatus project_counts(WorkspaceContext *context,
                                     const WorkspaceProject *project,
                                     uint64_t *out_files, uint64_t *out_symbols,
                                     uint64_t *out_errors,
                                     uint64_t *out_warnings) {
  size_t index;
  uint64_t symbols = 0;
  uint64_t errors = 0;
  uint64_t warnings = 0;
  for (index = 0; index < project->files->as.array.count; index++) {
    const AbValue *file = &project->files->as.array.items[index];
    const AbValue *rows =
        required_member(context, file, "symbols", AB_VALUE_ARRAY);
    if (!rows)
      return ARCHBIRD_INVALID_SCHEMA;
    if ((uint64_t)rows->as.array.count > UINT64_MAX - symbols)
      return archbird_error_set(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "workspace symbol count overflow");
    symbols += (uint64_t)rows->as.array.count;
  }
  for (index = 0; index < project->diagnostics->as.array.count; index++) {
    const AbValue *row = &project->diagnostics->as.array.items[index];
    const AbValue *severity =
        required_member(context, row, "severity", AB_VALUE_STRING);
    if (!severity)
      return ARCHBIRD_INVALID_SCHEMA;
    if (ab_value_string_is(severity, "error"))
      errors++;
    else if (ab_value_string_is(severity, "warning"))
      warnings++;
  }
  *out_files = (uint64_t)project->files->as.array.count;
  *out_symbols = symbols;
  *out_errors = errors;
  *out_warnings = warnings;
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_project_packages(WorkspaceContext *context,
                                              AbBuffer *buffer,
                                              const WorkspaceProject *project) {
  const AbValue **names = NULL;
  size_t count = 0;
  size_t index;
  ArchbirdStatus status =
      collect_named_rows(context, project->packages, &names, &count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    const AbValue *name = names[index];
    const AbValue *package = NULL;
    const AbValue *kind;
    const AbValue *identity;
    const AbValue *version;
    const AbValue *aliases;
    size_t row_index;
    for (row_index = 0; row_index < project->packages->as.array.count;
         row_index++) {
      const AbValue *candidate = &project->packages->as.array.items[row_index];
      if (ab_value_member(candidate, "name") == name) {
        package = candidate;
        break;
      }
    }
    if (!package) {
      status =
          workspace_error(context, "workspace package sort lost source row");
      break;
    }
    kind = required_member(context, package, "kind", AB_VALUE_STRING);
    identity = required_member(context, package, "identity", AB_VALUE_STRING);
    version = required_member(context, package, "version", AB_VALUE_STRING);
    aliases = required_member(context, package, "aliases", AB_VALUE_ARRAY);
    if (!kind || !identity || !version || !aliases) {
      status = ARCHBIRD_INVALID_SCHEMA;
      break;
    }
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"aliases\":");
    if (status == ARCHBIRD_OK)
      status = render_sorted_value_strings(context, buffer, aliases);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"identity\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, identity);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"kind\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, kind);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"name\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"version\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, version);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  ab_free(context->engine, names);
  return status;
}

static ArchbirdStatus
render_project_artifacts(WorkspaceContext *context, AbBuffer *buffer,
                         const WorkspaceProject *project) {
  const AbValue **names = NULL;
  size_t count = 0;
  size_t index;
  ArchbirdStatus status =
      collect_named_rows(context, project->artifacts, &names, &count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    const AbValue *name = names[index];
    const AbValue *artifact = NULL;
    const AbValue *output;
    size_t row_index;
    for (row_index = 0; row_index < project->artifacts->as.array.count;
         row_index++) {
      const AbValue *candidate = &project->artifacts->as.array.items[row_index];
      if (ab_value_member(candidate, "name") == name) {
        artifact = candidate;
        break;
      }
    }
    if (!artifact) {
      status =
          workspace_error(context, "workspace artifact sort lost source row");
      break;
    }
    output = required_member(context, artifact, "output", AB_VALUE_STRING);
    if (!output) {
      status = ARCHBIRD_INVALID_SCHEMA;
      break;
    }
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"name\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"output\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, output);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  ab_free(context->engine, names);
  return status;
}

static ArchbirdStatus render_project_summary(WorkspaceContext *context,
                                             AbBuffer *buffer,
                                             const WorkspaceProject *project) {
  const AbValue *config_digest =
      ab_value_member(project->evidence, "config_sha256");
  const AbValue *input_digest =
      ab_value_member(project->evidence, "input_sha256");
  uint64_t files = 0;
  uint64_t symbols = 0;
  uint64_t errors = 0;
  uint64_t warnings = 0;
  ArchbirdStatus status =
      project_counts(context, project, &files, &symbols, &errors, &warnings);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "{\"artifacts\":");
  if (status == ARCHBIRD_OK)
    status = render_project_artifacts(context, buffer, project);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"components\":");
  if (status == ARCHBIRD_OK)
    status = render_named_rows(context, buffer, project->components);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"config_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, config_digest);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"description\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, project->description);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"diagnostics\":{\"errors\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(buffer, errors);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"warnings\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(buffer, warnings);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "},\"files\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(buffer, files);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"input_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, input_digest);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"layers\":");
  if (status == ARCHBIRD_OK)
    status = render_named_rows(context, buffer, project->layers);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"name\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, project->name);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"packages\":");
  if (status == ARCHBIRD_OK)
    status = render_project_packages(context, buffer, project);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"symbols\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(buffer, symbols);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

static ArchbirdStatus render_routes(WorkspaceContext *context,
                                    AbBuffer *buffer) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < context->route_count;
       index++) {
    const WorkspaceRoute *route = &context->routes[index];
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"dependency\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, &route->dependency);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"evidence\":");
    if (status == ARCHBIRD_OK)
      status = render_string_array(buffer, &route->evidence);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"kind\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, &route->kind);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"source\":{\"package\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, &route->source_package);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"project\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, &route->source_project);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "},\"target\":{\"package\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, &route->target_package);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"project\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, &route->target_project);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_externals(WorkspaceContext *context,
                                       AbBuffer *buffer) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < context->external_count;
       index++) {
    const WorkspaceExternal *external = &context->externals[index];
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"dependency\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, &external->dependency);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"evidence\":");
    if (status == ARCHBIRD_OK)
      status = render_string_array(buffer, &external->evidence);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"manager\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, &external->manager);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"project\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, &external->project);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_diagnostics(WorkspaceContext *context,
                                         AbBuffer *buffer) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < context->diagnostic_count;
       index++) {
    const WorkspaceDiagnostic *row = &context->diagnostics[index];
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"code\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, &row->code);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"message\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, &row->message);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"path\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, &row->path);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"severity\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, &row->severity);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_workspace_document(WorkspaceContext *context,
                                                AbBuffer *buffer) {
  const AbValue *name = ab_value_member(context->config, "workspace");
  const AbValue *description = ab_value_member(context->config, "description");
  size_t index;
  ArchbirdStatus status;
  status =
      ab_buffer_literal(buffer, "{\"artifact\":\"workspace\",\"description\":");
  if (status == ARCHBIRD_OK) {
    if (description)
      status = ab_value_render(buffer, description);
    else
      status = ab_buffer_literal(buffer, "\"\"");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"diagnostics\":");
  if (status == ARCHBIRD_OK)
    status = render_diagnostics(context, buffer);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        buffer,
        ",\"evidence\":{\"absolute_paths_included\":false,\"config_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, context->config_sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"input_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, context->input_sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "},\"externals\":");
  if (status == ARCHBIRD_OK)
    status = render_externals(context, buffer);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"projects\":[");
  for (index = 0; status == ARCHBIRD_OK && index < context->project_count;
       index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status =
          render_project_summary(context, buffer, &context->projects[index]);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "],\"routes\":");
  if (status == ARCHBIRD_OK)
    status = render_routes(context, buffer);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        buffer, ",\"schema_version\":6,\"tool\":{\"implementation_sha256\":"
                "\"" ARCHBIRD_IMPLEMENTATION_SHA256
                "\",\"name\":\"archbird\",\"version\":\"" ARCHBIRD_VERSION
                "\"},\"workspace\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, name);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

static ArchbirdStatus render_workspace_plan(WorkspaceContext *context,
                                            AbBuffer *buffer) {
  const AbValue *name = ab_value_member(context->config, "workspace");
  const AbValue *description = ab_value_member(context->config, "description");
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "{\"config_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, context->config_sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"description\":");
  if (status == ARCHBIRD_OK) {
    if (description)
      status = ab_value_render(buffer, description);
    else
      status = ab_buffer_literal(buffer, "\"\"");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"projects\":[");
  for (index = 0;
       status == ARCHBIRD_OK && index < context->project_specs->as.array.count;
       index++) {
    const AbValue *row = &context->project_specs->as.array.items[index];
    const AbValue *config = ab_value_member(row, "config");
    const AbValue *root = ab_value_member(row, "root");
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"config\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, config);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"root\":");
    if (status == ARCHBIRD_OK) {
      if (root)
        status = ab_value_render(buffer, root);
      else
        status = ab_buffer_literal(buffer, "null");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "],\"workspace\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, name);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

ArchbirdStatus
archbird_workspace_plan(ArchbirdEngine *engine, const uint8_t *workspace_json,
                        size_t workspace_length, uint32_t json_flags,
                        ArchbirdWriteFn write_fn, void *user_data) {
  AbValue config = {0};
  WorkspaceContext context = {0};
  AbBuffer buffer;
  ArchbirdStatus status;
  if (!engine || (!workspace_json && workspace_length) || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  context.engine = engine;
  ab_buffer_init(&buffer, engine);
  status =
      ab_json_value_decode(engine, workspace_json, workspace_length, &config);
  if (status == ARCHBIRD_OK)
    status = validate_workspace_config(&context, &config);
  if (status == ARCHBIRD_OK)
    status = render_workspace_plan(&context, &buffer);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&buffer);
  workspace_context_free(&context);
  ab_value_free(engine, &config);
  return status;
}

ArchbirdStatus archbird_workspace_analyze(
    ArchbirdEngine *engine, const uint8_t *workspace_json,
    size_t workspace_length, const uint8_t *maps_json, size_t maps_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data) {
  AbValue config = {0};
  AbValue maps = {0};
  WorkspaceContext context = {0};
  AbBuffer buffer;
  ArchbirdStatus status;
  if (!engine || (!workspace_json && workspace_length) ||
      (!maps_json && maps_length) || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_build_identity_validate(engine);
  if (status != ARCHBIRD_OK)
    return status;
  context.engine = engine;
  ab_buffer_init(&buffer, engine);
  status =
      ab_json_value_decode(engine, workspace_json, workspace_length, &config);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, maps_json, maps_length, &maps);
  if (status == ARCHBIRD_OK)
    status = validate_workspace_config(&context, &config);
  if (status == ARCHBIRD_OK)
    status = decode_projects(&context, &maps);
  if (status == ARCHBIRD_OK)
    status = build_identities(&context);
  if (status == ARCHBIRD_OK)
    status = build_dependencies(&context);
  if (status == ARCHBIRD_OK)
    status = workspace_input_digest(&context);
  if (status == ARCHBIRD_OK)
    status = render_workspace_document(&context, &buffer);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&buffer);
  workspace_context_free(&context);
  ab_value_free(engine, &maps);
  ab_value_free(engine, &config);
  return status;
}
