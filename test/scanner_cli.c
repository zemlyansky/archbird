#include "sha256.h"
#include <archbird/archbird.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_stdout(void *user_data, const uint8_t *bytes, size_t length) {
  (void)user_data;
  return fwrite(bytes, 1, length, stdout) == length ? 0 : 1;
}

static int read_input(uint8_t **out, size_t *out_length) {
  uint8_t *bytes = NULL;
  size_t length = 0;
  size_t capacity = 0;
  for (;;) {
    size_t count;
    if (length == capacity) {
      size_t next = capacity ? capacity * 2 : 4096;
      uint8_t *resized = (uint8_t *)realloc(bytes, next);
      if (!resized) {
        free(bytes);
        return 0;
      }
      bytes = resized;
      capacity = next;
    }
    count = fread(bytes + length, 1, capacity - length, stdin);
    length += count;
    if (count == 0) {
      if (ferror(stdin)) {
        free(bytes);
        return 0;
      }
      break;
    }
  }
  *out = bytes;
  *out_length = length;
  return 1;
}

static int identifier_valid(const char *value) {
  size_t index;
  if (!value[0] || !((value[0] >= 'A' && value[0] <= 'Z') ||
                     (value[0] >= 'a' && value[0] <= 'z') || value[0] == '_'))
    return 0;
  for (index = 1; value[index]; index++) {
    if (!((value[index] >= 'A' && value[index] <= 'Z') ||
          (value[index] >= 'a' && value[index] <= 'z') ||
          (value[index] >= '0' && value[index] <= '9') || value[index] == '_'))
      return 0;
  }
  return 1;
}

