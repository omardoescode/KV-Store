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
  vclock a = clock_of("1:2:0,3:1:0");
  vclock b = clock_of("1:2:0,3:1:0");
  assert(vclock_compare(&a, &b) == VCLOCK_EQUAL);
  b = clock_of("1:3:0,3:1:0");
  assert(vclock_compare(&a, &b) == VCLOCK_BEFORE);
  assert(vclock_compare(&b, &a) == VCLOCK_AFTER);
  b = clock_of("1:1:0,2:1:0,3:1:0");
  assert(vclock_compare(&a, &b) == VCLOCK_CONCURRENT);
  vclock empty = clock_of("");
  assert(vclock_compare(&empty, &a) == VCLOCK_BEFORE);
  printf("  test_compare_orders_clocks OK\n");
}

static bool same(const vclock *got, const char *want) {
  vclock w = clock_of(want);
  return vclock_compare(got, &w) == VCLOCK_EQUAL && got->len == w.len;
}

static void test_merge_takes_the_max_of_each_entry_with_its_timestamp(void) {
  vclock a = clock_of("1:4:40,3:1:10");
  vclock b = clock_of("1:2:20,2:7:70,3:5:50");
  vclock_merge(&a, &b);
  char text[VCLOCK_TEXT_MAX];
  (void)vclock_encode(&a, text);
  assert(strcmp(text, "1:4:40,2:7:70,3:5:50") == 0);
  printf("  test_merge_takes_the_max_of_each_entry_with_its_timestamp OK\n");
}

static void test_increment_adds_one_to_the_entry_of_the_node_and_stamps_it(void) {
  vclock c = clock_of("2:3:5");
  vclock_increment(&c, 2, 8);
  vclock_increment(&c, 1, 9);
  char text[VCLOCK_TEXT_MAX];
  (void)vclock_encode(&c, text);
  assert(strcmp(text, "1:1:9,2:4:8") == 0);
  printf("  test_increment_adds_one_to_the_entry_of_the_node_and_stamps_it OK\n");
}

/** A clock of ids 1..VCLOCK_MAX where id i was last stamped at ts 100 + i, but id 5 at ts 1. */
static vclock full_clock(void) {
  vclock c = clock_of("");
  for (int id = 1; id <= VCLOCK_MAX; id++) {
    vclock_increment(&c, id, id == 5 ? 1 : 100 + (uint64_t)id);
  }
  return c;
}

static bool has(const vclock *c, int id) {
  for (size_t i = 0; i != c->len; i++) {
    if (c->e[i].id == id) {
      return true;
    }
  }
  return false;
}

static void test_a_full_clock_drops_its_oldest_entry_for_a_new_node(void) {
  vclock c = full_clock();
  vclock_increment(&c, VCLOCK_MAX + 1, 1000);
  assert(c.len == VCLOCK_MAX && !has(&c, 5) && has(&c, 1) && has(&c, VCLOCK_MAX + 1));
  vclock_increment(&c, 2, 1001);
  assert(c.len == VCLOCK_MAX && has(&c, 1));
  printf("  test_a_full_clock_drops_its_oldest_entry_for_a_new_node OK\n");
}

static void test_a_merge_over_the_limit_keeps_the_newest_entries(void) {
  vclock a = full_clock();
  vclock b = clock_of("");
  vclock_increment(&b, VCLOCK_MAX + 1, 1000);
  vclock_increment(&b, VCLOCK_MAX + 2, 2);
  vclock_merge(&a, &b);
  assert(a.len == VCLOCK_MAX && !has(&a, 5) && !has(&a, VCLOCK_MAX + 2) && has(&a, 1));
  assert(has(&a, VCLOCK_MAX + 1));
  vstamp sa = {full_clock(), 1, 1};
  vstamp sb = {b, 2, 2};
  vstamp out;
  assert(vstamp_pick(&sa, &sb, &out) == 1 && out.clock.len == VCLOCK_MAX);
  printf("  test_a_merge_over_the_limit_keeps_the_newest_entries OK\n");
}

static vstamp stamp(const char *clock, uint64_t ts, int coord) {
  return (vstamp){clock_of(clock), ts, coord};
}

