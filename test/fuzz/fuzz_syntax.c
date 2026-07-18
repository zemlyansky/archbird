#include "sha256.h"
#include <archbird/archbird.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct SyntaxCase {
  const char *provider;
  const char *language;
  const char *layer;
  const char *path;
} SyntaxCase;

static const SyntaxCase CASES[] = {
#ifdef ARCHBIRD_FUZZ_TREE_SITTER_C
    {"syntax:tree-sitter:c", "c", "core", "input.c"},
#endif
#if defined(ARCHBIRD_FUZZ_TREE_SITTER_C) &&                                    \
    defined(ARCHBIRD_FUZZ_TREE_SITTER_CPP)
    {"syntax:tree-sitter:c", "c", "auto-c", "input.h"},
#endif
#ifdef ARCHBIRD_FUZZ_TREE_SITTER_CPP
    {"syntax:tree-sitter:cpp", "cpp", "core", "input.cpp"},
#endif
#ifdef ARCHBIRD_FUZZ_TREE_SITTER_PYTHON
    {"syntax:tree-sitter:python", "python", "core", "input.py"},
#endif
#ifdef ARCHBIRD_FUZZ_TREE_SITTER_JAVASCRIPT
    {"syntax:tree-sitter:javascript", "javascript", "core", "input.js"},
#endif
#ifdef ARCHBIRD_FUZZ_TREE_SITTER_TYPESCRIPT
    {"syntax:tree-sitter:typescript", "typescript", "core", "input.ts"},
#endif
#ifdef ARCHBIRD_FUZZ_TREE_SITTER_TSX
    {"syntax:tree-sitter:tsx", "typescript", "core", "input.tsx"},
#endif
#ifdef ARCHBIRD_FUZZ_TREE_SITTER_R
    {"syntax:tree-sitter:r", "r", "core", "input.R"},
#endif
};

static int fuzz_discard(void *user_data, const uint8_t *bytes, size_t length) {
  (void)user_data;
  (void)bytes;
  (void)length;
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  const SyntaxCase *selected;
  const uint8_t *source;
  size_t source_size;
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;
  uint8_t digest[32];
  char sha[65];
  char manifest[1024];
  int manifest_length;
  ArchbirdStatus status;
  if (!size)
    return 0;
  selected = &CASES[data[0] % (sizeof(CASES) / sizeof(CASES[0]))];
  source = data + 1;
  source_size = size - 1;
  if (archbird_sha256(source, source_size, digest) != ARCHBIRD_OK)
    return 0;
  archbird_sha256_hex(digest, sha);
  manifest_length = snprintf(
      manifest, sizeof(manifest),
      "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":%zu,"
      "\"language\":\"%s\",\"layer\":\"%s\",\"path\":\"%s\","
      "\"roles\":[\"source\"],\"sha256\":\"%s\"}],\"producer\":{"
      "\"implementation_sha256\":\"111111111111111111111111111111111111"
      "1111111111111111111111111111\",\"name\":\"syntax-fuzzer\","
      "\"version\":\"1\"},\"project\":\"fuzz-syntax\",\"schema_version\":1}",
      source_size, selected->language, selected->layer, selected->path, sha);
  if (manifest_length < 0 || (size_t)manifest_length >= sizeof(manifest))
    return 0;
  archbird_engine_options_init(&options);
  if (archbird_engine_create(&options, &engine) != ARCHBIRD_OK)
    return 0;
  status = archbird_project_create(engine, (const uint8_t *)manifest,
                                   (size_t)manifest_length, &project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_add_source(engine, project, selected->path,
                                         strlen(selected->path), source,
                                         source_size);
  if (status == ARCHBIRD_OK)
    status = archbird_project_finalize_sources(engine, project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_scan_builtin_provider(
        engine, project, selected->provider, strlen(selected->provider),
        ARCHBIRD_PROVIDER_PRIMARY);
  if (status == ARCHBIRD_OK)
    status = archbird_project_finalize_providers(engine, project);
  if (status == ARCHBIRD_OK) {
    (void)archbird_project_render_provider_facts(engine, project, 0, 0,
                                                 fuzz_discard, NULL);
    (void)archbird_project_render_file_facts(engine, project, 0, fuzz_discard,
                                             NULL);
    (void)archbird_project_render_map(engine, project, 0, fuzz_discard, NULL);
  }
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
  return 0;
}
