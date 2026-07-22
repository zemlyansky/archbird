#include <archbird/archbird.h>

#include <stdio.h>
#include <string.h>

typedef struct Output {
  uint8_t bytes[131072];
  size_t length;
} Output;

static int collect(void *user_data, const uint8_t *bytes, size_t length) {
  Output *output = (Output *)user_data;
  if (length > sizeof(output->bytes) - output->length)
    return 1;
  memcpy(output->bytes + output->length, bytes, length);
  output->length += length;
  return 0;
}

static int contains(const Output *output, const char *needle) {
  size_t length = strlen(needle);
  size_t index;
  for (index = 0; index + length <= output->length; index++) {
    if (!memcmp(output->bytes + index, needle, length))
      return 1;
  }
  return 0;
}

static int resolve(ArchbirdEngine *engine, const char *config,
                   const char *request, const char *inventory, Output *out) {
  ArchbirdStatus status = archbird_discovery_resolve(
      engine, (const uint8_t *)config, strlen(config), (const uint8_t *)request,
      strlen(request), (const uint8_t *)inventory, strlen(inventory), 0,
      collect, out);
  if (status != ARCHBIRD_OK) {
    fprintf(stderr, "resolution failed: %s\n", archbird_engine_error(engine));
    return 0;
  }
  return 1;
}

static int rejects(ArchbirdEngine *engine, const char *config,
                   const char *request, const char *inventory,
                   const char *message) {
  Output output = {{0}, 0};
  ArchbirdStatus status = archbird_discovery_resolve(
      engine, (const uint8_t *)config, strlen(config), (const uint8_t *)request,
      strlen(request), (const uint8_t *)inventory, strlen(inventory), 0,
      collect, &output);
  return status == ARCHBIRD_LIMIT_EXCEEDED &&
         strstr(archbird_engine_error(engine), message) != NULL;
}

