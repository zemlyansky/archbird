#include <archbird/archbird.h>

#include "json_value.h"
#include "render_internal.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>

static const char CONFIG[] =
    "{\"constraints\":{\"FORBID-UNUSED\":{\"kind\":\"forbidden_paths\","
    "\"owner\":\"architecture\",\"paths\":[\"src/unused.c\"],"
    "\"rationale\":\"Unused legacy sources are absent.\"},"
    "\"FORBID-USED\":{\"kind\":\"forbidden_paths\","
    "\"owner\":\"architecture\",\"paths\":[\"src/legacy.h\"],"
    "\"rationale\":\"Consumers must migrate before removal.\"}},"
    "\"layers\":[{\"globs\":[\"src/**\"],\"import_roots\":[\"src\"],"
    "\"language\":\"c\",\"name\":\"core\"}],\"project\":\"plan-compile\"}";

static const char REQUEST[] =
    "{\"artifact\":\"archbird-map-request\",\"default_excludes\":true,"
    "\"exclude\":[],\"ignore\":false,\"only\":[],\"schema_version\":1,"
    "\"sources\":[]}";

static const char INVENTORY[] =
    "{\"artifact\":\"archbird-repository-inventory\",\"documents\":[],"
    "\"files\":[{\"bytes\":18,\"path\":\"src/legacy.h\"},"
    "{\"bytes\":49,\"path\":\"src/main.c\"},"
    "{\"bytes\":38,\"path\":\"src/unused.c\"}],\"ignore_files\":[],"
    "\"pruned_directories\":[],\"schema_version\":1}";

static int failures;

static int collect(void *user_data, const uint8_t *bytes, size_t length) {
  return ab_buffer_append((AbBuffer *)user_data, bytes, length) == ARCHBIRD_OK
             ? 0
             : 1;
}

static int contains(const AbBuffer *buffer, const char *needle) {
  size_t needle_length = strlen(needle);
  size_t index;
  if (needle_length > buffer->length)
    return 0;
  for (index = 0; index <= buffer->length - needle_length; index++)
    if (memcmp(buffer->data + index, needle, needle_length) == 0)
      return 1;
  return 0;
}

static void sha256(const uint8_t *bytes, size_t length, char out[65]) {
  uint8_t digest[32];
  if (archbird_sha256(bytes, length, digest) != ARCHBIRD_OK) {
    memset(out, '0', 64);
    out[64] = '\0';
    return;
  }
  archbird_sha256_hex(digest, out);
}

static ArchbirdStatus add_source(ArchbirdEngine *engine,
                                 ArchbirdProject *project, const char *path,
                                 const uint8_t *bytes, size_t length) {
  return archbird_project_add_source(engine, project, path, strlen(path), bytes,
                                     length);
}

static void expect_status(const char *name, ArchbirdStatus actual,
                          ArchbirdStatus expected, ArchbirdEngine *engine) {
  if (actual == expected)
    return;
  fprintf(stderr, "FAIL %s: status %d, expected %d: %s\n", name, (int)actual,
          (int)expected, archbird_engine_error(engine));
  failures++;
}

