#include "manifests/autoconf_manifest.h"
#include "map_internal.h"
#include "sha256.h"
#include <archbird/archbird.h>

#include <stdio.h>
#include <string.h>

typedef struct Output {
  char bytes[65536];
  size_t length;
} Output;

static int collect(void *user_data, const uint8_t *bytes, size_t length) {
  Output *output = (Output *)user_data;
  if (length > sizeof(output->bytes) - output->length - 1)
    return 1;
  memcpy(output->bytes + output->length, bytes, length);
  output->length += length;
  output->bytes[output->length] = '\0';
  return 0;
}

static int string_equals(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value->length == length && !memcmp(value->data, literal, length);
}

static int test_autoconf_metadata(ArchbirdEngine *engine) {
  static const uint8_t source[] =
      "AC_DEFUN([NOT_CALLED], [AC_CONFIG_FILES([ignored])])\n"
      "m4_define([VERSION], [1.2.3])\n"
      "AC_INIT([demo], [VERSION], [bugs@example.invalid])\n"
      "AS_IF([test x = y], [AC_CONFIG_SUBDIRS([vendor/lib])])\n"
      "AC_CONFIG_HEADERS([config.h:config.hin])\n"
      "AC_CONFIG_FILES(m4_normalize([\n"
      "  Makefile\n"
      "  src/Makefile:src/Makefile.in\n"
      "]))\n"
      "AC_CONFIG_FILES([Makefile])\n"
      "AC_CONFIG_LINKS([linked:source])\n"
      "AC_CONFIG_FILES([$DYNAMIC_OUTPUT])\n"
      "AC_OUTPUT([legacy/Makefile])\n";
  AbAutoconfMetadata metadata = {0};
  int failed = 0;
  if (ab_autoconf_metadata(engine, source, sizeof(source) - 1, &metadata) !=
      ARCHBIRD_OK) {
    fprintf(stderr, "FAIL parse Autoconf metadata: %s\n",
            archbird_engine_error(engine));
    return 1;
  }
  if (!string_equals(&metadata.package, "demo") ||
      !string_equals(&metadata.version, "1.2.3") || !metadata.has_output ||
      metadata.files.count != 4 ||
      !string_equals(&metadata.files.items[0], "Makefile") ||
      !string_equals(&metadata.files.items[1], "legacy/Makefile") ||
      !string_equals(&metadata.files.items[2], "linked") ||
      !string_equals(&metadata.files.items[3], "src/Makefile") ||
      metadata.headers.count != 1 ||
      !string_equals(&metadata.headers.items[0], "config.h") ||
      metadata.subdirectories.count != 1 ||
      !string_equals(&metadata.subdirectories.items[0], "vendor/lib")) {
    fputs("FAIL bounded Autoconf literal extraction\n", stderr);
    failed = 1;
  }
  ab_autoconf_metadata_free(engine, &metadata);
  return failed;
}

