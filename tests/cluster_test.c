#include "cp/cluster.h"

#include <assert.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Owners with vnodes 128 and 1 replica: {1,2,3} -> {1,2,3,4} moves k2 from 1 and k7 from 2 to
// node 4; k0 stays on 2. With 2 replicas k0 is on 2 and 3; with 3 replicas every key is on all.

static void quiet(void *ctx, log_level level, const char *fmt, ...) {
  (void)ctx;
  (void)level;
  (void)fmt;
}

static const cp_config cfg = {
    .max_nodes = 4,
    .replicas = 1,
    .read_quorum = 1,
    .write_quorum = 1,
    .vnodes = 128,
    .probe_fails = 2,
};
static const cp_config cfg_rf3 = {
    .max_nodes = 4,
    .replicas = 3,
    .read_quorum = 2,
    .write_quorum = 2,
    .vnodes = 128,
    .probe_fails = 2,
};

typedef struct {
  cp_store *store;
  cluster *c;
} fixture;

static fixture three_nodes_with(const cp_config *config) {
  fixture f;
  f.store = cp_store_open(":memory:");
  assert(f.store);
  f.c = cluster_new(config, f.store, (logger){quiet, NULL});
  assert(f.c);
  for (int id = 1; id <= 3; id++) {
    char addr[16];
    (void)snprintf(addr, sizeof(addr), "n%d", id);
    assert(cluster_put_node(f.c, id, addr, (node_sm){NODE_ACTIVE, 0, false}, true) == 0);
  }
  cluster_reserve_ts(f.c);
  return f;
}

static fixture three_nodes(void) { return three_nodes_with(&cfg); }

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

