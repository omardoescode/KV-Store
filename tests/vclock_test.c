#include "vclock/vclock.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static vclock clock_of(const char *text) {
  vclock c;
  assert(vclock_decode(text, strlen(text), &c) == 0);
  return c;
}

static void test_compare_orders_clocks(void) {
  vclock a = clock_of("1:2,3:1");
  vclock b = clock_of("1:2,3:1");
  assert(vclock_compare(&a, &b) == VCLOCK_EQUAL);
  b = clock_of("1:3,3:1");
  assert(vclock_compare(&a, &b) == VCLOCK_BEFORE);
  assert(vclock_compare(&b, &a) == VCLOCK_AFTER);
  b = clock_of("1:1,2:1,3:1");
  assert(vclock_compare(&a, &b) == VCLOCK_CONCURRENT);
  vclock empty = clock_of("");
  assert(vclock_compare(&empty, &a) == VCLOCK_BEFORE);
  printf("  test_compare_orders_clocks OK\n");
}

static bool same(const vclock *got, const char *want) {
  vclock w = clock_of(want);
  return vclock_compare(got, &w) == VCLOCK_EQUAL && got->len == w.len;
}

static void test_merge_takes_the_max_of_each_entry(void) {
  vclock a = clock_of("1:4,3:1");
  vclock b = clock_of("1:2,2:7,3:5");
  assert(vclock_merge(&a, &b) == 0);
  assert(same(&a, "1:4,2:7,3:5"));
  printf("  test_merge_takes_the_max_of_each_entry OK\n");
}

static void test_increment_adds_one_to_the_entry_of_the_node(void) {
  vclock c = clock_of("2:3");
  assert(vclock_increment(&c, 2) == 0);
  assert(same(&c, "2:4"));
  assert(vclock_increment(&c, 1) == 0);
  assert(same(&c, "1:1,2:4"));
  printf("  test_increment_adds_one_to_the_entry_of_the_node OK\n");
}

static void test_a_full_clock_refuses_a_new_node(void) {
  vclock c = clock_of("");
  for (int id = 1; id <= VCLOCK_MAX; id++) {
    assert(vclock_increment(&c, id) == 0);
  }
  assert(vclock_increment(&c, VCLOCK_MAX + 1) == -1);
  assert(vclock_increment(&c, 1) == 0);
  printf("  test_a_full_clock_refuses_a_new_node OK\n");
}

static vstamp stamp(const char *clock, uint64_t ts, int coord) {
  return (vstamp){clock_of(clock), ts, coord};
}

static void test_dominating_clock_wins_whatever_the_timestamp(void) {
  vstamp older = stamp("1:1", 900, 3);
  vstamp newer = stamp("1:2", 100, 1);
  vstamp out;
  assert(vstamp_pick(&older, &newer, &out) == 1);
  assert(same(&out.clock, "1:2") && out.ts == 100 && out.coord == 1);
  assert(vstamp_pick(&newer, &older, &out) == 0);
  assert(same(&out.clock, "1:2") && out.ts == 100 && out.coord == 1);
  printf("  test_dominating_clock_wins_whatever_the_timestamp OK\n");
}

static void test_concurrent_clocks_pick_the_later_timestamp_with_the_merged_clock(void) {
  vstamp a = stamp("1:2", 500, 1);
  vstamp b = stamp("1:1,2:1", 400, 2);
  vstamp out;
  assert(vstamp_pick(&a, &b, &out) == 0);
  assert(same(&out.clock, "1:2,2:1") && out.ts == 500 && out.coord == 1);
  assert(vstamp_pick(&b, &a, &out) == 1);
  assert(same(&out.clock, "1:2,2:1") && out.ts == 500 && out.coord == 1);
  printf("  test_concurrent_clocks_pick_the_later_timestamp_with_the_merged_clock OK\n");
}

