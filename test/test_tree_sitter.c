#include "sha256.h"
#include <archbird/archbird.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Output {
  char bytes[131072];
  size_t length;
  size_t writes;
} Output;

static int failures;

static int write_output(void *user_data, const uint8_t *bytes, size_t length) {
  Output *output = (Output *)user_data;
  output->writes++;
  if (length > sizeof(output->bytes) - output->length - 1)
    return 1;
  memcpy(output->bytes + output->length, bytes, length);
  output->length += length;
  output->bytes[output->length] = '\0';
  return 0;
}

static void expect(const char *name, ArchbirdStatus actual,
                   ArchbirdStatus expected) {
  if (actual != expected) {
    fprintf(stderr, "FAIL %s: status %d expected %d\n", name, (int)actual,
            (int)expected);
    failures++;
  }
}

static ArchbirdProject *
create_project_in_layer(ArchbirdEngine *engine, const char *source,
                        const char *path, const char *language,
                        const char *layer, const char *role) {
  static const char implementation[] =
      "1111111111111111111111111111111111111111111111111111111111111111";
  uint8_t digest[32];
  char hex[65];
  char manifest[2048];
  int length;
  ArchbirdProject *project = NULL;
  if (archbird_sha256((const uint8_t *)source, strlen(source), digest) !=
      ARCHBIRD_OK)
    return NULL;
  archbird_sha256_hex(digest, hex);
  length = snprintf(
      manifest, sizeof(manifest),
      "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":%zu,"
      "\"language\":\"%s\",\"layer\":\"%s\",\"path\":\"%s\","
      "\"roles\":[\"%s\"],\"sha256\":\"%s\"}],\"producer\":{"
      "\"implementation_sha256\":\"%s\",\"name\":\"tree-sitter-test\","
      "\"version\":\"1\"},\"project\":\"syntax-test\",\"schema_version\":1}",
      strlen(source), language, layer, path, role, hex, implementation);
  if (length < 0 || (size_t)length >= sizeof(manifest))
    return NULL;
  if (archbird_project_create(engine, (const uint8_t *)manifest, (size_t)length,
                              &project) != ARCHBIRD_OK)
    return NULL;
  if (archbird_project_add_source(engine, project, path, strlen(path),
                                  (const uint8_t *)source,
                                  strlen(source)) != ARCHBIRD_OK ||
      archbird_project_finalize_sources(engine, project) != ARCHBIRD_OK) {
    archbird_project_destroy(project);
    return NULL;
  }
  return project;
}

static ArchbirdProject *create_project(ArchbirdEngine *engine,
                                       const char *source, const char *path,
                                       const char *language) {
  return create_project_in_layer(engine, source, path, language, "core",
                                 "source");
}

