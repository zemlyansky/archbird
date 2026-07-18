#include <archbird/archbird.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Bytes {
  uint8_t *data;
  size_t length;
} Bytes;

static int read_file(const char *path, Bytes *out) {
  FILE *stream;
  long length;
  if (!strcmp(path, "-"))
    return 0;
  stream = fopen(path, "rb");
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
  Bytes inputs[6] = {{0}};
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = ARCHBIRD_OK;
  int index;
  if (argc != 7) {
    fprintf(stderr,
            "usage: %s MAP VERIFY|- PROPOSAL|- CONTRACT|- RESULT|- "
            "NORMALIZATION|-\n",
            argv[0]);
    return 2;
  }
  for (index = 0; index < 6; index++) {
    if (read_file(argv[index + 1], &inputs[index]) != 0) {
      fprintf(stderr, "cannot read %s\n", argv[index + 1]);
      status = ARCHBIRD_INVALID_ARGUMENT;
      break;
    }
  }
  archbird_engine_options_init(&options);
  if (status == ARCHBIRD_OK)
    status = archbird_engine_create(&options, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_okf_publish(
        engine, inputs[0].data, inputs[0].length, inputs[1].data,
        inputs[1].length, inputs[2].data, inputs[2].length, inputs[3].data,
        inputs[3].length, inputs[4].data, inputs[4].length, inputs[5].data,
        inputs[5].length, ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE,
        write_stdout, NULL);
  if (status != ARCHBIRD_OK)
    fprintf(stderr, "%s\n",
            engine ? archbird_engine_error(engine)
                   : "failed to create Archbird engine");
  archbird_engine_destroy(engine);
  for (index = 0; index < 6; index++)
    free(inputs[index].data);
  return status == ARCHBIRD_OK ? 0 : 1;
}
