#include "core.h"

static int helper(int value) { return value + 1; }

int core_mul(int a, int b) { return a * b; }

int core_unused(int value) { return value; }

int core_add(int a, int b) {
  return helper(a) + b + core_mul(0, b);
}

void register_entries(void *ctx) {
  core_register(ctx, "forward");
}
