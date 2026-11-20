#include "cp/controller.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cp/hclient.h"
#include "json/json.h"

enum {
  BATCH_ROWS = 500,
  DUMP_PAGE = 1000,
  SPAWN_WAIT_MS = 15000,
  DRAIN_TIMEOUT_MS = 30000,
  START_SPAWN_TRIES = 5,
  // Versions this far ahead of the clock are reserved in the store, in ms.
  VERSION_RESERVE_MS = 60000,
};

struct controller {
  cluster *c;
  const cp_config *cfg;
  cp_store *store;
  logger log;
  pthread_t thread;
  pthread_mutex_t mu;
  pthread_cond_t wake;
  bool stop;
  bool running;
  int64_t last_scale_ms;
  int64_t window_start_ms;
  uint64_t window_requests[CLUSTER_MAX_NODES + 1];
};

#define LOGF(ctl, level, ...) (ctl)->log.log((ctl)->log.ctx, (level), __VA_ARGS__)

static int64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((int64_t)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

static void sleep_ms(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

controller *controller_new(cluster *c, cp_store *store, logger log) {
  controller *ctl = calloc(1, sizeof(*ctl));
  if (!ctl) {
    return NULL;
  }
  ctl->c = c;
  ctl->cfg = c->cfg;
  ctl->store = store;
  ctl->log = log;
  pthread_mutex_init(&ctl->mu, NULL);
  pthread_cond_init(&ctl->wake, NULL);
  ctl->last_scale_ms = mono_ms() - ((int64_t)ctl->cfg->scale_cooldown_s * 1000);
  ctl->window_start_ms = mono_ms();
  return ctl;
}

// ---- Node calls ----

/** Send a request to a node. Returns the status, or -1. Frees the body unless out is given. */
static int call(const char *addr, const hclient_req *req, hclient_resp *out) {
  hclient_resp resp;
  if (hclient_do(addr, req, &resp) != 0) {
    return -1;
  }
  if (out) {
    *out = resp;
  } else {
    free(resp.body);
  }
  return resp.status;
}

/** Send req to addr. Returns the status, or -1. */
static int post(const char *addr, const hclient_req *req) { return call(addr, req, NULL); }

static bool healthy(controller *ctl, const char *addr) {
  int timeout = ctl->cfg->probe_interval_ms < ctl->cfg->forward_timeout_ms
                    ? ctl->cfg->probe_interval_ms
                    : ctl->cfg->forward_timeout_ms;
  hclient_req req = {"GET", "/_internal/health", NULL, NULL, 0, timeout};
  return call(addr, &req, NULL) == 200;
}

static bool wait_healthy(controller *ctl, const char *addr) {
  int64_t deadline = mono_ms() + SPAWN_WAIT_MS;
  while (mono_ms() < deadline) {
    if (healthy(ctl, addr)) {
      return true;
    }
    sleep_ms(50);
  }
  return false;
}

/** Resolve the host of "host:port" to a numeric "ip:port" in out. Returns 0 or -1. */
static int resolve(const char *in, char out[DRIVER_ADDR_LEN]) {
  const char *colon = strrchr(in, ':');
  if (!colon) {
    return -1;
  }
  char host[DRIVER_ADDR_LEN];
  (void)snprintf(host, sizeof(host), "%.*s", (int)(colon - in), in);
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
  struct addrinfo *ai = NULL;
  if (getaddrinfo(host, colon + 1, &hints, &ai) != 0) {
    return -1;
  }
  char ip[64];
  int rc = getnameinfo(ai->ai_addr, ai->ai_addrlen, ip, sizeof(ip), NULL, 0, NI_NUMERICHOST);
  freeaddrinfo(ai);
  if (rc != 0) {
    return -1;
  }
  (void)snprintf(out, DRIVER_ADDR_LEN, "%s%s", ip, colon);
  return 0;
}

// ---- Cluster state, always written to the store first ----

static int save_node(controller *ctl, int id) {
  pthread_mutex_lock(&ctl->c->mu);
  cnode n = ctl->c->nodes[id];
  pthread_mutex_unlock(&ctl->c->mu);
  if (!n.exists) {
    return cp_store_del_node(ctl->store, id);
  }
  stored_node s = {.id = id, .sm = n.sm, .in_ring = n.in_ring};
  memcpy(s.addr, n.addr, sizeof(s.addr));
  return cp_store_put_node(ctl->store, &s);
}

/** Apply ev to node id, persist a change, and return the actions. */
static unsigned apply_node_event(controller *ctl, int id, node_event ev) {
  pthread_mutex_lock(&ctl->c->mu);
  cnode *n = &ctl->c->nodes[id];
  node_sm before = n->sm;
  node_result r = node_step(before, ev, ctl->cfg->probe_fails);
  n->sm = r.next;
  pthread_mutex_unlock(&ctl->c->mu);
  if (r.next.state != before.state || r.next.rejoin != before.rejoin) {
    LOGF(ctl, LOG_INFO, "node %d: %s -> %s", id, node_state_name(before.state),
         node_state_name(r.next.state));
    (void)save_node(ctl, id);
  }
  return r.actions;
}

static int save_migration(controller *ctl, mig_phase phase, int target) {
  if (cp_store_put_migration(ctl->store, (stored_migration){phase, target}) != 0) {
    LOGF(ctl, LOG_ERROR, "migration: could not save phase %s", mig_phase_name(phase));
    return -1;
  }
  pthread_mutex_lock(&ctl->c->mu);
  ctl->c->phase = phase;
  ctl->c->target = target;
  pthread_mutex_unlock(&ctl->c->mu);
  LOGF(ctl, LOG_INFO, "migration: phase %s, target %d", mig_phase_name(phase), target);
  return 0;
}

/** Copy the ids and addrs of nodes for which want(state) holds into ids/addrs. */
static size_t snapshot(controller *ctl, bool (*want)(node_state), int *ids,
                       char (*addrs)[DRIVER_ADDR_LEN]) {
  size_t n = 0;
  pthread_mutex_lock(&ctl->c->mu);
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    const cnode *node = &ctl->c->nodes[id];
    if (node->exists && want(node->sm.state)) {
      ids[n] = id;
      memcpy(addrs[n], node->addr, DRIVER_ADDR_LEN);
      n++;
    }
  }
  pthread_mutex_unlock(&ctl->c->mu);
  return n;
}

static bool is_live(node_state s) { return (bool)(node_takes_writes(s) || s == NODE_SYNCING); }

static void set_purge_all(controller *ctl, bool paused) {
  int ids[CLUSTER_MAX_NODES];
  static char addrs[CLUSTER_MAX_NODES][DRIVER_ADDR_LEN];
  size_t n = snapshot(ctl, is_live, ids, addrs);
  for (size_t i = 0; i != n; i++) {
    const char *body = paused ? "{\"paused\":true}" : "{\"paused\":false}";
    hclient_req req = {
        "POST", "/_internal/purge", NULL, body, strlen(body), ctl->cfg->forward_timeout_ms * 5,
    };
    (void)post(addrs[i], &req);
  }
}

// ---- Bulk copy ----

/** Rows for one node, sent in batches of BATCH_ROWS. */
typedef struct {
  controller *ctl;
  const char *addr;
  const char *path;
  json_builder *b;
  json_node *items;
  size_t count;
  int failed;
} sink;

static void sink_reset(sink *s) {
  json_builder_free(s->b);
  s->b = json_builder_new();
  s->items = json_new_arr(s->b);
  s->count = 0;
}

static void sink_flush(sink *s) {
  if (s->count == 0 || s->failed) {
    return;
  }
  json_node *root = json_new_obj(s->b);
  bool batch = strcmp(s->path, "/_internal/batch") == 0;
  size_t len = 0;
  char *body =
      json_put(s->b, root, batch ? "items" : "keys", s->items) ? json_to_bytes(root, &len) : NULL;
  hclient_req req = {"POST", s->path, NULL, body, len, s->ctl->cfg->forward_timeout_ms * 5};
  if (!body || call(s->addr, &req, NULL) != 200) {
    s->failed = 1;
  }
  free(body);
  sink_reset(s);
}

static void sink_row(sink *s, const json_val *row) {
  size_t key_len = 0;
  const char *key = json_str(json_get(row, "key"), &key_len);
  bool ok;
  if (strcmp(s->path, "/_internal/batch") == 0) {
    size_t value_len = 0;
    const char *value = json_str(json_get(row, "value"), &value_len);
    json_node *item = json_new_obj(s->b);
    ok = (bool)(json_put(s->b, item, "key", json_new_str(s->b, key, key_len)) &&
                json_put(s->b, item, "value", json_new_str(s->b, value ? value : "", value_len)) &&
                json_put(s->b, item, "version",
                         json_new_int(s->b, json_int(json_get(row, "version")))) &&
                json_put(s->b, item, "deleted",
                         json_new_bool(s->b, json_bool(json_get(row, "deleted")))) &&
                json_push(s->items, item));
  } else {
    ok = json_push(s->items, json_new_str(s->b, key, key_len));
  }
  if (!ok) {
    s->failed = 1;
  }
  if (++s->count == BATCH_ROWS) {
    sink_flush(s);
  }
}

/** Decides per dumped row whether it goes to the sink. */
typedef bool (*row_filter)(controller *ctl, const char *key, size_t key_len, void *arg);

/** Flush src, dump all its rows and send those filter keeps to s. Returns 0 or -1. */
static int copy_rows(controller *ctl, const char *src, row_filter filter, void *arg, sink *s) {
  hclient_req flush = {
      "POST", "/_internal/flush", NULL, NULL, 0, ctl->cfg->forward_timeout_ms * 5,
  };
  if (post(src, &flush) != 200) {
    return -1;
  }
  char *cursor = strdup("");
  while (cursor) {
    size_t path_len = strlen(cursor) + 64;
    char *path = malloc(path_len);
    if (!path) {
      free(cursor);
      return -1;
    }
    (void)snprintf(path, path_len, "/_internal/dump?limit=%d&cursor=%s", DUMP_PAGE, cursor);
    hclient_req req = {"GET", path, NULL, NULL, 0, ctl->cfg->forward_timeout_ms * 5};
    hclient_resp resp = {0, NULL, 0};
    int status = call(src, &req, &resp);
    free(path);
    if (status != 200) {
      free(resp.body);
      free(cursor);
      return -1;
    }
    json_doc *doc = json_parse(resp.body, resp.len);
    free(resp.body);
    const json_val *items = json_get(json_doc_root(doc), "items");
    if (json_type_of(items) != JSON_ARRAY) {
      json_doc_free(doc);
      free(cursor);
      return -1;
    }
    for (size_t i = 0; i != json_len(items); i++) {
      const json_val *row = json_at(items, i);
      size_t key_len = 0;
      const char *key = json_str(json_get(row, "key"), &key_len);
      if (key && filter(ctl, key, key_len, arg)) {
        sink_row(s, row);
      }
    }
    const json_val *next = json_get(json_doc_root(doc), "next");
    size_t next_len = 0;
    const char *next_key = json_str(next, &next_len);
    free(cursor);
    cursor = NULL;
    if (!next_key) {
      json_doc_free(doc);
      sink_flush(s);
      return s->failed ? -1 : 0;
    }
    // Percent-encode the cursor key for the query string.
    static const char hex[] = "0123456789ABCDEF";
    cursor = malloc((next_len * 3) + 1);
    for (size_t i = 0; cursor && i != next_len; i++) {
      unsigned char ch = (unsigned char)next_key[i];
      cursor[i * 3] = '%';
      cursor[(i * 3) + 1] = hex[ch >> 4U];
      cursor[(i * 3) + 2] = hex[ch & 0xfU];
    }
    if (cursor) {
      cursor[next_len * 3] = '\0';
    }
    json_doc_free(doc);
  }
  return -1;
}

/** Return the first node of key's preference list in r that serves reads and is not skip. */
static int first_reader(controller *ctl, const ring *r, const char *key, size_t key_len, int skip) {
  int prefs[RING_MAX_REPLICAS];
  size_t n = ring_prefs(r, key, key_len, prefs);
  for (size_t i = 0; i != n; i++) {
    if (prefs[i] != skip && node_serves_reads(ctl->c->nodes[prefs[i]].sm.state)) {
      return prefs[i];
    }
  }
  return 0;
}

// ---- Repair ----

typedef struct {
  int node;
  int source;
} repair_arg;

static bool repair_filter(controller *ctl, const char *key, size_t key_len, void *arg) {
  repair_arg *a = arg;
  pthread_mutex_lock(&ctl->c->mu);
  bool keep = (bool)(ring_has(ctl->c->ring, a->node, key, key_len) &&
                     first_reader(ctl, ctl->c->ring, key, key_len, a->node) == a->source);
  pthread_mutex_unlock(&ctl->c->mu);
  return keep;
}

/** Copy the keys of node id from the replicas that serve reads. Returns 0 or -1. */
static int repair(controller *ctl, int id) {
  driver_fault_point("REPAIR", id);
  // Writes that started before the node was REPAIRING did not go to it.
  if (cluster_drain(ctl->c, DRAIN_TIMEOUT_MS) != 0) {
    return -1;
  }
  set_purge_all(ctl, true);
  pthread_mutex_lock(&ctl->c->mu);
  char addr[DRIVER_ADDR_LEN];
  memcpy(addr, ctl->c->nodes[id].addr, sizeof(addr));
  pthread_mutex_unlock(&ctl->c->mu);
  int ids[CLUSTER_MAX_NODES];
  static char addrs[CLUSTER_MAX_NODES][DRIVER_ADDR_LEN];
  size_t n = snapshot(ctl, node_serves_reads, ids, addrs);
  int rc = 0;
  for (size_t i = 0; i != n && rc == 0; i++) {
    repair_arg a = {id, ids[i]};
    sink s = {ctl, addr, "/_internal/batch", NULL, NULL, 0, 0};
    sink_reset(&s);
    rc = copy_rows(ctl, addrs[i], repair_filter, &a, &s);
    json_builder_free(s.b);
  }
  set_purge_all(ctl, false);
  return rc;
}

/** Spawn node id again with its data and repair it. */
static void respawn(controller *ctl, int id) {
  unsigned act = apply_node_event(ctl, id, NODE_EV_RESPAWN);
  if ((act & NODE_ACT_SPAWN) == 0) {
    return;
  }
  driver_fault_point("RESPAWN", id);
  char raw[DRIVER_ADDR_LEN];
  char addr[DRIVER_ADDR_LEN];
  if (driver_spawn(id, raw) != 0 || resolve(raw, addr) != 0 || !wait_healthy(ctl, addr)) {
    LOGF(ctl, LOG_ERROR, "node %d: spawn failed", id);
    (void)apply_node_event(ctl, id, NODE_EV_SPAWN_FAIL);
    return;
  }
  pthread_mutex_lock(&ctl->c->mu);
  memcpy(ctl->c->nodes[id].addr, addr, sizeof(addr));
  pthread_mutex_unlock(&ctl->c->mu);
  act = apply_node_event(ctl, id, NODE_EV_SPAWN_OK);
  while ((act & NODE_ACT_REPAIR) != 0) {
    if (repair(ctl, id) == 0) {
      (void)apply_node_event(ctl, id, NODE_EV_REPAIR_DONE);
      return;
    }
    LOGF(ctl, LOG_WARN, "node %d: repair failed", id);
    if (!healthy(ctl, addr)) {
      // Leave it REPAIRING: probes find it dead and spawn it again.
      return;
    }
    act = apply_node_event(ctl, id, NODE_EV_REPAIR_FAIL);
    sleep_ms(ctl->cfg->probe_interval_ms);
  }
}

// ---- Join ----

typedef struct {
  int source;
  int target;
} join_arg;

static bool copy_filter(controller *ctl, const char *key, size_t key_len, void *arg) {
  join_arg *a = arg;
  pthread_mutex_lock(&ctl->c->mu);
  bool keep = (bool)(ctl->c->next && ring_has(ctl->c->next, a->target, key, key_len) &&
                     first_reader(ctl, ctl->c->ring, key, key_len, a->target) == a->source);
  pthread_mutex_unlock(&ctl->c->mu);
  return keep;
}

static bool purge_filter(controller *ctl, const char *key, size_t key_len, void *arg) {
  int source = *(int *)arg;
  pthread_mutex_lock(&ctl->c->mu);
  bool drop = (bool)(ctl->c->prev && ring_has(ctl->c->prev, source, key, key_len) &&
                     !ring_has(ctl->c->ring, source, key, key_len));
  pthread_mutex_unlock(&ctl->c->mu);
  return drop;
}

static void abort_join(controller *ctl, int target, bool retry) {
  (void)save_migration(ctl, MIG_ABORT, target);
  driver_fault_point("ABORT", target);
  pthread_mutex_lock(&ctl->c->mu);
  ctl->c->double_write = false;
  ring_free(ctl->c->next);
  ctl->c->next = NULL;
  pthread_mutex_unlock(&ctl->c->mu);
  // Requests that write to the target finish before it goes.
  (void)cluster_drain(ctl->c, DRAIN_TIMEOUT_MS);
  (void)driver_stop(target);
  pthread_mutex_lock(&ctl->c->mu);
  memset(&ctl->c->nodes[target], 0, sizeof(ctl->c->nodes[target]));
  pthread_mutex_unlock(&ctl->c->mu);
  (void)save_node(ctl, target);
  set_purge_all(ctl, false);
  (void)save_migration(ctl, MIG_IDLE, 0);
  LOGF(ctl, LOG_WARN, "migration: join of node %d aborted", target);
  if (retry) {
    // The next loop runs it again once every node is ACTIVE (a dead source is respawned first).
    pthread_mutex_lock(&ctl->c->mu);
    ctl->c->add_requested = true;
    pthread_mutex_unlock(&ctl->c->mu);
  }
}

/** COPY: double-write on, then copy older rows of moving keys to the target. Returns 0 or -1. */
static int copy_phase(controller *ctl, int target) {
  pthread_mutex_lock(&ctl->c->mu);
  ring *next = cluster_build_ring(ctl->c, target);
  ring_free(ctl->c->next);
  ctl->c->next = next;
  ctl->c->double_write = next != NULL;
  ctl->c->double_write_failed = false;
  char addr[DRIVER_ADDR_LEN];
  memcpy(addr, ctl->c->nodes[target].addr, sizeof(addr));
  pthread_mutex_unlock(&ctl->c->mu);
  if (!next) {
    return -1;
  }
  driver_fault_point("COPY", target);
  // Writes planned before double-write did not go to the target: wait for them, then copy.
  if (cluster_drain(ctl->c, DRAIN_TIMEOUT_MS) != 0) {
    return -1;
  }
  set_purge_all(ctl, true);
  int ids[CLUSTER_MAX_NODES];
  static char addrs[CLUSTER_MAX_NODES][DRIVER_ADDR_LEN];
  size_t n = snapshot(ctl, node_serves_reads, ids, addrs);
  int rc = 0;
  for (size_t i = 0; i != n && rc == 0; i++) {
    join_arg a = {ids[i], target};
    sink s = {ctl, addr, "/_internal/batch", NULL, NULL, 0, 0};
    sink_reset(&s);
    rc = copy_rows(ctl, addrs[i], copy_filter, &a, &s);
    json_builder_free(s.b);
  }
  pthread_mutex_lock(&ctl->c->mu);
  bool lost_write = ctl->c->double_write_failed;
  pthread_mutex_unlock(&ctl->c->mu);
  if (rc == 0 && (lost_write || !healthy(ctl, addr))) {
    rc = -1;
  }
  // Copied rows must be durable on the target before PURGE deletes them from the old owners.
  hclient_req flush = {
      "POST", "/_internal/flush", NULL, NULL, 0, ctl->cfg->forward_timeout_ms * 5,
  };
  if (rc == 0 && post(addr, &flush) != 200) {
    rc = -1;
  }
  return rc;
}

/** FLIP: the ring with the target owns keys now. */
static void flip(controller *ctl, int target) {
  pthread_mutex_lock(&ctl->c->mu);
  cnode *t = &ctl->c->nodes[target];
  t->in_ring = true;
  if (t->sm.state == NODE_SYNCING) {
    t->sm = node_step(t->sm, NODE_EV_SYNC_DONE, ctl->cfg->probe_fails).next;
  }
  ring *next = ctl->c->next ? ctl->c->next : cluster_build_ring(ctl->c, 0);
  ctl->c->next = NULL;
  if (!ctl->c->prev) {
    t->in_ring = false;
    ctl->c->prev = cluster_build_ring(ctl->c, 0);
    t->in_ring = true;
  }
  ring_free(ctl->c->ring);
  ctl->c->ring = next;
  ctl->c->double_write = false;
  pthread_mutex_unlock(&ctl->c->mu);
  (void)save_node(ctl, target);
}

/** PURGE: delete from each old node the keys it no longer holds. Returns 0 or -1. */
static int purge_phase(controller *ctl, int target) {
  driver_fault_point("PURGE", target);
  int ids[CLUSTER_MAX_NODES];
  static char addrs[CLUSTER_MAX_NODES][DRIVER_ADDR_LEN];
  size_t n = snapshot(ctl, node_serves_reads, ids, addrs);
  int rc = 0;
  for (size_t i = 0; i != n && rc == 0; i++) {
    if (ids[i] == target) {
      continue;
    }
    sink s = {ctl, addrs[i], "/_internal/delete-batch", NULL, NULL, 0, 0};
    sink_reset(&s);
    rc = copy_rows(ctl, addrs[i], purge_filter, &ids[i], &s);
    json_builder_free(s.b);
  }
  if (rc == 0) {
    set_purge_all(ctl, false);
  }
  return rc;
}

/** Run the phases from FLIP to IDLE. */
static void finish_join(controller *ctl, mig_phase from, int target) {
  mig_phase phase = from;
  while (phase != MIG_IDLE) {
    if (phase == MIG_FLIP) {
      driver_fault_point("FLIP", target);
      flip(ctl, target);
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
      break;
    }
    if (save_migration(ctl, phase, phase == MIG_IDLE ? 0 : target) != 0) {
      return;
    }
  }
  pthread_mutex_lock(&ctl->c->mu);
  ring_free(ctl->c->prev);
  ctl->c->prev = NULL;
  pthread_mutex_unlock(&ctl->c->mu);
  ctl->last_scale_ms = mono_ms();
  LOGF(ctl, LOG_INFO, "migration: node %d joined", target);
}

/** Add one node. asked: POST /_cp/nodes asked for it, so an aborted join asks again. */
static void join(controller *ctl, bool asked) {
  pthread_mutex_lock(&ctl->c->mu);
  int target = 1;
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    if (ctl->c->nodes[id].exists) {
      target = id + 1;
    }
  }
  ctl->c->add_requested = false;
  pthread_mutex_unlock(&ctl->c->mu);
  if (target > CLUSTER_MAX_NODES || save_migration(ctl, MIG_SPAWN, target) != 0) {
    return;
  }
  driver_fault_point("SPAWN", target);
  pthread_mutex_lock(&ctl->c->mu);
  ctl->c->nodes[target] = (cnode){.exists = true, .sm = {NODE_SPAWNING, 0, false}};
  pthread_mutex_unlock(&ctl->c->mu);
  (void)save_node(ctl, target);
  char raw[DRIVER_ADDR_LEN];
  char addr[DRIVER_ADDR_LEN];
  if (driver_spawn(target, raw) != 0 || resolve(raw, addr) != 0 || !wait_healthy(ctl, addr)) {
    LOGF(ctl, LOG_ERROR, "migration: spawn of node %d failed", target);
    abort_join(ctl, target, asked);
    return;
  }
  pthread_mutex_lock(&ctl->c->mu);
  memcpy(ctl->c->nodes[target].addr, addr, sizeof(addr));
  pthread_mutex_unlock(&ctl->c->mu);
  (void)apply_node_event(ctl, target, NODE_EV_SPAWN_OK);
  if (save_migration(ctl, MIG_COPY, target) != 0 || copy_phase(ctl, target) != 0 ||
      save_migration(ctl, MIG_FLIP, target) != 0) {
    abort_join(ctl, target, asked);
    return;
  }
  finish_join(ctl, MIG_FLIP, target);
}

