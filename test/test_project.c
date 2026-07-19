#include <archbird/archbird.h>

#include <stdio.h>
#include <string.h>

static int failures;

typedef struct OutputBuffer {
  char *data;
  size_t length;
  size_t capacity;
} OutputBuffer;

static int write_output(void *user_data, const uint8_t *bytes, size_t length) {
  OutputBuffer *output = (OutputBuffer *)user_data;
  if (length > output->capacity - output->length - 1)
    return 1;
  memcpy(output->data + output->length, bytes, length);
  output->length += length;
  output->data[output->length] = '\0';
  return 0;
}

static void expect(const char *name, ArchbirdStatus actual,
                   ArchbirdStatus expected) {
  if (actual != expected) {
    fprintf(stderr, "FAIL %s: status %d, expected %d\n", name, (int)actual,
            (int)expected);
    failures++;
  }
}

static int make_provider(char *buffer, size_t capacity, const char *manifest,
                         unsigned span_end, const char *provider_name,
                         const char *configuration_sha256, const char *claim,
                         const char *fact_name) {
  return snprintf(
      buffer, capacity,
      "{\"artifact\":\"archbird-provider-facts\",\"capabilities\":[{"
      "\"claims\":[\"%s\"],\"coverage\":\"complete\","
      "\"domain\":\"symbols\"}],\"diagnostics\":[],\"facts\":[{"
      "\"attributes\":{\"signature\":\"a=1\"},"
      "\"claim\":\"%s\",\"domain\":\"symbols\","
      "\"id\":\"symbol:a:0\",\"key\":\"a\",\"kind\":\"variable\","
      "\"name\":\"%s\",\"path\":\"src/a.txt\",\"project\":\"sample\","
      "\"span\":{\"end\":%u,\"start\":0}}],\"inputs\":[{\"project\":"
      "\"sample\",\"source_manifest_sha256\":\"%s\"}],\"producer\":{"
      "\"configuration_sha256\":"
      "\"%s\","
      "\"implementation_sha256\":"
      "\"3333333333333333333333333333333333333333333333333333333333333333\","
      "\"name\":\"%s\",\"version\":\"1\"},\"provenance\":"
      "\"derived\",\"resolutions\":[],\"schema_version\":1,\"subject\":{"
      "\"path\":\"src/a.txt\",\"project\":\"sample\",\"scope\":\"file\"}}",
      claim, claim, fact_name, span_end, manifest, configuration_sha256,
      provider_name);
}

static int make_path_provider(char *buffer, size_t capacity,
                              const char *manifest, const char *path,
                              const char *fact_id, const char *key,
                              const char *provider_name,
                              const char *configuration_sha256) {
  return snprintf(
      buffer, capacity,
      "{\"artifact\":\"archbird-provider-facts\",\"capabilities\":[{"
      "\"claims\":[\"syntax-structure\"],\"coverage\":\"complete\","
      "\"domain\":\"symbols\"}],\"diagnostics\":[],\"facts\":[{"
      "\"claim\":\"syntax-structure\",\"domain\":\"symbols\","
      "\"id\":\"%s\",\"key\":\"%s\",\"kind\":\"variable\","
      "\"name\":\"%s\",\"path\":\"%s\",\"project\":\"multi\","
      "\"span\":{\"end\":3,\"start\":0}}],\"inputs\":[{\"project\":"
      "\"multi\",\"source_manifest_sha256\":\"%s\"}],\"producer\":{"
      "\"configuration_sha256\":\"%s\",\"implementation_sha256\":"
      "\"9999999999999999999999999999999999999999999999999999999999999999\","
      "\"name\":\"%s\",\"version\":\"1\"},\"provenance\":"
      "\"derived\",\"resolutions\":[],\"schema_version\":1,\"subject\":{"
      "\"path\":\"%s\",\"project\":\"multi\",\"scope\":\"file\"}}",
      fact_id, key, key, path, manifest, configuration_sha256, provider_name,
      path);
}

static int make_enriched_provider(char *buffer, size_t capacity,
                                  const char *manifest,
                                  const char *provider_name,
                                  const char *configuration_sha256) {
  return snprintf(
      buffer, capacity,
      "{\"artifact\":\"archbird-provider-facts\",\"capabilities\":[{"
      "\"claims\":[\"lexical-occurrence\"],\"coverage\":\"complete\","
      "\"domain\":\"symbols\"}],\"diagnostics\":[],\"facts\":[{"
      "\"attributes\":{\"scope\":\"module\",\"signature\":\"a = 1\"},"
      "\"claim\":\"lexical-occurrence\",\"domain\":\"symbols\","
      "\"id\":\"symbol:a:lexical\",\"key\":\"a\",\"kind\":\"variable\","
      "\"name\":\"a\",\"path\":\"src/a.txt\",\"project\":\"sample\","
      "\"span\":{\"end\":3,\"start\":0}}],\"inputs\":[{\"project\":"
      "\"sample\",\"source_manifest_sha256\":\"%s\"}],\"producer\":{"
      "\"configuration_sha256\":\"%s\",\"implementation_sha256\":"
      "\"3333333333333333333333333333333333333333333333333333333333333333\","
      "\"name\":\"%s\",\"version\":\"1\"},\"provenance\":\"derived\","
      "\"resolutions\":[],\"schema_version\":1,\"subject\":{\"path\":"
      "\"src/a.txt\",\"project\":\"sample\",\"scope\":\"file\"}}",
      manifest, configuration_sha256, provider_name);
}

static int make_named_symbol_provider(char *buffer, size_t capacity,
                                      const char *manifest,
                                      const char *provider_name,
                                      const char *configuration_sha256,
                                      const char *claim, const char *fact_id,
                                      const char *key, const char *name) {
  return snprintf(
      buffer, capacity,
      "{\"artifact\":\"archbird-provider-facts\",\"capabilities\":[{"
      "\"claims\":[\"%s\"],\"coverage\":\"complete\","
      "\"domain\":\"symbols\"}],\"diagnostics\":[],\"facts\":[{"
      "\"claim\":\"%s\",\"correlation\":\"span\","
      "\"domain\":\"symbols\",\"id\":\"%s\","
      "\"key\":\"%s\",\"kind\":\"method\",\"name\":\"%s\","
      "\"path\":\"src/a.txt\",\"project\":\"sample\","
      "\"span\":{\"end\":3,\"start\":0}}],\"inputs\":[{\"project\":"
      "\"sample\",\"source_manifest_sha256\":\"%s\"}],\"producer\":{"
      "\"configuration_sha256\":\"%s\",\"implementation_sha256\":"
      "\"3333333333333333333333333333333333333333333333333333333333333333\","
      "\"name\":\"%s\",\"version\":\"1\"},\"provenance\":\"derived\","
      "\"resolutions\":[],\"schema_version\":1,\"subject\":{\"path\":"
      "\"src/a.txt\",\"project\":\"sample\",\"scope\":\"file\"}}",
      claim, claim, fact_id, key, name, manifest, configuration_sha256,
      provider_name);
}