static void test_dominating_clock_wins_whatever_the_timestamp(void) {
  vstamp older = stamp("1:1:0", 900, 3);
  vstamp newer = stamp("1:2:0", 100, 1);
  vstamp out;
  assert(vstamp_pick(&older, &newer, &out) == 1);
  assert(same(&out.clock, "1:2:0") && out.ts == 100 && out.coord == 1);
  assert(vstamp_pick(&newer, &older, &out) == 0);
  assert(same(&out.clock, "1:2:0") && out.ts == 100 && out.coord == 1);
  printf("  test_dominating_clock_wins_whatever_the_timestamp OK\n");
}

static void test_concurrent_clocks_pick_the_later_timestamp_with_the_merged_clock(void) {
  vstamp a = stamp("1:2:0", 500, 1);
  vstamp b = stamp("1:1:0,2:1:0", 400, 2);
  vstamp out;
  assert(vstamp_pick(&a, &b, &out) == 0);
  assert(same(&out.clock, "1:2:0,2:1:0") && out.ts == 500 && out.coord == 1);
  assert(vstamp_pick(&b, &a, &out) == 1);
  assert(same(&out.clock, "1:2:0,2:1:0") && out.ts == 500 && out.coord == 1);
  printf("  test_concurrent_clocks_pick_the_later_timestamp_with_the_merged_clock OK\n");
}

static void test_concurrent_clocks_with_one_timestamp_pick_the_higher_coordinator(void) {
  vstamp a = stamp("2:1:0", 500, 2);
  vstamp b = stamp("3:1:0", 500, 3);
  vstamp out;
  assert(vstamp_pick(&a, &b, &out) == 1);
  assert(same(&out.clock, "2:1:0,3:1:0") && out.coord == 3);
  assert(vstamp_pick(&b, &a, &out) == 0);
  printf("  test_concurrent_clocks_with_one_timestamp_pick_the_higher_coordinator OK\n");
}

static void test_equal_clocks_pick_by_timestamp_then_the_first(void) {
  vstamp a = stamp("1:1:0", 10, 1);
  vstamp b = stamp("1:1:0", 20, 1);
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
  vclock_increment(&c, 12, 7);
  vclock_increment(&c, 3, 8);
  vclock_increment(&c, 3, 9);
  assert(vclock_encode(&c, text) == 12 && strcmp(text, "3:2:9,12:1:7") == 0);
  printf("  test_encode_writes_entries_in_id_order OK\n");
}

static void test_a_full_clock_with_the_largest_counters_fits_the_text_buffer(void) {
  vclock c = {0};
  for (int i = 0; i != VCLOCK_MAX; i++) {
    c.e[c.len++] = (vclock_entry){INT32_MAX - VCLOCK_MAX + i + 1, UINT64_MAX, UINT64_MAX};
  }
  char text[VCLOCK_TEXT_MAX];
  size_t len = vclock_encode(&c, text);
  assert(len == strlen(text) && len < sizeof(text));
  vclock back;
  assert(vclock_decode(text, len, &back) == 0 && back.len == VCLOCK_MAX);
  assert(back.e[0].id == INT32_MAX - VCLOCK_MAX + 1 && back.e[0].n == UINT64_MAX);
  assert(back.e[0].ts == UINT64_MAX);
  printf("  test_a_full_clock_with_the_largest_counters_fits_the_text_buffer OK\n");
}

static void test_decode_refuses_malformed_text(void) {
  static const char *const bad[] = {
      "1",
      "1:",
      ":1",
      "1:1",
      "1:1:",
      "1:0:0",
      "0:1:0",
      "2:1:0,1:1:0",
      "1:1:0,1:2:0",
      "1:1:0,",
      ",1:1:0",
      "1:1:0;2:1:0",
      "a:1:0",
      "1:1:a",
      "1:1:0:0",
      "1:18446744073709551616:0",
      "1:1:18446744073709551616",
      "2147483648:1:0",
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
  test_merge_takes_the_max_of_each_entry_with_its_timestamp();
  test_increment_adds_one_to_the_entry_of_the_node_and_stamps_it();
  test_a_full_clock_drops_its_oldest_entry_for_a_new_node();
  test_a_merge_over_the_limit_keeps_the_newest_entries();
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
