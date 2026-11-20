#include "cp/cluster.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Owners with vnodes 128 and 1 replica: {1,2,3} -> {1,2,3,4} moves k2 from 1 and k7 from 2 to
// node 4; k0 stays on 2.

static void quiet(void *ctx, log_level level, const char *fmt, ...) {
  (void)ctx;
  (void)level;
  (void)fmt;
}

static const cp_config cfg = {
    .max_nodes = 4,
    .replicas = 1,
    .vnodes = 128,
    .probe_fails = 2,
};

typedef struct {
  cp_store *store;
  cluster *c;
} fixture;

static fixture three_nodes(void) {
  fixture f;
  f.store = cp_store_open(":memory:");
  assert(f.store);
  f.c = cluster_new(&cfg, f.store, (logger){quiet, NULL});
  assert(f.c);
  for (int id = 1; id <= 3; id++) {
    char addr[16];
    (void)snprintf(addr, sizeof(addr), "n%d", id);
    assert(cluster_put_node(f.c, id, addr, (node_sm){NODE_ACTIVE, 0, false}, true) == 0);
  }
  cluster_reserve_versions(f.c);
  return f;
}

static void done(fixture f) {
  cluster_free(f.c);
  cp_store_close(f.store);
}

/** Start a join of node 4 and run it up to COPY. */
static void copy_to_4(cluster *c) {
  assert(cluster_begin_join(c) == 4);
  assert(cluster_put_node(c, 4, "", (node_sm){NODE_SPAWNING, 0, false}, false) == 0);
  (void)cluster_spawned(c, 4, "n4");
  assert(cluster_set_phase(c, MIG_COPY, 4) == 0);
  assert(cluster_begin_copy(c, 4) == 0);
}

static int ids(cluster *c, const char *key, bool write, int out[3]) {
  plan p;
  int rc = write ? cluster_plan_write(c, key, strlen(key), &p)
                 : cluster_plan_read(c, key, strlen(key), &p);
  if (rc != 0) {
    return 0;
  }
  cluster_done(c, p.epoch);
  for (size_t i = 0; i != p.count; i++) {
    out[i] = p.targets[i].id;
  }
  return (int)p.count;
}

static void test_copy_writes_moving_keys_to_target(void) {
  fixture f = three_nodes();
  copy_to_4(f.c);
  int got[3];
  assert(ids(f.c, "k2", true, got) == 2 && got[0] == 1 && got[1] == 4);
  assert(ids(f.c, "k0", true, got) == 1 && got[0] == 2);
  assert(ids(f.c, "k2", false, got) == 1 && got[0] == 1);
  done(f);
  printf("  test_copy_writes_moving_keys_to_target OK\n");
}

static void test_flip_moves_reads_to_new_ring(void) {
  fixture f = three_nodes();
  copy_to_4(f.c);
  cluster_flip(f.c, 4);
  int got[3];
  assert(ids(f.c, "k2", false, got) == 1 && got[0] == 4);
  assert(ids(f.c, "k7", false, got) == 1 && got[0] == 4);
  assert(ids(f.c, "k0", false, got) == 1 && got[0] == 2);
  assert(ids(f.c, "k2", true, got) == 1 && got[0] == 4);
  done(f);
  printf("  test_flip_moves_reads_to_new_ring OK\n");
}

static void test_abort_restores_old_plans(void) {
  fixture f = three_nodes();
  copy_to_4(f.c);
  cluster_abort_join(f.c, 4);
  assert(cluster_drop_node(f.c, 4) == 0);
  assert(cluster_set_phase(f.c, MIG_IDLE, 0) == 0);
  int got[3];
  assert(ids(f.c, "k2", true, got) == 1 && got[0] == 1);
  assert(ids(f.c, "k7", false, got) == 1 && got[0] == 2);
  cluster_view *v = malloc(sizeof(*v));
  assert(v);
  cluster_snapshot(f.c, v);
  assert(v->count == 3 && !v->nodes[4].exists && v->phase == MIG_IDLE);
  free(v);
  done(f);
  printf("  test_abort_restores_old_plans OK\n");
}

static void test_add_request_stops_at_max_nodes(void) {
  fixture f = three_nodes();
  cluster_view *v = malloc(sizeof(*v));
  assert(v);
  assert(cluster_request_add(f.c));
  cluster_snapshot(f.c, v);
  assert(v->count == 3 && v->add_requested);
  copy_to_4(f.c);
  cluster_snapshot(f.c, v);
  assert(v->count == 4 && !v->add_requested);
  assert(!cluster_request_add(f.c));
  free(v);
  done(f);
  printf("  test_add_request_stops_at_max_nodes OK\n");
}

int main(void) {
  printf("cluster tests:\n");
  test_copy_writes_moving_keys_to_target();
  test_flip_moves_reads_to_new_ring();
  test_abort_restores_old_plans();
  test_add_request_stops_at_max_nodes();
  printf("all cluster tests passed\n");
  return 0;
}
