typedef int (*Callback)(int);

typedef struct CallbackRoute {
  const char *name;
  Callback callback;
} CallbackRoute;

static int alpha_callback(int value) { return value + 1; }

static int beta_callback(int value) { return value + 2; }

static int invoke_callback(Callback callback, int value) {
  return callback(value);
}

int callback_registry_value(int value) {
  Callback selected = alpha_callback;
  CallbackRoute routes[] = {
      {"alpha", alpha_callback},
      {"beta", beta_callback},
  };
  selected = beta_callback;
  return invoke_callback(selected, routes[0].callback(value));
}
