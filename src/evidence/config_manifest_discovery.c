#include "evidence/config_manifest_discovery.h"

#include "base/archbird_internal.h"
#include "base/path_match.h"

#include <string.h>

static int string_has_suffix(const AbString *value, const char *suffix) {
  size_t length = strlen(suffix);
  return value->length >= length &&
         !memcmp(value->data + value->length - length, suffix, length);
}

static int path_leaf_is(const AbString *path, const char *leaf,
                        AbString *out_parent) {
  size_t leaf_length = strlen(leaf);
  size_t start = path->length;
  while (start && path->data[start - 1] != '/')
    start--;
  if (path->length - start != leaf_length ||
      memcmp(path->data + start, leaf, leaf_length))
    return 0;
  out_parent->data = path->data;
  out_parent->length = start ? start - 1 : 0;
  return 1;
}

static int workspace_pattern_view(const AbString *raw, AbString *out) {
  size_t segment = 0;
  size_t index;
  *out = *raw;
  if (out->length >= 2 && out->data[0] == '.' && out->data[1] == '/') {
    out->data += 2;
    out->length -= 2;
  }
  while (out->length && out->data[out->length - 1] == '/')
    out->length--;
  if (!out->length || out->data[0] == '/' || out->data[0] == '!')
    return 0;
  for (index = 0; index <= out->length; index++) {
    if (index < out->length && out->data[index] != '/') {
      if (out->data[index] == '\\' || out->data[index] == '\0')
        return 0;
      continue;
    }
    if (index == segment ||
        (index - segment == 1 && out->data[segment] == '.') ||
        (index - segment == 2 && out->data[segment] == '.' &&
         out->data[segment + 1] == '.'))
      return 0;
    segment = index + 1;
  }
  return 1;
}

static size_t unsupported_workspace_patterns(const AbString *patterns,
                                             size_t pattern_count) {
  size_t index;
  size_t unsupported = 0;
  for (index = 0; index < pattern_count; index++) {
    AbString normalized;
    unsupported += !workspace_pattern_view(&patterns[index], &normalized);
  }
  return unsupported;
}

static int path_ignored(AbIgnoreSet *ignores, int apply_ignores,
                        const AbString *path) {
  return apply_ignores && ab_ignore_set_matches(ignores, path, 0);
}

static size_t matching_workspace_pattern(const AbString *directory,
                                         const AbString *patterns,
                                         size_t pattern_count,
                                         const AbString **out_pattern) {
  size_t index;
  size_t matches = 0;
  *out_pattern = NULL;
  for (index = 0; index < pattern_count; index++) {
    AbString pattern;
    if (!workspace_pattern_view(&patterns[index], &pattern) ||
        !ab_map_collection_match(directory, &pattern))
      continue;
    if (!*out_pattern)
      *out_pattern = &patterns[index];
    matches++;
  }
  return matches;
}

static int python_source_path(const AbString *path) {
  return string_has_suffix(path, ".py") || string_has_suffix(path, ".pyi") ||
         string_has_suffix(path, ".pyw");
}

static int path_before_children(const AbString *path,
                                const AbString *directory) {
  size_t shared =
      path->length < directory->length ? path->length : directory->length;
  int compared = shared ? memcmp(path->data, directory->data, shared) : 0;
  if (compared)
    return compared < 0;
  if (path->length <= directory->length)
    return 1;
  return (unsigned char)path->data[directory->length] < (unsigned char)'/';
}

static size_t first_child(const AbManifestInventoryFile *files,
                          size_t file_count, const AbString *directory) {
  size_t low = 0;
  size_t high = file_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (path_before_children(files[middle].path, directory))
      low = middle + 1;
    else
      high = middle;
  }
  return low;
}

static const AbString *python_source_below(const AbManifestInventoryFile *files,
                                           size_t file_count,
                                           AbIgnoreSet *ignores,
                                           int apply_ignores,
                                           const AbString *directory) {
  size_t index = first_child(files, file_count, directory);
  for (; index < file_count; index++) {
    const AbString *path = files[index].path;
    if (path->length <= directory->length ||
        path->data[directory->length] != '/' ||
        memcmp(path->data, directory->data, directory->length))
      break;
    if (python_source_path(path) && !path_ignored(ignores, apply_ignores, path))
      return path;
  }
  return NULL;
}