static int make_offset_provider_with_coverage(
    char *buffer, size_t capacity, const char *manifest, unsigned span_start,
    unsigned span_end, const char *provider_name,
    const char *configuration_sha256, const char *fact_name,
    const char *coverage) {
  return snprintf(
      buffer, capacity,
      "{\"artifact\":\"archbird-provider-facts\",\"capabilities\":[{"
      "\"claims\":[\"syntax-structure\"],\"coverage\":\"%s\","
      "\"domain\":\"symbols\"}],\"diagnostics\":[],\"facts\":[{"
      "\"claim\":\"syntax-structure\",\"domain\":\"symbols\","
      "\"id\":\"symbol:%s:%u\",\"key\":\"%s\",\"kind\":\"variable\","
      "\"name\":\"%s\",\"path\":\"src/a.txt\",\"project\":\"sample\","
      "\"span\":{\"end\":%u,\"start\":%u}}],\"inputs\":[{\"project\":"
      "\"sample\",\"source_manifest_sha256\":\"%s\"}],\"producer\":{"
      "\"configuration_sha256\":\"%s\",\"implementation_sha256\":"
      "\"3333333333333333333333333333333333333333333333333333333333333333\","
      "\"name\":\"%s\",\"version\":\"1\"},\"provenance\":\"derived\","
      "\"resolutions\":[],\"schema_version\":1,\"subject\":{\"path\":"
      "\"src/a.txt\",\"project\":\"sample\",\"scope\":\"file\"}}",
      coverage, fact_name, span_start, fact_name, fact_name, span_end,
      span_start, manifest, configuration_sha256, provider_name);
}

static int make_offset_provider(char *buffer, size_t capacity,
                                const char *manifest, unsigned span_start,
                                unsigned span_end, const char *provider_name,
                                const char *configuration_sha256,
                                const char *fact_name) {
  return make_offset_provider_with_coverage(
      buffer, capacity, manifest, span_start, span_end, provider_name,
      configuration_sha256, fact_name, "complete");
}

static ArchbirdProject *make_sample_project(ArchbirdEngine *engine) {
  static const char manifest[] =
      "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":3,"
      "\"language\":\"text\",\"layer\":\"core\",\"path\":\"src/a.txt\","
      "\"roles\":[\"source\"],\"sha256\":"
      "\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}],"
      "\"producer\":{\"implementation_sha256\":"
      "\"1111111111111111111111111111111111111111111111111111111111111111\","
      "\"name\":\"fixture-host\",\"version\":\"1\"},\"project\":\"sample\","
      "\"schema_version\":1}";
  ArchbirdProject *project = NULL;
  if (archbird_project_create(engine, (const uint8_t *)manifest,
                              strlen(manifest), &project) != ARCHBIRD_OK)
    return NULL;
  if (archbird_project_add_source(engine, project, "src/a.txt", 9,
                                  (const uint8_t *)"abc", 3) != ARCHBIRD_OK ||
      archbird_project_finalize_sources(engine, project) != ARCHBIRD_OK) {
    archbird_project_destroy(project);
    return NULL;
  }
  return project;
}

static void test_source_bound_provider_reuse(void) {
  static const char provider[] =
      "{\"artifact\":\"archbird-provider-facts\",\"capabilities\":[],"
      "\"diagnostics\":[],\"facts\":[],\"inputs\":[{\"path\":\"src/a.txt\","
      "\"project\":\"sample\",\"source_sha256\":"
      "\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}],"
      "\"producer\":{\"configuration_sha256\":"
      "\"2222222222222222222222222222222222222222222222222222222222222222\","
      "\"implementation_sha256\":"
      "\"3333333333333333333333333333333333333333333333333333333333333333\","
      "\"name\":\"fixture-source-provider\",\"version\":\"1\"},"
      "\"provenance\":\"derived\",\"resolutions\":[],\"schema_version\":1,"
      "\"subject\":{\"path\":\"src/a.txt\",\"project\":\"sample\","
      "\"scope\":\"file\"}}";
  static const char expanded_manifest[] =
      "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":3,"
      "\"path\":\"src/a.txt\",\"roles\":[\"source\"],\"sha256\":"
      "\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"},{"
      "\"bytes\":3,\"path\":\"src/b.txt\",\"roles\":[\"source\"],\"sha256\":"
      "\"cb8379ac2098aa165029e3938a51da0bcecfc008fd6795f401178647f96c5b34\"}],"
      "\"producer\":{\"implementation_sha256\":"
      "\"1111111111111111111111111111111111111111111111111111111111111111\","
      "\"name\":\"fixture-host\",\"version\":\"1\"},\"project\":\"sample\","
      "\"schema_version\":1}";
  static const char changed_manifest[] =
      "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":3,"
      "\"path\":\"src/a.txt\",\"roles\":[\"source\"],\"sha256\":"
      "\"3608bca1e44ea6c4d268eb6db02260269892c0b42b86bbf1e77a6fa16c3c9282\"}],"
      "\"producer\":{\"implementation_sha256\":"
      "\"1111111111111111111111111111111111111111111111111111111111111111\","
      "\"name\":\"fixture-host\",\"version\":\"1\"},\"project\":\"sample\","
      "\"schema_version\":1}";
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;

  archbird_engine_options_init(&options);
  expect("source-input-engine", archbird_engine_create(&options, &engine),
         ARCHBIRD_OK);
  if (!engine)
    return;
  project = make_sample_project(engine);
  if (!project) {
    fputs("FAIL create source-input base project\n", stderr);
    failures++;
    goto done;
  }
  expect("source-input-base",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_PRIMARY,
             (const uint8_t *)provider, sizeof(provider) - 1),
         ARCHBIRD_OK);
  archbird_project_destroy(project);
  project = NULL;

  expect("source-input-expanded-project",
         archbird_project_create(engine, (const uint8_t *)expanded_manifest,
                                 sizeof(expanded_manifest) - 1, &project),
         ARCHBIRD_OK);
  if (!project)
    goto done;
  expect("source-input-expanded-a",
         archbird_project_add_source(engine, project, "src/a.txt", 9,
                                     (const uint8_t *)"abc", 3),
         ARCHBIRD_OK);
  expect("source-input-expanded-b",
         archbird_project_add_source(engine, project, "src/b.txt", 9,
                                     (const uint8_t *)"def", 3),
         ARCHBIRD_OK);
  expect("source-input-expanded-finalize",
         archbird_project_finalize_sources(engine, project), ARCHBIRD_OK);
  expect("source-input-unrelated-change",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_PRIMARY,
             (const uint8_t *)provider, sizeof(provider) - 1),
         ARCHBIRD_OK);
  archbird_project_destroy(project);
  project = NULL;

  expect("source-input-changed-project",
         archbird_project_create(engine, (const uint8_t *)changed_manifest,
                                 sizeof(changed_manifest) - 1, &project),
         ARCHBIRD_OK);
  if (!project)
    goto done;
  expect("source-input-changed-source",
         archbird_project_add_source(engine, project, "src/a.txt", 9,
                                     (const uint8_t *)"xyz", 3),
         ARCHBIRD_OK);
  expect("source-input-changed-finalize",
         archbird_project_finalize_sources(engine, project), ARCHBIRD_OK);
  expect("source-input-stale",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_PRIMARY,
             (const uint8_t *)provider, sizeof(provider) - 1),
         ARCHBIRD_CONFLICT);
