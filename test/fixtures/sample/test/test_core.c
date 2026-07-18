TEST(core, add) {
  ASSERT_EQ(core_add(1, 2), 3);
}

// TEST(fake, comment) { }
/* TEST(fake, block_comment) { } */
static const char *not_a_test = "TEST(fake, string)";
