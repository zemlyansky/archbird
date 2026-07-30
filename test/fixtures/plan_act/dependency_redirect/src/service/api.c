#include "service/api.h"

#include "storage/raw.h"

int service_value(void) { return raw_value(); }