// ---- Loop ----

static bool probed(node_state s) {
  return (bool)(s == NODE_ACTIVE || s == NODE_SUSPECT || s == NODE_REPAIRING);
}

static void probe_all(controller *ctl) {
  int ids[CLUSTER_MAX_NODES];
  static char addrs[CLUSTER_MAX_NODES][DRIVER_ADDR_LEN];
  size_t n = snapshot(ctl, probed, ids, addrs);
  for (size_t i = 0; i != n; i++) {
    pthread_mutex_lock(&ctl->c->mu);
    bool forward_error = ctl->c->nodes[ids[i]].forward_error;
    ctl->c->nodes[ids[i]].forward_error = false;
    pthread_mutex_unlock(&ctl->c->mu);
    if (forward_error) {
      (void)apply_node_event(ctl, ids[i], NODE_EV_FORWARD_ERROR);
    }
    (void)apply_node_event(ctl, ids[i],
                           healthy(ctl, addrs[i]) ? NODE_EV_PROBE_OK : NODE_EV_PROBE_FAIL);
  }
}

static bool is_dead(node_state s) { return s == NODE_DEAD; }
static bool is_repairing(node_state s) { return s == NODE_REPAIRING; }

static void heal(controller *ctl) {
  int ids[CLUSTER_MAX_NODES];
  static char addrs[CLUSTER_MAX_NODES][DRIVER_ADDR_LEN];
  size_t n = snapshot(ctl, is_dead, ids, addrs);
  for (size_t i = 0; i != n; i++) {
    respawn(ctl, ids[i]);
  }
  // A node left REPAIRING (repair failed, or cp restarted) gets its repair again.
  n = snapshot(ctl, is_repairing, ids, addrs);
  for (size_t i = 0; i != n; i++) {
    if (repair(ctl, ids[i]) == 0) {
      (void)apply_node_event(ctl, ids[i], NODE_EV_REPAIR_DONE);
    }
  }
}

