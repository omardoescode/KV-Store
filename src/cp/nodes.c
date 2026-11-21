// Node lifecycle on the controller thread: anti-entropy, respawn and repair, and joins.
#include "cp/controller_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cp/node_client.h"

enum {
  BATCH_ROWS = 500,
  AE_BATCH = 100,
  DRAIN_TIMEOUT_MS = 30000,
  SPAWN_WAIT_MS = 15000,
  START_SPAWN_TRIES = 5,
};

static void sleep_ms(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

/** Spawn node id, write its address to addr and wait until it answers. Returns 0 or -1. */
static int spawn_ready(int id, char addr[DRIVER_ADDR_LEN]) {
  if (driver_spawn(id, addr) != 0) {
    return -1;
  }
  int64_t deadline = ctl_mono_ms() + SPAWN_WAIT_MS;
  while (ctl_mono_ms() < deadline) {
    if (node_health(addr)) {
      return 0;
    }
    sleep_ms(50);
  }
  return -1;
}

typedef struct {
  controller *ctl;
  const xfer *x;
  int node;
  const char *addr;
  int target;
  const char *dst;
} move;

static int send_rows(const move *m, const node_row *rows, size_t n) {
  return m->target != 0 ? node_put_rows(m->dst, rows, n) : node_delete_keys(m->addr, rows, n);
}

static int move_page(void *arg, const node_row *rows, size_t n) {
  const move *m = arg;
  cluster_reserve_ts(m->ctl->c);
  static node_row batch[BATCH_ROWS];
  size_t count = 0;
  for (size_t i = 0; i != n; i++) {
    const node_row *r = &rows[i];
    bool moves = false;
    if (m->target != 0) {
      // Every reader sends: with W < N one replica may miss an acked write, and the target takes
      // the winner by the fixed rule.
      moves = (bool)(xfer_source(m->x, r->key, r->key_len, m->target) != 0 &&
                     ring_has(m->x->from, m->node, r->key, r->key_len));
    } else {
      moves = xfer_drops(m->x, m->node, r->key, r->key_len);
    }
    if (moves) {
      batch[count++] = *r;
    }
    if (count == BATCH_ROWS) {
      if (send_rows(m, batch, count) != 0) {
        return -1;
      }
      count = 0;
    }
  }
  return count == 0 ? 0 : send_rows(m, batch, count);
}

/**
 * Move the rows of node (at addr) that x moves: with target, flush node and send to dst the rows
 * that node is the source of; with target 0, delete from node the keys it drops. Returns 0 or -1.
 */
static int move_rows(controller *ctl, const xfer *x, int node, const char *addr, int target,
                     const char *dst) {
  if (node_flush(addr) != 0) {
    return -1;
  }
  move m = {ctl, x, node, addr, target, dst};
  return node_dump(addr, move_page, &m);
}

static bool is_live(node_state s) { return (bool)(node_takes_writes(s) || s == NODE_SYNCING); }

/** Pause or resume the tombstone purge on every live node. */
static void pause_purge(controller *ctl, bool paused) {
  static cluster_view v;
  cluster_snapshot(ctl->c, &v);
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    if (v.nodes[id].exists && is_live(v.nodes[id].sm.state)) {
      (void)node_pause_purge(v.nodes[id].addr, paused);
    }
  }
}

/** One entry of a replica, copied out of a page. */
typedef struct {
  char *key;
  size_t key_len;
  char *value;
  size_t value_len;
  vstamp stamp;
  bool deleted;
  /** Index of the replica in the segment's list. */
  size_t src;
} ae_row;

typedef struct {
  ae_row *rows;
  size_t n;
  size_t cap;
  size_t src;
} ae_rows;

static int collect(void *arg, const node_row *rows, size_t n) {
  ae_rows *a = arg;
  for (size_t i = 0; i != n; i++) {
    if (a->n == a->cap) {
      size_t cap = a->cap ? a->cap * 2 : 64;
      ae_row *grown = realloc(a->rows, cap * sizeof(*grown));
      if (!grown) {
        return -1;
      }
      a->rows = grown;
      a->cap = cap;
    }
    const node_row *r = &rows[i];
    ae_row *out = &a->rows[a->n];
    out->key = malloc(r->key_len + r->value_len + 1);
    if (!out->key) {
      return -1;
    }
    memcpy(out->key, r->key, r->key_len);
    out->key_len = r->key_len;
    out->value = out->key + r->key_len;
    memcpy(out->value, r->value, r->value_len);
    out->value_len = r->value_len;
    out->stamp = r->stamp;
    out->deleted = r->deleted;
    out->src = a->src;
    a->n++;
  }
  return 0;
}

