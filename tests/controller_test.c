#include "cp/controller.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cp/driver.h"
#include "cp_fake.h"

// Owners with vnodes 128. 1 replica, {1,2,3} -> {1,2,3,4}: k2 moves from 1 to 4, k7 from 2 to 4,
// k0 stays on 2. 2 replicas, {1,2,3}: k0 is on 2 and 3.

static void quiet(void *ctx, log_level level, const char *fmt, ...) {
  (void)ctx;
  (void)level;
  (void)fmt;
}

static cp_config config(int replicas) {
  return (cp_config){
      .initial_nodes = 3,
      .max_nodes = 8,
      .replicas = replicas,
      .vnodes = 128,
      .scale_window_s = 60,
      .probe_interval_ms = 1,
      .probe_fails = 2,
      .forward_timeout_ms = 100,
  };
}

typedef struct {
  cp_config cfg;
  cp_store *store;
  cluster *c;
  controller *ctl;
} world;

static void boot(world *w) {
  logger log = {quiet, NULL};
  w->c = cluster_new(&w->cfg, w->store, log);
  assert(w->c);
  w->ctl = controller_new(w->c, w->store, &w->cfg, log);
  assert(w->ctl && controller_start_up(w->ctl) == 0);
}

static world *start(int replicas) {
  fake_reset();
  world *w = calloc(1, sizeof(*w));
  assert(w);
  w->cfg = config(replicas);
  w->store = cp_store_open(":memory:");
  assert(w->store);
  boot(w);
  return w;
}

static void stop(world *w) {
  controller_free(w->ctl);
  cluster_free(w->c);
  cp_store_close(w->store);
  free(w);
}

static cluster_view *view(world *w) {
  static cluster_view v;
  cluster_snapshot(w->c, &v);
  return &v;
}

static bool any_purge_paused(void) {
  for (int id = 1; id <= 4; id++) {
    if (fake_purge_paused(id)) {
      return true;
    }
  }
  return false;
}

static bool is(const char *got, const char *want) {
  return got && want ? strcmp(got, want) == 0 : got == want;
}

static void test_join_moves_keys_to_the_new_node(void) {
  world *w = start(1);
  fake_put(1, "k2", "a");
  fake_put(2, "k7", "b");
  fake_put(2, "k0", "c");
  assert(cluster_request_add(w->c));
  controller_tick(w->ctl);
  cluster_view *v = view(w);
  assert(v->phase == MIG_IDLE && v->nodes[4].in_ring && v->nodes[4].sm.state == NODE_ACTIVE);
  assert(is(fake_get(4, "k2"), "a") && is(fake_get(4, "k7"), "b") && is(fake_get(4, "k0"), NULL));
  assert(is(fake_get(1, "k2"), NULL) && is(fake_get(2, "k7"), NULL));
  assert(is(fake_get(2, "k0"), "c"));
  assert(!any_purge_paused());
  stop(w);
  printf("  test_join_moves_keys_to_the_new_node OK\n");
}

static void test_dead_node_is_respawned_and_repaired(void) {
  world *w = start(2);
  fake_put(2, "k0", "old");
  fake_put(3, "k0", "old");
  fake_kill(3);
  fake_put(2, "k0", "new");
  for (int i = 0; i != 2; i++) {
    controller_tick(w->ctl);
  }
  cluster_view *v = view(w);
  assert(v->nodes[3].sm.state == NODE_ACTIVE && fake_running(3));
  assert(is(fake_get(3, "k0"), "new"));
  assert(!any_purge_paused());
  stop(w);
  printf("  test_dead_node_is_respawned_and_repaired OK\n");
}

static void test_purge_retries_until_old_owner_drops_moved_keys(void) {
  world *w = start(1);
  fake_put(1, "k2", "a");
  fake_fault("PURGE", 1);
  assert(cluster_request_add(w->c));
  controller_tick(w->ctl);
  assert(view(w)->phase == MIG_PURGE);
  for (int i = 0; i != 3; i++) {
    controller_tick(w->ctl);
  }
  cluster_view *v = view(w);
  assert(v->phase == MIG_IDLE && v->nodes[1].sm.state == NODE_ACTIVE);
  assert(is(fake_get(1, "k2"), NULL) && is(fake_get(4, "k2"), "a"));
  assert(!any_purge_paused());
  stop(w);
  printf("  test_purge_retries_until_old_owner_drops_moved_keys OK\n");
}

static void test_join_aborts_before_flip_then_retries(void) {
  world *w = start(1);
  fake_put(1, "k2", "a");
  fake_fault("COPY", 0);
  assert(cluster_request_add(w->c));
  controller_tick(w->ctl);
  cluster_view *v = view(w);
  assert(v->phase == MIG_IDLE && v->count == 3 && !v->nodes[4].exists && v->add_requested);
  assert(is(fake_get(1, "k2"), "a"));
  assert(!any_purge_paused());
  controller_tick(w->ctl);
  v = view(w);
  assert(v->phase == MIG_IDLE && v->nodes[4].in_ring);
  assert(is(fake_get(4, "k2"), "a") && is(fake_get(1, "k2"), NULL));
  stop(w);
  printf("  test_join_aborts_before_flip_then_retries OK\n");
}

static void test_new_cp_finishes_a_join_stored_in_flip(void) {
  world *w = start(1);
  char addr[DRIVER_ADDR_LEN];
  assert(driver_spawn(4, addr) == 0);
  assert(cp_store_put_node(w->store, &(stored_node){4, "", {NODE_SYNCING, 0, false}, false}) == 0);
  assert(cp_store_put_migration(w->store, (stored_migration){MIG_FLIP, 4}) == 0);
  fake_put(1, "k2", "a");
  fake_put(4, "k2", "a");
  controller_free(w->ctl);
  cluster_free(w->c);
  boot(w);
  cluster_view *v = view(w);
  assert(v->phase == MIG_IDLE && v->nodes[4].in_ring && v->nodes[4].sm.state == NODE_ACTIVE);
  assert(is(fake_get(1, "k2"), NULL) && is(fake_get(4, "k2"), "a"));
  plan p;
  assert(cluster_plan_read(w->c, "k2", 2, &p) == 0 && p.count == 1 && p.targets[0].id == 4);
  cluster_done(w->c, p.epoch);
  stop(w);
  printf("  test_new_cp_finishes_a_join_stored_in_flip OK\n");
}

static void test_new_cp_aborts_a_join_stored_in_copy(void) {
  world *w = start(1);
  char addr[DRIVER_ADDR_LEN];
  assert(driver_spawn(4, addr) == 0);
  assert(cp_store_put_node(w->store, &(stored_node){4, "", {NODE_SYNCING, 0, false}, false}) == 0);
  assert(cp_store_put_migration(w->store, (stored_migration){MIG_COPY, 4}) == 0);
  fake_put(1, "k2", "a");
  controller_free(w->ctl);
  cluster_free(w->c);
  boot(w);
  cluster_view *v = view(w);
  assert(v->phase == MIG_IDLE && !v->nodes[4].exists && !fake_running(4));
  assert(is(fake_get(1, "k2"), "a"));
  stop(w);
  printf("  test_new_cp_aborts_a_join_stored_in_copy OK\n");
}

int main(void) {
  printf("controller tests:\n");
  test_join_moves_keys_to_the_new_node();
  test_dead_node_is_respawned_and_repaired();
  test_purge_retries_until_old_owner_drops_moved_keys();
  test_join_aborts_before_flip_then_retries();
  test_new_cp_finishes_a_join_stored_in_flip();
  test_new_cp_aborts_a_join_stored_in_copy();
  printf("all controller tests passed\n");
  return 0;
}