/** Return whether a node is over KV_CP_SCALE_RPS in the last window; restart the window. */
static bool over_rps(controller *ctl) {
  int64_t now = mono_ms();
  int64_t elapsed = now - ctl->window_start_ms;
  if (elapsed < (int64_t)ctl->cfg->scale_window_s * 1000) {
    return false;
  }
  bool over = false;
  pthread_mutex_lock(&ctl->c->mu);
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    uint64_t total = ctl->c->nodes[id].requests;
    uint64_t delta = total - ctl->window_requests[id];
    ctl->window_requests[id] = total;
    if (ctl->c->nodes[id].exists && ctl->cfg->scale_rps > 0 &&
        (double)delta * 1000.0 / (double)elapsed > (double)ctl->cfg->scale_rps) {
      over = true;
    }
  }
  pthread_mutex_unlock(&ctl->c->mu);
  ctl->window_start_ms = now;
  return over;
}

static void maybe_join(controller *ctl) {
  bool over = over_rps(ctl);
  pthread_mutex_lock(&ctl->c->mu);
  int count = 0;
  bool all_active = true;
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    if (ctl->c->nodes[id].exists) {
      count++;
      all_active = (bool)(all_active && ctl->c->nodes[id].sm.state == NODE_ACTIVE);
    }
  }
  bool asked = ctl->c->add_requested;
  if (count >= ctl->cfg->max_nodes) {
    ctl->c->add_requested = false;
  }
  pthread_mutex_unlock(&ctl->c->mu);
  bool cooled = mono_ms() - ctl->last_scale_ms >= (int64_t)ctl->cfg->scale_cooldown_s * 1000;
  if (count < ctl->cfg->max_nodes && all_active && (asked || (over && cooled))) {
    LOGF(ctl, LOG_INFO, "migration: adding a node (%s)", asked ? "admin" : "autoscale");
    join(ctl, asked);
  }
}