static int by_key(const void *lhs, const void *rhs) {
  const ae_row *a = lhs;
  const ae_row *b = rhs;
  size_t n = a->key_len < b->key_len ? a->key_len : b->key_len;
  int c = memcmp(a->key, b->key, n);
  if (c != 0) {
    return c;
  }
  return (a->key_len > b->key_len) - (a->key_len < b->key_len);
}

/** The replicas of one segment and the rows to send each. */
typedef struct {
  const cluster_view *v;
  const int *ids;
  size_t m;
  bool to_missing;
  node_row queues[RING_MAX_REPLICAS][AE_BATCH];
  size_t lens[RING_MAX_REPLICAS];
} ae_out;

/** Send the rows queued for replica j. Returns 0 or -1. */
static int flush_queue(ae_out *out, size_t j) {
  int rc = out->lens[j]
               ? node_put_rows(out->v->nodes[out->ids[j]].addr, out->queues[j], out->lens[j])
               : 0;
  out->lens[j] = 0;
  return rc;
}

/**
 * Queue the winner of the key rows[0..n) to every replica without it. A tombstone goes to a
 * replica without the key only when out->to_missing (a repair): else a purged tombstone would
 * come back. Returns 0 or -1.
 */
static int send_winner(ae_out *out, const ae_row *rows, size_t n) {
  size_t win = 0;
  vstamp merged = rows[0].stamp;
  for (size_t k = 1; k != n; k++) {
    vstamp next;
    win = vstamp_pick(&merged, &rows[k].stamp, &next) == 1 ? k : win;
    merged = next;
  }
  bool has[RING_MAX_REPLICAS] = {false};
  bool fresh[RING_MAX_REPLICAS] = {false};
  for (size_t k = 0; k != n; k++) {
    has[rows[k].src] = true;
    fresh[rows[k].src] = vstamp_same(&rows[k].stamp, &merged);
  }
  const ae_row *w = &rows[win];
  node_row row = {w->key, w->key_len, w->value, w->value_len, merged, w->deleted};
  for (size_t j = 0; j != out->m; j++) {
    if (!fresh[j] && (has[j] || !w->deleted || out->to_missing)) {
      out->queues[j][out->lens[j]++] = row;
      if (out->lens[j] == AE_BATCH && flush_queue(out, j) != 0) {
        return -1;
      }
    }
  }
  return 0;
}

/** Make the replicas of out agree on the keys of segment r. Returns 0 or -1. */
static int sync_segment(ae_out *out, ring_range r) {
  ae_rows all = {0};
  int rc = 0;
  for (size_t j = 0; j != out->m && rc == 0; j++) {
    all.src = j;
    rc = node_range(out->v->nodes[out->ids[j]].addr, r.start, r.end, collect, &all);
  }
  if (all.n) {
    qsort(all.rows, all.n, sizeof(*all.rows), by_key);
  }
  for (size_t g = 0, next = 0; rc == 0 && g != all.n; g = next) {
    for (next = g + 1; next != all.n && by_key(&all.rows[g], &all.rows[next]) == 0; next++) {
    }
    rc = send_winner(out, &all.rows[g], next - g);
  }
  // Queued rows point into all.rows: send them before those are freed.
  for (size_t j = 0; j != out->m; j++) {
    rc = flush_queue(out, j) != 0 ? -1 : rc;
  }
  for (size_t i = 0; i != all.n; i++) {
    free(all.rows[i].key);
  }
  free(all.rows);
  return rc;
}

/**
 * Set digests[id] to a malloc'd digest of each of the n segments ending at ends, for each live
 * ring node that answers; NULL for the others.
 */
static void take_digests(const cluster_view *v, const uint64_t *ends, size_t n,
                         uint64_t **digests) {
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    const cluster_node *node = &v->nodes[id];
    digests[id] = NULL;
    if (node->exists && node->in_ring && node_takes_writes(node->sm.state)) {
      digests[id] = malloc(n * sizeof(**digests));
      if (digests[id] && node_digest(node->addr, ends, n, digests[id]) != 0) {
        free(digests[id]);
        digests[id] = NULL;
      }
    }
  }
}