done:
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
}

static void test_selective_builtin_provider_scan(void) {
  static const char manifest[] =
      "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":22,"
      "\"language\":\"python\",\"path\":\"src/a.py\","
      "\"roles\":[\"source\"],\"sha256\":"
      "\"5b76d0962c09ab4ee309fac65fad3568c97abdec983b405146ae3e86a235e352\"}],"
      "\"producer\":{\"implementation_sha256\":"
      "\"1111111111111111111111111111111111111111111111111111111111111111\","
      "\"name\":\"fixture-host\",\"version\":\"1\"},\"project\":\"sample\","
      "\"schema_version\":1}";
  static const char source[] = "def f():\n    return 1\n";
  char rendered_bytes[8192];
  OutputBuffer rendered = {rendered_bytes, 0, sizeof(rendered_bytes)};
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;

  archbird_engine_options_init(&options);
  expect("selective-scan-engine", archbird_engine_create(&options, &engine),
         ARCHBIRD_OK);
  if (!engine)
    return;
  expect("selective-scan-project",
         archbird_project_create(engine, (const uint8_t *)manifest,
                                 sizeof(manifest) - 1, &project),
         ARCHBIRD_OK);
  if (!project)
    goto done;
  expect("selective-scan-source",
         archbird_project_add_source(engine, project, "src/a.py", 8,
                                     (const uint8_t *)source,
                                     sizeof(source) - 1),
         ARCHBIRD_OK);
  expect("selective-scan-finalize",
         archbird_project_finalize_sources(engine, project), ARCHBIRD_OK);
  expect("selective-scan-missing-path",
         archbird_project_scan_builtin_provider_file(
             engine, project, "lexical:python", 14, "src/missing.py", 14,
             ARCHBIRD_PROVIDER_AUGMENT),
         ARCHBIRD_INVALID_ARGUMENT);
  expect("selective-scan-semantic",
         archbird_project_scan_builtin_provider_file(
             engine, project, "semantic:scip", 13, "src/a.py", 8,
             ARCHBIRD_PROVIDER_AUGMENT),
         ARCHBIRD_INVALID_ARGUMENT);
  expect("selective-scan-python",
         archbird_project_scan_builtin_provider_file(
             engine, project, "lexical:python", 14, "src/a.py", 8,
             ARCHBIRD_PROVIDER_AUGMENT),
         ARCHBIRD_OK);
  if (archbird_project_provider_count(project) != 1) {
    fputs("FAIL selective scan provider count\n", stderr);
    failures++;
  }
  expect("selective-scan-render",
         archbird_project_render_provider_facts(engine, project, 0, 0,
                                                write_output, &rendered),
         ARCHBIRD_OK);
  if (!strstr(rendered.data, "\"path\":\"src/a.py\",\"project\":\"sample\","
                             "\"source_sha256\":"
                             "\"5b76d0962c09ab4ee309fac65fad3568c97abdec983b405"
                             "146ae3e86a235e352\"") ||
      strstr(rendered.data, "\"source_manifest_sha256\"")) {
    fputs("FAIL selective scan source binding\n", stderr);
    failures++;
  }
done:
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
}

static void test_manifest_role_limits(void) {
  static const char index_manifest[] =
      "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":3,"
      "\"path\":\"semantic.scip\",\"roles\":[\"index\"],\"sha256\":"
      "\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}],"
      "\"producer\":{\"implementation_sha256\":"
      "\"1111111111111111111111111111111111111111111111111111111111111111\","
      "\"name\":\"fixture-host\",\"version\":\"1\"},\"project\":\"sample\","
      "\"schema_version\":1}";
  static const char source_manifest[] =
      "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":3,"
      "\"path\":\"src/a.txt\",\"roles\":[\"source\"],\"sha256\":"
      "\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}],"
      "\"producer\":{\"implementation_sha256\":"
      "\"1111111111111111111111111111111111111111111111111111111111111111\","
      "\"name\":\"fixture-host\",\"version\":\"1\"},\"project\":\"sample\","
      "\"schema_version\":1}";
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;
  archbird_engine_options_init(&options);
  options.max_file_bytes = 2;
  options.max_index_bytes = 4;
  expect("role-limit-engine", archbird_engine_create(&options, &engine),
         ARCHBIRD_OK);
  if (!engine)
    return;
  expect("role-limit-index",
         archbird_project_create(engine, (const uint8_t *)index_manifest,
                                 sizeof(index_manifest) - 1, &project),
         ARCHBIRD_OK);
  archbird_project_destroy(project);
  project = NULL;
  expect("role-limit-source",
         archbird_project_create(engine, (const uint8_t *)source_manifest,
                                 sizeof(source_manifest) - 1, &project),
         ARCHBIRD_LIMIT_EXCEEDED);
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
}

