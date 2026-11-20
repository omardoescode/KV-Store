#include "cp/cluster.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Versions this far ahead of the clock are reserved in the store, in ms.
enum { VERSION_RESERVE_MS = 60000 };

typedef struct {
  cluster_node n;
  /** The router saw a forward to this node fail since the controller last looked. */
  bool forward_error;
  /** A write was acked without this node since the controller last looked. */
  bool missed_write;
  /** n.sm changed in memory and is not in the store yet. */
  bool unsaved;
} cnode;

struct cluster {
  pthread_mutex_t mu;
  const cp_config *cfg;
  cp_store *store;
  logger log;
  cnode nodes[CLUSTER_MAX_NODES + 1];
  ring *ring;
  ring *next;
  ring *prev;
  mig_phase phase;
  int target;
  /** COPY: writes of keys that move to the target also go to the target. */
  bool double_write;
  /** A write to the target failed during COPY: the join must abort. */
  bool double_write_failed;
  uint64_t last_version;
  uint64_t version_mark;
  /** Requests take a ticket of the current epoch; a drain waits for older tickets. */
  uint64_t epoch;
  long inflight[2];
  bool add_requested;
};

#define LOGF(c, level, ...) (c)->log.log((c)->log.ctx, (level), __VA_ARGS__)

/** Build a ring of in-ring nodes, plus with and less without (either may be 0). Call with mu. */
static ring *build_ring(cluster *c, int with, int without) {
  int ids[CLUSTER_MAX_NODES + 1];
  size_t n = 0;
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    const cluster_node *node = &c->nodes[id].n;
    if (id != without && ((node->exists && node->in_ring) || id == with)) {
      ids[n++] = id;
    }
  }
  return ring_new(ids, n, (ring_params){c->cfg->vnodes, c->cfg->replicas});
}

cluster *cluster_new(const cp_config *cfg, cp_store *store, logger log) {
  cluster *c = calloc(1, sizeof(*c));
  if (!c) {
    return NULL;
  }
  c->cfg = cfg;
  c->store = store;
  c->log = log;
  c->ring = build_ring(c, 0, 0);
  if (!c->ring || cp_store_version_mark(store, &c->version_mark) != 0 ||
      pthread_mutex_init(&c->mu, NULL) != 0) {
    ring_free(c->ring);
    free(c);
    return NULL;
  }
  c->last_version = c->version_mark;
  return c;
}

void cluster_free(cluster *c) {
  if (!c) {
    return;
  }
  ring_free(c->ring);
  ring_free(c->next);
  ring_free(c->prev);
  pthread_mutex_destroy(&c->mu);
  free(c);
}

static uint64_t next_version(cluster *c) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  uint64_t ms = ((uint64_t)ts.tv_sec * 1000) + ((uint64_t)ts.tv_nsec / 1000000);
  uint64_t v = ms << 16U;
  if (v <= c->last_version) {
    v = c->last_version + 1;
  }
  // Above the stored mark, a restart could hand out this version again.
  if (v >= c->version_mark) {
    return 0;
  }
  c->last_version = v;
  return v;
}

static void add_target(plan *p, int id, const cluster_node *n) {
  plan_target *t = &p->targets[p->count++];
  t->id = id;
  memcpy(t->addr, n->addr, sizeof(t->addr));
  t->counts = node_serves_reads(n->sm.state);
}

static void take_ticket(cluster *c, plan *p) {
  p->epoch = c->epoch;
  c->inflight[c->epoch & 1U]++;
}