int ctl_anti_entropy(controller *ctl, int repair) {
  static cluster_view v;
  cluster_snapshot(ctl->c, &v);
  size_t n = v.ring ? ring_segments(v.ring) : 0;
  if (n == 0) {
    return 0;
  }
  uint64_t *ends = malloc(n * sizeof(*ends));
  if (!ends) {
    return -1;
  }
  for (size_t i = 0; i != n; i++) {
    ends[i] = ring_segment_end(v.ring, i);
  }
  static uint64_t *digests[CLUSTER_MAX_NODES + 1];
  take_digests(&v, ends, n, digests);
  int rc = repair && !digests[repair] ? -1 : 0;
  static ae_out out;
  int ids[RING_MAX_REPLICAS];
  out = (ae_out){.v = &v, .ids = ids, .to_missing = repair != 0};
  for (size_t i = 0; rc == 0 && i != n; i++) {
    int prefs[RING_MAX_REPLICAS];
    size_t np = ring_segment_prefs(v.ring, i, prefs);
    out.m = 0;
    bool differ = false;
    bool has_repair = repair == 0;
    for (size_t j = 0; j != np; j++) {
      const uint64_t *d = digests[prefs[j]];
      if (d) {
        differ = (bool)(differ || (out.m != 0 && d[i] != digests[ids[0]][i]));
        has_repair = (bool)(has_repair || prefs[j] == repair);
        ids[out.m++] = prefs[j];
      }
    }
    if (differ && has_repair) {
      rc = sync_segment(&out, (ring_range){ends[i == 0 ? n - 1 : i - 1], ends[i]});
    }
  }
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    free(digests[id]);
  }
  free(ends);
  return rc;
}

/** Bring node id up to date with anti-entropy against its peers. Returns 0 or -1. */
static int repair(controller *ctl, int id) {
  driver_fault_point("REPAIR", id);
  // Writes that started before the node was REPAIRING did not go to it.
  if (cluster_drain(ctl->c, DRAIN_TIMEOUT_MS) != 0) {
    return -1;
  }
  return ctl_anti_entropy(ctl, id);
}

void ctl_heal(controller *ctl, int id, node_state state) {
  if (state == NODE_REPAIRING) {
    if (repair(ctl, id) == 0) {
      (void)cluster_node_event(ctl->c, id, NODE_EV_REPAIR_DONE);
    }
    return;
  }
  unsigned act = cluster_node_event(ctl->c, id, NODE_EV_RESPAWN);
  if ((act & NODE_ACT_SPAWN) == 0) {
    return;
  }
  driver_fault_point("RESPAWN", id);
  char addr[DRIVER_ADDR_LEN];
  if (spawn_ready(id, addr) != 0) {
    LOGF(ctl, LOG_ERROR, "node %d: spawn failed", id);
    (void)cluster_node_event(ctl->c, id, NODE_EV_SPAWN_FAIL);
    return;
  }
  act = cluster_spawned(ctl->c, id, addr);
  while ((act & NODE_ACT_REPAIR) != 0) {
    if (repair(ctl, id) == 0) {
      (void)cluster_node_event(ctl->c, id, NODE_EV_REPAIR_DONE);
      return;
    }
    LOGF(ctl, LOG_WARN, "node %d: repair failed", id);
    if (!node_health(addr)) {
      // Leave it REPAIRING: probes find it dead and spawn it again.
      return;
    }
    act = cluster_node_event(ctl->c, id, NODE_EV_REPAIR_FAIL);
    sleep_ms(ctl->cfg->probe_interval_ms);
  }
}

static void abort_join(controller *ctl, int target, bool retry) {
  (void)cluster_set_phase(ctl->c, MIG_ABORT, target);
  driver_fault_point("ABORT", target);
  cluster_abort_join(ctl->c, target);
  // Requests that write to the target finish before it goes.
  (void)cluster_drain(ctl->c, DRAIN_TIMEOUT_MS);
  (void)driver_stop(target);
  (void)cluster_drop_node(ctl->c, target);
  pause_purge(ctl, false);
  (void)cluster_set_phase(ctl->c, MIG_IDLE, 0);
  LOGF(ctl, LOG_WARN, "migration: join of node %d aborted", target);
  if (retry) {
    // The next loop runs it again once every node is ACTIVE (a dead source is respawned first).
    (void)cluster_request_add(ctl->c);
  }
}