static void test_primary_inventory_and_augment_fallback(void) {
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;
  ArchbirdMergeSummary summary = {0};
  char provider[2048];
  char facts_bytes[4096];
  OutputBuffer facts = {facts_bytes, 0, sizeof(facts_bytes)};
  int length;
  const char *manifest;

  archbird_engine_options_init(&options);
  expect("inventory-engine", archbird_engine_create(&options, &engine),
         ARCHBIRD_OK);
  project = make_sample_project(engine);
  if (!project) {
    fputs("FAIL create inventory project\n", stderr);
    failures++;
    goto done;
  }
  manifest = archbird_project_manifest_sha256(project);
  length = make_offset_provider(
      provider, sizeof(provider), manifest, 0, 1, "inventory-primary",
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "a");
  expect("inventory-primary",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_PRIMARY,
             (const uint8_t *)provider, (size_t)length),
         ARCHBIRD_OK);
  length = make_offset_provider(
      provider, sizeof(provider), manifest, 1, 2, "inventory-augment",
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "b");
  expect("inventory-augment",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_AUGMENT,
             (const uint8_t *)provider, (size_t)length),
         ARCHBIRD_OK);
  expect("inventory-finalize",
         archbird_project_finalize_providers(engine, project), ARCHBIRD_OK);
  summary.struct_size = sizeof(summary);
  expect("inventory-summary", archbird_project_merge_summary(project, &summary),
         ARCHBIRD_OK);
  expect("inventory-render",
         archbird_project_render_file_facts(engine, project, 0, write_output,
                                            &facts),
         ARCHBIRD_OK);
  if (summary.selected_facts != 1 || summary.contributed != 1 ||
      strstr(facts.data, "\"name\":\"b\"") != NULL || summary.conflicts != 0) {
    fprintf(stderr,
            "FAIL primary provider did not own its domain inventory: "
            "selected=%zu contributed=%zu conflicts=%zu\n",
            summary.selected_facts, summary.contributed, summary.conflicts);
    failures++;
  }
  archbird_project_destroy(project);
  project = make_sample_project(engine);
  if (!project) {
    fputs("FAIL create bounded-primary project\n", stderr);
    failures++;
    goto done;
  }
  manifest = archbird_project_manifest_sha256(project);
  length = make_offset_provider_with_coverage(
      provider, sizeof(provider), manifest, 0, 1, "bounded-primary",
      "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd", "a",
      "bounded");
  expect("bounded-primary",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_PRIMARY,
             (const uint8_t *)provider, (size_t)length),
         ARCHBIRD_OK);
  length = make_offset_provider(
      provider, sizeof(provider), manifest, 1, 2, "bounded-augment",
      "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", "b");
  expect("bounded-augment",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_AUGMENT,
             (const uint8_t *)provider, (size_t)length),
         ARCHBIRD_OK);
  expect("bounded-finalize",
         archbird_project_finalize_providers(engine, project), ARCHBIRD_OK);
  summary = (ArchbirdMergeSummary){0};
  summary.struct_size = sizeof(summary);
  expect("bounded-summary", archbird_project_merge_summary(project, &summary),
         ARCHBIRD_OK);
  facts.length = 0;
  facts.data[0] = '\0';
  expect("bounded-render",
         archbird_project_render_file_facts(engine, project, 0, write_output,
                                            &facts),
         ARCHBIRD_OK);
  if (summary.selected_facts != 2 || summary.contributed != 2 ||
      strstr(facts.data, "\"name\":\"a\"") == NULL ||
      strstr(facts.data, "\"name\":\"b\"") == NULL || summary.conflicts != 0) {
    fprintf(stderr,
            "FAIL bounded primary suppressed independent augment evidence: "
            "selected=%zu contributed=%zu conflicts=%zu\n",
            summary.selected_facts, summary.contributed, summary.conflicts);
    failures++;
  }
  archbird_project_destroy(project);
  project = make_sample_project(engine);
  if (!project) {
    fputs("FAIL create augment-only project\n", stderr);
    failures++;
    goto done;
  }
  manifest = archbird_project_manifest_sha256(project);
  length = make_offset_provider(
      provider, sizeof(provider), manifest, 1, 2, "fallback-augment",
      "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc", "b");
  expect("fallback-augment",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_AUGMENT,
             (const uint8_t *)provider, (size_t)length),
         ARCHBIRD_OK);
  expect("fallback-finalize",
         archbird_project_finalize_providers(engine, project), ARCHBIRD_OK);
  summary = (ArchbirdMergeSummary){0};
  summary.struct_size = sizeof(summary);
  expect("fallback-summary", archbird_project_merge_summary(project, &summary),
         ARCHBIRD_OK);
  if (summary.selected_facts != 1 || summary.contributed != 1 ||
      summary.conflicts != 0) {
    fputs("FAIL augment-only domain did not preserve fallback facts\n", stderr);
    failures++;
  }
done:
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
}

static void test_scoped_primary_selection(void) {
  static const char manifest[] =
      "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":3,"
      "\"language\":\"text\",\"layer\":\"core\",\"path\":\"src/a.txt\","
      "\"roles\":[\"source\"],\"sha256\":"
      "\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"},{"
      "\"bytes\":3,\"language\":\"text\",\"layer\":\"core\",\"path\":"
      "\"src/b.txt\",\"roles\":[\"source\"],\"sha256\":"
      "\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}],"
      "\"producer\":{\"implementation_sha256\":"
      "\"1111111111111111111111111111111111111111111111111111111111111111\","
      "\"name\":\"fixture-host\",\"version\":\"1\"},\"project\":\"multi\","
      "\"schema_version\":1}";
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;
  ArchbirdMergeSummary summary = {0};
  char facts_a_bytes[4096];
  char facts_b_bytes[4096];
  char ledger_bytes[8192];
  OutputBuffer facts_a = {facts_a_bytes, 0, sizeof(facts_a_bytes)};
  OutputBuffer facts_b = {facts_b_bytes, 0, sizeof(facts_b_bytes)};
  OutputBuffer ledger = {ledger_bytes, 0, sizeof(ledger_bytes)};
  const char *digest;
  char provider[2048];
  int length;

  archbird_engine_options_init(&options);
  expect("multi-engine", archbird_engine_create(&options, &engine),
         ARCHBIRD_OK);
  expect("multi-project",
         archbird_project_create(engine, (const uint8_t *)manifest,
                                 strlen(manifest), &project),
         ARCHBIRD_OK);
  if (!project)
    goto done;
  expect("multi-source-a",
         archbird_project_add_source(engine, project, "src/a.txt", 9,
                                     (const uint8_t *)"abc", 3),
         ARCHBIRD_OK);
  expect("multi-source-b",
         archbird_project_add_source(engine, project, "src/b.txt", 9,
                                     (const uint8_t *)"abc", 3),
         ARCHBIRD_OK);
  expect("multi-finalize-sources",
         archbird_project_finalize_sources(engine, project), ARCHBIRD_OK);
  digest = archbird_project_manifest_sha256(project);
  length = make_path_provider(
      provider, sizeof(provider), digest, "src/a.txt", "symbol:a:0", "a",
      "fixture-a",
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  expect("multi-provider-a",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_PRIMARY,
             (const uint8_t *)provider, (size_t)length),
         ARCHBIRD_OK);
  length = make_path_provider(
      provider, sizeof(provider), digest, "src/b.txt", "symbol:b:0", "b",
      "fixture-b",
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
  expect("multi-provider-b",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_PRIMARY,
             (const uint8_t *)provider, (size_t)length),
         ARCHBIRD_OK);
  expect("multi-provider-finalize",
         archbird_project_finalize_providers(engine, project), ARCHBIRD_OK);
  summary.struct_size = sizeof(summary);
  expect("multi-summary", archbird_project_merge_summary(project, &summary),
         ARCHBIRD_OK);
  if (summary.providers != 2 || summary.selections != 2 ||
      summary.selected_facts != 2 || summary.conflicts != 0) {
    fputs("FAIL exact-subject provider selection\n", stderr);
    failures++;
  }
  expect("multi-render-a",
         archbird_project_render_file_facts(engine, project, 0, write_output,
                                            &facts_a),
         ARCHBIRD_OK);
  expect("multi-render-b",
         archbird_project_render_file_facts(engine, project, 1, write_output,
                                            &facts_b),
         ARCHBIRD_OK);
  expect("multi-render-ledger",
         archbird_project_render_merge_ledger(engine, project, 0, write_output,
                                              &ledger),
         ARCHBIRD_OK);
  if (!strstr(facts_a.data, "\"name\":\"a\"") ||
      !strstr(facts_a.data, "\"name\":\"b\"") ||
      !strstr(facts_b.data, "\"name\": \"a\"") ||
      !strstr(facts_b.data, "\"name\": \"b\"") ||
      !strstr(ledger.data, "\"selected_facts\":2") ||
      !strstr(ledger.data, "\"fact_id\":\"symbol:a:0\"") ||
      !strstr(ledger.data, "\"fact_id\":\"symbol:b:0\"")) {
    fputs("FAIL primary-only merged fact rendering\n", stderr);
    failures++;
  }
done:
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
}