int main(int argc, char **argv) {
  static const char implementation[] =
      "1111111111111111111111111111111111111111111111111111111111111111";
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdProject *project = NULL;
  uint8_t *source = NULL;
  size_t source_length = 0;
  char *header = NULL;
  size_t header_length = 0;
  size_t header_capacity = 0;
  uint8_t source_digest[32];
  uint8_t header_digest[32];
  char source_hex[65];
  char header_hex[65];
  char *manifest = NULL;
  size_t manifest_capacity;
  int manifest_length;
  int has_header;
  int render_file_facts = 0;
  const char *provider_id;
  int arg_index;
  ArchbirdStatus status;
  if (argc > 1 && strcmp(argv[1], "--file-facts") == 0) {
    render_file_facts = 1;
    argc--;
    argv++;
  }
  if (argc < 3 ||
      (strcmp(argv[1], "c") != 0 && strcmp(argv[1], "cpp") != 0 &&
       strcmp(argv[1], "r") != 0 && strcmp(argv[1], "javascript") != 0 &&
       strcmp(argv[1], "typescript") != 0 && strcmp(argv[1], "vue") != 0 &&
       strcmp(argv[1], "python") != 0)) {
    fputs("usage: scanner_cli LANGUAGE PATH [C_PUBLIC_NAME ...]\n", stderr);
    return 2;
  }
  if (strcmp(argv[1], "c") != 0 && strcmp(argv[1], "cpp") != 0 && argc != 3) {
    fputs("only C/C++ scanners accept public-header names\n", stderr);
    return 2;
  }
  if (strcmp(argv[1], "c") == 0 || strcmp(argv[1], "cpp") == 0)
    provider_id = "lexical:c";
  else if (strcmp(argv[1], "javascript") == 0 ||
           strcmp(argv[1], "typescript") == 0 || strcmp(argv[1], "vue") == 0)
    provider_id = "lexical:javascript";
  else if (strcmp(argv[1], "python") == 0)
    provider_id = "lexical:python";
  else
    provider_id = "lexical:r";
  for (arg_index = 3; arg_index < argc; arg_index++) {
    size_t name_length;
    int written;
    if (!identifier_valid(argv[arg_index])) {
      fputs("invalid public identifier\n", stderr);
      return 2;
    }
    name_length = strlen(argv[arg_index]);
    if (header_length + name_length + 20 > header_capacity) {
      size_t next = header_capacity ? header_capacity * 2 : 256;
      char *resized;
      while (next < header_length + name_length + 20)
        next *= 2;
      resized = (char *)realloc(header, next);
      if (!resized) {
        free(header);
        return 2;
      }
      header = resized;
      header_capacity = next;
    }
    written = snprintf(header + header_length, header_capacity - header_length,
                       "int %s(void);\n", argv[arg_index]);
    if (written < 0) {
      free(header);
      return 2;
    }
    header_length += (size_t)written;
  }
  has_header = header_length != 0;
  if (!read_input(&source, &source_length)) {
    free(header);
    return 2;
  }
  if (archbird_sha256(source, source_length, source_digest) != ARCHBIRD_OK ||
      archbird_sha256((const uint8_t *)header, header_length, header_digest) !=
          ARCHBIRD_OK) {
    free(source);
    free(header);
    return 2;
  }
  archbird_sha256_hex(source_digest, source_hex);
  archbird_sha256_hex(header_digest, header_hex);
  manifest_capacity = strlen(argv[2]) + 2048;
  manifest = (char *)malloc(manifest_capacity);
  if (!manifest) {
    free(source);
    free(header);
    return 2;
  }
  if (has_header) {
    manifest_length = snprintf(
        manifest, manifest_capacity,
        "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":%zu,"
        "\"language\":\"%s\",\"layer\":\"core\",\"path\":\"__public.h\","
        "\"roles\":[\"public-header\",\"source\"],\"sha256\":\"%s\"},{"
        "\"bytes\":%zu,\"language\":\"%s\",\"layer\":\"core\",\"path\":"
        "\"%s\",\"roles\":[\"source\"],\"sha256\":\"%s\"}],\"producer\":{"
        "\"implementation_sha256\":\"%s\",\"name\":\"scanner-host\","
        "\"version\":\"1\"},\"project\":\"scan\",\"schema_version\":1}",
        header_length, argv[1], header_hex, source_length, argv[1], argv[2],
        source_hex, implementation);
  } else {
    manifest_length = snprintf(
        manifest, manifest_capacity,
        "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":%zu,"
        "\"language\":\"%s\",\"layer\":\"core\",\"path\":\"%s\","
        "\"roles\":[\"source\"],\"sha256\":\"%s\"}],\"producer\":{"
        "\"implementation_sha256\":\"%s\",\"name\":\"scanner-host\","
        "\"version\":\"1\"},\"project\":\"scan\",\"schema_version\":1}",
        source_length, argv[1], argv[2], source_hex, implementation);
  }
  if (manifest_length < 0 || (size_t)manifest_length >= manifest_capacity) {
    free(manifest);
    free(source);
    free(header);
    return 2;
  }
  archbird_engine_options_init(&options);
  status = archbird_engine_create(&options, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_project_create(engine, (const uint8_t *)manifest,
                                     (size_t)manifest_length, &project);
  if (status == ARCHBIRD_OK && has_header)
    status =
        archbird_project_add_source(engine, project, "__public.h", 10,
                                    (const uint8_t *)header, header_length);
  if (status == ARCHBIRD_OK)
    status = archbird_project_add_source(
        engine, project, argv[2], strlen(argv[2]), source, source_length);
  if (status == ARCHBIRD_OK)
    status = archbird_project_finalize_sources(engine, project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_scan_builtin_provider(
        engine, project, provider_id, strlen(provider_id),
        ARCHBIRD_PROVIDER_PRIMARY);
  if (status == ARCHBIRD_OK && render_file_facts)
    status = archbird_project_finalize_providers(engine, project);
  if (status != ARCHBIRD_OK) {
    fprintf(stderr, "%s\n",
            engine ? archbird_engine_error(engine) : "failed to create engine");
  } else if (render_file_facts) {
    status = archbird_project_render_file_facts(engine, project, 0,
                                                write_stdout, NULL);
    if (status == ARCHBIRD_OK && putchar('\n') == EOF)
      status = ARCHBIRD_WRITE_FAILED;
  } else {
    size_t provider_index;
    for (provider_index = 0;
         provider_index < archbird_project_provider_count(project);
         provider_index++) {
      status = archbird_project_render_provider_facts(
          engine, project, provider_index, 0, write_stdout, NULL);
      if (status != ARCHBIRD_OK)
        break;
      if (putchar('\n') == EOF) {
        status = ARCHBIRD_WRITE_FAILED;
        break;
      }
    }
  }
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
  free(manifest);
  free(source);
  free(header);
  return status == ARCHBIRD_OK ? 0 : 1;
}
