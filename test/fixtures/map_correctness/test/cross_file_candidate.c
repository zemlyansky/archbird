#include "cross_file_target.h"

static int cross_file_caller_one(void) { return cross_file_only(); }

static int cross_file_caller_two(void) { return cross_file_only(); }

TEST_COMMON(cross, candidate) {
  PASS_IF(cross_file_caller_one() + cross_file_caller_two() +
              cross_file_only() ==
          21);
}
