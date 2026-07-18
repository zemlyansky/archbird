#include "gitignore.h"
#include <archbird/archbird.h>

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(AbIgnoreSet *set, const char *name, const char *path,
                   int directory, int wanted) {
  AbString value = {(char *)path, strlen(path)};
  int actual = ab_ignore_set_matches(set, &value, directory);
  if (actual != wanted) {
    fprintf(stderr, "FAIL %s: path=%s actual=%d wanted=%d\n", name, path,
            actual, wanted);
    failures++;
  }
}

static int add(AbIgnoreSet *set, const char *path, const char *rules) {
  ArchbirdStatus status = ab_ignore_set_add(
      set, path, strlen(path), (const uint8_t *)rules, strlen(rules));
  if (status != ARCHBIRD_OK) {
    fprintf(stderr, "could not add %s: %d\n", path, (int)status);
    return 0;
  }
  return 1;
}

int main(void) {
  ArchbirdEngine *engine = NULL;
  AbIgnoreSet set;
  if (archbird_engine_create(NULL, &engine) != ARCHBIRD_OK)
    return 2;
  ab_ignore_set_init(&set, engine);
  if (!add(&set, ".gitignore",
           "# comment\n"
           "doc/special/\n"
           "frotz\n"
           "abc/**\n"
           "a/**/b\n"
           "**/logs\n"
           "trimmed   \n"
           "space\\ \n"
           "\\#literal\n"
           "\\!bang\n"
           "[ab].tmp\n"
           "parent/\n"
           "!parent/\n"
           "parent/*\n"
           "!parent/keep.c\n"
           "blocked/\n"
           "!blocked/file.c\n") ||
      !add(&set, "nested/.gitignore", "/local\n!frotz\n") ||
      ab_ignore_set_finalize(&set) != ARCHBIRD_OK) {
    ab_ignore_set_free(&set);
    archbird_engine_destroy(engine);
    return 2;
  }

  expect(&set, "slash-relative", "doc/special", 1, 1);
  expect(&set, "slash-not-anywhere", "a/doc/special", 1, 0);
  expect(&set, "basename-root", "frotz", 0, 1);
  expect(&set, "basename-nested", "a/frotz", 0, 1);
  expect(&set, "nested-negation", "nested/frotz", 0, 0);
  expect(&set, "nested-anchored", "nested/local", 0, 1);
  expect(&set, "nested-anchored-deeper", "nested/a/local", 0, 0);
  expect(&set, "trailing-globstar", "abc/file.c", 0, 1);
  expect(&set, "middle-globstar-zero", "a/b", 0, 1);
  expect(&set, "middle-globstar-many", "a/x/y/b", 0, 1);
  expect(&set, "leading-globstar-root", "logs", 1, 1);
  expect(&set, "leading-globstar-nested", "x/y/logs", 1, 1);
  expect(&set, "escaped-leading-comment", "#literal", 0, 1);
  expect(&set, "escaped-leading-negation", "!bang", 0, 1);
  expect(&set, "class", "a.tmp", 0, 1);
  expect(&set, "class-miss", "c.tmp", 0, 0);
  expect(&set, "reinclude-parent", "parent/keep.c", 0, 0);
  expect(&set, "sibling-stays-ignored", "parent/drop.c", 0, 1);
  expect(&set, "cannot-reinclude-under-ignored-parent", "blocked/file.c", 0, 1);
  expect(&set, "unescaped-trailing-space", "trimmed", 0, 1);
  expect(&set, "escaped-trailing-space", "space ", 0, 1);

  if (set.source_count != 2 || set.sources[0].rule_count != 16 ||
      strlen(set.sources[0].sha256) != 64) {
    fprintf(stderr, "FAIL evidence inventory\n");
    failures++;
  }
  ab_ignore_set_free(&set);
  archbird_engine_destroy(engine);
  if (failures)
    return 1;
  puts("native gitignore-contract tests passed");
  return 0;
}