static void reserve_versions(controller *ctl) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  uint64_t now = ((uint64_t)ts.tv_sec * 1000) + ((uint64_t)ts.tv_nsec / 1000000);
  pthread_mutex_lock(&ctl->c->mu);
  uint64_t mark = ctl->c->version_mark;
  uint64_t last = ctl->c->last_version;
  pthread_mutex_unlock(&ctl->c->mu);
  uint64_t want = (now + (VERSION_RESERVE_MS / 2)) << 16U;
  if (mark > want && mark > last + (1ULL << 30U)) {
    return;
  }
  uint64_t next = (now + VERSION_RESERVE_MS) << 16U;
  if (next <= last) {
    next = last + (1ULL << 32U);
  }
  if (cp_store_put_version_mark(ctl->store, next) == 0) {
    pthread_mutex_lock(&ctl->c->mu);
    ctl->c->version_mark = next;
    pthread_mutex_unlock(&ctl->c->mu);
  }
}

static void *controller_main(void *arg) {
  controller *ctl = arg;
  pthread_setname_np(pthread_self(), "cp-controller");
  for (;;) {
    pthread_mutex_lock(&ctl->mu);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    long ns = deadline.tv_nsec + ((long)ctl->cfg->probe_interval_ms * 1000000L);
    deadline.tv_sec += ns / 1000000000L;
    deadline.tv_nsec = ns % 1000000000L;
    while (!ctl->stop && pthread_cond_timedwait(&ctl->wake, &ctl->mu, &deadline) == 0) {
    }
    bool stop = ctl->stop;
    pthread_mutex_unlock(&ctl->mu);
    if (stop) {
      return NULL;
    }
    reserve_versions(ctl);
    probe_all(ctl);
    heal(ctl);
    pthread_mutex_lock(&ctl->c->mu);
    mig_phase phase = ctl->c->phase;
    int target = ctl->c->target;
    pthread_mutex_unlock(&ctl->c->mu);
    if (phase == MIG_PURGE) {
      finish_join(ctl, phase, target);
    } else {
      maybe_join(ctl);
    }
  }
}