/** COPY: double-write on, then copy older rows of moving keys to the target. Returns 0 or -1. */
static int copy_phase(controller *ctl, int target) {
  if (cluster_begin_copy(ctl->c, target) != 0) {
    return -1;
  }
  driver_fault_point("COPY", target);
  // Writes planned before double-write did not go to the target: wait for them, then copy.
  if (cluster_drain(ctl->c, DRAIN_TIMEOUT_MS) != 0) {
    return -1;
  }
  pause_purge(ctl, true);
  static cluster_view v;
  cluster_snapshot(ctl->c, &v);
  static bool readers[CLUSTER_MAX_NODES + 1];
  for (int src = 1; src <= CLUSTER_MAX_NODES; src++) {
    readers[src] = (bool)(v.nodes[src].exists && node_serves_reads(v.nodes[src].sm.state));
  }
  xfer x = {v.ring, v.next, readers};
  const char *addr = v.nodes[target].addr;
  int rc = 0;
  for (int src = 1; src <= CLUSTER_MAX_NODES && rc == 0; src++) {
    if (readers[src]) {
      rc = move_rows(ctl, &x, src, v.nodes[src].addr, target, addr);
    }
  }
  if (rc == 0 && (cluster_copy_lost_write(ctl->c) || !node_health(addr))) {
    rc = -1;
  }
  // Copied rows must be durable on the target before PURGE deletes them from the old owners.
  if (rc == 0 && node_flush(addr) != 0) {
    rc = -1;
  }
  return rc;
}

/** PURGE: delete from each old node the keys it no longer holds. Returns 0 or -1. */
static int purge_phase(controller *ctl, int target) {
  driver_fault_point("PURGE", target);
  static cluster_view v;
  cluster_snapshot(ctl->c, &v);
  xfer x = {v.prev, v.ring, NULL};
  int rc = 0;
  for (int src = 1; src <= CLUSTER_MAX_NODES && rc == 0 && v.prev; src++) {
    if (src != target && v.nodes[src].exists && node_takes_writes(v.nodes[src].sm.state)) {
      rc = move_rows(ctl, &x, src, v.nodes[src].addr, 0, NULL);
    }
  }
  if (rc == 0) {
    pause_purge(ctl, false);
  }
  return rc;
}

void ctl_finish_join(controller *ctl, mig_phase from, int target) {
  mig_phase phase = from;
  while (phase != MIG_IDLE) {
    if (phase == MIG_FLIP) {
      driver_fault_point("FLIP", target);
      cluster_flip(ctl->c, target);
      phase = mig_step(phase, MIG_EV_FLIP_DONE).next;
    } else if (phase == MIG_DRAIN) {
      driver_fault_point("DRAIN", target);
      (void)cluster_drain(ctl->c, DRAIN_TIMEOUT_MS);
      phase = mig_step(phase, MIG_EV_DRAIN_DONE).next;
    } else if (phase == MIG_PURGE) {
      if (purge_phase(ctl, target) != 0) {
        // Retry on the next tick, after probes: a dead node is skipped once it is marked DEAD.
        LOGF(ctl, LOG_WARN, "migration: purge failed, retrying");
        return;
      }
      phase = mig_step(phase, MIG_EV_PURGE_DONE).next;
    } else {
      return;
    }
    if (cluster_set_phase(ctl->c, phase, phase == MIG_IDLE ? 0 : target) != 0) {
      return;
    }
  }
  ctl->last_scale_ms = ctl_mono_ms();
  LOGF(ctl, LOG_INFO, "migration: node %d joined", target);
}

void ctl_join(controller *ctl, bool asked) {
  int target = cluster_begin_join(ctl->c);
  if (target < 0) {
    return;
  }
  driver_fault_point("SPAWN", target);
  (void)cluster_put_node(ctl->c, target, "", (node_sm){NODE_SPAWNING, 0, false}, false);
  char addr[DRIVER_ADDR_LEN];
  if (spawn_ready(target, addr) != 0) {
    LOGF(ctl, LOG_ERROR, "migration: spawn of node %d failed", target);
    abort_join(ctl, target, asked);
    return;
  }
  (void)cluster_spawned(ctl->c, target, addr);
  if (cluster_set_phase(ctl->c, MIG_COPY, target) != 0 || copy_phase(ctl, target) != 0 ||
      cluster_set_phase(ctl->c, MIG_FLIP, target) != 0) {
    abort_join(ctl, target, asked);
    return;
  }
  ctl_finish_join(ctl, MIG_FLIP, target);
}

