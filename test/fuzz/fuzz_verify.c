#include "fuzz_common.h"

#include <string.h>

static const uint8_t fixed_suite[] =
    "{\"schema_version\":1,\"suite\":\"fuzz\",\"projects\":{\"subject\":"
    "{\"map\":\"subject.json\"}},\"extractors\":{\"expected\":{\"kind\":"
    "\"literal_set\",\"values\":[\"A\"]},\"actual\":{\"kind\":"
    "\"literal_set\",\"values\":[\"B\"]}},\"checks\":[{\"id\":"
    "\"FUZZ-SET\",\"assert\":\"set_equal\",\"expected\":\"expected\","
    "\"actual\":\"actual\",\"owner\":\"fuzz\",\"rationale\":"
    "\"Exercise native verification.\"}]}";

static const uint8_t fixed_input[] =
    "{\"schema_version\":1,\"artifact\":\"verification-input\","
    "\"suite_path\":\"fuzz.verify.json\",\"projects\":[{\"name\":"
    "\"subject\",\"map\":{\"artifact\":\"map\",\"schema_version\":6,"
    "\"project\":\"fuzz\",\"evidence\":{\"config_sha256\":"
    "\"1111111111111111111111111111111111111111111111111111111111111111\","
    "\"input_sha256\":"
    "\"2222222222222222222222222222222222222222222222222222222222222222\"},"
    "\"tool\":{\"name\":\"archbird\",\"version\":\"fixture\","
    "\"implementation_sha256\":"
    "\"3333333333333333333333333333333333333333333333333333333333333333\"},"
    "\"diagnostics\":[]},\"sources\":[]}],\"provided_facts\":[],"
    "\"attestations\":[],\"baseline\":null}";

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  ArchbirdEngine *engine = fuzz_engine();
  ArchbirdVerificationFormat format;
  if (!engine)
    return 0;
#ifdef ARCHBIRD_FUZZ_VERIFICATION_INPUT
  (void)archbird_verification_analyze(engine, fixed_suite,
                                      sizeof(fixed_suite) - 1, data, size, 0,
                                      fuzz_discard, NULL);
  for (format = ARCHBIRD_VERIFICATION_MARKDOWN;
       format <= ARCHBIRD_VERIFICATION_JUNIT;
       format = (ArchbirdVerificationFormat)(format + 1))
    (void)archbird_verification_analyze_report(
        engine, fixed_suite, sizeof(fixed_suite) - 1, data, size, format, 200,
        0, fuzz_discard, NULL);
#else
  (void)archbird_verification_plan(engine, data, size, 0, fuzz_discard, NULL);
  (void)archbird_verification_analyze(engine, data, size, fixed_input,
                                      sizeof(fixed_input) - 1, 0, fuzz_discard,
                                      NULL);
  for (format = ARCHBIRD_VERIFICATION_MARKDOWN;
       format <= ARCHBIRD_VERIFICATION_JUNIT;
       format = (ArchbirdVerificationFormat)(format + 1))
    (void)archbird_verification_analyze_report(engine, data, size, fixed_input,
                                               sizeof(fixed_input) - 1, format,
                                               200, 0, fuzz_discard, NULL);
#endif
  archbird_engine_destroy(engine);
  return 0;
}
