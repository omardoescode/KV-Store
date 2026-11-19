#include "cp/ring.h"

#include <assert.h>
#include <stdio.h>
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

int main(void) {
  printf("ring tests:\n");
  test_fixed_owners();
  test_order_of_ids_does_not_matter();
  test_prefs_are_distinct();
  test_share_is_balanced();
  test_join_moves_about_one_nth();
  test_empty_ring();
  printf("all ring tests passed\n");
  return 0;
}
