#include "cp/ring.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int owner_of(const ring *r, const char *key) {
  int prefs[RING_MAX_REPLICAS];
  assert(ring_prefs(r, key, strlen(key), prefs) >= 1);
  return prefs[0];
}

static ring *make(const int *ids, size_t n, int vnodes, int replicas) {
  return ring_new(ids, n, (ring_params){vnodes, replicas});
}

static void test_fixed_owners(void) {
  int ids[] = {1, 2, 3};
  ring *r = make(ids, 3, 128, 1);
  assert(r);
  // Pinned: cp and tests must agree on owners across builds.
  assert(owner_of(r, "key_0") == 3);
  assert(owner_of(r, "key_1") == 3);
  assert(owner_of(r, "hello") == 2);
  ring_free(r);
  printf("  test_fixed_owners OK\n");
}

static void test_order_of_ids_does_not_matter(void) {
  int a[] = {1, 2, 3};
  int b[] = {3, 1, 2};
  ring *ra = make(a, 3, 64, 1);
  ring *rb = make(b, 3, 64, 1);
  char key[32];
  for (int i = 0; i != 1000; i++) {
    (void)snprintf(key, sizeof(key), "k%d", i);
    assert(owner_of(ra, key) == owner_of(rb, key));
  }
  ring_free(ra);
  ring_free(rb);
  printf("  test_order_of_ids_does_not_matter OK\n");
}

static void test_prefs_are_distinct(void) {
  int ids[] = {1, 2, 3, 4};
  ring *r = make(ids, 4, 128, 3);
  ring *all = make(ids, 4, 128, 9);
  char key[32];
  for (int i = 0; i != 1000; i++) {
    (void)snprintf(key, sizeof(key), "k%d", i);
    int prefs[RING_MAX_REPLICAS];
    assert(ring_prefs(r, key, strlen(key), prefs) == 3);
    assert(prefs[0] != prefs[1] && prefs[0] != prefs[2] && prefs[1] != prefs[2]);
    assert(ring_has(r, prefs[2], key, strlen(key)));
    // More replicas than nodes gives every node once.
    assert(ring_prefs(all, key, strlen(key), prefs) == 4);
  }
  ring_free(r);
  ring_free(all);
  printf("  test_prefs_are_distinct OK\n");
}

static void test_share_is_balanced(void) {
  int ids[] = {1, 2, 3};
  ring *r = make(ids, 3, 128, 1);
  int count[4] = {0};
  char key[32];
  for (int i = 0; i != 30000; i++) {
    (void)snprintf(key, sizeof(key), "key_%d", i);
    count[owner_of(r, key)]++;
  }
  for (int id = 1; id <= 3; id++) {
    assert(count[id] > 7000 && count[id] < 13000);
  }
  ring_free(r);
  printf("  test_share_is_balanced OK\n");
}

static void test_join_moves_about_one_nth(void) {
  int before[] = {1, 2, 3};
  int after[] = {1, 2, 3, 4};
  ring *r1 = make(before, 3, 128, 1);
  ring *r2 = make(after, 4, 128, 1);
  int moved = 0;
  char key[32];
  for (int i = 0; i != 40000; i++) {
    (void)snprintf(key, sizeof(key), "key_%d", i);
    int o1 = owner_of(r1, key);
    int o2 = owner_of(r2, key);
    if (o1 != o2) {
      // A key only moves to the new node.
      assert(o2 == 4);
      moved++;
    }
  }
  assert(moved > 7000 && moved < 13000);
  ring_free(r1);
  ring_free(r2);
  printf("  test_join_moves_about_one_nth OK\n");
}

static void test_empty_ring(void) {
  ring *r = make(NULL, 0, 128, 1);
  assert(r);
  int prefs[RING_MAX_REPLICAS];
  assert(ring_prefs(r, "k", 1, prefs) == 0);
  ring_free(r);
  printf("  test_empty_ring OK\n");
}

// Owners with vnodes 128 and 2 replicas, {1,2,3} -> {1,2,3,4}: k2 {1,3} -> {4,1},
// k7 {2,3} -> {4,2}, k6 {1,3} -> {1,4}, k0 {2,3} -> {2,3}.
typedef struct {
  ring *old;
  ring *now;
  bool readers[5];
} join_rings;

static join_rings join_3_to_4(void) {
  int before[] = {1, 2, 3};
  int after[] = {1, 2, 3, 4};
  return (join_rings){make(before, 3, 128, 2), make(after, 4, 128, 2), {0, 1, 1, 1, 1}};
}

