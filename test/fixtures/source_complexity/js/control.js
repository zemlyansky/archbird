export function jsFlow(a, b) {
  if (a && b) {
    for (let i = 0; i < a; i++) {
      if (i === b)
        a--;
    }
  } else if (a || b) {
    return a ? 1 : 0;
  }
  switch (a) {
  case 0:
    return 0;
  case 1:
    return 1;
  default:
    break;
  }
  try {
    while (a > 0)
      a--;
  } catch (error) {
    return 0;
  }
  return a;
}

export function jsOuter(callback) {
  callback((value) => value ? 1 : 0);
  return 0;
}

export const jsBound =
  (value) => {
    if (value)
      return 1;
    return 0;
  };
