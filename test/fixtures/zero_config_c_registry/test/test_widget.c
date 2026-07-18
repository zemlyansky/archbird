int widget_run(void);

#define TEST_IMPL(name) int test_##name(void)
#define WIDGET(name) { #name, widget_##name##_test }

TEST_IMPL(direct) { return widget_run(); }

static int widget_explicit_test(void) { return widget_run(); }
static int widget_forwarded_test(void) { return widget_run(); }

struct widget_testcase {
  const char *name;
  int (*function)(void);
};

struct widget_testcase widget_testcases[] = {
    {"explicit", widget_explicit_test},
    WIDGET(forwarded),
};
