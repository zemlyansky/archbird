#include <archbird/archbird.h>

#include "json_value.h"
#include "render_internal.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>

#define VERIFY_SHA                                                             \
  "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"

static const char PROJECT_CONFIG[] =
    "{\"constraints\":{\"UNCHANGED\":{\"actual\":{\"literal\":[\"ok\"]},"
    "\"assert\":\"set_equal\",\"expected\":{\"literal\":[\"ok\"]},"
    "\"owner\":\"architecture\",\"rationale\":\"The acceptance fixture "
    "remains satisfied.\"}},"
    "\"layers\":[{\"globs\":[\"**\"],\"language\":\"c\","
    "\"name\":\"core\"}],\"project\":\"demo\"}";

static const char FAILING_CONFIG[] =
    "{\"constraints\":{\"BROKEN\":{\"actual\":{\"literal\":[]},"
    "\"assert\":\"required_subset\",\"expected\":{\"literal\":[\"needed\"]},"
    "\"owner\":\"architecture\",\"rationale\":\"Exercise acceptance "
    "rejection.\"}},"
    "\"layers\":[{\"globs\":[\"**\"],\"language\":\"c\","
    "\"name\":\"core\"}],\"project\":\"demo\"}";

static int failures;

static int collect(void *user_data, const uint8_t *bytes, size_t length) {
  return ab_buffer_append((AbBuffer *)user_data, bytes, length) == ARCHBIRD_OK
             ? 0
             : 1;
}

static void digest(const uint8_t *bytes, size_t length, char out[65]) {
  uint8_t value[32];
  if (archbird_sha256(bytes, length, value) != ARCHBIRD_OK) {
    memset(out, '0', 64);
    out[64] = '\0';
    return;
  }
  archbird_sha256_hex(value, out);
}

static const AbValue *field(const AbValue *object, const char *name) {
  return ab_value_member(object, name);
}

static const uint8_t *find_bytes(const AbBuffer *buffer, const char *needle) {
  size_t needle_length = strlen(needle);
  size_t index;
  if (needle_length > buffer->length)
    return NULL;
  for (index = 0; index <= buffer->length - needle_length; index++)
    if (memcmp(buffer->data + index, needle, needle_length) == 0)
      return buffer->data + index;
  return NULL;
}

