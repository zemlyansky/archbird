#include "map_internal.h"
#include <archbird/archbird.h>

#include <stdio.h>
#include <string.h>

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