static size_t candidate_limit(AbManifestCandidateKind kind) {
  return kind == AB_MANIFEST_CANDIDATE_NPM ? AB_MANIFEST_DISCOVERY_NPM_LIMIT
                                           : AB_MANIFEST_DISCOVERY_PYTHON_LIMIT;
}

static ArchbirdStatus ensure_capacity(ArchbirdEngine *engine, void **items,
                                      size_t *capacity, size_t count,
                                      size_t item_size) {
  void *resized;
  size_t next;
  if (count < *capacity)
    return ARCHBIRD_OK;
  next = *capacity ? *capacity * 2 : 8;
  if (next < *capacity || next > SIZE_MAX / item_size)
    return ARCHBIRD_LIMIT_EXCEEDED;
  resized = ab_realloc(engine, *items, next * item_size);
  if (!resized)
    return ARCHBIRD_OUT_OF_MEMORY;
  *items = resized;
  *capacity = next;
  return ARCHBIRD_OK;
}

static ArchbirdStatus append_candidate(
    AbManifestDiscovery *discovery, const AbManifestInventoryFile *file,
    AbManifestCandidateKind kind, const char *source, const AbString *pattern,
    const AbString *witness, size_t match_count,
    AbManifestDiagnosticFn diagnostic, void *diagnostic_data) {
  AbManifestCandidate *candidate;
  discovery->matches[kind]++;
  if (file->bytes > AB_MANIFEST_DISCOVERY_MAX_BYTES) {
    if (!discovery->oversized[kind]) {
      ArchbirdStatus status = diagnostic(
          diagnostic_data, "discovery-manifest-oversized", "warning",
          file->path, "bytes", file->bytes, AB_MANIFEST_DISCOVERY_MAX_BYTES);
      if (status != ARCHBIRD_OK)
        return status;
    }
    discovery->oversized[kind]++;
    return ARCHBIRD_OK;
  }
  if (discovery->requested[kind] >= candidate_limit(kind)) {
    if (!discovery->first_omitted[kind])
      discovery->first_omitted[kind] = file->path;
    return ARCHBIRD_OK;
  }
  {
    ArchbirdStatus status = ensure_capacity(
        discovery->engine, (void **)&discovery->candidates,
        &discovery->candidate_capacity, discovery->candidate_count,
        sizeof(*discovery->candidates));
    if (status != ARCHBIRD_OK)
      return status;
  }
  candidate = &discovery->candidates[discovery->candidate_count++];
  memset(candidate, 0, sizeof(*candidate));
  candidate->path = file->path;
  candidate->pattern = pattern;
  candidate->witness = witness;
  candidate->source = source;
  candidate->bytes = file->bytes;
  candidate->match_count = match_count;
  candidate->kind = kind;
  discovery->requested[kind]++;
  if (match_count > 1)
    return diagnostic(diagnostic_data, "discovery-manifest-pattern-overlap",
                      "warning", file->path, "matches", match_count, 1);
  return ARCHBIRD_OK;
}

static ArchbirdStatus report_truncation(const AbManifestDiscovery *discovery,
                                        AbManifestDiagnosticFn diagnostic,
                                        void *diagnostic_data) {
  size_t kind;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (kind = 0; status == ARCHBIRD_OK && kind < 2; kind++) {
    size_t eligible = discovery->matches[kind] - discovery->oversized[kind];
    size_t limit = candidate_limit((AbManifestCandidateKind)kind);
    if (eligible > limit && discovery->first_omitted[kind])
      status = diagnostic(
          diagnostic_data, "discovery-manifest-candidates-truncated", "warning",
          discovery->first_omitted[kind], "candidates", eligible, limit);
  }
  return status;
}

void ab_manifest_discovery_init(AbManifestDiscovery *discovery,
                                ArchbirdEngine *engine) {
  if (!discovery)
    return;
  memset(discovery, 0, sizeof(*discovery));
  discovery->engine = engine;
}

