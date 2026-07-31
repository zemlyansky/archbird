#include "core.h"

int core_sum(int left, int right) { return left + right; }

int core_peer(int left, int right) { return core_sum(left, right); }
