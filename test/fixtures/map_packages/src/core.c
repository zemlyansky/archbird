#include "../include/poly.h"

int poly_add(int left, int right) { return left + right; }

void register_all(void *instance) {
  poly_register_entrypoint(instance, "train");
  poly_register_entrypoint(instance, helper(1, 2), "eval");
}
