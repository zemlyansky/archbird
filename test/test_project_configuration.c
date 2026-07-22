#include <archbird/archbird.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Output {
  uint8_t *data;
  size_t length;
  size_t capacity;
} Output;

static int collect(void *user_data, const uint8_t *bytes, size_t length) {
  Output *output = (Output *)user_data;
  uint8_t *resized;
  size_t needed;
  size_t capacity;
  if (length > SIZE_MAX - output->length)
    return 1;
  needed = output->length + length;
  if (needed > output->capacity) {
    capacity = output->capacity ? output->capacity : 1024;
    while (capacity < needed) {
      if (capacity > SIZE_MAX / 2) {
        capacity = needed;
        break;
      }
      capacity *= 2;
    }
    resized = (uint8_t *)realloc(output->data, capacity);
    if (!resized)
      return 1;
    output->data = resized;
    output->capacity = capacity;
  }
  if (length)
    memcpy(output->data + output->length, bytes, length);
  output->length = needed;
  return 0;
}

static int compile(ArchbirdEngine *engine, const char *json, Output *out) {
  ArchbirdStatus status = archbird_project_configuration_compile(
      engine, (const uint8_t *)json, strlen(json), 0, collect, out);
  if (status != ARCHBIRD_OK) {
    fprintf(stderr, "compile failed: %s\n", archbird_engine_error(engine));
    return 0;
  }
  return 1;
}

static int rejects(ArchbirdEngine *engine, const char *json,
                   const char *message) {
  Output output = {0};
  ArchbirdStatus status = archbird_project_configuration_compile(
      engine, (const uint8_t *)json, strlen(json), 0, collect, &output);
  free(output.data);
  return status == ARCHBIRD_INVALID_SCHEMA &&
         strstr(archbird_engine_error(engine), message) != NULL;
}