int cluster_plan_write(cluster *c, const char *key, size_t key_len, plan *out) {
  out->count = 0;
  int prefs[RING_MAX_REPLICAS];
  pthread_mutex_lock(&c->mu);
  out->version = next_version(c);
  size_t n = c->ring && out->version ? ring_prefs(c->ring, key, key_len, prefs) : 0;
  out->acks = n > 1 ? n - 1 : 1;
  size_t counting = 0;
  for (size_t i = 0; i != n; i++) {
    const cluster_node *node = &c->nodes[prefs[i]].n;
    if (node_takes_writes(node->sm.state)) {
      add_target(out, prefs[i], node);
      counting += out->targets[out->count - 1].counts ? 1 : 0;
    }
  }
  out->replicas = out->count;
  if (counting < out->acks) {
    out->count = 0;
    pthread_mutex_unlock(&c->mu);
    return -1;
  }
  for (size_t i = 0; i != out->count; i++) {
    c->nodes[out->targets[i].id].n.requests++;
  }
  if (c->double_write && c->next && ring_has(c->next, c->target, key, key_len)) {
    add_target(out, c->target, &c->nodes[c->target].n);
  }
  take_ticket(c, out);
  pthread_mutex_unlock(&c->mu);
  return 0;
}

int cluster_plan_read(cluster *c, const char *key, size_t key_len, plan *out) {
  out->count = 0;
  out->version = 0;
  int prefs[RING_MAX_REPLICAS];
  pthread_mutex_lock(&c->mu);
  size_t n = c->ring ? ring_prefs(c->ring, key, key_len, prefs) : 0;
  for (size_t i = 0; i != n; i++) {
    const cluster_node *node = &c->nodes[prefs[i]].n;
    if (node_serves_reads(node->sm.state)) {
      add_target(out, prefs[i], node);
    }
  }
  if (out->count != 0) {
    c->nodes[out->targets[0].id].n.requests++;
    take_ticket(c, out);
  }
  out->replicas = out->count;
  pthread_mutex_unlock(&c->mu);
  return out->count ? 0 : -1;
}

void cluster_done(cluster *c, uint64_t epoch) {
  pthread_mutex_lock(&c->mu);
  c->inflight[epoch & 1U]--;
  pthread_mutex_unlock(&c->mu);
}

void cluster_forward_failed(cluster *c, int id, bool join_target) {
  pthread_mutex_lock(&c->mu);
  c->nodes[id].forward_error = true;
  if (join_target) {
    c->double_write_failed = true;
  }
  pthread_mutex_unlock(&c->mu);
}

void cluster_missed_write(cluster *c, int id) {
  pthread_mutex_lock(&c->mu);
  cnode *node = &c->nodes[id];
  node_sm before = node->n.sm;
  node->n.sm = node_step(before, NODE_EV_MISSED_WRITE, c->cfg->probe_fails).next;
  node->missed_write = true;
  bool changed = node->n.sm.state != before.state;
  node->unsaved = (bool)(node->unsaved || changed);
  pthread_mutex_unlock(&c->mu);
  if (changed) {
    LOGF(c, LOG_WARN, "node %d: %s -> REPAIRING, missed a write", id,
         node_state_name(before.state));
  }
}

static int node_count(const cluster *c) {
  int count = 0;
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    count += c->nodes[id].n.exists ? 1 : 0;
  }
  return count;
}

bool cluster_request_add(cluster *c) {
  pthread_mutex_lock(&c->mu);
  bool room = node_count(c) < c->cfg->max_nodes;
  if (room) {
    c->add_requested = true;
  }
  pthread_mutex_unlock(&c->mu);
  return room;
}

void cluster_snapshot(cluster *c, cluster_view *out) {
  pthread_mutex_lock(&c->mu);
  for (int id = 0; id <= CLUSTER_MAX_NODES; id++) {
    out->nodes[id] = c->nodes[id].n;
  }
  out->count = node_count(c);
  out->phase = c->phase;
  out->target = c->target;
  out->add_requested = c->add_requested;
  out->ring = c->ring;
  out->next = c->next;
  out->prev = c->prev;
  pthread_mutex_unlock(&c->mu);
}

int cluster_drain(cluster *c, int timeout_ms) {
  pthread_mutex_lock(&c->mu);
  uint64_t old = c->epoch++;
  pthread_mutex_unlock(&c->mu);
  struct timespec tick = {0, 2000000};
  for (int waited = 0; waited < timeout_ms * 1000; waited += 2000) {
    pthread_mutex_lock(&c->mu);
    long left = c->inflight[old & 1U];
    pthread_mutex_unlock(&c->mu);
    if (left == 0) {
      return 0;
    }
    nanosleep(&tick, NULL);
  }
  return -1;
}