static int test_autoconf_routes(ArchbirdEngine *engine) {
  static const char source[] = "AC_INIT([demo], [1.2.3])\n"
                               "AC_CONFIG_HEADERS([config.h])\n"
                               "AC_CONFIG_FILES([Makefile src/Makefile])\n"
                               "AC_CONFIG_SUBDIRS([vendor/lib])\n"
                               "AC_OUTPUT\n";
  static const char config[] =
      "{\"builds\":[{\"kind\":\"autoconf\",\"name\":\"autoconf\","
      "\"path\":\"configure.ac\"}],\"layers\":[{\"globs\":["
      "\"configure.ac\"],\"language\":\"c\",\"name\":\"build\"}],"
      "\"project\":\"demo\",\"schema_version\":1}";
  static const char implementation[] =
      "1111111111111111111111111111111111111111111111111111111111111111";
  uint8_t digest[32];
  char hex[65];
  char manifest[1024];
  int manifest_length;
  ArchbirdProject *project = NULL;
  Output output = {{0}, 0};
  ArchbirdStatus status;
  if (archbird_sha256((const uint8_t *)source, sizeof(source) - 1, digest) !=
      ARCHBIRD_OK)
    return 1;
  archbird_sha256_hex(digest, hex);
  manifest_length =
      snprintf(manifest, sizeof(manifest),
               "{\"artifact\":\"archbird-source-manifest\",\"files\":[{"
               "\"bytes\":%zu,\"language\":\"c\",\"layer\":\"build\","
               "\"path\":\"configure.ac\",\"roles\":[\"build\"],\"sha256\":"
               "\"%s\"}],\"producer\":{\"implementation_sha256\":\"%s\","
               "\"name\":\"autoconf-test\",\"version\":\"1\"},\"project\":"
               "\"demo\",\"schema_version\":1}",
               sizeof(source) - 1, hex, implementation);
  if (manifest_length < 0 || (size_t)manifest_length >= sizeof(manifest))
    return 1;
  status = archbird_project_create(engine, (const uint8_t *)manifest,
                                   (size_t)manifest_length, &project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_add_source(engine, project, "configure.ac", 12,
                                         (const uint8_t *)source,
                                         sizeof(source) - 1);
  if (status == ARCHBIRD_OK)
    status = archbird_project_finalize_sources(engine, project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_set_config(
        engine, project, (const uint8_t *)config, sizeof(config) - 1);
  if (status == ARCHBIRD_OK)
    status = archbird_project_finalize_providers(engine, project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_render_map(engine, project, 0, collect, &output);
  if (status != ARCHBIRD_OK ||
      !strstr(output.bytes, "{\"command\":\"autoreconf -i\",\"conditions\":[],"
                            "\"deps\":[],\"name\":\"autoreconf\",\"paths\":["
                            "\"configure\"],\"source\":\"configure.ac\"}") ||
      !strstr(output.bytes,
              "{\"command\":\"./configure\",\"conditions\":[],"
              "\"deps\":[\"autoreconf\",\"vendor/lib/configure\"],"
              "\"name\":\"configure\",\"paths\":[\"Makefile\","
              "\"config.h\",\"config.status\",\"src/Makefile\"],"
              "\"source\":\"configure.ac\"}")) {
    fprintf(stderr, "FAIL rendered Autoconf routes: status=%d error=%s\n",
            (int)status, archbird_engine_error(engine));
    archbird_project_destroy(project);
    return 1;
  }
  archbird_project_destroy(project);
  return 0;
}

int main(void) {
  static const uint8_t source[] = "SELF := $(SELF)\n"
                                  "A = one\n"
                                  "VALUE := $(A)\n"
                                  "A = two\n";
  ArchbirdEngine *engine = NULL;
  AbString name = {(char *)"SELF", 4};
  AbString value = {0};
  int found = 0;
  if (archbird_engine_create(NULL, &engine) != ARCHBIRD_OK)
    return 1;
  if (test_autoconf_metadata(engine) || test_autoconf_routes(engine)) {
    archbird_engine_destroy(engine);
    return 1;
  }
  if (ab_make_variable_value(engine, source, sizeof(source) - 1, &name, &value,
                             &found) != ARCHBIRD_OK ||
      !found || value.length != 7 || memcmp(value.data, "$(SELF)", 7) != 0) {
    fputs("FAIL recursive Make value\n", stderr);
    ab_string_free(engine, &value);
    archbird_engine_destroy(engine);
    return 1;
  }
  ab_string_free(engine, &value);
  name.data = (char *)"VALUE";
  name.length = 5;
  found = 0;
  if (ab_make_variable_value(engine, source, sizeof(source) - 1, &name, &value,
                             &found) != ARCHBIRD_OK ||
      !found || value.length != 3 || memcmp(value.data, "one", 3) != 0) {
    fputs("FAIL immediate Make value\n", stderr);
    ab_string_free(engine, &value);
    archbird_engine_destroy(engine);
    return 1;
  }
  ab_string_free(engine, &value);
  archbird_engine_destroy(engine);
  return 0;
}
