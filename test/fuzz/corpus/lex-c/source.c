#include <stddef.h>

static int helper(int value) { return value + 1; }
int public_api(void) { return helper(41); }