static void test_anchor_correlation_and_name_enrichment(void) {
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;
  ArchbirdMergeSummary summary = {0};
  char provider[2048];
  char facts_bytes[4096];
  OutputBuffer facts = {facts_bytes, 0, sizeof(facts_bytes)};
  const char *manifest;
  int length;

  archbird_engine_options_init(&options);
  expect("anchor-engine", archbird_engine_create(&options, &engine),
         ARCHBIRD_OK);
  project = make_sample_project(engine);
  if (!project) {
    fputs("FAIL create anchor-correlation project\n", stderr);
    failures++;
    goto done;
  }
  manifest = archbird_project_manifest_sha256(project);
  length = make_named_symbol_provider(
      provider, sizeof(provider), manifest, "anchor-primary",
      "1111111111111111111111111111111111111111111111111111111111111111",
      "syntax-structure", "symbol:parse:primary", "parse", "parse");
  expect("anchor-primary",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_PRIMARY,
             (const uint8_t *)provider, (size_t)length),
         ARCHBIRD_OK);
  length = make_named_symbol_provider(
      provider, sizeof(provider), manifest, "anchor-augment",
      "2222222222222222222222222222222222222222222222222222222222222222",
      "lexical-occurrence", "symbol:parse:augment", "Box.parse", "Box.parse");
  expect("anchor-augment",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_AUGMENT,
             (const uint8_t *)provider, (size_t)length),
         ARCHBIRD_OK);
  expect("anchor-finalize",
         archbird_project_finalize_providers(engine, project), ARCHBIRD_OK);
  summary.struct_size = sizeof(summary);
  expect("anchor-summary", archbird_project_merge_summary(project, &summary),
         ARCHBIRD_OK);
  expect("anchor-render",
         archbird_project_render_file_facts(engine, project, 0, write_output,
                                            &facts),
         ARCHBIRD_OK);
  if (summary.selected_facts != 1 || summary.enriched != 1 ||
      summary.conflicts != 0 ||
      strstr(facts.data, "\"name\":\"Box.parse\"") == NULL ||
      strstr(facts.data, "\"name\":\"parse\"") != NULL) {
    fprintf(stderr,
            "FAIL exact anchors did not correlate qualified symbol names: "
            "selected=%zu enriched=%zu conflicts=%zu facts=%s\n",
            summary.selected_facts, summary.enriched, summary.conflicts,
            facts.data);
    failures++;
  }

  archbird_project_destroy(project);
  project = make_sample_project(engine);
  if (!project) {
    fputs("FAIL create anchor-conflict project\n", stderr);
    failures++;
    goto done;
  }
  manifest = archbird_project_manifest_sha256(project);
  length = make_named_symbol_provider(
      provider, sizeof(provider), manifest, "conflict-primary",
      "3333333333333333333333333333333333333333333333333333333333333333",
      "syntax-structure", "symbol:parse:conflict-primary", "Box.parse",
      "Box.parse");
  expect("anchor-conflict-primary",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_PRIMARY,
             (const uint8_t *)provider, (size_t)length),
         ARCHBIRD_OK);
  length = make_named_symbol_provider(
      provider, sizeof(provider), manifest, "conflict-augment",
      "4444444444444444444444444444444444444444444444444444444444444444",
      "lexical-occurrence", "symbol:run:conflict-augment", "Other.run",
      "Other.run");
  expect("anchor-conflict-augment",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_AUGMENT,
             (const uint8_t *)provider, (size_t)length),
         ARCHBIRD_OK);
  expect("anchor-conflict-finalize",
         archbird_project_finalize_providers(engine, project),
         ARCHBIRD_CONFLICT);
  summary = (ArchbirdMergeSummary){0};
  summary.struct_size = sizeof(summary);
  expect("anchor-conflict-summary",
         archbird_project_merge_summary(project, &summary), ARCHBIRD_OK);
  if (summary.conflicts != 1) {
    fputs("FAIL incompatible names at one exact anchor were not a conflict\n",
          stderr);
    failures++;
  }

  archbird_project_destroy(project);
  project = make_sample_project(engine);
  if (!project) {
    fputs("FAIL create keyed-correlation project\n", stderr);
    failures++;
    goto done;
  }
  manifest = archbird_project_manifest_sha256(project);
  length = make_offset_provider_with_coverage(
      provider, sizeof(provider), manifest, 0, 3, "keyed-left",
      "5555555555555555555555555555555555555555555555555555555555555555",
      "left", "bounded");
  expect("keyed-left",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_AUGMENT,
             (const uint8_t *)provider, (size_t)length),
         ARCHBIRD_OK);
  length = make_offset_provider_with_coverage(
      provider, sizeof(provider), manifest, 0, 3, "keyed-right",
      "6666666666666666666666666666666666666666666666666666666666666666",
      "right", "bounded");
  expect("keyed-right",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_AUGMENT,
             (const uint8_t *)provider, (size_t)length),
         ARCHBIRD_OK);
  expect("keyed-finalize", archbird_project_finalize_providers(engine, project),
         ARCHBIRD_OK);
  summary = (ArchbirdMergeSummary){0};
  summary.struct_size = sizeof(summary);
  expect("keyed-summary", archbird_project_merge_summary(project, &summary),
         ARCHBIRD_OK);
  if (summary.selected_facts != 2 || summary.conflicts != 0) {
    fprintf(stderr,
            "FAIL key-qualified facts sharing a range collapsed: "
            "selected=%zu conflicts=%zu\n",
            summary.selected_facts, summary.conflicts);
    failures++;
  }
done:
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
}

static void test_augment_claim_precedence(void) {
  static const char low_bundle_configuration[] =
      "0000000000000000000000000000000000000000000000000000000000000022";
  static const char high_digest[] =
      "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;
  char provider[4096];
  char ledger_bytes[16384];
  OutputBuffer ledger = {ledger_bytes, 0, sizeof(ledger_bytes)};
  const char *manifest_digest;
  int provider_length;

  archbird_engine_options_init(&options);
  expect("claim-precedence-engine", archbird_engine_create(&options, &engine),
         ARCHBIRD_OK);
  if (!engine)
    return;
  project = make_sample_project(engine);
  if (!project) {
    fputs("FAIL create claim-precedence project\n", stderr);
    failures++;
    goto done;
  }
  manifest_digest = archbird_project_manifest_sha256(project);
  provider_length = make_named_symbol_provider(
      provider, sizeof(provider), manifest_digest, "lexical-provider",
      low_bundle_configuration, "lexical-occurrence", "symbol:lexical",
      "Box.run", "Box.run");
  expect("claim-precedence-lexical",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_AUGMENT,
             (const uint8_t *)provider, (size_t)provider_length),
         ARCHBIRD_OK);
  provider_length = make_named_symbol_provider(
      provider, sizeof(provider), manifest_digest, "syntax-provider",
      high_digest, "syntax-structure", "symbol:syntax", "Box.run", "Box.run");
  expect("claim-precedence-syntax",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_AUGMENT,
             (const uint8_t *)provider, (size_t)provider_length),
         ARCHBIRD_OK);
  expect("claim-precedence-finalize",
         archbird_project_finalize_providers(engine, project), ARCHBIRD_OK);
  expect("claim-precedence-ledger",
         archbird_project_render_merge_ledger(engine, project, 0, write_output,
                                              &ledger),
         ARCHBIRD_OK);
  if (!strstr(ledger.data, "\"canonical\":{\"claim\":\"syntax-structure\"")) {
    fputs("FAIL stronger augment claim was not canonical\n", stderr);
    failures++;
  }