static void test_concurrent_clocks_with_one_timestamp_pick_the_higher_coordinator(void) {
  vstamp a = stamp("2:1", 500, 2);
  vstamp b = stamp("3:1", 500, 3);
  vstamp out;
  assert(vstamp_pick(&a, &b, &out) == 1);
  assert(same(&out.clock, "2:1,3:1") && out.coord == 3);
  assert(vstamp_pick(&b, &a, &out) == 0);
  printf("  test_concurrent_clocks_with_one_timestamp_pick_the_higher_coordinator OK\n");
}

static void test_equal_clocks_pick_by_timestamp_then_the_first(void) {
  vstamp a = stamp("1:1", 10, 1);
  vstamp b = stamp("1:1", 20, 1);
  vstamp out;
  assert(vstamp_pick(&a, &b, &out) == 1 && out.ts == 20);
  assert(vstamp_pick(&a, &a, &out) == 0);
  vstamp none = stamp("", 0, 0);
  assert(vstamp_pick(&none, &none, &out) == 0);
  printf("  test_equal_clocks_pick_by_timestamp_then_the_first OK\n");
}

static void test_encode_writes_entries_in_id_order(void) {
  vclock c = clock_of("");
  char text[VCLOCK_TEXT_MAX];
  assert(vclock_encode(&c, text) == 0 && strcmp(text, "") == 0);
  assert(vclock_increment(&c, 12) == 0 && vclock_increment(&c, 3) == 0);
  assert(vclock_increment(&c, 3) == 0);
  assert(vclock_encode(&c, text) == 8 && strcmp(text, "3:2,12:1") == 0);
  printf("  test_encode_writes_entries_in_id_order OK\n");
}

static void test_a_full_clock_with_the_largest_counters_fits_the_text_buffer(void) {
  vclock c = {0};
  for (int i = 0; i != VCLOCK_MAX; i++) {
    c.e[c.len++] = (vclock_entry){INT32_MAX - VCLOCK_MAX + i + 1, UINT64_MAX};
  }
  char text[VCLOCK_TEXT_MAX];
  size_t len = vclock_encode(&c, text);
  assert(len == strlen(text) && len < sizeof(text));
  vclock back;
  assert(vclock_decode(text, len, &back) == 0 && back.len == VCLOCK_MAX);
  assert(back.e[0].id == INT32_MAX - VCLOCK_MAX + 1 && back.e[0].n == UINT64_MAX);
  printf("  test_a_full_clock_with_the_largest_counters_fits_the_text_buffer OK\n");
}

static void test_decode_refuses_malformed_text(void) {
  static const char *const bad[] = {
      "1",
      "1:",
      ":1",
      "1:0",
      "0:1",
      "2:1,1:1",
      "1:1,1:2",
      "1:1,",
      ",1:1",
      "1:1;2:1",
      "a:1",
      "1:18446744073709551616",
      "2147483648:1",
  };
  for (size_t i = 0; i != sizeof(bad) / sizeof(bad[0]); i++) {
    vclock c;
    assert(vclock_decode(bad[i], strlen(bad[i]), &c) == -1);
  }
  printf("  test_decode_refuses_malformed_text OK\n");
}

int main(void) {
  printf("vclock tests:\n");
  test_compare_orders_clocks();
  test_merge_takes_the_max_of_each_entry();
  test_increment_adds_one_to_the_entry_of_the_node();
  test_a_full_clock_refuses_a_new_node();
  test_dominating_clock_wins_whatever_the_timestamp();
  test_concurrent_clocks_pick_the_later_timestamp_with_the_merged_clock();
  test_concurrent_clocks_with_one_timestamp_pick_the_higher_coordinator();
  test_equal_clocks_pick_by_timestamp_then_the_first();
  test_encode_writes_entries_in_id_order();
  test_a_full_clock_with_the_largest_counters_fits_the_text_buffer();
  test_decode_refuses_malformed_text();
  printf("all vclock tests passed\n");
  return 0;
}