#ifdef ARCHBIRD_TEST_TREE_SITTER_C
static void test_composed_c_evidence(ArchbirdEngine *engine) {
  static const char source[] = "#include \"api.h\"\n"
                               "static int helper(int x) { return x; }\n"
                               "int run(void) { return helper(1); }\n";
  ArchbirdProject *project =
      create_project(engine, source, "src/sample.c", "c");
  ArchbirdMergeSummary summary = {0};
  Output syntax_provider = {{0}, 0, 0};
  Output facts = {{0}, 0, 0};
  Output ledger = {{0}, 0, 0};
  if (!project) {
    fputs("FAIL create composed syntax project\n", stderr);
    failures++;
    return;
  }
  expect("lexical-primary",
         archbird_project_scan_builtin_provider(engine, project, "lexical:c", 9,
                                                ARCHBIRD_PROVIDER_PRIMARY),
         ARCHBIRD_OK);
  expect("syntax-augment",
         archbird_project_scan_builtin_provider(engine, project,
                                                "syntax:tree-sitter:c", 20,
                                                ARCHBIRD_PROVIDER_AUGMENT),
         ARCHBIRD_OK);
  expect("syntax-clean-provider",
         archbird_project_render_provider_facts(engine, project, 1, 0,
                                                write_output, &syntax_provider),
         ARCHBIRD_OK);
  if (!strstr(syntax_provider.bytes, "\"error_nodes\":0") ||
      !strstr(syntax_provider.bytes, "\"missing_nodes\":0") ||
      strstr(syntax_provider.bytes, "\"recovery_nodes\":") ||
      strstr(syntax_provider.bytes, "\"syntax_recovery\":")) {
    fputs("FAIL clean syntax facts were marked as recovered\n", stderr);
    failures++;
  }
  expect("syntax-finalize",
         archbird_project_finalize_providers(engine, project), ARCHBIRD_OK);
  summary.struct_size = sizeof(summary);
  expect("syntax-summary", archbird_project_merge_summary(project, &summary),
         ARCHBIRD_OK);
  if (summary.providers != 2 || summary.selected_facts != 6 ||
      summary.enriched != 3 || summary.conflicts != 0) {
    fprintf(stderr,
            "FAIL syntax composition summary: providers=%zu facts=%zu "
            "enriched=%zu conflicts=%zu\n",
            summary.providers, summary.selected_facts, summary.enriched,
            summary.conflicts);
    failures++;
  }
  expect("syntax-file-facts",
         archbird_project_render_file_facts(engine, project, 0, write_output,
                                            &facts),
         ARCHBIRD_OK);
  if (!strstr(facts.bytes, "\"imports\":[\"api.h\"]") ||
      !strstr(facts.bytes, "\"calls\":[\"helper\"]") ||
      !strstr(facts.bytes, "\"name\":\"run\"")) {
    fputs("FAIL composed syntax facts were not reduced into common IR\n",
          stderr);
    failures++;
  }
  expect("syntax-ledger",
         archbird_project_render_merge_ledger(engine, project, 0, write_output,
                                              &ledger),
         ARCHBIRD_OK);
  if (!strstr(ledger.bytes, "archbird-provider-merge") ||
      !strstr(ledger.bytes, "syntax-structure") ||
      !strstr(ledger.bytes, "lexical-occurrence") ||
      !strstr(ledger.bytes, "\"domain\":\"symbol-references\"") ||
      !strstr(ledger.bytes, "\"domain\":\"imports\",\"primary\":null")) {
    fputs("FAIL composed syntax provenance ledger\n", stderr);
    failures++;
  }
  archbird_project_destroy(project);
}

static void test_syntax_diagnostics(ArchbirdEngine *engine) {
  static const char source[] = "int broken( { return 0; }\n";
  static const char config[] =
      "{\"schema_version\":1,\"project\":\"syntax-test\",\"layers\":[{"
      "\"name\":\"core\",\"language\":\"c\",\"globs\":[\"src/**\"]}]}";
  ArchbirdProject *project =
      create_project(engine, source, "src/broken.c", "c");
  Output provider = {{0}, 0, 0};
  Output map = {{0}, 0, 0};
  if (!project) {
    fputs("FAIL create malformed syntax project\n", stderr);
    failures++;
    return;
  }
  expect("malformed-syntax-config",
         archbird_project_set_config(engine, project, (const uint8_t *)config,
                                     sizeof(config) - 1),
         ARCHBIRD_OK);
  expect("malformed-syntax-provider",
         archbird_project_scan_builtin_provider(engine, project,
                                                "syntax:tree-sitter:c", 20,
                                                ARCHBIRD_PROVIDER_PRIMARY),
         ARCHBIRD_OK);
  expect("malformed-provider-render",
         archbird_project_render_provider_facts(engine, project, 0, 0,
                                                write_output, &provider),
         ARCHBIRD_OK);
  if (!strstr(provider.bytes, "tree-sitter-") ||
      (!strstr(provider.bytes, "tree-sitter-error") &&
       !strstr(provider.bytes, "tree-sitter-missing"))) {
    fputs("FAIL Tree-sitter ERROR/MISSING evidence is absent\n", stderr);
    failures++;
  }
  expect("malformed-provider-finalize",
         archbird_project_finalize_providers(engine, project), ARCHBIRD_OK);
  expect("malformed-map-render",
         archbird_project_render_map(engine, project, 0, write_output, &map),
         ARCHBIRD_OK);
  if (map.writes < 2) {
    fputs("FAIL compact Map output was not streamed\n", stderr);
    failures++;
  }
  if (!strstr(map.bytes, "\"code\":\"tree-sitter-") ||
      !strstr(map.bytes, "\"span\":{\"end\":")) {
    fputs("FAIL Map dropped exact Tree-sitter recovery span\n", stderr);
    failures++;
  }
  archbird_project_destroy(project);
}