/** Abort or finish a join that an earlier cp left in phase mig. */
static void restart_migration(controller *ctl, stored_migration mig) {
  mig_result r = mig_step(mig.phase, MIG_EV_RESTART);
  if ((r.actions & MIG_ACT_ABORT) != 0) {
    LOGF(ctl, LOG_WARN, "start: aborting the join of node %d left in %s", mig.target,
         mig_phase_name(mig.phase));
    abort_join(ctl, mig.target, false);
    return;
  }
  if ((r.actions & (MIG_ACT_FLIP | MIG_ACT_DRAIN | MIG_ACT_PURGE)) == 0) {
    return;
  }
  LOGF(ctl, LOG_INFO, "start: finishing the join of node %d from %s", mig.target,
       mig_phase_name(mig.phase));
  // The stored nodes give the ring with the target; the ring before the join is rebuilt as prev.
  cluster_flip(ctl->c, mig.target);
  mig_phase from = mig.phase == MIG_FLIP ? MIG_DRAIN : mig.phase;
  if (cluster_set_phase(ctl->c, from, mig.target) == 0) {
    ctl_finish_join(ctl, from, mig.target);
  }
}

static int spawn_initial(controller *ctl, int id, int *failures) {
  for (;;) {
    char addr[DRIVER_ADDR_LEN];
    if (spawn_ready(id, addr) == 0) {
      return cluster_put_node(ctl->c, id, addr, (node_sm){NODE_ACTIVE, 0, true}, true);
    }
    if (++*failures >= START_SPAWN_TRIES) {
      (void)fprintf(stderr, "cp: spawn of node %d failed %d times, exiting\n", id, *failures);
      return -1;
    }
    LOGF(ctl, LOG_WARN, "start: spawn of node %d failed, retrying", id);
    sleep_ms((int)(200U << (unsigned)*failures));
  }
}

/** Load the stored nodes into the cluster; a node that does not run is DEAD. */
static void load_nodes(controller *ctl, const stored_node *stored, int n, const driver_node *live,
                       int n_live) {
  for (int i = 0; i != n; i++) {
    stored_node s = stored[i];
    bool running = false;
    for (int j = 0; j != n_live; j++) {
      if (live[j].id == s.id) {
        running = true;
        memcpy(s.addr, live[j].addr, sizeof(s.addr));
      }
    }
    if (!running || s.sm.state == NODE_SPAWNING) {
      s.sm = (node_sm){NODE_DEAD, 0, true};
    } else if (s.sm.state == NODE_DEAD) {
      s.sm.state = NODE_REPAIRING;
    }
    s.sm.rejoin = true;
    (void)cluster_put_node(ctl->c, s.id, s.addr, s.sm, s.in_ring);
  }
}

/** Adopt running nodes as the ring when cp has no state (cp.db was lost). */
static void adopt_nodes(controller *ctl, const driver_node *live, int n_live) {
  for (int i = 0; i != n_live; i++) {
    (void)cluster_put_node(ctl->c, live[i].id, live[i].addr, (node_sm){NODE_ACTIVE, 0, true}, true);
  }
}

int controller_start_up(controller *ctl) {
  static stored_node stored[CLUSTER_MAX_NODES];
  static driver_node live[CLUSTER_MAX_NODES];
  int n = cp_store_nodes(ctl->store, stored, CLUSTER_MAX_NODES);
  int n_live = driver_list(live, CLUSTER_MAX_NODES);
  stored_migration mig;
  if (n < 0 || n_live < 0 || cp_store_migration(ctl->store, &mig) != 0) {
    (void)fprintf(stderr, "cp: could not read the state store or the node list\n");
    return -1;
  }
  if (n == 0) {
    adopt_nodes(ctl, live, n_live);
  } else {
    load_nodes(ctl, stored, n, live, n_live);
  }
  static cluster_view v;
  cluster_snapshot(ctl->c, &v);
  int failures = 0;
  for (int id = 1; id <= ctl->cfg->initial_nodes; id++) {
    if (!v.nodes[id].exists && spawn_initial(ctl, id, &failures) != 0) {
      return -1;
    }
  }
  restart_migration(ctl, mig);
  cluster_reserve_ts(ctl->c);
  return 0;
}