int main(void) {
  static const uint8_t main_source[] =
      "#include \"legacy.h\"\nint main(void) { return 0; }\n";
  static const uint8_t legacy_source[] = "int legacy(void);\n";
  static const uint8_t unused_source[] =
      "static int unused(void) { return 0; }\n";
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;
  AbBuffer map;
  AbBuffer resolution;
  AbBuffer coverage_json;
  AbBuffer profile_json;
  AbBuffer verification;
  AbBuffer plan;
  AbBuffer selected_plan;
  char main_sha[65];
  char legacy_sha[65];
  char unused_sha[65];
  char manifest[4096];
  int manifest_length;
  AbValue resolution_document = {0};
  const AbValue *configuration_sha;
  const AbValue *coverage;
  const AbValue *profile;
  const AbValue *resolution_sha;
  ArchbirdStatus status;

  if (archbird_engine_create(NULL, &engine) != ARCHBIRD_OK)
    return 2;
  ab_buffer_init(&map, engine);
  ab_buffer_init(&resolution, engine);
  ab_buffer_init(&coverage_json, engine);
  ab_buffer_init(&profile_json, engine);
  ab_buffer_init(&verification, engine);
  ab_buffer_init(&plan, engine);
  ab_buffer_init(&selected_plan, engine);
  sha256(main_source, sizeof(main_source) - 1, main_sha);
  sha256(legacy_source, sizeof(legacy_source) - 1, legacy_sha);
  sha256(unused_source, sizeof(unused_source) - 1, unused_sha);
  status = archbird_discovery_resolve(
      engine, (const uint8_t *)CONFIG, sizeof(CONFIG) - 1,
      (const uint8_t *)REQUEST, sizeof(REQUEST) - 1, (const uint8_t *)INVENTORY,
      sizeof(INVENTORY) - 1, 0, collect, &resolution);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, resolution.data, resolution.length,
                                  &resolution_document);
  configuration_sha =
      status == ARCHBIRD_OK
          ? ab_value_member(&resolution_document, "configuration_sha256")
          : NULL;
  coverage = status == ARCHBIRD_OK
                 ? ab_value_member(&resolution_document, "coverage")
                 : NULL;
  profile = status == ARCHBIRD_OK
                ? ab_value_member(&resolution_document, "profile")
                : NULL;
  resolution_sha = status == ARCHBIRD_OK
                       ? ab_value_member(&resolution_document, "sha256")
                       : NULL;
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&coverage_json, coverage);
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&profile_json, profile);
  expect_status("resolve configuration", status, ARCHBIRD_OK, engine);
  if (status != ARCHBIRD_OK || !configuration_sha || !resolution_sha)
    goto cleanup;
  manifest_length = snprintf(
      manifest, sizeof(manifest),
      "{\"artifact\":\"archbird-source-manifest\","
      "\"configuration_sha256\":\"%.*s\",\"files\":[{"
      "\"bytes\":%zu,\"language\":\"c\",\"layer\":\"core\","
      "\"path\":\"src/legacy.h\",\"roles\":[\"source\"],\"sha256\":\"%s\"},"
      "{\"bytes\":%zu,\"language\":\"c\",\"layer\":\"core\","
      "\"path\":\"src/main.c\",\"roles\":[\"source\"],\"sha256\":\"%s\"},{"
      "\"bytes\":%zu,\"language\":\"c\",\"layer\":\"core\","
      "\"path\":\"src/unused.c\",\"roles\":[\"source\"],\"sha256\":\"%s\"}],"
      "\"producer\":{\"implementation_sha256\":\"%s\","
      "\"name\":\"plan-compile-test\",\"version\":\"1\"},"
      "\"project\":\"plan-compile\",\"resolution\":{\"coverage\":%.*s,"
      "\"profile\":%.*s,\"sha256\":\"%.*s\"},\"schema_version\":1}",
      (int)configuration_sha->as.text.length, configuration_sha->as.text.data,
      sizeof(legacy_source) - 1, legacy_sha, sizeof(main_source) - 1, main_sha,
      sizeof(unused_source) - 1, unused_sha, archbird_implementation_sha256(),
      (int)coverage_json.length, coverage_json.data, (int)profile_json.length,
      profile_json.data, (int)resolution_sha->as.text.length,
      resolution_sha->as.text.data);
  if (manifest_length < 0 || (size_t)manifest_length >= sizeof(manifest)) {
    failures++;
    goto cleanup;
  }
  status = archbird_project_create(engine, (const uint8_t *)manifest,
                                   (size_t)manifest_length, &project);
  if (status == ARCHBIRD_OK)
    status = add_source(engine, project, "src/legacy.h", legacy_source,
                        sizeof(legacy_source) - 1);
  if (status == ARCHBIRD_OK)
    status = add_source(engine, project, "src/main.c", main_source,
                        sizeof(main_source) - 1);
  if (status == ARCHBIRD_OK)
    status = add_source(engine, project, "src/unused.c", unused_source,
                        sizeof(unused_source) - 1);
  if (status == ARCHBIRD_OK)
    status = archbird_project_finalize_sources(engine, project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_set_config(
        engine, project, (const uint8_t *)CONFIG, sizeof(CONFIG) - 1);
  if (status == ARCHBIRD_OK)
    status = archbird_project_scan_builtin(engine, project,
                                           ARCHBIRD_PROVIDER_PRIMARY);
  if (status == ARCHBIRD_OK)
    status = archbird_project_finalize_providers(engine, project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_render_map(engine, project, 0, collect, &map);
  expect_status("construct Map", status, ARCHBIRD_OK, engine);
  if (status != ARCHBIRD_OK)
    goto cleanup;

  status = archbird_constraints_evaluate(
      engine, (const uint8_t *)CONFIG, sizeof(CONFIG) - 1, map.data, map.length,
      resolution.data, resolution.length, NULL, 0, 0, collect, &verification);
  expect_status("evaluate constraints", status, ARCHBIRD_OK, engine);
  if (status != ARCHBIRD_OK)
    goto cleanup;

  status = archbird_plan_compile(engine, project, map.data, map.length, NULL, 0,
                                 verification.data, verification.length, NULL,
                                 0, 0, collect, &plan);
  expect_status("compile complete Plan", status, ARCHBIRD_OK, engine);
  if (status != ARCHBIRD_OK) {
    fwrite(verification.data, 1, verification.length, stderr);
    fputc('\n', stderr);
  }
  if (status == ARCHBIRD_OK) {
    expect_status("validate compiled Plan",
                  archbird_plan_validate(engine, plan.data, plan.length),
                  ARCHBIRD_OK, engine);
    if (!contains(&plan, "\"action\":\"delete_file\",\"path\":"
                         "\"src/unused.c\"") ||
        !contains(&plan, "\"action\":\"manual\",\"candidate_paths\":"
                         "[\"src/legacy.h\"]") ||
        !contains(&plan, "Known consumers require a reviewed rewrite") ||
        !contains(&plan, "\"preserved_constraints\":[]")) {
      fprintf(stderr, "FAIL compiled Plan did not distinguish safe deletion "
                      "from a consumed path\n");
      fwrite(plan.data, 1, plan.length, stderr);
      fputc('\n', stderr);
      failures++;
    }
  }

  {
    static const uint8_t request[] = "{\"constraint_ids\":[\"FORBID-UNUSED\"]}";
    status =
        archbird_plan_compile(engine, project, map.data, map.length, NULL, 0,
                              verification.data, verification.length, request,
                              sizeof(request) - 1, 0, collect, &selected_plan);
    expect_status("compile selected Plan", status, ARCHBIRD_OK, engine);
    if (status == ARCHBIRD_OK &&
        (!contains(&selected_plan,
                   "\"preserved_constraints\":[\"FORBID-USED\"]") ||
         contains(&selected_plan, "\"candidate_paths\":[\"src/legacy.h\"]"))) {
      fprintf(stderr, "FAIL selected Plan target/preservation boundary\n");
      failures++;
    }
  }

cleanup:
  ab_value_free(engine, &resolution_document);
  ab_buffer_free(&profile_json);
  ab_buffer_free(&coverage_json);
  ab_buffer_free(&selected_plan);
  ab_buffer_free(&plan);
  ab_buffer_free(&verification);
  ab_buffer_free(&resolution);
  ab_buffer_free(&map);
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
  return failures ? 1 : 0;
}