static void test_vendor_role_is_bounded(ArchbirdEngine *engine) {
  static const char source[] = "int vendored(void) { return helper(); }\n";
  ArchbirdProject *project = create_project_in_layer(
      engine, source, "vendor/generated.c", "c", "vendor", "vendor");
  Output provider = {{0}, 0, 0};
  if (!project) {
    fputs("FAIL create vendor syntax project\n", stderr);
    failures++;
    return;
  }
  expect("vendor-syntax-provider",
         archbird_project_scan_builtin_provider(engine, project,
                                                "syntax:tree-sitter:c", 20,
                                                ARCHBIRD_PROVIDER_PRIMARY),
         ARCHBIRD_OK);
  expect("vendor-provider-render",
         archbird_project_render_provider_facts(engine, project, 0, 0,
                                                write_output, &provider),
         ARCHBIRD_OK);
  if (!strstr(provider.bytes, "\"coverage\":\"none\"") ||
      !strstr(provider.bytes, "\"facts\":[]") ||
      strstr(provider.bytes, "tree-sitter-resource-limit")) {
    fputs("FAIL vendor syntax role was not represented as bounded exclusion\n",
          stderr);
    failures++;
  }
  archbird_project_destroy(project);
}

static void test_resource_limit_is_evidence(void) {
  static const char source[] = "int limited(void) { return 1; }\n";
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;
  Output provider = {{0}, 0, 0};
  archbird_engine_options_init(&options);
  options.max_syntax_bytes = 1;
  expect("limited-engine", archbird_engine_create(&options, &engine),
         ARCHBIRD_OK);
  if (!engine)
    return;
  project = create_project(engine, source, "src/limited.c", "c");
  if (!project) {
    fputs("FAIL create syntax-limit project\n", stderr);
    failures++;
    goto done;
  }
  expect("syntax-limit-provider",
         archbird_project_scan_builtin_provider(engine, project,
                                                "syntax:tree-sitter:c", 20,
                                                ARCHBIRD_PROVIDER_PRIMARY),
         ARCHBIRD_OK);
  expect("syntax-limit-render",
         archbird_project_render_provider_facts(engine, project, 0, 0,
                                                write_output, &provider),
         ARCHBIRD_OK);
  if (!strstr(provider.bytes, "\"coverage\":\"none\"") ||
      !strstr(provider.bytes, "\"code\":\"tree-sitter-resource-limit\"") ||
      !strstr(provider.bytes, "\"facts\":[]")) {
    fputs("FAIL syntax resource limit did not become bounded evidence\n",
          stderr);
    failures++;
  }
done:
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
}

static void test_logical_limit_is_not_resource_evidence(void) {
  static const char source[] =
      "#include \"api.h\"\nint limited(void) { return helper(); }\n";
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;
  ArchbirdStatus status;
  archbird_engine_options_init(&options);
  options.max_facts = 1;
  expect("logical-limit-engine", archbird_engine_create(&options, &engine),
         ARCHBIRD_OK);
  if (!engine)
    return;
  project = create_project(engine, source, "src/limited.c", "c");
  if (!project) {
    fputs("FAIL create syntax logical-limit project\n", stderr);
    failures++;
    goto done;
  }
  status = archbird_project_scan_builtin_provider(
      engine, project, "syntax:tree-sitter:c", 20, ARCHBIRD_PROVIDER_PRIMARY);
  expect("syntax-logical-limit-provider", status, ARCHBIRD_LIMIT_EXCEEDED);
  if (!strstr(archbird_engine_error(engine), "provider fact limit exceeded") ||
      strstr(archbird_engine_error(engine), "allocation limit")) {
    fputs(
        "FAIL logical syntax limit was mislabeled as parser memory evidence\n",
        stderr);
    failures++;
  }
  if (archbird_project_provider_count(project) != 0) {
    fputs("FAIL partial provider survived a logical syntax limit\n", stderr);
    failures++;
  }
done:
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
}
#endif

