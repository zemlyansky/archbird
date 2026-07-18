#include "path_match.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *name, const char *path_text,
                   const char *pattern_text, int wanted) {
  AbString path = {(char *)path_text, strlen(path_text)};
  AbString pattern = {(char *)pattern_text, strlen(pattern_text)};
  int actual = ab_map_path_match(&path, &pattern);
  if (actual != wanted) {
    fprintf(stderr, "FAIL %s: path=%s pattern=%s actual=%d wanted=%d\n", name,
            path_text, pattern_text, actual, wanted);
    failures++;
  }
}

static void expect_collection(const char *name, const char *path_text,
                              const char *pattern_text, int wanted) {
  AbString path = {(char *)path_text, strlen(path_text)};
  AbString pattern = {(char *)pattern_text, strlen(pattern_text)};
  int actual = ab_map_collection_match(&path, &pattern);
  if (actual != wanted) {
    fprintf(stderr, "FAIL %s: path=%s collection=%s actual=%d wanted=%d\n",
            name, path_text, pattern_text, actual, wanted);
    failures++;
  }
}

int main(void) {
  expect("direct", "src/nested/file.c", "src/**/*.c", 1);
  expect("collapsed", "src/file.c", "src/**/*.c", 1);
  expect("trailing-root", "src", "src/**", 1);
  expect("trailing-child", "src/deep/file.c", "src/**", 1);
  expect("all-globstars-collapsed", "a/b/c", "a/**/b/**/c", 1);
  expect("all-globstars-direct", "a/x/b/y/c", "a/**/b/**/c", 1);
  expect("no-partial-collapse", "a/b/x/c", "a/**/b/**/c", 0);
  expect_collection("collection-star-top", "src/lka/run.py", "src/lka/*.py", 1);
  expect_collection("collection-star-segment", "src/lka/corpus/merge.py",
                    "src/lka/*.py", 0);
  expect_collection("collection-globstar-top", "src/lka/run.py",
                    "src/lka/**/*.py", 1);
  expect_collection("collection-globstar-nested", "src/lka/corpus/merge.py",
                    "src/lka/**/*.py", 1);
  expect_collection("collection-terminal-root", "src/lka", "src/lka/**", 1);
  expect_collection("collection-terminal-top", "src/lka/run.py", "src/lka/**",
                    1);
  expect_collection("collection-terminal-nested", "src/lka/corpus/merge.py",
                    "src/lka/**", 1);
  expect_collection("collection-relative-prefix", "src/lka/run.py",
                    "./src/lka/*.py", 1);
  if (failures)
    return 1;
  puts("native path-match tests passed");
  return 0;
}
