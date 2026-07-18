#include "sha256.h"
#include <archbird/archbird.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fuzz_discard(void *user_data, const uint8_t *bytes, size_t length) {
  (void)user_data;
  (void)bytes;
  (void)length;
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static const char config[] =
      "{\"schema_version\":1,\"project\":\"fuzz-scip\",\"layers\":[{"
      "\"name\":\"core\",\"role\":\"core\",\"language\":\"c\","
      "\"globs\":[\"src/**\"],\"required\":false}],\"indexes\":[{"
      "\"name\":\"fuzz\",\"format\":\"scip\",\"path\":\"input.scip\","
      "\"required\":false}]}";
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;
  uint8_t digest[32];
  char sha[65];
  char manifest[1024];
  int manifest_length;
  ArchbirdStatus status;
  if (archbird_sha256(data, size, digest) != ARCHBIRD_OK)
    return 0;
  archbird_sha256_hex(digest, sha);
  manifest_length = snprintf(
      manifest, sizeof(manifest),
      "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":%zu,"
      "\"path\":\"input.scip\",\"roles\":[\"index\"],\"sha256\":\"%s\"}],"
      "\"producer\":{\"implementation_sha256\":\"111111111111111111111111"
      "1111111111111111111111111111111111111111\",\"name\":\"fuzzer\","
      "\"version\":\"1\"},\"project\":\"fuzz-scip\",\"schema_version\":1}",
      size, sha);
  if (manifest_length < 0 || (size_t)manifest_length >= sizeof(manifest))
    return 0;
  archbird_engine_options_init(&options);
  if (archbird_engine_create(&options, &engine) != ARCHBIRD_OK)
    return 0;
  status = archbird_project_create(engine, (const uint8_t *)manifest,
                                   (size_t)manifest_length, &project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_add_source(engine, project, "input.scip", 10,
                                         data, size);
  if (status == ARCHBIRD_OK)
    status = archbird_project_finalize_sources(engine, project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_set_config(
        engine, project, (const uint8_t *)config, sizeof(config) - 1);
  if (status == ARCHBIRD_OK)
    status = archbird_project_scan_builtin_provider(
        engine, project, "semantic:scip", 13, ARCHBIRD_PROVIDER_PRIMARY);
  if (status == ARCHBIRD_OK)
    status = archbird_project_finalize_providers(engine, project);
  if (status == ARCHBIRD_OK) {
    size_t index;
    for (index = 0; index < archbird_project_provider_count(project); index++)
      (void)archbird_project_render_provider_facts(engine, project, index, 0,
                                                   fuzz_discard, NULL);
    (void)archbird_project_render_map(engine, project, 0, fuzz_discard, NULL);
  }
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
  return 0;
}