#if defined(ARCHBIRD_TEST_TREE_SITTER_C) &&                                    \
    defined(ARCHBIRD_TEST_TREE_SITTER_CPP)
static void test_discovered_header_routing(ArchbirdEngine *engine) {
  static const char cpp_source[] =
      "namespace demo { template <typename T> class Box { public: T run(T "
      "value) { return value; } }; }\n";
  static const char c_source[] = "int helper(int value);\n";
  ArchbirdProject *cpp_project = create_project_in_layer(
      engine, cpp_source, "include/demo.h", "c", "auto-c", "source");
  ArchbirdProject *configured_project = create_project_in_layer(
      engine, cpp_source, "include/demo.h", "c", "core", "source");
  ArchbirdProject *c_project = create_project_in_layer(
      engine, c_source, "include/demo.h", "c", "auto-c", "source");
  Output cpp_provider = {{0}, 0, 0};
  Output configured_provider = {{0}, 0, 0};
  Output c_provider = {{0}, 0, 0};
  if (!cpp_project || !configured_project || !c_project) {
    fputs("FAIL create discovered header routing projects\n", stderr);
    failures++;
    goto done;
  }
  expect("route-discovered-cpp-header",
         archbird_project_scan_builtin_provider(engine, cpp_project,
                                                "syntax:tree-sitter:c", 20,
                                                ARCHBIRD_PROVIDER_PRIMARY),
         ARCHBIRD_OK);
  expect("render-discovered-cpp-header",
         archbird_project_render_provider_facts(engine, cpp_project, 0, 0,
                                                write_output, &cpp_provider),
         ARCHBIRD_OK);
  if (!strstr(cpp_provider.bytes, "\"name\":\"archbird-tree-sitter-cpp\"") ||
      strstr(cpp_provider.bytes, "tree-sitter-error")) {
    fputs("FAIL discovery-owned C++ header did not select C++ syntax\n",
          stderr);
    failures++;
  }
  expect("route-configured-c-header",
         archbird_project_scan_builtin_provider(engine, configured_project,
                                                "syntax:tree-sitter:c", 20,
                                                ARCHBIRD_PROVIDER_PRIMARY),
         ARCHBIRD_OK);
  expect("render-configured-c-header",
         archbird_project_render_provider_facts(engine, configured_project, 0,
                                                0, write_output,
                                                &configured_provider),
         ARCHBIRD_OK);
  if (!strstr(configured_provider.bytes,
              "\"name\":\"archbird-tree-sitter-c\"")) {
    fputs("FAIL reviewed C header classification was overridden\n", stderr);
    failures++;
  }
  expect("route-discovered-c-header",
         archbird_project_scan_builtin_provider(engine, c_project,
                                                "syntax:tree-sitter:c", 20,
                                                ARCHBIRD_PROVIDER_PRIMARY),
         ARCHBIRD_OK);
  expect("render-discovered-c-header",
         archbird_project_render_provider_facts(engine, c_project, 0, 0,
                                                write_output, &c_provider),
         ARCHBIRD_OK);
  if (!strstr(c_provider.bytes, "\"name\":\"archbird-tree-sitter-c\"")) {
    fputs("FAIL C syntax was not preferred for an equal-quality C header\n",
          stderr);
    failures++;
  }
done:
  archbird_project_destroy(cpp_project);
  archbird_project_destroy(configured_project);
  archbird_project_destroy(c_project);
}
#endif