done:
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
}

static void test_builtin_provider_selection(void) {
  static const char source[] = "def run():\n    return helper()\n";
  static const char manifest[] =
      "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":31,"
      "\"language\":\"python\",\"layer\":\"core\",\"path\":\"pkg/api.py\","
      "\"roles\":[\"source\"],\"sha256\":"
      "\"4f5487a12760e1cfd1c5830344eb1c2c988004c2ba2d4c584a992aa2dc820d62\"}],"
      "\"producer\":{\"implementation_sha256\":"
      "\"1111111111111111111111111111111111111111111111111111111111111111\","
      "\"name\":\"fixture-host\",\"version\":\"1\"},\"project\":"
      "\"lexical-selection\",\"schema_version\":1}";
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;
  char output_bytes[4096];
  OutputBuffer output = {output_bytes, 0, sizeof(output_bytes)};

  archbird_engine_options_init(&options);
  expect("lexical-engine", archbird_engine_create(&options, &engine),
         ARCHBIRD_OK);
  expect("lexical-project",
         archbird_project_create(engine, (const uint8_t *)manifest,
                                 strlen(manifest), &project),
         ARCHBIRD_OK);
  if (!project)
    goto done;
  expect("lexical-source",
         archbird_project_add_source(engine, project, "pkg/api.py", 10,
                                     (const uint8_t *)source,
                                     sizeof(source) - 1),
         ARCHBIRD_OK);
  expect("lexical-finalize-sources",
         archbird_project_finalize_sources(engine, project), ARCHBIRD_OK);
  expect("lexical-unknown",
         archbird_project_scan_builtin_provider(
             engine, project, "lexical:missing", 15, ARCHBIRD_PROVIDER_PRIMARY),
         ARCHBIRD_INVALID_ARGUMENT);
  expect("lexical-empty-c",
         archbird_project_scan_builtin_provider(engine, project, "lexical:c", 9,
                                                ARCHBIRD_PROVIDER_PRIMARY),
         ARCHBIRD_OK);
  if (archbird_project_provider_count(project) != 0) {
    fputs("FAIL empty provider selection added evidence\n", stderr);
    failures++;
  }
  expect("lexical-python",
         archbird_project_scan_builtin_provider(
             engine, project, "lexical:python", 14, ARCHBIRD_PROVIDER_PRIMARY),
         ARCHBIRD_OK);
  if (archbird_project_provider_count(project) != 1 ||
      archbird_project_provider_fact_count(project) != 3) {
    fputs("FAIL Python provider selection counts\n", stderr);
    failures++;
  }
  expect("lexical-python-duplicate",
         archbird_project_scan_builtin_provider(
             engine, project, "lexical:python", 14, ARCHBIRD_PROVIDER_PRIMARY),
         ARCHBIRD_CONFLICT);
  if (archbird_project_provider_count(project) != 1) {
    fputs("FAIL duplicate provider selection mutated project\n", stderr);
    failures++;
  }
  expect("lexical-finalize-providers",
         archbird_project_finalize_providers(engine, project), ARCHBIRD_OK);
  expect("lexical-render",
         archbird_project_render_file_facts(engine, project, 0, write_output,
                                            &output),
         ARCHBIRD_OK);
  if (!strstr(output.data, "\"calls\":[\"helper\"]") ||
      !strstr(output.data, "\"exports\":[\"run\"]") ||
      !strstr(output.data, "\"name\":\"run\"")) {
    fputs("FAIL Python lexical file-fact reduction\n", stderr);
    failures++;
  }
done:
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
}

static void test_shared_public_header_names(void) {
  static const char header[] = "int api_a(void);\nint api_b(void);\n";
  static const char source_a[] = "int api_a(void) { return 1; }\n";
  static const char source_b[] = "int api_b(void) { return 2; }\n";
  static const char manifest[] =
      "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":34,"
      "\"language\":\"c\",\"layer\":\"core\",\"path\":\"include/api.h\","
      "\"roles\":[\"public-header\",\"source\"],\"sha256\":"
      "\"0fbac5b4709336ed3627d580f479af459078b24b79c4a008d10053cb7d111292\"},"
      "{\"bytes\":30,\"language\":\"c\",\"layer\":\"core\",\"path\":"
      "\"src/a.c\",\"roles\":[\"source\"],\"sha256\":"
      "\"fc8985fd2208b19b2191cc2ea9959cd7d2835fa8fcd9ccc1e61418e19f0a60b1\"},"
      "{\"bytes\":30,\"language\":\"c\",\"layer\":\"core\",\"path\":"
      "\"src/b.c\",\"roles\":[\"source\"],\"sha256\":"
      "\"7dccd287d9d6f906bd05cbe36333afd185f861d896706dae2c468ae7cf15d7ec\"}],"
      "\"producer\":{\"implementation_sha256\":"
      "\"1111111111111111111111111111111111111111111111111111111111111111\","
      "\"name\":\"fixture-host\",\"version\":\"1\"},\"project\":"
      "\"public-header-cache\",\"schema_version\":1}";
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;
  char output_bytes[8192];
  OutputBuffer output = {output_bytes, 0, sizeof(output_bytes)};

  archbird_engine_options_init(&options);
  expect("public-cache-engine", archbird_engine_create(&options, &engine),
         ARCHBIRD_OK);
  expect("public-cache-project",
         archbird_project_create(engine, (const uint8_t *)manifest,
                                 sizeof(manifest) - 1, &project),
         ARCHBIRD_OK);
  if (!project)
    goto done;
  expect("public-cache-header",
         archbird_project_add_source(engine, project, "include/api.h", 13,
                                     (const uint8_t *)header,
                                     sizeof(header) - 1),
         ARCHBIRD_OK);
  expect("public-cache-source-a",
         archbird_project_add_source(engine, project, "src/a.c", 7,
                                     (const uint8_t *)source_a,
                                     sizeof(source_a) - 1),
         ARCHBIRD_OK);
  expect("public-cache-source-b",
         archbird_project_add_source(engine, project, "src/b.c", 7,
                                     (const uint8_t *)source_b,
                                     sizeof(source_b) - 1),
         ARCHBIRD_OK);
  expect("public-cache-finalize-sources",
         archbird_project_finalize_sources(engine, project), ARCHBIRD_OK);
  expect("public-cache-scan",
         archbird_project_scan_builtin_provider(engine, project, "lexical:c", 9,
                                                ARCHBIRD_PROVIDER_PRIMARY),
         ARCHBIRD_OK);
  if (archbird_project_provider_count(project) != 3) {
    fputs("FAIL shared public-header provider count\n", stderr);
    failures++;
  }
  expect("public-cache-finalize-providers",
         archbird_project_finalize_providers(engine, project), ARCHBIRD_OK);
  expect("public-cache-render",
         archbird_project_render_file_facts(engine, project, 0, write_output,
                                            &output),
         ARCHBIRD_OK);
  if (!strstr(output.data,
              "\"kind\":\"function\",\"line\":1,\"name\":\"api_a\","
              "\"scope\":\"public\"") ||
      !strstr(output.data,
              "\"kind\":\"function\",\"line\":1,\"name\":\"api_b\","
              "\"scope\":\"public\"")) {
    fputs("FAIL shared public-header names were not reused by layer\n", stderr);
    failures++;
  }
done:
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
}