ArchbirdStatus ab_manifest_discovery_select(
    AbManifestDiscovery *discovery, const AbManifestInventoryFile *files,
    size_t file_count, AbIgnoreSet *ignores, int apply_ignores,
    const AbNpmDiscoveryMetadata *npm, const AbPyprojectMetadata *pyproject,
    AbManifestDiagnosticFn diagnostic, void *diagnostic_data) {
  size_t index;
  size_t unsupported;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!discovery || !discovery->engine || (!files && file_count) || !ignores ||
      !npm || !pyproject || !diagnostic || discovery->candidate_count)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (npm->workspaces_present && !npm->workspaces_supported) {
    AbString root_path = {(char *)"package.json", 12};
    status =
        diagnostic(diagnostic_data, "discovery-workspace-shape-unsupported",
                   "warning", &root_path, NULL, 0, 0);
  }
  unsupported =
      unsupported_workspace_patterns(npm->workspaces, npm->workspace_count);
  if (status == ARCHBIRD_OK && unsupported) {
    AbString root_path = {(char *)"package.json", 12};
    status =
        diagnostic(diagnostic_data, "discovery-workspace-pattern-unsupported",
                   "warning", &root_path, "patterns", unsupported, 0);
  }
  if (status == ARCHBIRD_OK && ((pyproject->workspace_members_present &&
                                 !pyproject->workspace_members_supported) ||
                                (pyproject->workspace_excludes_present &&
                                 !pyproject->workspace_excludes_supported))) {
    AbString root_path = {(char *)"pyproject.toml", 14};
    status =
        diagnostic(diagnostic_data, "discovery-workspace-shape-unsupported",
                   "warning", &root_path, NULL, 0, 0);
  }
  unsupported = unsupported_workspace_patterns(
      pyproject->workspace_members, pyproject->workspace_member_count);
  unsupported += unsupported_workspace_patterns(
      pyproject->workspace_excludes, pyproject->workspace_exclude_count);
  if (status == ARCHBIRD_OK && unsupported) {
    AbString root_path = {(char *)"pyproject.toml", 14};
    status =
        diagnostic(diagnostic_data, "discovery-workspace-pattern-unsupported",
                   "warning", &root_path, "patterns", unsupported, 0);
  }
  for (index = 0; status == ARCHBIRD_OK && index < file_count; index++) {
    const AbManifestInventoryFile *file = &files[index];
    AbString parent;
    const AbString *pattern = NULL;
    const AbString *exclude_pattern = NULL;
    size_t matches;
    if (path_ignored(ignores, apply_ignores, file->path))
      continue;
    if (path_leaf_is(file->path, "package.json", &parent) && parent.length) {
      matches = matching_workspace_pattern(&parent, npm->workspaces,
                                           npm->workspace_count, &pattern);
      if (matches)
        status = append_candidate(discovery, file, AB_MANIFEST_CANDIDATE_NPM,
                                  "npm-workspace", pattern, NULL, matches,
                                  diagnostic, diagnostic_data);
      continue;
    }
    if (!path_leaf_is(file->path, "pyproject.toml", &parent) || !parent.length)
      continue;
    matches =
        matching_workspace_pattern(&parent, pyproject->workspace_members,
                                   pyproject->workspace_member_count, &pattern);
    if (matches && matching_workspace_pattern(
                       &parent, pyproject->workspace_excludes,
                       pyproject->workspace_exclude_count, &exclude_pattern))
      continue;
    if (matches) {
      const AbString *witness = NULL;
      if (file->bytes <= AB_MANIFEST_DISCOVERY_MAX_BYTES &&
          discovery->requested[AB_MANIFEST_CANDIDATE_PYTHON] <
              AB_MANIFEST_DISCOVERY_PYTHON_LIMIT)
        witness = python_source_below(files, file_count, ignores, apply_ignores,
                                      &parent);
      status = append_candidate(discovery, file, AB_MANIFEST_CANDIDATE_PYTHON,
                                "python-workspace", pattern, witness, matches,
                                diagnostic, diagnostic_data);
    } else if (!memchr(parent.data, '/', parent.length)) {
      const AbString *witness = python_source_below(files, file_count, ignores,
                                                    apply_ignores, &parent);
      if (witness)
        status = append_candidate(discovery, file, AB_MANIFEST_CANDIDATE_PYTHON,
                                  "python-top-level", NULL, witness, 1,
                                  diagnostic, diagnostic_data);
    }
  }
  if (status == ARCHBIRD_OK)
    status = report_truncation(discovery, diagnostic, diagnostic_data);
  return status;
}

AbManifestCandidate *ab_manifest_discovery_find(AbManifestDiscovery *discovery,
                                                const AbString *path) {
  size_t index;
  if (!discovery || !path)
    return NULL;
  for (index = 0; index < discovery->candidate_count; index++)
    if (ab_string_equal(discovery->candidates[index].path, path))
      return &discovery->candidates[index];
  return NULL;
}