#ifdef ARCHBIRD_TEST_TREE_SITTER_TYPESCRIPT
static void test_typescript_class_scopes(ArchbirdEngine *engine) {
  static const char source[] =
      "interface Service {}\n"
      "function outer() {\n"
      "  const anonymous = new class implements Service { anonymousRun() {} "
      "};\n"
      "  class Inner implements Service { run() {} }\n"
      "  return class Returned implements Service { returnedRun() {} };\n"
      "}\n"
      "const Bound = class implements Service { boundRun() {} };\n"
      "const Named = class ExpressionName implements Service { namedRun() {} "
      "};\n"
      "export const enum PluginFormat { Copilot }\n"
      "const COPILOT_FORMAT = { parseHooks() {} };\n";
  ArchbirdProject *project =
      create_project(engine, source, "src/scopes.ts", "typescript");
  Output facts = {{0}, 0, 0};
  if (!project) {
    fputs("FAIL create TypeScript class-scope project\n", stderr);
    failures++;
    return;
  }
  expect("typescript-class-scope-lexical",
         archbird_project_scan_builtin_provider(engine, project,
                                                "lexical:javascript", 18,
                                                ARCHBIRD_PROVIDER_AUGMENT),
         ARCHBIRD_OK);
  expect("typescript-class-scope-syntax",
         archbird_project_scan_builtin_provider(engine, project,
                                                "syntax:tree-sitter:typescript",
                                                29, ARCHBIRD_PROVIDER_PRIMARY),
         ARCHBIRD_OK);
  expect("typescript-class-scope-finalize",
         archbird_project_finalize_providers(engine, project), ARCHBIRD_OK);
  expect("typescript-class-scope-facts",
         archbird_project_render_file_facts(engine, project, 0, write_output,
                                            &facts),
         ARCHBIRD_OK);
  if (!strstr(facts.bytes, "\"name\":\"Inner.run\"") ||
      !strstr(facts.bytes, "\"name\":\"Bound.boundRun\"") ||
      !strstr(facts.bytes, "\"name\":\"outer.anonymousRun\"") ||
      !strstr(facts.bytes, "\"name\":\"Returned.returnedRun\"") ||
      !strstr(facts.bytes, "\"name\":\"ExpressionName.namedRun\"") ||
      !strstr(facts.bytes, "\"name\":\"COPILOT_FORMAT.parseHooks\"") ||
      strstr(facts.bytes, "\"name\":\"implements") ||
      strstr(facts.bytes, "\"name\":\"enum.parseHooks\"") ||
      strstr(facts.bytes, "\"name\":\"outer.Inner") ||
      strstr(facts.bytes, "\"name\":\"outer.Returned")) {
    fputs("FAIL TypeScript class scopes diverged across providers\n", stderr);
    failures++;
  }
  archbird_project_destroy(project);
}
#endif

#ifdef ARCHBIRD_TEST_TREE_SITTER_TSX
static void test_tsx_class_scopes(ArchbirdEngine *engine) {
  static const char source[] =
      "class First { render() { return <div>Here's code</div>; } }\n"
      "class Second { run() {} }\n";
  ArchbirdProject *project =
      create_project(engine, source, "src/scopes.tsx", "typescript");
  Output facts = {{0}, 0, 0};
  if (!project) {
    fputs("FAIL create TSX class-scope project\n", stderr);
    failures++;
    return;
  }
  expect("tsx-class-scope-lexical",
         archbird_project_scan_builtin_provider(engine, project,
                                                "lexical:javascript", 18,
                                                ARCHBIRD_PROVIDER_AUGMENT),
         ARCHBIRD_OK);
  expect("tsx-class-scope-syntax",
         archbird_project_scan_builtin_provider(engine, project,
                                                "syntax:tree-sitter:tsx", 22,
                                                ARCHBIRD_PROVIDER_PRIMARY),
         ARCHBIRD_OK);
  expect("tsx-class-scope-finalize",
         archbird_project_finalize_providers(engine, project), ARCHBIRD_OK);
  expect("tsx-class-scope-facts",
         archbird_project_render_file_facts(engine, project, 0, write_output,
                                            &facts),
         ARCHBIRD_OK);
  if (!strstr(facts.bytes, "\"name\":\"First.render\"") ||
      !strstr(facts.bytes, "\"name\":\"Second.run\"") ||
      strstr(facts.bytes, "\"name\":\"First.run\"")) {
    fputs("FAIL TSX text changed the following class scope\n", stderr);
    failures++;
  }
  archbird_project_destroy(project);
}
#endif

