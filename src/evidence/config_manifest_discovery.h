#ifndef ARCHBIRD_CONFIG_MANIFEST_DISCOVERY_H
#define ARCHBIRD_CONFIG_MANIFEST_DISCOVERY_H

#include "evidence/gitignore.h"
#include "evidence/manifests/npm_workspace_manifest.h"
#include "evidence/manifests/pyproject_manifest.h"

typedef struct AbManifestInventoryFile {
  const AbString *path;
  size_t bytes;
} AbManifestInventoryFile;

typedef enum AbManifestCandidateKind {
  AB_MANIFEST_CANDIDATE_NPM,
  AB_MANIFEST_CANDIDATE_PYTHON,
  AB_MANIFEST_CANDIDATE_SETUP_CFG,
  AB_MANIFEST_CANDIDATE_CMAKE_PROJECT,
  AB_MANIFEST_CANDIDATE_KIND_COUNT
} AbManifestCandidateKind;

typedef struct AbManifestCandidate {
  const AbString *path;
  const AbString *pattern;
  const AbString *witness;
  const char *source;
  size_t bytes;
  size_t match_count;
  AbManifestCandidateKind kind;
  int supplied;
} AbManifestCandidate;

typedef struct AbDiscoveredNpmPackage {
  const AbManifestCandidate *candidate;
  AbNpmDiscoveryMetadata metadata;
} AbDiscoveredNpmPackage;

typedef struct AbDiscoveredPythonPackage {
  const AbManifestCandidate *candidate;
  AbPyprojectMetadata metadata;
  AbString import_root;
} AbDiscoveredPythonPackage;

typedef ArchbirdStatus (*AbManifestDiagnosticFn)(
    void *user_data, const char *code, const char *severity,
    const AbString *path, const char *metric, size_t observed, size_t limit);

typedef struct AbManifestDiscovery {
  ArchbirdEngine *engine;
  AbManifestCandidate *candidates;
  size_t candidate_count;
  size_t candidate_capacity;
  size_t matches[AB_MANIFEST_CANDIDATE_KIND_COUNT];
  size_t oversized[AB_MANIFEST_CANDIDATE_KIND_COUNT];
  size_t requested[AB_MANIFEST_CANDIDATE_KIND_COUNT];
  const AbString *first_omitted[AB_MANIFEST_CANDIDATE_KIND_COUNT];
  AbDiscoveredNpmPackage *npm_packages;
  size_t npm_package_count;
  size_t npm_package_capacity;
  AbDiscoveredPythonPackage *python_packages;
  size_t python_package_count;
  size_t python_package_capacity;
} AbManifestDiscovery;

enum {
  AB_MANIFEST_DISCOVERY_MAX_BYTES = 256 * 1024,
  AB_MANIFEST_DISCOVERY_NPM_LIMIT = 128,
  AB_MANIFEST_DISCOVERY_PYTHON_LIMIT = 32,
  AB_MANIFEST_DISCOVERY_ROOT_LIMIT = 1
};

void ab_manifest_discovery_init(AbManifestDiscovery *discovery,
                                ArchbirdEngine *engine);

/* Select exact manifest reads from the sorted inventory. Workspace patterns,
 * ignore rules, source witnesses, byte caps, and count caps are native policy;
 * this function performs no I/O. */
ArchbirdStatus ab_manifest_discovery_select(
    AbManifestDiscovery *discovery, const AbManifestInventoryFile *files,
    size_t file_count, AbIgnoreSet *ignores, int apply_ignores,
    const AbNpmDiscoveryMetadata *npm, const AbPyprojectMetadata *pyproject,
    AbManifestDiagnosticFn diagnostic, void *diagnostic_data);

AbManifestCandidate *ab_manifest_discovery_find(AbManifestDiscovery *discovery,
                                                const AbString *path);

/* Decode one requested document. Invalid package metadata stays a fulfilled
 * request with a warning; allocation and internal failures remain fatal. */
ArchbirdStatus ab_manifest_discovery_supply(AbManifestDiscovery *discovery,
                                            AbManifestCandidate *candidate,
                                            const uint8_t *bytes, size_t length,
                                            AbManifestDiagnosticFn diagnostic,
                                            void *diagnostic_data);

ArchbirdStatus
ab_manifest_discovery_report_missing(const AbManifestDiscovery *discovery,
                                     AbManifestDiagnosticFn diagnostic,
                                     void *diagnostic_data);

void ab_manifest_discovery_free(AbManifestDiscovery *discovery);

#endif
