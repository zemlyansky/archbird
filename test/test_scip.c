#include "scip_fixture.h"
#include "sha256.h"
#include <archbird/archbird.h>

#include <stdio.h>
#include <string.h>

typedef struct Output {
  char bytes[524288];
  size_t length;
} Output;

static int failures;

static int write_output(void *user_data, const uint8_t *bytes, size_t length) {
  Output *output = (Output *)user_data;
  if (length > sizeof(output->bytes) - output->length - 1)
    return 1;
  memcpy(output->bytes + output->length, bytes, length);
  output->length += length;
  output->bytes[output->length] = '\0';
  return 0;
}

static void fail(const char *message) {
  fprintf(stderr, "FAIL SCIP: %s\n", message);
  failures++;
}

static void expect_contains(const char *bytes, const char *needle,
                            const char *message) {
  if (!strstr(bytes, needle))
    fail(message);
}

static void expect_absent(const char *bytes, const char *needle,
                          const char *message) {
  if (strstr(bytes, needle))
    fail(message);
}

static void digest_hex(const uint8_t *bytes, size_t length, char out[65]) {
  uint8_t digest[32];
  if (archbird_sha256(bytes, length, digest) != ARCHBIRD_OK) {
    memset(out, '0', 64);
    out[64] = '\0';
    return;
  }
  archbird_sha256_hex(digest, out);
}

static void append_manifest_file(char *manifest, size_t capacity,
                                 size_t *length, int *first, const char *path,
                                 const char *layer, const char *language,
                                 const char *roles, const uint8_t *bytes,
                                 size_t byte_length) {
  char sha[65];
  int written;
  digest_hex(bytes, byte_length, sha);
  written = snprintf(
      manifest + *length, capacity - *length,
      "%s{\"bytes\":%zu%s%s%s%s%s%s,\"path\":\"%s\",\"roles\":[\"%s\"],"
      "\"sha256\":\"%s\"}",
      *first ? "" : ",", byte_length, layer ? ",\"layer\":\"" : "",
      layer ? layer : "", layer ? "\"" : "", language ? ",\"language\":\"" : "",
      language ? language : "", language ? "\"" : "", path, roles, sha);
  if (written < 0 || (size_t)written >= capacity - *length) {
    fail("manifest buffer overflow");
    return;
  }
  *length += (size_t)written;
  *first = 0;
}