int main(void) {
  static const char keyed[] =
      "{\"constraints\":{\"NO-CYCLES\":{\"kind\":\"component_cycles\","
      "\"owner\":\"architecture\",\"rationale\":\"Components remain "
      "acyclic.\"}},"
      "\"layers\":[{\"globs\":[\"**/*.c\"],\"language\":\"c\","
      "\"name\":\"c\"}],"
      "\"project\":\"demo\",\"projections\":{\"public-api\":{"
      "\"paths\":[\"include/**\"],\"select\":\"symbols\"}},"
      "\"queries\":{\"api-impact\":{\"direction\":\"upstream\","
      "\"projection\":\"public-api\"}},\"schema_version\":2}";
  static const char arrays[] =
      "{\"constraints\":[{\"id\":\"NO-CYCLES\",\"kind\":"
      "\"component_cycles\",\"owner\":\"architecture\",\"rationale\":"
      "\"Components remain acyclic.\"}],\"layers\":[{\"globs\":[\"**/*.c\"],"
      "\"language\":\"c\",\"name\":\"c\"}],\"project\":\"demo\","
      "\"projections\":[{"
      "\"id\":\"public-api\",\"paths\":[\"include/**\"],\"select\":"
      "\"symbols\"}],\"queries\":[{\"direction\":\"upstream\",\"id\":"
      "\"api-impact\",\"projection\":\"public-api\"}],"
      "\"schema_version\":2}";
  ArchbirdEngine *engine = NULL;
  Output first = {0};
  Output second = {0};
  if (archbird_engine_create(NULL, &engine) != ARCHBIRD_OK)
    return 1;
  if (!compile(engine, keyed, &first) || !compile(engine, arrays, &second) ||
      first.length != second.length ||
      memcmp(first.data, second.data, first.length) != 0) {
    fprintf(stderr, "keyed and array collection forms did not normalize\n");
    goto failed;
  }
  if (!rejects(engine,
               "{\"project\":\"demo\",\"root\":\".\","
               "\"layers\":[{\"globs\":[\"**/*.c\"],\"language\":\"c\","
               "\"name\":\"c\"}],"
               "\"schema_version\":2}",
               "unknown field 'root'")) {
    fprintf(stderr, "root field was not rejected\n");
    goto failed;
  }
  if (!rejects(engine,
               "{\"checks\":[],\"extractors\":{},"
               "\"layers\":[{\"globs\":[\"**/*.c\"],\"language\":\"c\","
               "\"name\":\"c\"}],\"project\":\"demo\",\"projects\":{},"
               "\"schema_version\":2}",
               "unknown field")) {
    fprintf(stderr, "legacy Verify suite fields were not rejected\n");
    goto failed;
  }
  if (!rejects(engine,
               "{\"layers\":[{\"globs\":[\"**/*.c\"],\"language\":\"c\","
               "\"name\":\"c\"}],\"project\":\"demo\",\"queries\":{"
               "\"empty\":{}},\"schema_version\":2}",
               "requires a projection or focus selector")) {
    fprintf(stderr, "empty named query was not rejected\n");
    goto failed;
  }
  if (!rejects(engine,
               "{\"layers\":[{\"globs\":[\"**/*.c\"],\"language\":\"c\","
               "\"name\":\"c\"}],\"project\":\"demo\",\"queries\":{"
               "\"bad\":{\"search\":[\"engine\"],\"search_limit\":0}},"
               "\"schema_version\":2}",
               "search_limit must be from 1 to 100")) {
    fprintf(stderr, "invalid named-query search limit was not rejected\n");
    goto failed;
  }
  if (!rejects(engine,
               "{\"layers\":[{\"globs\":[\"**/*.c\"],\"language\":\"c\","
               "\"name\":\"c\"}],\"project\":\"demo\","
               "\"queries\":[{\"id\":\"same\"},"
               "{\"id\":\"same\"}],\"schema_version\":2}",
               "duplicate ids")) {
    fprintf(stderr, "duplicate collection ids were not rejected\n");
    goto failed;
  }
  if (!rejects(engine,
               "{\"layers\":[{\"globs\":[\"**/*.c\"],\"language\":\"c\","
               "\"name\":\"c\"}],\"project\":\"demo\","
               "\"queries\":{\"left\":{"
               "\"id\":\"right\"}},\"schema_version\":2}",
               "does not match its key")) {
    fprintf(stderr, "key/id mismatch was not rejected\n");
    goto failed;
  }
  if (!rejects(engine,
               "{\"layers\":[{\"globs\":[\"**/*.c\"],\"language\":\"c\","
               "\"name\":\"c\"}],\"project\":\"demo\",\"projections\":{"
               "\"bad\":{\"select\":\"ranked_symbols\"}},\"schema_version\":2}",
               "unsupported select operator")) {
    fprintf(stderr, "unknown projection operator was not rejected\n");
    goto failed;
  }
  if (!rejects(engine,
               "{\"layers\":[{\"globs\":[\"**/*.c\"],\"language\":\"c\","
               "\"name\":\"c\"}],\"project\":\"demo\",\"queries\":{"
               "\"bad\":{\"projection\":\"missing\"}},\"schema_version\":2}",
               "unknown projection")) {
    fprintf(stderr, "unknown named projection was not rejected\n");
    goto failed;
  }
  if (!rejects(engine,
               "{\"constraints\":{\"bad\":{\"actual\":{\"literal\":[\"x\"]},"
               "\"assert\":\"required_subset\",\"owner\":\"architecture\","
               "\"rationale\":\"A reviewed invariant.\"}},\"layers\":[{"
               "\"globs\":[\"**/*.c\"],\"language\":\"c\",\"name\":\"c\"}],"
               "\"project\":\"demo\",\"schema_version\":2}",
               "assertion requires expected")) {
    fprintf(stderr, "incomplete primitive constraint was not rejected\n");
    goto failed;
  }
  if (!rejects(
          engine,
          "{\"constraints\":{\"bad\":{\"kind\":\"component_membership\","
          "\"max\":1,\"min\":2,\"owner\":\"architecture\",\"rationale\":"
          "\"A reviewed invariant.\"}},\"layers\":[{\"globs\":[\"**/*.c\"],"
          "\"language\":\"c\",\"name\":\"c\"}],\"project\":\"demo\","
          "\"schema_version\":2}",
          "requires min <= max")) {
    fprintf(stderr, "inverted constraint bounds were not rejected\n");
    goto failed;
  }
  free(first.data);
  free(second.data);
  archbird_engine_destroy(engine);
  return 0;

failed:
  free(first.data);
  free(second.data);
  archbird_engine_destroy(engine);
  return 1;
}