static int save_node(cluster *c, int id, const cluster_node *n) {
  if (!n->exists) {
    return cp_store_del_node(c->store, id);
  }
  stored_node s = {.id = id, .sm = n->sm, .in_ring = n->in_ring};
  memcpy(s.addr, n->addr, sizeof(s.addr));
  return cp_store_put_node(c->store, &s);
}

/** Write node id as n, then apply it and rebuild the ring if its ring membership changed. */
static int replace_node(cluster *c, int id, const cluster_node *n) {
  if (id < 1 || id > CLUSTER_MAX_NODES || save_node(c, id, n) != 0) {
    return -1;
  }
  pthread_mutex_lock(&c->mu);
  cnode *cur = &c->nodes[id];
  bool was_in_ring = (bool)(cur->n.exists && cur->n.in_ring);
  cur->n = *n;
  cur->forward_error = false;
  int rc = 0;
  if (was_in_ring != (bool)(n->exists && n->in_ring)) {
    ring *r = build_ring(c, 0, 0);
    if (r) {
      ring_free(c->ring);
      c->ring = r;
    } else {
      rc = -1;
    }
  }
  pthread_mutex_unlock(&c->mu);
  return rc;
}

int cluster_put_node(cluster *c, int id, const char *addr, node_sm sm, bool in_ring) {
  cluster_node n = {.exists = true, .sm = sm, .in_ring = in_ring};
  strncpy(n.addr, addr, sizeof(n.addr) - 1);
  return replace_node(c, id, &n);
}

int cluster_drop_node(cluster *c, int id) {
  return replace_node(c, id, &(cluster_node){.exists = false});
}

/**
 * Apply ev to node id and write the new state (and addr, if not NULL) to the store first. A missed
 * write the router marks meanwhile makes the step start again from the marked state.
 */
static unsigned step_node(cluster *c, int id, node_event ev, const char *addr) {
  for (;;) {
    pthread_mutex_lock(&c->mu);
    cluster_node n = c->nodes[id].n;
    bool unsaved = c->nodes[id].unsaved;
    pthread_mutex_unlock(&c->mu);
    node_sm before = n.sm;
    node_result r = node_step(before, ev, c->cfg->probe_fails);
    bool changed = (bool)(r.next.state != before.state || r.next.rejoin != before.rejoin);
    n.sm = r.next;
    if (addr) {
      memset(n.addr, 0, sizeof(n.addr));
      strncpy(n.addr, addr, sizeof(n.addr) - 1);
    }
    if (changed || addr || unsaved) {
      (void)save_node(c, id, &n);
    }
    pthread_mutex_lock(&c->mu);
    cnode *cur = &c->nodes[id];
    if (cur->n.sm.state != before.state) {
      pthread_mutex_unlock(&c->mu);
      continue;
    }
    cur->n.sm = r.next;
    memcpy(cur->n.addr, n.addr, sizeof(n.addr));
    cur->unsaved = false;
    pthread_mutex_unlock(&c->mu);
    if (changed) {
      LOGF(c, LOG_INFO, "node %d: %s -> %s", id, node_state_name(before.state),
           node_state_name(r.next.state));
    }
    return r.actions;
  }
}

unsigned cluster_node_event(cluster *c, int id, node_event ev) {
  return step_node(c, id, ev, NULL);
}

unsigned cluster_spawned(cluster *c, int id, const char *addr) {
  return step_node(c, id, NODE_EV_SPAWN_OK, addr);
}

bool cluster_take_missed_write(cluster *c, int id) {
  pthread_mutex_lock(&c->mu);
  bool missed = c->nodes[id].missed_write;
  c->nodes[id].missed_write = false;
  pthread_mutex_unlock(&c->mu);
  return missed;
}

