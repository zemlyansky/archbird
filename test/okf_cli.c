#include <archbird/archbird.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Bytes {
  uint8_t *data;
  size_t length;
} Bytes;

static int read_file(const char *path, Bytes *out) {
  FILE *stream = fopen(path, "rb");
  long length;
  if (!stream)
    return -1;
  if (fseek(stream, 0, SEEK_END) != 0 || (length = ftell(stream)) < 0 ||
      fseek(stream, 0, SEEK_SET) != 0) {
    fclose(stream);
    return -1;
  }
  if (length) {
    out->data = (uint8_t *)malloc((size_t)length);
    if (!out->data) {
      fclose(stream);
      return -1;
    }
    if (fread(out->data, 1, (size_t)length, stream) != (size_t)length) {
      free(out->data);
      memset(out, 0, sizeof(*out));
      fclose(stream);
      return -1;
    }
  }
  out->length = (size_t)length;
  fclose(stream);
  return 0;
}

static int write_stdout(void *unused, const uint8_t *bytes, size_t length) {
  (void)unused;
  return fwrite(bytes, 1, length, stdout) == length ? 0 : -1;
}

int main(int argc, char **argv) {
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  ArchbirdOkfFormat format;
  Bytes source = {0};
  Bytes query = {0};
  const uint8_t *query_data = NULL;
  size_t query_length = 0;
  int include_body;
  if (argc != 5 || (strcmp(argv[1], "json") && strcmp(argv[1], "markdown")) ||
      (strcmp(argv[3], "-") && read_file(argv[3], &query) != 0) ||
      (strcmp(argv[4], "0") && strcmp(argv[4], "1")) ||
      read_file(argv[2], &source) != 0) {
    fprintf(stderr, "usage: %s json|markdown SOURCE QUERY|- INCLUDE_BODY\n",
            argv[0]);
    free(query.data);
    return 2;
  }
  if (strcmp(argv[3], "-")) {
    query_data = query.data;
    query_length = query.length;
  }
  format = !strcmp(argv[1], "json") ? ARCHBIRD_OKF_JSON : ARCHBIRD_OKF_MARKDOWN;
  include_body = !strcmp(argv[4], "1");
  archbird_engine_options_init(&options);
  status = archbird_engine_create(&options, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_okf_analyze(
        engine, source.data, source.length, query_data, query_length, format,
        include_body, ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE,
        write_stdout, NULL);
  if (status != ARCHBIRD_OK)
    fprintf(stderr, "%s\n",
            engine ? archbird_engine_error(engine)
                   : "failed to create Archbird engine");
  archbird_engine_destroy(engine);
  free(query.data);
  free(source.data);
  return status == ARCHBIRD_OK ? 0 : 1;
}