ArchbirdStatus ab_manifest_discovery_supply(AbManifestDiscovery *discovery,
                                            AbManifestCandidate *candidate,
                                            const uint8_t *bytes, size_t length,
                                            AbManifestDiagnosticFn diagnostic,
                                            void *diagnostic_data) {
  ArchbirdStatus status;
  if (!discovery || !discovery->engine || !candidate || (!bytes && length) ||
      !diagnostic || candidate->supplied)
    return ARCHBIRD_INVALID_ARGUMENT;
  candidate->supplied = 1;
  if (candidate->kind == AB_MANIFEST_CANDIDATE_NPM) {
    AbDiscoveredNpmPackage *package;
    status = ensure_capacity(
        discovery->engine, (void **)&discovery->npm_packages,
        &discovery->npm_package_capacity, discovery->npm_package_count,
        sizeof(*discovery->npm_packages));
    if (status != ARCHBIRD_OK)
      return status;
    package = &discovery->npm_packages[discovery->npm_package_count];
    memset(package, 0, sizeof(*package));
    package->candidate = candidate;
    status = ab_npm_discovery_metadata(discovery->engine, bytes, length,
                                       &package->metadata);
    if (status == ARCHBIRD_INVALID_JSON || status == ARCHBIRD_DUPLICATE_KEY ||
        status == ARCHBIRD_INVALID_SCHEMA) {
      archbird_error_clear(discovery->engine);
      memset(&package->metadata, 0, sizeof(package->metadata));
      return diagnostic(diagnostic_data, "discovery-manifest-invalid",
                        "warning", candidate->path, NULL, 0, 0);
    }
    if (status == ARCHBIRD_OK)
      discovery->npm_package_count++;
    return status;
  }
  {
    AbDiscoveredPythonPackage *package;
    status = ensure_capacity(
        discovery->engine, (void **)&discovery->python_packages,
        &discovery->python_package_capacity, discovery->python_package_count,
        sizeof(*discovery->python_packages));
    if (status != ARCHBIRD_OK)
      return status;
    package = &discovery->python_packages[discovery->python_package_count];
    memset(package, 0, sizeof(*package));
    package->candidate = candidate;
    status = ab_pyproject_metadata(discovery->engine, bytes, length,
                                   &package->metadata);
    if (status == ARCHBIRD_INVALID_SCHEMA) {
      archbird_error_clear(discovery->engine);
      memset(&package->metadata, 0, sizeof(package->metadata));
      return diagnostic(diagnostic_data, "discovery-manifest-invalid",
                        "warning", candidate->path, NULL, 0, 0);
    }
    if (status == ARCHBIRD_OK)
      discovery->python_package_count++;
    return status;
  }
}

ArchbirdStatus
ab_manifest_discovery_report_missing(const AbManifestDiscovery *discovery,
                                     AbManifestDiagnosticFn diagnostic,
                                     void *diagnostic_data) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!discovery || !diagnostic)
    return ARCHBIRD_INVALID_ARGUMENT;
  for (index = 0; status == ARCHBIRD_OK && index < discovery->candidate_count;
       index++) {
    const AbManifestCandidate *candidate = &discovery->candidates[index];
    if (!candidate->supplied)
      status = diagnostic(diagnostic_data, "discovery-manifest-input-missing",
                          "warning", candidate->path, NULL, 0, 0);
  }
  return status;
}

void ab_manifest_discovery_free(AbManifestDiscovery *discovery) {
  size_t index;
  if (!discovery || !discovery->engine)
    return;
  for (index = 0; index < discovery->npm_package_count; index++)
    ab_npm_discovery_metadata_free(discovery->engine,
                                   &discovery->npm_packages[index].metadata);
  ab_free(discovery->engine, discovery->npm_packages);
  for (index = 0; index < discovery->python_package_count; index++) {
    ab_pyproject_metadata_free(discovery->engine,
                               &discovery->python_packages[index].metadata);
    ab_string_free(discovery->engine,
                   &discovery->python_packages[index].import_root);
  }
  ab_free(discovery->engine, discovery->python_packages);
  ab_free(discovery->engine, discovery->candidates);
  memset(discovery, 0, sizeof(*discovery));
}