int main(void) {
  static const char request[] =
      "{\"artifact\":\"archbird-map-request\",\"default_excludes\":true,"
      "\"exclude\":[],\"ignore\":true,\"only\":[],\"schema_version\":1,"
      "\"sources\":[]}";
  static const char inventory[] =
      "{\"artifact\":\"archbird-repository-inventory\",\"documents\":["
      "{\"content_hex\":"
      "\"7b226e616d65223a224061726368626972642f64656d6f222c2276657273696f6e223a"
      "22312e322e33227d\",\"path\":\"package.json\"}],"
      "\"files\":["
      "{\"bytes\":12,\"path\":\"src/main.py\"},"
      "{\"bytes\":4,\"path\":\"ignored.py\"},"
      "{\"bytes\":8,\"path\":\"Makefile\"},"
      "{\"bytes\":20,\"path\":\"tests/test_main.py\"},"
      "{\"bytes\":20,\"path\":\"test.js\"},"
      "{\"bytes\":20,\"path\":\"test.config.js\"},"
      "{\"bytes\":5000001,\"path\":\"huge.py\"},"
      "{\"bytes\":10,\"path\":\"code.rs\"},"
      "{\"bytes\":15,\"path\":\"package.json\"},"
      "{\"bytes\":3,\"path\":\"build/leak.py\"},"
      "{\"bytes\":11,\"path\":\".gitignore\"}],"
      "\"ignore_files\":[{\"content_hex\":\"69676e6f7265642e70790a\",\"path\":"
      "\".gitignore\"}],"
      "\"schema_version\":1}";
  static const char inventory_reversed[] =
      "{\"artifact\":\"archbird-repository-inventory\",\"documents\":["
      "{\"content_hex\":"
      "\"7b226e616d65223a224061726368626972642f64656d6f222c2276657273696f6e223a"
      "22312e322e33227d\",\"path\":\"package.json\"}],"
      "\"files\":["
      "{\"bytes\":11,\"path\":\".gitignore\"},"
      "{\"bytes\":3,\"path\":\"build/leak.py\"},"
      "{\"bytes\":15,\"path\":\"package.json\"},"
      "{\"bytes\":10,\"path\":\"code.rs\"},"
      "{\"bytes\":5000001,\"path\":\"huge.py\"},"
      "{\"bytes\":20,\"path\":\"tests/test_main.py\"},"
      "{\"bytes\":20,\"path\":\"test.config.js\"},"
      "{\"bytes\":20,\"path\":\"test.js\"},"
      "{\"bytes\":8,\"path\":\"Makefile\"},"
      "{\"bytes\":4,\"path\":\"ignored.py\"},"
      "{\"bytes\":12,\"path\":\"src/main.py\"}],"
      "\"ignore_files\":[{\"content_hex\":\"69676e6f7265642e70790a\",\"path\":"
      "\".gitignore\"}],"
      "\"schema_version\":1}";
  static const char python_inventory[] =
      "{\"artifact\":\"archbird-repository-inventory\",\"documents\":["
      "{\"content_hex\":\"5b70726f6a6563745d0a6e616d65203d202264656d6f2d"
      "707974686f6e220a76657273696f6e203d2022322e302e30220a0a5b746f6f6c"
      "2e666c69742e6d6f64756c655d0a6e616d65203d202264656d6f5f6d6f64756c"
      "65220a\",\"path\":\"pyproject.toml\"}],\"files\":["
      "{\"bytes\":91,\"path\":\"pyproject.toml\"},"
      "{\"bytes\":21,\"path\":\"src/demo_module/__init__.py\"}],"
      "\"ignore_files\":[],\"schema_version\":1}";
  static const char autoconf_inventory[] =
      "{\"artifact\":\"archbird-repository-inventory\",\"documents\":["
      "{\"content_hex\":\"6d345f646566696e65285b56455253494f4e5d2c205b322e"
      "345d290a41435f494e4954285b6e61746976652d64656d6f5d2c205b56455253494f"
      "4e5d290a41435f434f4e4649475f46494c4553285b4d616b6566696c655d290a4143"
      "5f4f55545055540a\",\"path\":\"configure.ac\"}],\"files\":["
      "{\"bytes\":100,\"path\":\"configure.ac\"},"
      "{\"bytes\":20,\"path\":\"src/main.c\"}],\"ignore_files\":[],"
      "\"schema_version\":1}";
  static const char venv_inventory[] =
      "{\"artifact\":\"archbird-repository-inventory\",\"documents\":[],"
      "\"files\":[{\"bytes\":12,\"path\":\"venv/bin/activate.py\"},"
      "{\"bytes\":12,\"path\":\"Lib/venv/__init__.py\"},"
      "{\"bytes\":12,\"path\":\"Lib/.venv/hidden.py\"}],"
      "\"ignore_files\":[],\"schema_version\":1}";
  static const char r_inventory[] =
      "{\"artifact\":\"archbird-repository-inventory\",\"documents\":["
      "{\"content_hex\":\"5061636b6167653a207a65726f520a56657273696f6e3a"
      "20312e322e330a\",\"path\":\"DESCRIPTION\"}],\"files\":["
      "{\"bytes\":32,\"path\":\"DESCRIPTION\"},"
      "{\"bytes\":20,\"path\":\"NAMESPACE\"},"
      "{\"bytes\":24,\"path\":\"R/api.R\"}],"
      "\"ignore_files\":[],\"schema_version\":1}";
  static const char cpp_inventory[] =
      "{\"artifact\":\"archbird-repository-inventory\",\"documents\":[],"
      "\"files\":[{\"bytes\":20,\"path\":\"include/api.h\"},"
      "{\"bytes\":20,\"path\":\"src/api.cc\"}],"
      "\"ignore_files\":[],\"schema_version\":1}";
  static const char mixed_c_cpp_inventory[] =
      "{\"artifact\":\"archbird-repository-inventory\",\"documents\":[],"
      "\"files\":[{\"bytes\":20,\"path\":\"include/api.h\"},"
      "{\"bytes\":20,\"path\":\"src/api.c\"},"
      "{\"bytes\":20,\"path\":\"src/api.cc\"}],"
      "\"ignore_files\":[],\"schema_version\":1}";
  static const char compiler_inventory[] =
      "{\"artifact\":\"archbird-repository-inventory\",\"documents\":[],"
      "\"files\":[{\"bytes\":2,\"path\":\"compile_commands.json\"},"
      "{\"bytes\":1,\"path\":\"index.scip\"},"
      "{\"bytes\":20,\"path\":\"src/main.c\"}],"
      "\"ignore_files\":[],\"schema_version\":1}";
  static const char configured[] =
      "{\"layers\":[{\"globs\":[\"**/*.py\"],\"language\":\"python\","
      "\"name\":\"configured\"}],\"project\":\"base\",\"schema_version\":2}";
  static const char override_request[] =
      "{\"artifact\":\"archbird-map-request\",\"default_excludes\":true,"
      "\"exclude\":[],\"ignore\":false,\"only\":[\"src/**\"],"
      "\"project\":\"cli\",\"schema_version\":1,\"sources\":[]}";
  static const char index_config[] =
      "{\"indexes\":[{\"format\":\"scip\",\"name\":\"semantic\","
      "\"path\":\"semantic.scip\"}],\"layers\":[{\"globs\":[\"src/**\"],"
      "\"language\":\"python\",\"name\":\"source\"}],\"limits\":{"
      "\"max_file_bytes\":100,\"max_index_bytes\":1000},\"project\":"
      "\"bounded\",\"schema_version\":2}";
  static const char index_inventory[] =
      "{\"artifact\":\"archbird-repository-inventory\",\"documents\":[],"
      "\"files\":[{\"bytes\":500,\"path\":\"semantic.scip\"},{\"bytes\":"
      "12,\"path\":\"src/main.py\"}],\"ignore_files\":[],"
      "\"schema_version\":1}";
  static const char index_override_request[] =
      "{\"artifact\":\"archbird-map-request\",\"exclude\":[],"
      "\"ignore_files\":[],\"max_index_bytes\":400,\"only\":[],"
      "\"schema_version\":1,\"sources\":[]}";
  static const char vendor_config[] =
      "{\"layers\":[{\"globs\":[\"deps/*.c\"],\"language\":\"c\","
      "\"name\":\"dependencies\",\"role\":\"vendor\"}],\"project\":"
      "\"roles\",\"schema_version\":2}";
  static const char vendor_inventory[] =
      "{\"artifact\":\"archbird-repository-inventory\",\"documents\":[],"
      "\"files\":[{\"bytes\":12,\"path\":\"deps/library.c\"}],"
      "\"ignore_files\":[],\"schema_version\":1}";
  ArchbirdEngine *engine = NULL;
  Output first = {{0}, 0};
  Output second = {{0}, 0};
  Output override = {{0}, 0};
  Output scoped = {{0}, 0};
  Output python = {{0}, 0};
  Output autoconf = {{0}, 0};
  Output venv = {{0}, 0};
  Output r = {{0}, 0};
  Output cpp = {{0}, 0};
  Output mixed_c_cpp = {{0}, 0};
  Output compiler = {{0}, 0};
  Output compiler_scoped = {{0}, 0};
  Output index = {{0}, 0};
  Output vendor = {{0}, 0};
  ArchbirdDiscovery *discovery = NULL;
  int descend_temp = 1;
  int descend_src = 0;
  int descend_root_venv = 1;
  int descend_nested_venv = 0;
  int descend_nested_dot_venv = 1;
  int failed = 0;
  if (archbird_engine_create(NULL, &engine) != ARCHBIRD_OK)
    return 2;
  if (archbird_discovery_create(engine, (const uint8_t *)configured,
                                sizeof(configured) - 1,
                                &discovery) != ARCHBIRD_OK ||
      archbird_discovery_add_ignore(engine, discovery, ".gitignore", 10,
                                    (const uint8_t *)"temp/\n",
                                    6) != ARCHBIRD_OK ||
      archbird_discovery_should_descend(engine, discovery, "temp", 4,
                                        &descend_temp) != ARCHBIRD_OK ||
      archbird_discovery_should_descend(engine, discovery, "src", 3,
                                        &descend_src) != ARCHBIRD_OK ||
      archbird_discovery_should_descend(engine, discovery, "venv", 4,
                                        &descend_root_venv) != ARCHBIRD_OK ||
      archbird_discovery_should_descend(engine, discovery, "Lib/venv", 8,
                                        &descend_nested_venv) != ARCHBIRD_OK ||
      archbird_discovery_should_descend(engine, discovery, "Lib/.venv", 9,
                                        &descend_nested_dot_venv) !=
          ARCHBIRD_OK ||
      descend_temp || !descend_src || descend_root_venv ||
      !descend_nested_venv || descend_nested_dot_venv) {
    fprintf(stderr, "incremental ignore descent is incorrect\n");
    failed = 1;
  }
  archbird_discovery_destroy(discovery);
  if (!resolve(engine, "", request, inventory, &first) ||
      !resolve(engine, "", request, inventory_reversed, &second) ||
      first.length != second.length ||
      memcmp(first.bytes, second.bytes, first.length)) {
    fprintf(stderr, "config-free resolution is not deterministic\n");
    failed = 1;
  }
  if (!contains(&first, "\"project\":\"demo\"") ||
      !contains(&first, "\"identity\":\"@archbird/demo\"") ||
      !contains(&first, "\"path\":\"src/main.py\"") ||
      !contains(&first, "\"path\":\"tests/test_main.py\"") ||
      !contains(
          &first,
          "\"path\":\"test.js\",\"roles\":[\"source\",\"test-candidate\"]") ||
      !contains(&first, "\"path\":\"test.config.js\",\"roles\":[\"source\"]") ||
      !contains(&first, "test-candidate") ||
      !contains(&first, "discovery-file-oversized") ||
      !contains(&first, "\"path\":\"huge.py\",\"severity\":\"warning\"") ||
      !contains(&first, "\"unsupported_known\":1") ||
      contains(&first, "\"layer\":\"auto-python\",\"path\":\"ignored.py\"") ||
      contains(&first, "\"layer\":\"auto-python\",\"path\":"
                       "\"build/leak.py\"")) {
    fprintf(stderr, "config-free selection evidence is incorrect\n");
    failed = 1;
  }
  if (!resolve(engine, configured, override_request, inventory, &override) ||
      !contains(&override, "\"project\":\"cli\"") ||
      !contains(&override, "\"path\":\"src/main.py\"") ||
      contains(&override, "\"layer\":\"configured\",\"path\":"
                          "\"tests/test_main.py\"")) {
    fprintf(stderr, "CLI/config/discovery precedence is incorrect\n");
    failed = 1;
  }
  if (!resolve(engine, "", override_request, inventory, &scoped) ||
      !contains(&scoped, "\"selected\":1") ||
      !contains(&scoped, "\"packages\":[]") ||
      !contains(&scoped, "\"builds\":[]") ||
      !contains(&scoped, "\"path\":\"src/main.py\"") ||
      contains(&scoped, "\"packages\":[{") ||
      contains(&scoped, "\"builds\":[{")) {
    fprintf(stderr, "selected discovery scope retained inferred records\n");
    failed = 1;
  }
  if (!resolve(engine, "", request, python_inventory, &python) ||
      !contains(&python, "\"project\":\"demo-python\"") ||
      !contains(&python, "\"identity\":\"demo-python\"") ||
      !contains(&python, "\"aliases\":[\"demo_module\"]") ||
      !contains(&python, "\"kind\":\"python\"") ||
      !contains(&python, "\"name\":\"python-root\"") ||
      !contains(&python, "\"path\":\"pyproject.toml\"") ||
      !contains(&python, "\"evidence\":[\"pyproject.toml\"]") ||
      !contains(&python, "\"selected\":2")) {
    fprintf(stderr, "zero-config Python package evidence is incorrect\n");
    failed = 1;
  }
  if (!resolve(engine, "", request, autoconf_inventory, &autoconf) ||
      !contains(&autoconf, "\"project\":\"native-demo\"") ||
      !contains(&autoconf, "\"identity\":\"native-demo\"") ||
      !contains(&autoconf, "\"kind\":\"generic\"") ||
      !contains(&autoconf, "\"name\":\"autoconf-root\"") ||
      !contains(&autoconf, "\"version\":\"2.4\"") ||
      !contains(&autoconf, "\"kind\":\"autoconf\"") ||
      !contains(&autoconf, "\"path\":\"configure.ac\"") ||
      !contains(&autoconf, "\"selected\":2")) {
    fprintf(stderr, "zero-config Autoconf evidence is incorrect\n");
    failed = 1;
  }
  if (!resolve(engine, "", request, venv_inventory, &venv) ||
      !contains(&venv, "\"selected\":1") ||
      !contains(&venv, "\"files\":[{\"language\":\"python\","
                       "\"layer\":\"auto-python\",\"path\":"
                       "\"Lib/venv/__init__.py\"") ||
      contains(&venv, "\"layer\":\"auto-python\",\"path\":"
                      "\"venv/bin/activate.py\"") ||
      contains(&venv, "\"layer\":\"auto-python\",\"path\":"
                      "\"Lib/.venv/hidden.py\"")) {
    fprintf(stderr, "ambiguous nested venv discovery is incorrect\n");
    failed = 1;
  }
  if (!resolve(engine, "", request, r_inventory, &r) ||
      !contains(&r, "\"project\":\"zeroR\"") ||
      !contains(&r, "\"identity\":\"zeroR\"") ||
      !contains(&r, "\"kind\":\"r\"") || !contains(&r, "\"name\":\"r-root\"") ||
      !contains(&r, "\"path\":\"DESCRIPTION\"") ||
      !contains(&r, "\"evidence\":[\"DESCRIPTION\"]") ||
      !contains(&r, "\"selected\":3")) {
    fprintf(stderr, "zero-config CRAN package evidence is incorrect\n");
    failed = 1;
  }
  if (!resolve(engine, "", request, cpp_inventory, &cpp) ||
      !contains(&cpp, "\"language\":\"cpp\",\"layer\":\"auto-cpp\","
                      "\"path\":\"include/api.h\"") ||
      contains(&cpp, "\"language\":\"c\",\"layer\":\"auto-c\","
                     "\"path\":\"include/api.h\"")) {
    fprintf(stderr, "unambiguous C++ header inference is incorrect\n");
    failed = 1;
  }
  if (!resolve(engine, "", request, mixed_c_cpp_inventory, &mixed_c_cpp) ||
      !contains(&mixed_c_cpp, "\"language\":\"c\",\"layer\":\"auto-c\","
                              "\"path\":\"include/api.h\"")) {
    fprintf(stderr, "mixed C/C++ header inference is not conservative\n");
    failed = 1;
  }
  if (!resolve(engine, "", request, compiler_inventory, &compiler) ||
      !contains(&compiler,
                "\"kind\":\"compile_commands\",\"name\":"
                "\"compile_commands\",\"path\":\"compile_commands.json\","
                "\"variant\":\"default\"") ||
      !contains(&compiler, "\"format\":\"scip\",\"name\":\"scip\",\"path\":"
                           "\"index.scip\",\"required\":true") ||
      !contains(&compiler,
                "\"path\":\"compile_commands.json\",\"roles\":[\"build\","
                "\"index\"]") ||
      !contains(&compiler, "\"path\":\"index.scip\",\"roles\":[\"index\"]")) {
    fprintf(stderr, "zero-config compiler evidence is incorrect\n");
    failed = 1;
  }
  if (!resolve(engine, "", override_request, compiler_inventory,
               &compiler_scoped) ||
      !contains(&compiler_scoped, "\"builds\":[]") ||
      !contains(&compiler_scoped, "\"indexes\":[]") ||
      !contains(&compiler_scoped, "\"selected\":1") ||
      contains(&compiler_scoped, "\"roles\":[\"build\",\"index\"]") ||
      contains(&compiler_scoped, "\"roles\":[\"index\"]")) {
    fprintf(stderr, "selected discovery scope retained compiler evidence\n");
    failed = 1;
  }
  if (!resolve(engine, index_config, request, index_inventory, &index) ||
      !contains(&index, "\"max_file_bytes\":100") ||
      !contains(&index, "\"max_index_bytes\":1000") ||
      !contains(&index, "\"path\":\"semantic.scip\",\"roles\":[\"index\"]") ||
      !rejects(engine, index_config, index_override_request, index_inventory,
               "configured index exceeds limits.max_index_bytes")) {
    fprintf(stderr, "role-aware source/index limits are incorrect\n");
    failed = 1;
  }
  if (!resolve(engine, vendor_config, request, vendor_inventory, &vendor) ||
      !contains(&vendor, "\"path\":\"deps/library.c\",\"roles\":[\"source\","
                         "\"vendor\"]")) {
    fprintf(stderr, "asserted vendor analysis role is not preserved\n");
    failed = 1;
  }
  archbird_engine_destroy(engine);
  if (failed)
    return 1;
  puts("native config-resolution tests passed");
  return 0;
}
