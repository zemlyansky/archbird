int c_flow(int x, int y) {
  if (x && y) {
    for (int i = 0; i < x; i++) {
      if (i == y)
        x--;
    }
  } else if (y || x) {
    x = x ? y : x;
  }
  switch (x) {
  case 1:
    x++;
    break;
  case 2:
    x--;
    break;
  default:
    break;
  }
  do {
    x--;
  } while (x > 0);
  return x;
}