bool cluster_take_forward_error(cluster *c, int id) {
  pthread_mutex_lock(&c->mu);
  bool failed = c->nodes[id].forward_error;
  c->nodes[id].forward_error = false;
  pthread_mutex_unlock(&c->mu);
  return failed;
}

void cluster_reserve_versions(cluster *c) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  uint64_t now = ((uint64_t)ts.tv_sec * 1000) + ((uint64_t)ts.tv_nsec / 1000000);
  pthread_mutex_lock(&c->mu);
  uint64_t mark = c->version_mark;
  uint64_t last = c->last_version;
  pthread_mutex_unlock(&c->mu);
  uint64_t want = (now + (VERSION_RESERVE_MS / 2)) << 16U;
  if (mark > want && mark > last + (1ULL << 30U)) {
    return;
  }
  uint64_t next = (now + VERSION_RESERVE_MS) << 16U;
  if (next <= last) {
    next = last + (1ULL << 32U);
  }
  if (cp_store_put_version_mark(c->store, next) == 0) {
    pthread_mutex_lock(&c->mu);
    c->version_mark = next;
    pthread_mutex_unlock(&c->mu);
  }
}

int cluster_set_phase(cluster *c, mig_phase phase, int target) {
  if (cp_store_put_migration(c->store, (stored_migration){phase, target}) != 0) {
    LOGF(c, LOG_ERROR, "migration: could not save phase %s", mig_phase_name(phase));
    return -1;
  }
  pthread_mutex_lock(&c->mu);
  c->phase = phase;
  c->target = target;
  if (phase == MIG_IDLE) {
    ring_free(c->prev);
    c->prev = NULL;
  }
  pthread_mutex_unlock(&c->mu);
  LOGF(c, LOG_INFO, "migration: phase %s, target %d", mig_phase_name(phase), target);
  return 0;
}

int cluster_begin_join(cluster *c) {
  pthread_mutex_lock(&c->mu);
  int target = 1;
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    if (c->nodes[id].n.exists) {
      target = id + 1;
    }
  }
  c->add_requested = false;
  pthread_mutex_unlock(&c->mu);
  if (target > CLUSTER_MAX_NODES || cluster_set_phase(c, MIG_SPAWN, target) != 0) {
    return -1;
  }
  return target;
}

int cluster_begin_copy(cluster *c, int target) {
  pthread_mutex_lock(&c->mu);
  ring *next = build_ring(c, target, 0);
  ring_free(c->next);
  c->next = next;
  c->double_write = next != NULL;
  c->double_write_failed = false;
  pthread_mutex_unlock(&c->mu);
  return next ? 0 : -1;
}

bool cluster_copy_lost_write(cluster *c) {
  pthread_mutex_lock(&c->mu);
  bool lost = c->double_write_failed;
  pthread_mutex_unlock(&c->mu);
  return lost;
}

void cluster_flip(cluster *c, int target) {
  pthread_mutex_lock(&c->mu);
  cluster_node t = c->nodes[target].n;
  pthread_mutex_unlock(&c->mu);
  t.in_ring = true;
  if (t.sm.state == NODE_SYNCING) {
    t.sm = node_step(t.sm, NODE_EV_SYNC_DONE, c->cfg->probe_fails).next;
  }
  (void)save_node(c, target, &t);
  pthread_mutex_lock(&c->mu);
  c->nodes[target].n.in_ring = true;
  c->nodes[target].n.sm = t.sm;
  ring *next = c->next ? c->next : build_ring(c, 0, 0);
  c->next = NULL;
  if (!c->prev) {
    c->prev = build_ring(c, 0, target);
  }
  ring_free(c->ring);
  c->ring = next;
  c->double_write = false;
  pthread_mutex_unlock(&c->mu);
}

void cluster_abort_join(cluster *c, int target) {
  pthread_mutex_lock(&c->mu);
  c->double_write = false;
  ring_free(c->next);
  c->next = NULL;
  c->nodes[target].n.in_ring = false;
  pthread_mutex_unlock(&c->mu);
}