// ---- Start-up ----

static int spawn_initial(controller *ctl, int id, int *failures) {
  for (;;) {
    char raw[DRIVER_ADDR_LEN];
    char addr[DRIVER_ADDR_LEN];
    if (driver_spawn(id, raw) == 0 && resolve(raw, addr) == 0 && wait_healthy(ctl, addr)) {
      pthread_mutex_lock(&ctl->c->mu);
      ctl->c->nodes[id] = (cnode){.exists = true, .sm = {NODE_ACTIVE, 0, true}, .in_ring = true};
      memcpy(ctl->c->nodes[id].addr, addr, sizeof(addr));
      pthread_mutex_unlock(&ctl->c->mu);
      return save_node(ctl, id);
    }
    if (++*failures >= START_SPAWN_TRIES) {
      (void)fprintf(stderr, "cp: spawn of node %d failed %d times, exiting\n", id, *failures);
      return -1;
    }
    LOGF(ctl, LOG_WARN, "start: spawn of node %d failed, retrying", id);
    sleep_ms((int)(200U << (unsigned)*failures));
  }
}

/** Load the stored nodes into the cluster; a node that does not run is DEAD. Returns the count. */
static int load_nodes(controller *ctl, const stored_node *stored, int n, const driver_node *live,
                      int n_live) {
  cluster *c = ctl->c;
  for (int i = 0; i != n; i++) {
    const stored_node *s = &stored[i];
    if (s->id < 1 || s->id > CLUSTER_MAX_NODES || c->nodes[s->id].exists) {
      continue;
    }
    cnode *node = &c->nodes[s->id];
    *node = (cnode){.exists = true, .sm = s->sm, .in_ring = s->in_ring};
    memcpy(node->addr, s->addr, sizeof(node->addr));
    bool running = false;
    for (int j = 0; j != n_live; j++) {
      if (live[j].id == s->id) {
        running = resolve(live[j].addr, node->addr) == 0;
      }
    }
    if (!running || node->sm.state == NODE_SPAWNING) {
      node->sm = (node_sm){NODE_DEAD, 0, true};
    } else if (node->sm.state == NODE_DEAD) {
      node->sm.state = NODE_REPAIRING;
    }
    node->sm.rejoin = true;
    (void)save_node(ctl, s->id);
  }
  return n;
}

