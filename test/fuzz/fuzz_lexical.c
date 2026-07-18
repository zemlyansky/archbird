#include "fuzz_common.h"

#include "sha256.h"

#include <stdio.h>
#include <string.h>

#ifndef ARCHBIRD_FUZZ_LANGUAGE
#error "ARCHBIRD_FUZZ_LANGUAGE must name the lexical language"
#endif
#ifndef ARCHBIRD_FUZZ_PATH
#error "ARCHBIRD_FUZZ_PATH must name the lexical source path"
#endif
#ifndef ARCHBIRD_FUZZ_PROVIDER
#error "ARCHBIRD_FUZZ_PROVIDER must name the lexical provider"
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static const char implementation[] =
      "1111111111111111111111111111111111111111111111111111111111111111";
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;
  uint8_t digest[32];
  char digest_hex[65];
  char config[1024];
  char manifest[1024];
  int config_length;
  int manifest_length;
  ArchbirdStatus status;
  size_t provider_index;
  if (archbird_sha256(data, size, digest) != ARCHBIRD_OK)
    return 0;
  archbird_sha256_hex(digest, digest_hex);
  manifest_length = snprintf(
      manifest, sizeof(manifest),
      "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":%zu,"
      "\"language\":\"%s\",\"layer\":\"core\",\"path\":\"%s\","
      "\"roles\":[\"source\"],\"sha256\":\"%s\"}],\"producer\":{"
      "\"implementation_sha256\":\"%s\",\"name\":\"fuzz-host\","
      "\"version\":\"1\"},\"project\":\"fuzz\",\"schema_version\":1}",
      size, ARCHBIRD_FUZZ_LANGUAGE, ARCHBIRD_FUZZ_PATH, digest_hex,
      implementation);
  if (manifest_length < 0 || (size_t)manifest_length >= sizeof(manifest))
    return 0;
  config_length =
      snprintf(config, sizeof(config),
               "{\"description\":\"\",\"layers\":[{\"globs\":[\"**/*\"],"
               "\"import_roots\":[\".\"],\"language\":\"%s\",\"name\":"
               "\"core\",\"role\":\"core\"}],\"project\":\"fuzz\","
               "\"root\":\".\",\"schema_version\":1}",
               ARCHBIRD_FUZZ_LANGUAGE);
  if (config_length < 0 || (size_t)config_length >= sizeof(config))
    return 0;
  engine = fuzz_engine();
  if (!engine)
    return 0;
  status = archbird_project_create(engine, (const uint8_t *)manifest,
                                   (size_t)manifest_length, &project);
  if (status == ARCHBIRD_OK)
    status =
        archbird_project_add_source(engine, project, ARCHBIRD_FUZZ_PATH,
                                    strlen(ARCHBIRD_FUZZ_PATH), data, size);
  if (status == ARCHBIRD_OK)
    status = archbird_project_finalize_sources(engine, project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_set_config(
        engine, project, (const uint8_t *)config, (size_t)config_length);
  if (status == ARCHBIRD_OK)
    status = archbird_project_scan_builtin_provider(
        engine, project, ARCHBIRD_FUZZ_PROVIDER, strlen(ARCHBIRD_FUZZ_PROVIDER),
        ARCHBIRD_PROVIDER_PRIMARY);
  if (status == ARCHBIRD_OK)
    status = archbird_project_finalize_providers(engine, project);
  for (provider_index = 0;
       status == ARCHBIRD_OK &&
       provider_index < archbird_project_provider_count(project);
       provider_index++)
    status = archbird_project_render_provider_facts(
        engine, project, provider_index, 0, fuzz_discard, NULL);
  if (status == ARCHBIRD_OK)
    (void)archbird_project_render_file_facts(engine, project, 0, fuzz_discard,
                                             NULL);
  if (status == ARCHBIRD_OK)
    (void)archbird_project_render_map(engine, project, 0, fuzz_discard, NULL);
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
  return 0;
}
