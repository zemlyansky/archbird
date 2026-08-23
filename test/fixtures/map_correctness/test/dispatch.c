typedef void (*CaseFunction)(void);

typedef struct CaseEntry {
  const char *name;
  CaseFunction function;
} CaseEntry;

static void case_alpha(void) {}
static void case_beta(void) {}

static const CaseEntry CASES[] = {
    {" ", case_alpha},
    {"alpha", case_alpha},
    {"beta_2d", case_beta},
};