static int render_project(ArchbirdEngine *engine, ArchbirdProject **out_project,
                          AbBuffer *map, char a_sha[65], char b_sha[65],
                          char json_sha[65], char make_sha[65]) {
  static const uint8_t a_bytes[] = "alpha\n";
  static const uint8_t b_bytes[] = "beta\n";
  static const uint8_t json_bytes[] = "{\"name\":\"old\"}\n";
  static const uint8_t make_bytes[] = "WASM_EXPORTS = _old\n";
  char manifest[4096];
  int length;
  ArchbirdStatus status;
  digest(a_bytes, sizeof(a_bytes) - 1, a_sha);
  digest(b_bytes, sizeof(b_bytes) - 1, b_sha);
  digest(json_bytes, sizeof(json_bytes) - 1, json_sha);
  digest(make_bytes, sizeof(make_bytes) - 1, make_sha);
  length = snprintf(
      manifest, sizeof(manifest),
      "{\"artifact\":\"archbird-source-manifest\",\"files\":[{"
      "\"bytes\":%zu,\"language\":\"c\",\"layer\":\"core\","
      "\"path\":\"Makefile\",\"roles\":[\"source\"],\"sha256\":\"%s\"},"
      "{\"bytes\":%zu,\"language\":\"c\",\"layer\":\"core\","
      "\"path\":\"config.json\",\"roles\":[\"source\"],\"sha256\":\"%s\"},{"
      "\"bytes\":%zu,\"language\":\"c\",\"layer\":\"core\","
      "\"path\":\"src/a.c\",\"roles\":[\"source\"],\"sha256\":\"%s\"},"
      "{\"bytes\":%zu,\"language\":\"c\",\"layer\":\"core\","
      "\"path\":\"src/b.c\",\"roles\":[\"source\"],\"sha256\":\"%s\"}],"
      "\"producer\":{\"implementation_sha256\":\"%s\",\"name\":"
      "\"patch-test\",\"version\":\"1\"},\"project\":\"demo\","
      "\"schema_version\":1}",
      sizeof(make_bytes) - 1, make_sha, sizeof(json_bytes) - 1, json_sha,
      sizeof(a_bytes) - 1, a_sha, sizeof(b_bytes) - 1, b_sha,
      archbird_implementation_sha256());
  if (length < 0 || (size_t)length >= sizeof(manifest))
    return 0;
  status = archbird_project_create(engine, (const uint8_t *)manifest,
                                   (size_t)length, out_project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_add_source(engine, *out_project, "Makefile", 8,
                                         make_bytes, sizeof(make_bytes) - 1);
  if (status == ARCHBIRD_OK)
    status =
        archbird_project_add_source(engine, *out_project, "config.json", 11,
                                    json_bytes, sizeof(json_bytes) - 1);
  if (status == ARCHBIRD_OK)
    status = archbird_project_add_source(engine, *out_project, "src/a.c", 7,
                                         a_bytes, sizeof(a_bytes) - 1);
  if (status == ARCHBIRD_OK)
    status = archbird_project_add_source(engine, *out_project, "src/b.c", 7,
                                         b_bytes, sizeof(b_bytes) - 1);
  if (status == ARCHBIRD_OK)
    status = archbird_project_finalize_sources(engine, *out_project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_set_config(engine, *out_project,
                                         (const uint8_t *)PROJECT_CONFIG,
                                         sizeof(PROJECT_CONFIG) - 1);
  if (status == ARCHBIRD_OK)
    status = archbird_project_finalize_providers(engine, *out_project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_render_map(engine, *out_project, 0, collect, map);
  if (status != ARCHBIRD_OK) {
    fprintf(stderr, "FAIL render project: %s\n", archbird_engine_error(engine));
    return 0;
  }
  return 1;
}

static int make_plan(ArchbirdEngine *engine, const AbBuffer *map,
                     const AbBuffer *verification, const char *a_sha,
                     const char *b_sha, const char *json_sha,
                     const char *make_sha, int empty,
                     const char *preserved_constraint, AbBuffer *out) {
  AbValue document = {0};
  AbValue verification_document = {0};
  const AbValue *evidence;
  const AbValue *tool;
  const AbValue *verification_tool;
  const AbValue *verification_policy;
  const AbValue *verification_sha;
  char map_sha[65];
  char items[24576];
  char body[40960];
  int items_length;
  int length;
  ArchbirdStatus status =
      ab_json_value_decode(engine, map->data, map->length, &document);
  if (status != ARCHBIRD_OK)
    return 0;
  status = ab_json_value_decode(engine, verification->data,
                                verification->length, &verification_document);
  if (status != ARCHBIRD_OK) {
    ab_value_free(engine, &document);
    return 0;
  }
  evidence = field(&document, "evidence");
  tool = field(&document, "tool");
  verification_tool = field(&verification_document, "tool");
  verification_policy = field(&verification_document, "policy");
  verification_sha =
      field(&verification_document, "verification_result_sha256");
  digest(map->data, map->length, map_sha);
  if (empty) {
    memcpy(items, "[]", 3);
    items_length = 2;
  } else {
    items_length = snprintf(
        items, sizeof(items),
        "[{\"acceptance\":{\"constraints\":[\"C-REPLACE\"]},"
        "\"depends_on\":[],\"evidence\":[],\"executable\":true,"
        "\"id\":\"item:replace\",\"non_executable_reasons\":[],"
        "\"operation\":{\"action\":\"replace_range\",\"before\":"
        "\"alpha\",\"end_byte\":5,\"path\":\"src/a.c\","
        "\"replacement\":\"ALPHA\",\"source_sha256\":\"%s\","
        "\"start_byte\":0},\"origins\":[{\"constraint_id\":"
        "\"C-REPLACE\",\"constraint_result_sha256\":\"" VERIFY_SHA
        "\",\"issue_fingerprint\":\"" VERIFY_SHA "\"}],"
        "\"provenance\":\"derived\",\"statement\":\"Replace alpha.\","
        "\"unknowns\":[]},{\"acceptance\":{\"constraints\":"
        "[\"C-MOVE\"]},\"depends_on\":[],\"evidence\":[],"
        "\"executable\":true,\"id\":\"item:move\","
        "\"non_executable_reasons\":[],\"operation\":{\"action\":"
        "\"move_file\",\"destination_path\":\"src/z.c\","
        "\"source_path\":\"src/b.c\",\"source_sha256\":\"%s\"},"
        "\"origins\":[{\"constraint_id\":\"C-MOVE\","
        "\"constraint_result_sha256\":\"" VERIFY_SHA
        "\",\"issue_fingerprint\":\"" VERIFY_SHA "\"}],"
        "\"provenance\":\"derived\",\"statement\":\"Move beta.\","
        "\"unknowns\":[]},{\"acceptance\":{\"constraints\":"
        "[\"C-CREATE\"]},\"depends_on\":[],\"evidence\":[],"
        "\"executable\":true,\"id\":\"item:create\","
        "\"non_executable_reasons\":[],\"operation\":{\"action\":"
        "\"create_file\",\"content\":\"new\\n\",\"path\":\"src/c.c\"},"
        "\"origins\":[{\"constraint_id\":\"C-CREATE\","
        "\"constraint_result_sha256\":\"" VERIFY_SHA
        "\",\"issue_fingerprint\":\"" VERIFY_SHA "\"}],"
        "\"provenance\":\"derived\",\"statement\":\"Create new file.\","
        "\"unknowns\":[]},{\"acceptance\":{\"constraints\":"
        "[\"C-JSON\"]},\"depends_on\":[],\"evidence\":[],"
        "\"executable\":true,\"id\":\"item:json\","
        "\"non_executable_reasons\":[],\"operation\":{\"action\":"
        "\"edit_json_pointer\",\"expected\":\"old\","
        "\"expected_absent\":false,\"path\":\"config.json\","
        "\"pointer\":\"/name\",\"replacement\":\"new\","
        "\"source_sha256\":\"%s\"},\"origins\":[{\"constraint_id\":"
        "\"C-JSON\",\"constraint_result_sha256\":\"" VERIFY_SHA
        "\",\"issue_fingerprint\":\"" VERIFY_SHA "\"}],"
        "\"provenance\":\"asserted\",\"statement\":\"Edit JSON value.\","
        "\"unknowns\":[]},{\"acceptance\":{\"constraints\":"
        "[\"C-MAKE\"]},\"depends_on\":[],\"evidence\":[],"
        "\"executable\":true,\"id\":\"item:make-insert\","
        "\"non_executable_reasons\":[],\"operation\":{\"action\":"
        "\"insert_make_variable_token\",\"anchor_token\":\"_old\","
        "\"path\":\"Makefile\",\"position\":\"after\","
        "\"source_sha256\":\"%s\",\"token\":\"_new\","
        "\"variable\":\"WASM_EXPORTS\"},\"origins\":[{\"constraint_id\":"
        "\"C-MAKE\",\"constraint_result_sha256\":\"" VERIFY_SHA
        "\",\"issue_fingerprint\":\"" VERIFY_SHA "\"}],"
        "\"provenance\":\"asserted\",\"statement\":\"Insert new export.\","
        "\"unknowns\":[]},{\"acceptance\":{\"constraints\":"
        "[\"C-MAKE\"]},\"depends_on\":[\"item:make-insert\"],"
        "\"evidence\":[],\"executable\":true,\"id\":\"item:make-remove\","
        "\"non_executable_reasons\":[],\"operation\":{\"action\":"
        "\"edit_make_variable_token\",\"expected_token\":\"_old\","
        "\"path\":\"Makefile\",\"replacement_token\":\"\","
        "\"source_sha256\":\"%s\",\"variable\":\"WASM_EXPORTS\"},"
        "\"origins\":[{\"constraint_id\":\"C-MAKE\","
        "\"constraint_result_sha256\":\"" VERIFY_SHA
        "\",\"issue_fingerprint\":\"" VERIFY_SHA "\"}],"
        "\"provenance\":\"asserted\",\"statement\":\"Remove old export.\","
        "\"unknowns\":[]}]",
        a_sha, b_sha, json_sha, make_sha, make_sha);
  }
  if (items_length < 0 || (size_t)items_length >= sizeof(items)) {
    ab_value_free(engine, &document);
    return 0;
  }
  length = snprintf(
      body, sizeof(body),
      "{\"artifact\":\"plan\",\"items\":%s,\"objective\":\"Exercise native "
      "Plan materialization.\",\"preserved_constraints\":[\"%s\"],"
      "\"provenance\":\"derived\",\"schema_version\":1,\"source\":{"
      "\"map\":{\"configuration_sha256\":\"%.*s\",\"input_sha256\":\"%.*s\","
      "\"producer_implementation_sha256\":\"%.*s\",\"sha256\":\"%s\"},"
      "\"project\":\"demo\",\"verification\":{\"policy_sha256\":\""
      "%.*s\",\"producer_implementation_sha256\":\"%.*s\","
      "\"sha256\":\"%.*s\"}},\"tool\":{"
      "\"implementation_sha256\":\"%s\",\"name\":\"archbird\","
      "\"version\":\"test\"},\"unknowns\":[]}",
      items, preserved_constraint, 64,
      field(evidence, "config_sha256")->as.text.data, 64,
      field(evidence, "input_sha256")->as.text.data, 64,
      field(tool, "implementation_sha256")->as.text.data, map_sha, 64,
      field(verification_policy, "constraint_policy_sha256")->as.text.data, 64,
      field(verification_tool, "implementation_sha256")->as.text.data, 64,
      verification_sha->as.text.data, archbird_implementation_sha256());
  ab_value_free(engine, &verification_document);
  ab_value_free(engine, &document);
  if (length < 0 || (size_t)length >= sizeof(body))
    return 0;
  return ab_buffer_append(out, (const uint8_t *)body, (size_t)length) ==
         ARCHBIRD_OK;
}

static int make_metadata(AbBuffer *out, const char *a_sha, const char *b_sha,
                         const char *json_sha, const char *make_sha) {
  char body[2048];
  int length = snprintf(
      body, sizeof(body),
      "{\"absent\":[\"src/c.c\",\"src/z.c\"],\"files\":[{"
      "\"executable\":false,\"path\":\"Makefile\",\"sha256\":\"%s\"},{"
      "\"executable\":false,\"path\":\"config.json\",\"sha256\":\"%s\"},{"
      "\"executable\":false,\"path\":\"src/a.c\",\"sha256\":\"%s\"},{"
      "\"executable\":false,\"path\":\"src/b.c\",\"sha256\":\"%s\"}]}",
      make_sha, json_sha, a_sha, b_sha);
  return length > 0 && (size_t)length < sizeof(body) &&
         ab_buffer_append(out, (const uint8_t *)body, (size_t)length) ==
             ARCHBIRD_OK;
}

static void expect_status(const char *name, ArchbirdStatus actual,
                          ArchbirdStatus expected, ArchbirdEngine *engine) {
  if (actual != expected) {
    fprintf(stderr, "FAIL %s: status %d, expected %d: %s\n", name, (int)actual,
            (int)expected, archbird_engine_error(engine));
    failures++;
  }
}

int main(void) {
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;
  AbBuffer map;
  AbBuffer verification;
  AbBuffer failing_verification;
  AbBuffer plan;
  AbBuffer empty_plan;
  AbBuffer source_requirements;
  AbBuffer empty_source_requirements;
  AbBuffer metadata;
  AbBuffer patch;
  AbBuffer empty_patch;
  AbBuffer accepted_patch;
  AbBuffer patch_requirements;
  AbBuffer failing_plan;
  AbBuffer failing_patch;
  AbBuffer drift_map;
  char a_sha[65];
  char b_sha[65];
  char json_sha[65];
  char make_sha[65];
  ArchbirdStatus status;
  if (archbird_engine_create(NULL, &engine) != ARCHBIRD_OK)
    return 2;
  ab_buffer_init(&map, engine);
  ab_buffer_init(&verification, engine);
  ab_buffer_init(&failing_verification, engine);
  ab_buffer_init(&plan, engine);
  ab_buffer_init(&empty_plan, engine);
  ab_buffer_init(&source_requirements, engine);
  ab_buffer_init(&empty_source_requirements, engine);
  ab_buffer_init(&metadata, engine);
  ab_buffer_init(&patch, engine);
  ab_buffer_init(&empty_patch, engine);
  ab_buffer_init(&accepted_patch, engine);
  ab_buffer_init(&patch_requirements, engine);
  ab_buffer_init(&failing_plan, engine);
  ab_buffer_init(&failing_patch, engine);
  ab_buffer_init(&drift_map, engine);
  if (!render_project(engine, &project, &map, a_sha, b_sha, json_sha,
                      make_sha)) {
    fprintf(stderr, "FAIL fixture construction\n");
    failures++;
    goto cleanup;
  }
  status = archbird_constraints_evaluate(
      engine, (const uint8_t *)PROJECT_CONFIG, sizeof(PROJECT_CONFIG) - 1,
      map.data, map.length, NULL, 0, NULL, 0, 0, collect, &verification);
  expect_status("evaluate empty policy", status, ARCHBIRD_OK, engine);
  if (status != ARCHBIRD_OK ||
      !make_plan(engine, &map, &verification, a_sha, b_sha, json_sha, make_sha,
                 0, "UNCHANGED", &plan) ||
      !make_plan(engine, &map, &verification, a_sha, b_sha, json_sha, make_sha,
                 1, "UNCHANGED", &empty_plan) ||
      !make_metadata(&metadata, a_sha, b_sha, json_sha, make_sha)) {
    fprintf(stderr, "FAIL fixture construction\n");
    failures++;
    goto cleanup;
  }
  status = archbird_act_source_requirements(engine, plan.data, plan.length, 0,
                                            collect, &source_requirements);
  expect_status("collect source requirements", status, ARCHBIRD_OK, engine);
  if (status == ARCHBIRD_OK &&
      (!find_bytes(&source_requirements,
                   "\"absent\":[\"src/c.c\",\"src/z.c\"]") ||
       !find_bytes(&source_requirements,
                   "\"files\":[\"Makefile\",\"config.json\",\"src/a.c\","
                   "\"src/b.c\"]"))) {
    fprintf(stderr, "FAIL source requirements content/order\n");
    failures++;
  }
  status = archbird_act_source_requirements(engine, empty_plan.data,
                                            empty_plan.length, 0, collect,
                                            &empty_source_requirements);
  expect_status("collect empty source requirements", status, ARCHBIRD_OK,
                engine);
  if (status == ARCHBIRD_OK &&
      (!find_bytes(&empty_source_requirements, "\"absent\":[]") ||
       !find_bytes(&empty_source_requirements, "\"files\":[]"))) {
    fprintf(stderr, "FAIL empty source requirements shape\n");
    failures++;
  }
  status = archbird_act_materialize_patch(
      engine, project, plan.data, plan.length, map.data, map.length,
      verification.data, verification.length, metadata.data, metadata.length, 0,
      collect, &patch);
  expect_status("materialize", status, ARCHBIRD_OK, engine);
  if (status == ARCHBIRD_OK)
    expect_status("validate materialized",
                  archbird_patch_validate(engine, patch.data, patch.length),
                  ARCHBIRD_OK, engine);
  if (!find_bytes(&patch, "\"content_base64\":\"QUxQSEEK\"") ||
      !find_bytes(&patch, "\"kind\":\"create\",\"path\":\"src/c.c\"") ||
      !find_bytes(&patch, "\"kind\":\"move\",\"path\":\"src/z.c\","
                          "\"source_path\":\"src/b.c\"") ||
      !find_bytes(&patch, "\"content_base64\":\"eyJuYW1lIjoibmV3In0K\"") ||
      !find_bytes(&patch,
                  "\"content_base64\":\"V0FTTV9FWFBPUlRTID0gX25ldwo=\"") ||
      find_bytes(&patch, "\"path\":\"src/z.c\"") <
          find_bytes(&patch, "\"path\":\"src/c.c\"")) {
    fprintf(stderr, "FAIL materialized Patch content/order\n");
    failures++;
  }
  status = archbird_constraints_evaluate(
      engine, (const uint8_t *)FAILING_CONFIG, sizeof(FAILING_CONFIG) - 1,
      map.data, map.length, NULL, 0, NULL, 0, 0, collect,
      &failing_verification);
  expect_status("evaluate failing policy", status, ARCHBIRD_OK, engine);
  if (status == ARCHBIRD_OK &&
      make_plan(engine, &map, &failing_verification, a_sha, b_sha, json_sha,
                make_sha, 1, "BROKEN", &failing_plan)) {
    status = archbird_act_materialize_patch(
        engine, project, failing_plan.data, failing_plan.length, map.data,
        map.length, failing_verification.data, failing_verification.length,
        metadata.data, metadata.length, 0, collect, &failing_patch);
    expect_status("materialize failing policy", status, ARCHBIRD_OK, engine);
    if (status == ARCHBIRD_OK)
      expect_status("reject failing policy",
                    archbird_patch_accept(
                        engine, failing_patch.data, failing_patch.length,
                        map.data, map.length, map.data, map.length,
                        failing_verification.data, failing_verification.length,
                        0, collect, &accepted_patch),
                    ARCHBIRD_POLICY_REJECTED, engine);
  } else {
    fprintf(stderr, "FAIL failing policy fixture construction\n");
    failures++;
  }
  {
    static const char empty_description[] = "\"description\":\"\"";
    static const char changed_description[] = "\"description\":\"drift\"";
    const uint8_t *description = find_bytes(&map, empty_description);
    size_t offset = description ? (size_t)(description - map.data) : 0;
    if (description &&
        ab_buffer_append(&drift_map, map.data, offset) == ARCHBIRD_OK &&
        ab_buffer_append(&drift_map, (const uint8_t *)changed_description,
                         sizeof(changed_description) - 1) == ARCHBIRD_OK &&
        ab_buffer_append(&drift_map,
                         description + sizeof(empty_description) - 1,
                         map.length - offset -
                             (sizeof(empty_description) - 1)) == ARCHBIRD_OK) {
      accepted_patch.length = 0;
      status = archbird_patch_accept(
          engine, empty_patch.data, empty_patch.length, drift_map.data,
          drift_map.length, map.data, map.length, verification.data,
          verification.length, 0, collect, &accepted_patch);
      if (status == ARCHBIRD_OK || accepted_patch.length) {
        fprintf(stderr, "FAIL altered before Map produced an accepted Patch\n");
        failures++;
      }
    } else {
      fprintf(stderr, "FAIL construct before Map drift fixture\n");
      failures++;
    }
  }
  status = archbird_act_materialize_patch(
      engine, project, empty_plan.data, empty_plan.length, map.data, map.length,
      verification.data, verification.length, metadata.data, metadata.length, 0,
      collect, &empty_patch);
  expect_status("materialize empty", status, ARCHBIRD_OK, engine);
  if (status == ARCHBIRD_OK)
    expect_status(
        "validate empty",
        archbird_patch_validate(engine, empty_patch.data, empty_patch.length),
        ARCHBIRD_OK, engine);
  if (!find_bytes(&empty_patch, "\"executors\":[],\"plan_sha256\":") ||
      !find_bytes(&empty_patch, "\"transitions\":[]")) {
    fprintf(stderr, "FAIL empty Patch shape\n");
    failures++;
  }
  status = archbird_patch_accept(engine, empty_patch.data, empty_patch.length,
                                 map.data, map.length, map.data, map.length,
                                 verification.data, verification.length, 0,
                                 collect, &accepted_patch);
  expect_status("accept empty", status, ARCHBIRD_OK, engine);
  if (status == ARCHBIRD_OK)
    expect_status("validate accepted",
                  archbird_patch_validate(engine, accepted_patch.data,
                                          accepted_patch.length),
                  ARCHBIRD_OK, engine);
  if (!find_bytes(&accepted_patch, "\"state\":\"accepted\"") ||
      !find_bytes(&accepted_patch, "\"status\":\"satisfied\"") ||
      !find_bytes(&accepted_patch, "\"content_sha256\":")) {
    fprintf(stderr, "FAIL accepted Patch shape\n");
    failures++;
  }
  status = archbird_patch_source_requirements(engine, accepted_patch.data,
                                              accepted_patch.length, 0, collect,
                                              &patch_requirements);
  expect_status("collect accepted Patch requirements", status, ARCHBIRD_OK,
                engine);
  if (status == ARCHBIRD_OK &&
      (!find_bytes(&patch_requirements, "\"absent\":[]") ||
       !find_bytes(&patch_requirements, "\"files\":[]"))) {
    fprintf(stderr, "FAIL accepted Patch requirements shape\n");
    failures++;
  }
  expect_status("reject materialized Patch requirements",
                archbird_patch_source_requirements(
                    engine, empty_patch.data, empty_patch.length, 0, collect,
                    &patch_requirements),
                ARCHBIRD_POLICY_REJECTED, engine);
  expect_status("preflight accepted",
                archbird_patch_preflight_apply(engine, accepted_patch.data,
                                               accepted_patch.length,
                                               metadata.data, metadata.length),
                ARCHBIRD_OK, engine);
  expect_status("reject materialized apply",
                archbird_patch_preflight_apply(engine, empty_patch.data,
                                               empty_patch.length,
                                               metadata.data, metadata.length),
                ARCHBIRD_POLICY_REJECTED, engine);
  {
    uint8_t *locked_sha = (uint8_t *)find_bytes(&metadata, b_sha);
    if (!locked_sha) {
      fprintf(stderr, "FAIL locate metadata source lock\n");
      failures++;
      goto cleanup;
    }
    locked_sha[0] = locked_sha[0] == '0' ? '1' : '0';
  }
  expect_status("stale metadata",
                archbird_act_materialize_patch(
                    engine, project, plan.data, plan.length, map.data,
                    map.length, verification.data, verification.length,
                    metadata.data, metadata.length, 0, collect, &empty_patch),
                ARCHBIRD_CONFLICT, engine);

cleanup:
  archbird_project_destroy(project);
  ab_buffer_free(&drift_map);
  ab_buffer_free(&failing_patch);
  ab_buffer_free(&failing_plan);
  ab_buffer_free(&patch_requirements);
  ab_buffer_free(&accepted_patch);
  ab_buffer_free(&empty_patch);
  ab_buffer_free(&patch);
  ab_buffer_free(&metadata);
  ab_buffer_free(&empty_source_requirements);
  ab_buffer_free(&source_requirements);
  ab_buffer_free(&empty_plan);
  ab_buffer_free(&plan);
  ab_buffer_free(&verification);
  ab_buffer_free(&failing_verification);
  ab_buffer_free(&map);
  archbird_engine_destroy(engine);
  if (failures)
    fprintf(stderr, "%d Patch materialization test(s) failed\n", failures);
  return failures ? 1 : 0;
}