static int source(const join_rings *j, const char *key) {
  xfer x = {j->old, j->now, j->readers};
  return xfer_source(&x, key, strlen(key), 4);
}

static void test_join_has_one_source_per_moved_key(void) {
  join_rings j = join_3_to_4();
  assert(source(&j, "k2") == 1);
  assert(source(&j, "k7") == 2);
  assert(source(&j, "k6") == 1);
  assert(source(&j, "k0") == 0);
  ring_free(j.old);
  ring_free(j.now);
  printf("  test_join_has_one_source_per_moved_key OK\n");
}

static void test_dead_reader_is_skipped(void) {
  join_rings j = join_3_to_4();
  j.readers[1] = false;
  assert(source(&j, "k2") == 3);
  j.readers[3] = false;
  assert(source(&j, "k2") == 0);
  ring_free(j.old);
  ring_free(j.now);
  printf("  test_dead_reader_is_skipped OK\n");
}

static void test_repair_sends_each_key_of_the_node_from_one_reader(void) {
  join_rings j = join_3_to_4();
  xfer x = {j.now, j.now, j.readers};
  assert(xfer_source(&x, "k2", 2, 1) == 4);
  assert(xfer_source(&x, "k0", 2, 3) == 2);
  assert(xfer_source(&x, "k0", 2, 1) == 0);
  ring_free(j.old);
  ring_free(j.now);
  printf("  test_repair_sends_each_key_of_the_node_from_one_reader OK\n");
}

static void test_purge_drops_keys_a_node_lost(void) {
  join_rings j = join_3_to_4();
  xfer x = {j.old, j.now, j.readers};
  assert(xfer_drops(&x, 3, "k2", 2));
  assert(!xfer_drops(&x, 1, "k2", 2));
  assert(xfer_drops(&x, 3, "k7", 2));
  assert(!xfer_drops(&x, 2, "k7", 2));
  assert(!xfer_drops(&x, 3, "k0", 2));
  assert(!xfer_drops(&x, 4, "k2", 2));
  ring_free(j.old);
  ring_free(j.now);
  printf("  test_purge_drops_keys_a_node_lost OK\n");
}

static bool changed(const ring_range *ranges, size_t n, const char *key) {
  return ring_ranges_have(ranges, n, key, strlen(key));
}

static void test_diff_has_keys_whose_prefs_change(void) {
  join_rings j = join_3_to_4();
  ring_range *ranges = NULL;
  size_t n = 0;
  assert(ring_diff(j.old, j.now, &ranges, &n) == 0);
  assert(n > 0);
  assert(changed(ranges, n, "k2"));
  assert(changed(ranges, n, "k7"));
  assert(changed(ranges, n, "k6"));
  assert(!changed(ranges, n, "k0"));
  free(ranges);
  ring_free(j.old);
  ring_free(j.now);
  printf("  test_diff_has_keys_whose_prefs_change OK\n");
}

static void test_diff_of_same_ring_is_empty(void) {
  join_rings j = join_3_to_4();
  ring_range *ranges = NULL;
  size_t n = 7;
  assert(ring_diff(j.now, j.now, &ranges, &n) == 0);
  assert(n == 0);
  free(ranges);
  ring_free(j.old);
  ring_free(j.now);
  printf("  test_diff_of_same_ring_is_empty OK\n");
}

static void test_diff_from_empty_ring_is_whole_ring(void) {
  int ids[] = {1};
  ring *none = make(NULL, 0, 4, 1);
  ring *one = make(ids, 1, 4, 1);
  ring_range *ranges = NULL;
  size_t n = 0;
  assert(ring_diff(none, one, &ranges, &n) == 0);
  assert(n == 1 && ranges[0].start == ranges[0].end);
  assert(changed(ranges, n, "k0") && changed(ranges, n, "hello"));
  free(ranges);
  ring_free(none);
  ring_free(one);
  printf("  test_diff_from_empty_ring_is_whole_ring OK\n");
}

int main(void) {
  printf("ring tests:\n");
  test_fixed_owners();
  test_order_of_ids_does_not_matter();
  test_prefs_are_distinct();
  test_share_is_balanced();
  test_join_moves_about_one_nth();
  test_empty_ring();
  test_join_has_one_source_per_moved_key();
  test_dead_reader_is_skipped();
  test_repair_sends_each_key_of_the_node_from_one_reader();
  test_purge_drops_keys_a_node_lost();
  test_diff_has_keys_whose_prefs_change();
  test_diff_of_same_ring_is_empty();
  test_diff_from_empty_ring_is_whole_ring();
  printf("all ring tests passed\n");
  return 0;
}