/** Write the nodes a write goes to, or a read may ask, to out; return their count. */
static int ids(cluster *c, const char *key, bool write, int out[3]) {
  plan p;
  int rc = write ? cluster_plan_write(c, key, strlen(key), &p)
                 : cluster_plan_read(c, key, strlen(key), &p);
  if (rc != 0) {
    return 0;
  }
  cluster_done(c, p.epoch);
  int n = 0;
  for (size_t i = 0; i != p.count; i++) {
    if (write || p.targets[i].reads) {
      out[n++] = p.targets[i].id;
    }
  }
  return n;
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

static uint64_t write_ts(cluster *c, const char *key) {
  plan p;
  if (cluster_plan_write(c, key, strlen(key), &p) != 0) {
    return 0;
  }
  cluster_done(c, p.epoch);
  return p.ts;
}

static void test_timestamps_after_restart_are_above_the_stored_mark(void) {
  fixture f = three_nodes();
  uint64_t before = write_ts(f.c, "k0");
  assert(before != 0);
  uint64_t mark = 0;
  assert(cp_store_ts_mark(f.store, &mark) == 0);
  assert(mark > before);
  cluster_free(f.c);
  f.c = cluster_new(&cfg, f.store, (logger){quiet, NULL});
  assert(f.c);
  assert(cluster_put_node(f.c, 2, "n2", (node_sm){NODE_ACTIVE, 0, false}, true) == 0);
  // Nothing is reserved above the mark yet: the timestamp would pass it, so the write is refused.
  assert(write_ts(f.c, "k0") == 0);
  cluster_reserve_ts(f.c);
  assert(write_ts(f.c, "k0") > mark);
  done(f);
  printf("  test_timestamps_after_restart_are_above_the_stored_mark OK\n");
}

static void test_an_older_cp_db_gives_its_version_mark_as_the_ts_mark(void) {
  char path[] = "/tmp/kv-cluster-test-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  (void)close(fd);
  cp_store *store = cp_store_open(path);
  assert(store);
  cp_store_close(store);
  sqlite3 *db = NULL;
  assert(sqlite3_open(path, &db) == SQLITE_OK);
  assert(sqlite3_exec(db, "INSERT INTO meta(key, value) VALUES('version_mark', '123456789')", NULL,
                      NULL, NULL) == SQLITE_OK);
  sqlite3_close(db);
  store = cp_store_open(path);
  assert(store);
  uint64_t mark = 0;
  assert(cp_store_ts_mark(store, &mark) == 0 && mark == 123456789);
  assert(cp_store_put_ts_mark(store, 987654321) == 0);
  assert(cp_store_ts_mark(store, &mark) == 0 && mark == 987654321);
  cp_store_close(store);
  (void)unlink(path);
  printf("  test_an_older_cp_db_gives_its_version_mark_as_the_ts_mark OK\n");
}

/** The preference list of k0 with nodes 1, 2 and 3 and 3 replicas: every node, in ring order. */
static void k0_prefs(int out[RING_MAX_REPLICAS]) {
  ring *r = ring_new((int[]){1, 2, 3}, 3, (ring_params){128, 3});
  assert(r && ring_prefs(r, "k0", 2, out) == 3);
  ring_free(r);
}

static void kill(fixture f, int id) {
  char addr[16];
  (void)snprintf(addr, sizeof(addr), "n%d", id);
  assert(cluster_put_node(f.c, id, addr, (node_sm){NODE_DEAD, 0, true}, true) == 0);
}

static void test_write_goes_to_live_home_replicas_and_needs_w_acks(void) {
  int prefs[RING_MAX_REPLICAS];
  k0_prefs(prefs);
  fixture f = three_nodes_with(&cfg_rf3);
  plan p;
  assert(cluster_plan_write(f.c, "k0", 2, &p) == 0);
  cluster_done(f.c, p.epoch);
  assert(p.count == 3 && p.replicas == 3 && p.write_quorum == 2 && p.read_quorum == 2);
  assert(p.coord == prefs[0] && p.ts != 0);
  kill(f, prefs[0]);
  assert(cluster_plan_write(f.c, "k0", 2, &p) == 0);
  cluster_done(f.c, p.epoch);
  assert(p.count == 2 && p.replicas == 2 && p.coord == prefs[1]);
  done(f);
  printf("  test_write_goes_to_live_home_replicas_and_needs_w_acks OK\n");
}

static void test_write_fails_with_fewer_live_home_replicas_than_w(void) {
  fixture f = three_nodes_with(&cfg_rf3);
  kill(f, 1);
  kill(f, 2);
  plan p;
  assert(cluster_plan_write(f.c, "k0", 2, &p) == -1);
  done(f);
  printf("  test_write_fails_with_fewer_live_home_replicas_than_w OK\n");
}

static void test_a_respawned_replica_counts_for_writes_but_serves_no_reads(void) {
  fixture f = three_nodes_with(&cfg_rf3);
  kill(f, 1);
  assert(cluster_put_node(f.c, 2, "n2", (node_sm){NODE_REPAIRING, 0, true}, true) == 0);
  plan p;
  assert(cluster_plan_write(f.c, "k0", 2, &p) == -1);
  assert(cluster_plan_read(f.c, "k0", 2, &p) == -1);
  assert(cluster_put_node(f.c, 1, "n1", (node_sm){NODE_ACTIVE, 0, true}, true) == 0);
  assert(cluster_plan_write(f.c, "k0", 2, &p) == 0);
  cluster_done(f.c, p.epoch);
  assert(p.count == 3);
  for (size_t i = 0; i != p.count; i++) {
    assert(p.targets[i].reads == (p.targets[i].id != 2));
  }
  done(f);
  printf("  test_a_respawned_replica_counts_for_writes_but_serves_no_reads OK\n");
}

static void test_read_needs_r_replicas_that_serve_reads(void) {
  fixture f = three_nodes_with(&cfg_rf3);
  plan p;
  kill(f, 3);
  assert(cluster_plan_read(f.c, "k0", 2, &p) == 0);
  cluster_done(f.c, p.epoch);
  assert(p.count == 2 && p.read_quorum == 2 && p.write_quorum == 2);
  assert(p.targets[0].reads && p.targets[1].reads);
  kill(f, 2);
  assert(cluster_plan_read(f.c, "k0", 2, &p) == -1);
  done(f);
  printf("  test_read_needs_r_replicas_that_serve_reads OK\n");
}

int main(void) {
  printf("cluster tests:\n");
  test_copy_writes_moving_keys_to_target();
  test_flip_moves_reads_to_new_ring();
  test_abort_restores_old_plans();
  test_add_request_stops_at_max_nodes();
  test_timestamps_after_restart_are_above_the_stored_mark();
  test_an_older_cp_db_gives_its_version_mark_as_the_ts_mark();
  test_an_older_cp_db_gives_its_version_mark_as_the_ts_mark();
  test_write_goes_to_live_home_replicas_and_needs_w_acks();
  test_write_fails_with_fewer_live_home_replicas_than_w();
  test_a_respawned_replica_counts_for_writes_but_serves_no_reads();
  test_read_needs_r_replicas_that_serve_reads();
  printf("all cluster tests passed\n");
  return 0;
}