static void test_scip_pipeline(void) {
  static const uint8_t api[] = "use(add); use(add);\n";
  static const uint8_t multiline[] = "zero\none\ntwo add\n";
  static const uint8_t stale[] = "x\n";
  static const uint8_t mismatch[] = "add\n";
  static const uint8_t unknown[] = "use(unknown_add)\n";
  static const uint8_t unicode_source[] = "\xf0\x9f\x9a\x80x=add\n";
  static const uint8_t core[] = "int add(void) { return 1; }\n";
  static const uint8_t unknown_core[] = "int unknown_add(void) { return 1; }\n";
  static const uint8_t contract[] = "int contract(void) { return 1; }\n";
  static const char config[] =
      "{\"schema_version\":1,\"project\":\"scip-test\",\"layers\":[{"
      "\"name\":\"python\",\"role\":\"frontend\",\"language\":\"python\","
      "\"globs\":[\"py/**\"]},{\"name\":\"core\",\"role\":\"core\","
      "\"language\":\"c\",\"globs\":[\"src/**\"]}],\"indexes\":[{"
      "\"name\":\"compiler\",\"format\":\"scip\",\"path\":\"valid.scip\","
      "\"position_encoding_fallback\":\"utf8\"},"
      "{\"name\":\"unsafe\",\"format\":\"scip\",\"path\":"
      "\"invalid.scip\"},{\"name\":\"optional-bad\",\"format\":\"scip\","
      "\"path\":\"bad.scip\",\"required\":false},"
      "{\"name\":\"source-mismatch\",\"format\":\"scip\","
      "\"path\":\"mismatch.scip\",\"required\":false},"
      "{\"name\":\"no-text\",\"format\":\"scip\","
      "\"path\":\"no-text.scip\",\"required\":false},"
      "{\"name\":\"stale\",\"format\":\"scip\",\"path\":\"stale.scip\","
      "\"required\":false},"
      "{\"name\":\"late-metadata\",\"format\":\"scip\",\"path\":"
      "\"late.scip\",\"required\":false},"
      "{\"name\":\"invalid-utf8\",\"format\":\"scip\",\"path\":"
      "\"utf8.scip\",\"required\":false}]}";
  ScipTestBuffer valid;
  ScipTestBuffer invalid;
  ScipTestBuffer malformed;
  ScipTestBuffer stale_index;
  ScipTestBuffer mismatch_index;
  ScipTestBuffer no_text_index;
  ScipTestBuffer late_metadata;
  ScipTestBuffer invalid_utf8;
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;
  ArchbirdMergeSummary summary = {0};
  char manifest[16384];
  size_t manifest_length = 0;
  int first = 1;
  int written;
  size_t provider;
  Output map = {{0}, 0};
  Output second = {{0}, 0};
  Output facts = {{0}, 0};
  ArchbirdStatus status;
  scip_test_valid_index(&valid);
  scip_test_invalid_path_index(&invalid);
  scip_test_malformed_index(&malformed);
  scip_test_stale_index(&stale_index);
  scip_test_source_mismatch_index(&mismatch_index);
  scip_test_no_text_index(&no_text_index);
  scip_test_late_metadata_index(&late_metadata);
  scip_test_invalid_utf8_index(&invalid_utf8);
  if (!valid.ok || !invalid.ok || !malformed.ok || !stale_index.ok ||
      !mismatch_index.ok || !no_text_index.ok || !late_metadata.ok ||
      !invalid_utf8.ok) {
    fail("fixture encoder overflow");
    return;
  }
  written = snprintf(manifest, sizeof(manifest),
                     "{\"artifact\":\"archbird-source-manifest\",\"files\":[");
  if (written < 0) {
    fail("manifest prefix");
    return;
  }
  manifest_length = (size_t)written;
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "bad.scip", NULL, NULL, "index", malformed.bytes,
                       malformed.length);
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "invalid.scip", NULL, NULL, "index", invalid.bytes,
                       invalid.length);
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "late.scip", NULL, NULL, "index", late_metadata.bytes,
                       late_metadata.length);
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "mismatch.scip", NULL, NULL, "index",
                       mismatch_index.bytes, mismatch_index.length);
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "no-text.scip", NULL, NULL, "index", no_text_index.bytes,
                       no_text_index.length);
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "py/api.py", "python", "python", "source", api,
                       sizeof(api) - 1);
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "py/mismatch.py", "python", "python", "source", mismatch,
                       sizeof(mismatch) - 1);
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "py/multiline.py", "python", "python", "source",
                       multiline, sizeof(multiline) - 1);
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "py/stale.py", "python", "python", "source", stale,
                       sizeof(stale) - 1);
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "py/unknown.py", "python", "python", "source", unknown,
                       sizeof(unknown) - 1);
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "py/utf16.py", "python", "python", "source",
                       unicode_source, sizeof(unicode_source) - 1);
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "py/utf32.py", "python", "python", "source",
                       unicode_source, sizeof(unicode_source) - 1);
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "src/contract.c", "core", "c", "source", contract,
                       sizeof(contract) - 1);
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "src/core.c", "core", "c", "source", core,
                       sizeof(core) - 1);
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "src/unknown_core.c", "core", "c", "source",
                       unknown_core, sizeof(unknown_core) - 1);
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "stale.scip", NULL, NULL, "index", stale_index.bytes,
                       stale_index.length);
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "utf8.scip", NULL, NULL, "index", invalid_utf8.bytes,
                       invalid_utf8.length);
  append_manifest_file(manifest, sizeof(manifest), &manifest_length, &first,
                       "valid.scip", NULL, NULL, "index", valid.bytes,
                       valid.length);
  written = snprintf(
      manifest + manifest_length, sizeof(manifest) - manifest_length,
      "],\"producer\":{\"implementation_sha256\":\"111111111111111111111111"
      "1111111111111111111111111111111111111111\",\"name\":\"scip-test\","
      "\"version\":\"1\"},\"project\":\"scip-test\",\"schema_version\":1}");
  if (written < 0 || (size_t)written >= sizeof(manifest) - manifest_length) {
    fail("manifest suffix overflow");
    return;
  }
  manifest_length += (size_t)written;

  archbird_engine_options_init(&options);
  status = archbird_engine_create(&options, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_project_create(engine, (const uint8_t *)manifest,
                                     manifest_length, &project);
#define ADD_SOURCE(path, value, length)                                        \
  if (status == ARCHBIRD_OK)                                                   \
  status = archbird_project_add_source(engine, project, path, strlen(path),    \
                                       value, length)
  ADD_SOURCE("bad.scip", malformed.bytes, malformed.length);
  ADD_SOURCE("invalid.scip", invalid.bytes, invalid.length);
  ADD_SOURCE("late.scip", late_metadata.bytes, late_metadata.length);
  ADD_SOURCE("mismatch.scip", mismatch_index.bytes, mismatch_index.length);
  ADD_SOURCE("no-text.scip", no_text_index.bytes, no_text_index.length);
  ADD_SOURCE("py/api.py", api, sizeof(api) - 1);
  ADD_SOURCE("py/mismatch.py", mismatch, sizeof(mismatch) - 1);
  ADD_SOURCE("py/multiline.py", multiline, sizeof(multiline) - 1);
  ADD_SOURCE("py/unknown.py", unknown, sizeof(unknown) - 1);
  ADD_SOURCE("py/stale.py", stale, sizeof(stale) - 1);
  ADD_SOURCE("py/utf16.py", unicode_source, sizeof(unicode_source) - 1);
  ADD_SOURCE("py/utf32.py", unicode_source, sizeof(unicode_source) - 1);
  ADD_SOURCE("src/contract.c", contract, sizeof(contract) - 1);
  ADD_SOURCE("src/core.c", core, sizeof(core) - 1);
  ADD_SOURCE("src/unknown_core.c", unknown_core, sizeof(unknown_core) - 1);
  ADD_SOURCE("stale.scip", stale_index.bytes, stale_index.length);
  ADD_SOURCE("valid.scip", valid.bytes, valid.length);
  ADD_SOURCE("utf8.scip", invalid_utf8.bytes, invalid_utf8.length);
#undef ADD_SOURCE
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
  if (status != ARCHBIRD_OK) {
    fail(engine ? archbird_engine_error(engine) : "engine creation failed");
    goto done;
  }
  summary.struct_size = sizeof(summary);
  if (archbird_project_merge_summary(project, &summary) != ARCHBIRD_OK ||
      summary.providers != 8 || summary.conflicts != 0)
    fail("multiple named indexes did not merge independently");
  for (provider = 0; provider < archbird_project_provider_count(project);
       provider++) {
    if (archbird_project_render_provider_facts(
            engine, project, provider, 0, write_output, &facts) != ARCHBIRD_OK)
      fail("provider facts did not render");
  }
  expect_contains(facts.bytes,
                  "scip-external:", "external identity evidence is absent");
  expect_contains(facts.bytes, "\"domain\":\"semantic-relationships\"",
                  "semantic relationship facts are absent");
  expect_contains(facts.bytes, "\"display_name_state\":\"provided\"",
                  "provided display-name evidence is absent");
  expect_contains(facts.bytes, "\"display_name_state\":\"source-range\"",
                  "source-range display-name evidence is absent");
  expect_contains(facts.bytes, "\"occurrence_count\":2",
                  "grouped occurrence count is absent");
  expect_contains(facts.bytes, "\"reference_facts\":4",
                  "reference fact count is incorrect");
  expect_contains(facts.bytes, "\"references\":5",
                  "reference occurrence count is incorrect");
  expect_contains(facts.bytes,
                  "\"semantic_symbol\":\"scip c demo 1.0 demo/add().\"",
                  "semantic symbol identity is absent");
  expect_absent(facts.bytes, "\"path\":\"py/stale.py\"",
                "stale document emitted semantic facts");
  expect_absent(facts.bytes, "\"path\":\"py/mismatch.py\"",
                "source-mismatched document emitted semantic facts");
  status = archbird_project_render_map(engine, project, 0, write_output, &map);
  if (status == ARCHBIRD_OK)
    status =
        archbird_project_render_map(engine, project, 0, write_output, &second);
  if (status != ARCHBIRD_OK || map.length != second.length ||
      memcmp(map.bytes, second.bytes, map.length) != 0)
    fail("Map rendering is not deterministic");
  if (!strstr(map.bytes,
              "\"evidence_state\":\"current\",\"name\":\"compiler\"") ||
      !strstr(map.bytes,
              "\"evidence_state\":\"unknown\",\"name\":\"optional-bad\"") ||
      !strstr(map.bytes,
              "\"evidence_state\":\"unknown\",\"name\":\"no-text\"") ||
      !strstr(map.bytes, "\"evidence_state\":\"stale\",\"name\":\"stale\"") ||
      !strstr(map.bytes, "\"code\":\"scip-invalid-document-path\"") ||
      !strstr(map.bytes, "\"code\":\"scip-invalid-index\"") ||
      !strstr(map.bytes, "\"code\":\"scip-configured-position-encoding\"") ||
      !strstr(map.bytes, "\"code\":\"scip-invalid-range\"") ||
      !strstr(map.bytes, "\"code\":\"scip-source-mismatch\"") ||
      !strstr(map.bytes, "\"documents_stale\":1") ||
      !strstr(map.bytes, "\"documents_source_verified\":6") ||
      !strstr(map.bytes, "\"invalid_ranges\":1") ||
      !strstr(map.bytes, "\"source_mismatches\":1") ||
      !strstr(map.bytes, "\"position_encoding_fallback\":\"utf8\"") ||
      !strstr(map.bytes, "\"position_encoding_fallback_documents\":1") ||
      !strstr(map.bytes, "SCIP metadata must be the first index field") ||
      !strstr(map.bytes, "SCIP tool name is not valid UTF-8") ||
      !strstr(map.bytes, "\"severity\":\"warning\"") ||
      strstr(map.bytes, "../escape.py"))
    fail("SCIP evidence state or safe diagnostics are incorrect");
  expect_contains(map.bytes, "\"kind\":\"semantic-reference\"",
                  "semantic-reference edge is absent");
  expect_contains(map.bytes,
                  "\"evidence\":[{\"basis\":\"semantic-index\","
                  "\"provider\":\"compiler\",\"state\":\"current\"}]",
                  "current semantic-index evidence is absent");
  expect_contains(map.bytes,
                  "\"evidence\":[{\"basis\":\"semantic-index\","
                  "\"provider\":\"no-text\",\"state\":\"unknown\"}],"
                  "\"kind\":\"semantic-reference\",\"names\":[\"scip c "
                  "demo 1.0 demo/unknown_add().\"],\"source\":"
                  "\"py/unknown.py\",\"target\":\"src/unknown_core.c\"",
                  "unknown zero-width semantic target is absent");
  expect_absent(map.bytes, "\"kind\":\"scip-reference\"",
                "legacy SCIP relation leaked into canonical Map");
  expect_contains(map.bytes,
                  "\"kind\":\"scip-implementation\",\"names\":[\"add\"],"
                  "\"source\":\"src/core.c\","
                  "\"target\":\"src/contract.c\"",
                  "SCIP implementation relationship is absent");
  expect_contains(map.bytes,
                  "\"kind\":\"semantic-reference\",\"names\":[\"add\"]",
                  "grouped semantic reference name is absent");
  expect_contains(map.bytes,
                  "\"kind\":\"function\",\"line\":1,\"name\":\"add\"",
                  "projected semantic symbol is absent");
  expect_contains(map.bytes, "\"source\":\"py/utf16.py\"",
                  "UTF-16 occurrence route is absent");
  expect_contains(map.bytes, "\"source\":\"py/utf32.py\"",
                  "UTF-32 occurrence route is absent");
  expect_contains(map.bytes, "\"source\":\"py/multiline.py\"",
                  "multiline occurrence route is absent");
done:
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
}

int main(void) {
  test_scip_pipeline();
  if (failures)
    return 1;
  puts("native SCIP integration tests passed");
  return 0;
}