/** Adopt running nodes as the ring when cp has no state (cp.db was lost). */
static void adopt_nodes(controller *ctl, const driver_node *live, int n_live) {
  for (int i = 0; i != n_live; i++) {
    char addr[DRIVER_ADDR_LEN];
    if (resolve(live[i].addr, addr) == 0) {
      ctl->c->nodes[live[i].id] =
          (cnode){.exists = true, .sm = {NODE_ACTIVE, 0, true}, .in_ring = true};
      memcpy(ctl->c->nodes[live[i].id].addr, addr, sizeof(addr));
      (void)save_node(ctl, live[i].id);
    }
  }
}

/** Abort or finish a join that an earlier cp left in phase mig. */
static void restart_migration(controller *ctl, stored_migration mig) {
  cluster *c = ctl->c;
  mig_result r = mig_step(mig.phase, MIG_EV_RESTART);
  if ((r.actions & MIG_ACT_ABORT) != 0) {
    LOGF(ctl, LOG_WARN, "start: aborting the join of node %d left in %s", mig.target,
         mig_phase_name(mig.phase));
    pthread_mutex_lock(&c->mu);
    c->nodes[mig.target].in_ring = false;
    pthread_mutex_unlock(&c->mu);
    abort_join(ctl, mig.target, false);
    return;
  }
  if ((r.actions & (MIG_ACT_FLIP | MIG_ACT_DRAIN | MIG_ACT_PURGE)) == 0) {
    return;
  }
  LOGF(ctl, LOG_INFO, "start: finishing the join of node %d from %s", mig.target,
       mig_phase_name(mig.phase));
  pthread_mutex_lock(&c->mu);
  cnode *t = &c->nodes[mig.target];
  t->in_ring = false;
  c->prev = cluster_build_ring(c, 0);
  t->in_ring = true;
  ring_free(c->ring);
  c->ring = cluster_build_ring(c, 0);
  c->phase = mig.phase;
  c->target = mig.target;
  pthread_mutex_unlock(&c->mu);
  if (mig.phase == MIG_FLIP) {
    flip(ctl, mig.target);
    if (save_migration(ctl, MIG_DRAIN, mig.target) == 0) {
      finish_join(ctl, MIG_DRAIN, mig.target);
    }
  } else {
    finish_join(ctl, mig.phase, mig.target);
  }
}

