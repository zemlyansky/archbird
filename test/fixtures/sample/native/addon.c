#include <stddef.h>

typedef int napi_value;
typedef napi_value (*napi_callback)(void);
typedef struct {
  const char *name;
  void *data;
  napi_callback method;
} napi_property_descriptor;

static napi_value napi_core_add(void) { return 0; }
static napi_value napi_core_mul(void) { return 0; }

static const napi_property_descriptor props[] = {
  {"core_add", NULL, napi_core_add},
  {"core_mul", NULL, napi_core_mul},
};