static void test_language_pack(ArchbirdEngine *engine, const char *name,
                               const char *provider_id, const char *language,
                               const char *path, const char *source,
                               const char *expected_name,
                               const char *expected_call,
                               const char *expected_import) {
  ArchbirdProject *project = create_project(engine, source, path, language);
  Output first = {{0}, 0, 0};
  Output second = {{0}, 0, 0};
  if (!project) {
    fprintf(stderr, "FAIL create %s syntax project\n", name);
    failures++;
    return;
  }
  expect(name,
         archbird_project_scan_builtin_provider(engine, project, provider_id,
                                                strlen(provider_id),
                                                ARCHBIRD_PROVIDER_PRIMARY),
         ARCHBIRD_OK);
  expect(name,
         archbird_project_render_provider_facts(engine, project, 0, 0,
                                                write_output, &first),
         ARCHBIRD_OK);
  expect(name,
         archbird_project_render_provider_facts(engine, project, 0, 0,
                                                write_output, &second),
         ARCHBIRD_OK);
  if (first.length != second.length ||
      memcmp(first.bytes, second.bytes, first.length) != 0 ||
      !strstr(first.bytes, "\"domain\":\"syntax-summaries\"") ||
      (expected_name && !strstr(first.bytes, expected_name)) ||
      (expected_call && !strstr(first.bytes, expected_call)) ||
      (expected_import && !strstr(first.bytes, expected_import))) {
    fprintf(stderr, "FAIL %s syntax facts or deterministic replay\n", name);
    failures++;
  }
  if (strcmp(name, "python-pack") == 0 &&
      !strstr(first.bytes, "\"enclosing\":\"Box.run\"")) {
    fputs("FAIL Python syntax enclosing name is not qualified\n", stderr);
    failures++;
  }
  if (strcmp(name, "python-pack") == 0 &&
      !strstr(first.bytes, "\"kind\":\"variable\",\"name\":\"VALUE\"")) {
    fputs("FAIL Python module variable syntax symbol is absent\n", stderr);
    failures++;
  }
  if (strcmp(name, "cpp-pack") == 0 &&
      !strstr(first.bytes, "\"enclosing\":\"demo.Box.run\"")) {
    fputs("FAIL C++ syntax enclosing name is not qualified\n", stderr);
    failures++;
  }
  if (strcmp(name, "cpp-pack") == 0 &&
      !strstr(first.bytes, "\"code\":\"tree-sitter-missing\"")) {
    fputs("FAIL recovered C++ missing-name evidence is absent\n", stderr);
    failures++;
  }
  if (strcmp(name, "cpp-pack") == 0 &&
      (!strstr(first.bytes, "\"recovery_nodes\":1") ||
       !strstr(first.bytes, "\"syntax_recovery\":\"intersects\""))) {
    fputs("FAIL recovered C++ fact lacks local recovery evidence\n", stderr);
    failures++;
  }
  if (strcmp(name, "javascript-pack") == 0 &&
      !strstr(first.bytes, "\"name\":\"ops.nested.mul\"")) {
    fputs("FAIL JavaScript object method name is not qualified\n", stderr);
    failures++;
  }
  if (strcmp(name, "javascript-pack") == 0 &&
      (!strstr(first.bytes, "\"name\":\"api.run\"") ||
       !strstr(first.bytes, "\"name\":\"module.exports.start\"") ||
       !strstr(first.bytes, "\"name\":\"table.stop\""))) {
    fputs("FAIL JavaScript assignment/object function symbols are absent\n",
          stderr);
    failures++;
  }
  if (strcmp(name, "javascript-pack") == 0 &&
      (!strstr(first.bytes, "\"key\":\"start\",\"kind\":\"export\"") ||
       !strstr(first.bytes, "\"key\":\"default\",\"kind\":\"export\"") ||
       !strstr(first.bytes, "\"local\":\"value\",\"module\":\"./dep.js\"") ||
       strstr(first.bytes, "\"key\":\"api\",\"kind\":\"export\""))) {
    fputs("FAIL JavaScript ESM/CommonJS module surface facts\n", stderr);
    failures++;
  }
  if (strcmp(name, "typescript-pack") == 0 &&
      !strstr(first.bytes, "\"name\":\"api.run\"")) {
    fputs("FAIL TypeScript assignment-bound function symbol is absent\n",
          stderr);
    failures++;
  }
  if (strcmp(name, "typescript-pack") == 0 &&
      (!strstr(first.bytes, "\"name\":\"AbstractBox.parse\"") ||
       strstr(first.bytes, "\"name\":\"parse\""))) {
    fputs("FAIL TypeScript abstract-class method name is not qualified\n",
          stderr);
    failures++;
  }
  archbird_project_destroy(project);
}

