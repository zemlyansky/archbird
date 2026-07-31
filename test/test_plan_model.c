#include <archbird/archbird.h>

#include <stdio.h>
#include <string.h>

#define SHA_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define SHA_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define SHA_C "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"

static int failures;

typedef struct Output {
  char bytes[16384];
  size_t length;
} Output;

static int write_output(void *user_data, const uint8_t *bytes, size_t length) {
  Output *output = (Output *)user_data;
  if (length > sizeof(output->bytes) - output->length - 1)
    return 1;
  memcpy(output->bytes + output->length, bytes, length);
  output->length += length;
  output->bytes[output->length] = '\0';
  return 0;
}

static void expect_status(ArchbirdEngine *engine, const char *name,
                          const char *json, ArchbirdStatus expected) {
  ArchbirdStatus actual =
      archbird_plan_validate(engine, (const uint8_t *)json, strlen(json));
  if (actual != expected) {
    fprintf(stderr, "FAIL %s: status %d, expected %d\n", name, actual,
            expected);
    failures++;
  }
}

int main(void) {
  ArchbirdEngine *engine = NULL;
  const char *empty =
      "{\"artifact\":\"plan\",\"items\":[],\"objective\":\"No work.\","
      "\"preserved_constraints\":[],\"provenance\":\"derived\","
      "\"schema_version\":5,\"source\":{\"map\":{\"configuration_sha256\":"
      "\"" SHA_A "\",\"input_sha256\":\"" SHA_B
      "\",\"producer_implementation_sha256\":\"" SHA_C "\",\"sha256\":\"" SHA_A
      "\"},\"project\":\"demo\",\"verification\":{"
      "\"policy_sha256\":\"" SHA_B
      "\",\"producer_implementation_sha256\":\"" SHA_C "\",\"sha256\":\"" SHA_A
      "\"}},\"tool\":{\"implementation_sha256\":\"" SHA_C
      "\",\"name\":\"archbird\",\"version\":\"test\"},\"unknowns\":[]}";
  const char *manual =
      "{\"artifact\":\"plan\",\"items\":[{\"acceptance\":{\"constraints\":["
      "\"NO-EDGE\"]},\"depends_on\":[],\"evidence\":[],\"executable\":false,"
      "\"id\":\"item:manual\",\"non_executable_reasons\":[\"review\"],"
      "\"operation\":{\"action\":\"manual\",\"candidate_paths\":[\"src/a.c\"],"
      "\"instructions\":\"Choose a replacement.\"},\"origins\":[{"
      "\"constraint_id\":\"NO-EDGE\",\"constraint_result_sha256\":\"" SHA_A
      "\",\"issue_fingerprint\":\"" SHA_B
      "\"}],\"provenance\":\"derived\",\"statement\":\"Redirect edge.\","
      "\"unknowns\":[\"unknown:manual\"]}],\"objective\":\"Repair edge.\","
      "\"preserved_constraints\":[],\"provenance\":\"derived\","
      "\"schema_version\":5,\"source\":{\"map\":{\"configuration_sha256\":"
      "\"" SHA_A "\",\"input_sha256\":\"" SHA_B
      "\",\"producer_implementation_sha256\":\"" SHA_C "\",\"sha256\":\"" SHA_A
      "\"},\"project\":\"demo\",\"verification\":{"
      "\"policy_sha256\":\"" SHA_B
      "\",\"producer_implementation_sha256\":\"" SHA_C "\",\"sha256\":\"" SHA_A
      "\"}},\"tool\":{\"implementation_sha256\":\"" SHA_C
      "\",\"name\":\"archbird\",\"version\":\"test\"},\"unknowns\":[{"
      "\"constraint_id\":\"NO-EDGE\",\"id\":\"unknown:manual\","
      "\"item_id\":\"item:manual\",\"statement\":\"Replacement is "
      "unknown.\"}]}";
  const char *dangling =
      "{\"artifact\":\"plan\",\"items\":[{\"acceptance\":{\"constraints\":["
      "\"NO-EDGE\"]},\"depends_on\":[\"item:missing\"],\"evidence\":[],"
      "\"executable\":false,\"id\":\"item:manual\","
      "\"non_executable_reasons\":[\"review\"],\"operation\":{"
      "\"action\":\"manual\",\"candidate_paths\":[],"
      "\"instructions\":\"Choose a replacement.\"},\"origins\":[{"
      "\"constraint_id\":\"NO-EDGE\",\"constraint_result_sha256\":\"" SHA_A
      "\",\"issue_fingerprint\":\"" SHA_B
      "\"}],\"provenance\":\"derived\",\"statement\":\"Redirect edge.\","
      "\"unknowns\":[]}],\"objective\":\"Repair edge.\","
      "\"preserved_constraints\":[],\"provenance\":\"derived\","
      "\"schema_version\":5,\"source\":{\"map\":{\"configuration_sha256\":"
      "\"" SHA_A "\",\"input_sha256\":\"" SHA_B
      "\",\"producer_implementation_sha256\":\"" SHA_C "\",\"sha256\":\"" SHA_A
      "\"},\"project\":\"demo\",\"verification\":{"
      "\"policy_sha256\":\"" SHA_B
      "\",\"producer_implementation_sha256\":\"" SHA_C "\",\"sha256\":\"" SHA_A
      "\"}},\"tool\":{\"implementation_sha256\":\"" SHA_C
      "\",\"name\":\"archbird\",\"version\":\"test\"},\"unknowns\":[]}";
  if (archbird_engine_create(NULL, &engine) != ARCHBIRD_OK)
    return 2;
  expect_status(engine, "empty", empty, ARCHBIRD_OK);
  expect_status(engine, "manual", manual, ARCHBIRD_OK);
  {
    Output report = {{0}, 0};
    ArchbirdStatus status = archbird_plan_render_markdown(
        engine, (const uint8_t *)manual, strlen(manual), write_output, &report);
    if (status != ARCHBIRD_OK || !strstr(report.bytes, "# Change Plan: demo") ||
        !strstr(report.bytes, "### 1. INPUT REQUIRED `item:manual`") ||
        !strstr(report.bytes, "- Depends on: none") ||
        !strstr(report.bytes, "- Acceptance: `NO-EDGE`") ||
        !strstr(report.bytes,
                "Result: items=1; executable=0; input-required=1; "
                "unknowns=1; preserved-constraints=0; plan=`")) {
      fprintf(stderr, "FAIL markdown report: status %d\n", status);
      failures++;
    }
  }
  expect_status(engine, "dangling", dangling, ARCHBIRD_INVALID_SCHEMA);
  expect_status(engine, "not-object", "[]", ARCHBIRD_INVALID_SCHEMA);
  expect_status(engine, "duplicate-key",
                "{\"artifact\":\"plan\",\"artifact\":\"plan\"}",
                ARCHBIRD_DUPLICATE_KEY);
  archbird_engine_destroy(engine);
  if (failures)
    fprintf(stderr, "%d Plan model test(s) failed\n", failures);
  return failures ? 1 : 0;
}