int main(int argc, char **argv) {
  static const char manifest[] =
      "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":3,"
      "\"language\":\"text\",\"layer\":\"core\",\"path\":\"src/a.txt\","
      "\"roles\":[\"source\"],\"sha256\":"
      "\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}],"
      "\"producer\":{\"implementation_sha256\":"
      "\"1111111111111111111111111111111111111111111111111111111111111111\","
      "\"name\":\"fixture-host\",\"version\":\"1\"},\"project\":\"sample\","
      "\"schema_version\":1}";
  static const char config[] =
      "{\"schema_version\":1,\"project\":\"sample\",\"description\":"
      "\"Native map\",\"layers\":[{\"name\":\"core\",\"role\":\"core\","
      "\"language\":\"text\",\"globs\":[\"src/**\"]}],\"components\":[{"
      "\"name\":\"core\",\"paths\":[\"src/**\"]}],\"limits\":{"
      "\"compact_symbols\":7}}";
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;
  ArchbirdSourceView view = {0};
  const char *manifest_digest;
  char provider[2048];
  char stale_provider[2048];
  char provider_roundtrip_bytes[4096];
  int provider_length;
  int stale_provider_length;
  ArchbirdMergeSummary summary = {0};
  OutputBuffer provider_roundtrip = {provider_roundtrip_bytes, 0,
                                     sizeof(provider_roundtrip_bytes)};
  char ledger_bytes[16384];
  char ledger_repeat_bytes[16384];
  char conflict_bytes[16384];
  char conflict_repeat_bytes[16384];
  char enriched_facts_bytes[4096];
  OutputBuffer ledger = {ledger_bytes, 0, sizeof(ledger_bytes)};
  OutputBuffer ledger_repeat = {ledger_repeat_bytes, 0,
                                sizeof(ledger_repeat_bytes)};
  OutputBuffer conflicts = {conflict_bytes, 0, sizeof(conflict_bytes)};
  OutputBuffer conflicts_repeat = {conflict_repeat_bytes, 0,
                                   sizeof(conflict_repeat_bytes)};
  OutputBuffer enriched_facts = {enriched_facts_bytes, 0,
                                 sizeof(enriched_facts_bytes)};
  int emit_ledger = argc == 2 && strcmp(argv[1], "--ledger") == 0;

  if (argc > 2 || (argc == 2 && !emit_ledger)) {
    fputs("usage: test_project [--ledger]\n", stderr);
    return 2;
  }
  test_manifest_role_limits();

  archbird_engine_options_init(&options);
  expect("engine", archbird_engine_create(&options, &engine), ARCHBIRD_OK);
  expect("project",
         archbird_project_create(engine, (const uint8_t *)manifest,
                                 strlen(manifest), &project),
         ARCHBIRD_OK);
  if (!project)
    return 1;
  expect("set-config",
         archbird_project_set_config(engine, project, (const uint8_t *)config,
                                     strlen(config)),
         ARCHBIRD_OK);
  if (!archbird_project_config_sha256(project) ||
      strcmp(
          archbird_project_config_sha256(project),
          "8e7a6d73c1c622964b58a76b932e95f5053f331878a6af4444d4e47dd48e04b9") !=
          0) {
    fputs("FAIL configuration digest\n", stderr);
    failures++;
  }
  expect("duplicate-config",
         archbird_project_set_config(engine, project, (const uint8_t *)config,
                                     strlen(config)),
         ARCHBIRD_CONFLICT);
  if (archbird_project_source_count(project) != 1) {
    fputs("FAIL source count\n", stderr);
    failures++;
  }
  expect("premature-finalize",
         archbird_project_finalize_sources(engine, project), ARCHBIRD_CONFLICT);
  expect("wrong-path",
         archbird_project_add_source(engine, project, "src/b.txt", 9,
                                     (const uint8_t *)"abc", 3),
         ARCHBIRD_CONFLICT);
  expect("wrong-hash",
         archbird_project_add_source(engine, project, "src/a.txt", 9,
                                     (const uint8_t *)"abd", 3),
         ARCHBIRD_CONFLICT);
  expect("add-source",
         archbird_project_add_source(engine, project, "src/a.txt", 9,
                                     (const uint8_t *)"abc", 3),
         ARCHBIRD_OK);
  expect("duplicate-source",
         archbird_project_add_source(engine, project, "src/a.txt", 9,
                                     (const uint8_t *)"abc", 3),
         ARCHBIRD_CONFLICT);
  expect("finalize", archbird_project_finalize_sources(engine, project),
         ARCHBIRD_OK);
  view.struct_size = sizeof(view);
  expect("source-view", archbird_project_source(project, 0, &view),
         ARCHBIRD_OK);
  if (view.path_length != 9 || memcmp(view.path, "src/a.txt", 9) != 0 ||
      view.byte_length != 3 || memcmp(view.bytes, "abc", 3) != 0) {
    fputs("FAIL source view content\n", stderr);
    failures++;
  }
  manifest_digest = archbird_project_manifest_sha256(project);
  if (!manifest_digest ||
      strcmp(
          manifest_digest,
          "4efe0850da9effefb2f8234d24c31ecd3b409e02859397bd93c7af16c2d4bc1c") !=
          0) {
    fputs("FAIL manifest digest\n", stderr);
    archbird_project_destroy(project);
    archbird_engine_destroy(engine);
    return 1;
  }
  provider_length = make_provider(
      provider, sizeof(provider), manifest_digest, 3, "fixture-primary",
      "2222222222222222222222222222222222222222222222222222222222222222",
      "syntax-structure", "a");
  if (provider_length < 0 || (size_t)provider_length >= sizeof(provider)) {
    fputs("FAIL provider formatting\n", stderr);
    failures++;
  } else {
    expect("provider-add",
           archbird_project_add_provider_facts(
               engine, project, ARCHBIRD_PROVIDER_PRIMARY,
               (const uint8_t *)provider, (size_t)provider_length),
           ARCHBIRD_OK);
    expect("provider-typed-roundtrip",
           archbird_project_render_provider_facts(
               engine, project, 0, 0, write_output, &provider_roundtrip),
           ARCHBIRD_OK);
    if (provider_roundtrip.length != (size_t)provider_length ||
        memcmp(provider_roundtrip.data, provider, (size_t)provider_length) !=
            0) {
      fputs("FAIL provider typed roundtrip\n", stderr);
      failures++;
    }
    expect("provider-duplicate",
           archbird_project_add_provider_facts(
               engine, project, ARCHBIRD_PROVIDER_PRIMARY,
               (const uint8_t *)provider, (size_t)provider_length),
           ARCHBIRD_CONFLICT);
  }
  stale_provider_length = make_provider(
      stale_provider, sizeof(stale_provider),
      "0000000000000000000000000000000000000000000000000000000000000000", 3,
      "fixture-stale",
      "3333333333333333333333333333333333333333333333333333333333333333",
      "syntax-structure", "a");
  expect("provider-stale",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_AUGMENT,
             (const uint8_t *)stale_provider, (size_t)stale_provider_length),
         ARCHBIRD_CONFLICT);
  provider_length = make_provider(
      provider, sizeof(provider), manifest_digest, 4, "fixture-span",
      "4444444444444444444444444444444444444444444444444444444444444444",
      "syntax-structure", "a");
  expect("provider-span",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_AUGMENT,
             (const uint8_t *)provider, (size_t)provider_length),
         ARCHBIRD_CONFLICT);
  provider_length = make_provider(
      provider, sizeof(provider), manifest_digest, 3, "fixture-duplicate",
      "5555555555555555555555555555555555555555555555555555555555555555",
      "syntax-structure", "a");
  expect("provider-augment-duplicate",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_AUGMENT,
             (const uint8_t *)provider, (size_t)provider_length),
         ARCHBIRD_OK);
  provider_length = make_enriched_provider(
      provider, sizeof(provider), manifest_digest, "fixture-enriched",
      "6666666666666666666666666666666666666666666666666666666666666666");
  expect("provider-augment-enriched",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_AUGMENT,
             (const uint8_t *)provider, (size_t)provider_length),
         ARCHBIRD_OK);
  provider_length = make_provider(
      provider, sizeof(provider), manifest_digest, 3, "fixture-conflict",
      "7777777777777777777777777777777777777777777777777777777777777777",
      "syntax-structure", "b");
  expect("provider-augment-conflict",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_AUGMENT,
             (const uint8_t *)provider, (size_t)provider_length),
         ARCHBIRD_OK);
  provider_length = make_provider(
      provider, sizeof(provider), manifest_digest, 3, "fixture-audit",
      "8888888888888888888888888888888888888888888888888888888888888888",
      "syntax-structure", "b");
  expect("provider-audit",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_AUDIT,
             (const uint8_t *)provider, (size_t)provider_length),
         ARCHBIRD_OK);
  if (archbird_project_provider_count(project) != 5 ||
      archbird_project_provider_fact_count(project) != 5) {
    fputs("FAIL provider counts or partial acceptance\n", stderr);
    failures++;
  }
  expect("provider-finalize",
         archbird_project_finalize_providers(engine, project),
         ARCHBIRD_CONFLICT);
  summary.struct_size = sizeof(summary);
  expect("provider-summary", archbird_project_merge_summary(project, &summary),
         ARCHBIRD_OK);
  if (summary.providers != 5 || summary.selections != 1 ||
      summary.selected_facts != 1 || summary.contributed != 1 ||
      summary.deduplicated != 1 || summary.enriched != 1 ||
      summary.variations != 1 || summary.conflicts != 1 ||
      summary.audit_matches != 0 || summary.audit_differences != 1) {
    fputs("FAIL provider merge summary\n", stderr);
    failures++;
  }
  expect("provider-finalize-repeat",
         archbird_project_finalize_providers(engine, project),
         ARCHBIRD_CONFLICT);
  expect("provider-materialized-enrichment",
         archbird_project_render_file_facts(engine, project, 0, write_output,
                                            &enriched_facts),
         ARCHBIRD_OK);
  if (!strstr(enriched_facts.data, "\"scope\":\"module\"")) {
    fputs("FAIL provider enrichment was not materialized\n", stderr);
    failures++;
  }
  expect("provider-ledger",
         archbird_project_render_merge_ledger(engine, project, 0, write_output,
                                              &ledger),
         ARCHBIRD_OK);
  expect("provider-ledger-repeat",
         archbird_project_render_merge_ledger(engine, project, 0, write_output,
                                              &ledger_repeat),
         ARCHBIRD_OK);
  if (ledger.length != ledger_repeat.length ||
      memcmp(ledger.data, ledger_repeat.data, ledger.length) != 0 ||
      !strstr(ledger.data, "\"conflicts\":1") ||
      !strstr(ledger.data, "\"variations\":1") ||
      !strstr(ledger.data, "\"reason\":\"augment-mismatch\"")) {
    fputs("FAIL provider ledger bytes\n", stderr);
    failures++;
  }
  expect("provider-ledger-json",
         archbird_json_validate(engine, (const uint8_t *)ledger.data,
                                ledger.length),
         ARCHBIRD_OK);
  expect("provider-conflicts",
         archbird_project_render_merge_conflicts(engine, project, 0,
                                                 write_output, &conflicts),
         ARCHBIRD_OK);
  expect("provider-conflicts-repeat",
         archbird_project_render_merge_conflicts(
             engine, project, 0, write_output, &conflicts_repeat),
         ARCHBIRD_OK);
  if (conflicts.length != conflicts_repeat.length ||
      memcmp(conflicts.data, conflicts_repeat.data, conflicts.length) != 0 ||
      !strstr(conflicts.data,
              "\"artifact\":\"archbird-provider-merge-conflicts\"") ||
      !strstr(conflicts.data, "\"left_fact\":{") ||
      !strstr(conflicts.data, "\"right_fact\":{") ||
      !strstr(conflicts.data, "\"providers_in_conflicts\":2") ||
      !strstr(conflicts.data, "\"providers_total\":5")) {
    fputs("FAIL compact provider conflict ledger bytes\n", stderr);
    failures++;
  }
  expect("provider-conflicts-json",
         archbird_json_validate(engine, (const uint8_t *)conflicts.data,
                                conflicts.length),
         ARCHBIRD_OK);
  expect("provider-after-finalize",
         archbird_project_add_provider_facts(
             engine, project, ARCHBIRD_PROVIDER_AUDIT,
             (const uint8_t *)provider, (size_t)provider_length),
         ARCHBIRD_CONFLICT);
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
  test_scoped_primary_selection();
  test_anchor_correlation_and_name_enrichment();
  test_augment_claim_precedence();
  test_builtin_provider_selection();
  test_shared_public_header_names();
  test_primary_inventory_and_augment_fallback();
  test_source_bound_provider_reuse();
  test_selective_builtin_provider_scan();
  if (failures) {
    fprintf(stderr, "%d native project test(s) failed\n", failures);
    return 1;
  }
  if (emit_ledger) {
    if (fwrite(ledger.data, 1, ledger.length, stdout) != ledger.length)
      return 2;
    return 0;
  }
  puts("native source project tests passed");
  return 0;
}