int main(void) {
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  archbird_engine_options_init(&options);
  expect("engine", archbird_engine_create(&options, &engine), ARCHBIRD_OK);
  if (engine) {
#ifdef ARCHBIRD_TEST_TREE_SITTER_C
    test_composed_c_evidence(engine);
    test_syntax_diagnostics(engine);
    test_vendor_role_is_bounded(engine);
    test_resource_limit_is_evidence();
    test_logical_limit_is_not_resource_evidence();
#endif
#if defined(ARCHBIRD_TEST_TREE_SITTER_C) &&                                    \
    defined(ARCHBIRD_TEST_TREE_SITTER_CPP)
    test_discovered_header_routing(engine);
#endif
#ifdef ARCHBIRD_TEST_TREE_SITTER_CPP
    test_language_pack(
        engine, "cpp-pack", "syntax:tree-sitter:cpp", "cpp", "src/sample.cpp",
        "#include <vector>\nnamespace demo { class Box { public: int run() "
        "{ return helper(); } }; int helper() { return 1; } }\nnamespace { "
        "int hidden() { return 2; } }\nvoid recovered() { value.; }\n",
        "\"name\":\"demo.Box.run\"", "\"name\":\"helper\"",
        "\"name\":\"vector\"");
#endif
#ifdef ARCHBIRD_TEST_TREE_SITTER_PYTHON
    test_language_pack(
        engine, "python-pack", "syntax:tree-sitter:python", "python",
        "pkg/sample.py",
        "import os\nVALUE = helper()\nclass Box:\n  def run(self):\n    return "
        "helper()\n",
        "\"name\":\"Box.run\"", "\"name\":\"helper\"", "\"name\":\"os\"");
#endif
#ifdef ARCHBIRD_TEST_TREE_SITTER_JAVASCRIPT
    test_language_pack(
        engine, "javascript-pack", "syntax:tree-sitter:javascript",
        "javascript", "src/sample.js",
        "import value from './dep.js';\nexport default function main() {}\n"
        "class Box { run() { return helper(); } "
        "}\nconst ops = { nested: { mul() { return helper(); } } };\n"
        "const api = {}; api.run = function run() { return helper(); };\n"
        "module.exports.start = () => helper();\n"
        "const table = { stop: function () { return helper(); } };\n",
        "\"name\":\"Box.run\"", "\"name\":\"helper\"", "\"name\":\"./dep.js\"");
#endif
#ifdef ARCHBIRD_TEST_TREE_SITTER_TYPESCRIPT
    test_typescript_class_scopes(engine);
    test_language_pack(
        engine, "typescript-pack", "syntax:tree-sitter:typescript",
        "typescript", "src/sample.ts",
        "import {Value} from './dep.js';\nclass Box { run(x: Value) { return "
        "helper(x); } }\nconst api: any = {}; api.run = (x: Value) => "
        "helper(x);\nexport abstract class AbstractBox { parse(x: Value) { "
        "return helper(x); } }\n",
        "\"name\":\"Box.run\"", "\"name\":\"helper\"", "\"name\":\"./dep.js\"");
#endif
#ifdef ARCHBIRD_TEST_TREE_SITTER_TSX
    test_tsx_class_scopes(engine);
    test_language_pack(engine, "tsx-pack", "syntax:tree-sitter:tsx",
                       "typescript", "src/sample.tsx",
                       "import React from 'react';\nexport const Box = () => "
                       "<div>{helper()}</div>;\n",
                       "\"name\":\"Box\"", "\"name\":\"helper\"",
                       "\"name\":\"react\"");
#endif
#ifdef ARCHBIRD_TEST_TREE_SITTER_R
    test_language_pack(
        engine, "r-pack", "syntax:tree-sitter:r", "r", "R/sample.R",
        "helper <- function(x) { x }\nrun <- function() { helper(1) }\n",
        "\"name\":\"run\"", "\"name\":\"helper\"", NULL);
#endif
  }
  archbird_engine_destroy(engine);
  if (failures)
    return 1;
  puts("Tree-sitter C composition tests passed");
  return 0;
}