int controller_start_up(controller *ctl) {
  static stored_node stored[CLUSTER_MAX_NODES];
  static driver_node live[CLUSTER_MAX_NODES];
  int n = cp_store_nodes(ctl->store, stored, CLUSTER_MAX_NODES);
  int n_live = driver_list(live, CLUSTER_MAX_NODES);
  stored_migration mig;
  uint64_t mark = 0;
  if (n < 0 || n_live < 0 || cp_store_migration(ctl->store, &mig) != 0 ||
      cp_store_version_mark(ctl->store, &mark) != 0) {
    (void)fprintf(stderr, "cp: could not read the state store or the node list\n");
    return -1;
  }
  cluster *c = ctl->c;
  c->last_version = mark;
  c->version_mark = mark;
  if (n == 0) {
    adopt_nodes(ctl, live, n_live);
  } else {
    (void)load_nodes(ctl, stored, n, live, n_live);
  }
  int failures = 0;
  for (int id = 1; id <= ctl->cfg->initial_nodes; id++) {
    if (!c->nodes[id].exists && spawn_initial(ctl, id, &failures) != 0) {
      return -1;
    }
  }
  pthread_mutex_lock(&c->mu);
  c->ring = cluster_build_ring(c, 0);
  pthread_mutex_unlock(&c->mu);
  if (!c->ring) {
    return -1;
  }
  restart_migration(ctl, mig);
  reserve_versions(ctl);
  return 0;
}

int controller_run(controller *ctl) {
  if (pthread_create(&ctl->thread, NULL, controller_main, ctl) != 0) {
    return -1;
  }
  ctl->running = true;
  return 0;
}

void controller_free(controller *ctl) {
  if (!ctl) {
    return;
  }
  if (ctl->running) {
    pthread_mutex_lock(&ctl->mu);
    ctl->stop = true;
    pthread_cond_signal(&ctl->wake);
    pthread_mutex_unlock(&ctl->mu);
    pthread_join(ctl->thread, NULL);
  }
  pthread_mutex_destroy(&ctl->mu);
  pthread_cond_destroy(&ctl->wake);
  free(ctl);
}
