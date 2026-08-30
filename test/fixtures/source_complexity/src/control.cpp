int cpp_flow(int x) {
  try {
    while (x > 0) {
      if (x == 2)
        break;
      x--;
    }
  } catch (...) {
    return x ? 1 : 0;
  }
  return x;
}

int cpp_outer(void) {
  auto callback = [](int value) { return value ? 1 : 0; };
  return callback(1);
}
