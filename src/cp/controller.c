#include "cp/controller_internal.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int64_t ctl_mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((int64_t)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

void ctl_sleep_ms(int ms) {
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
  ctl->last_scale_ms = ctl_mono_ms() - ((int64_t)ctl->cfg->scale_cooldown_s * 1000);
  ctl->window_start_ms = ctl_mono_ms();
  return ctl;
}

int ctl_call(const char *addr, const hclient_req *req, hclient_resp *out) {
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

bool ctl_probe_health(controller *ctl, const char *addr) {
  int timeout = ctl->cfg->probe_interval_ms < ctl->cfg->forward_timeout_ms
                    ? ctl->cfg->probe_interval_ms
                    : ctl->cfg->forward_timeout_ms;
  hclient_req req = {"GET", "/_internal/health", NULL, NULL, 0, timeout};
  return ctl_call(addr, &req, NULL) == 200;
}

bool ctl_wait_healthy(controller *ctl, const char *addr) {
  int64_t deadline = ctl_mono_ms() + SPAWN_WAIT_MS;
  while (ctl_mono_ms() < deadline) {
    if (ctl_probe_health(ctl, addr)) {
      return true;
    }
    ctl_sleep_ms(50);
  }
  return false;
}

int ctl_resolve(const char *in, char out[DRIVER_ADDR_LEN]) {
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

int ctl_save_node(controller *ctl, int id) {
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

unsigned ctl_node_event(controller *ctl, int id, node_event ev) {
  pthread_mutex_lock(&ctl->c->mu);
  cnode *n = &ctl->c->nodes[id];
  node_sm before = n->sm;
  node_result r = node_step(before, ev, ctl->cfg->probe_fails);
  n->sm = r.next;
  pthread_mutex_unlock(&ctl->c->mu);
  if (r.next.state != before.state || r.next.rejoin != before.rejoin) {
    LOGF(ctl, LOG_INFO, "node %d: %s -> %s", id, node_state_name(before.state),
         node_state_name(r.next.state));
    (void)ctl_save_node(ctl, id);
  }
  return r.actions;
}

int ctl_save_migration(controller *ctl, mig_phase phase, int target) {
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

size_t ctl_snapshot(controller *ctl, bool (*want)(node_state), node_ref *out) {
  size_t n = 0;
  pthread_mutex_lock(&ctl->c->mu);
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    const cnode *node = &ctl->c->nodes[id];
    if (node->exists && want(node->sm.state)) {
      out[n].id = id;
      memcpy(out[n].addr, node->addr, DRIVER_ADDR_LEN);
      n++;
    }
  }
  pthread_mutex_unlock(&ctl->c->mu);
  return n;
}

static bool is_live(node_state s) { return (bool)(node_takes_writes(s) || s == NODE_SYNCING); }

void ctl_set_purge_all(controller *ctl, bool paused) {
  static node_ref nodes[CLUSTER_MAX_NODES];
  size_t n = ctl_snapshot(ctl, is_live, nodes);
  for (size_t i = 0; i != n; i++) {
    const char *body = paused ? "{\"paused\":true}" : "{\"paused\":false}";
    hclient_req req = {
        "POST", "/_internal/purge", NULL, body, strlen(body), ctl->cfg->forward_timeout_ms * 5,
    };
    (void)ctl_call(nodes[i].addr, &req, NULL);
  }
}

void ctl_reserve_versions(controller *ctl) {
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

static bool probed(node_state s) {
  return (bool)(s == NODE_ACTIVE || s == NODE_SUSPECT || s == NODE_REPAIRING);
}

static void probe_all(controller *ctl) {
  static node_ref nodes[CLUSTER_MAX_NODES];
  size_t n = ctl_snapshot(ctl, probed, nodes);
  for (size_t i = 0; i != n; i++) {
    int id = nodes[i].id;
    pthread_mutex_lock(&ctl->c->mu);
    bool forward_error = ctl->c->nodes[id].forward_error;
    ctl->c->nodes[id].forward_error = false;
    pthread_mutex_unlock(&ctl->c->mu);
    if (forward_error) {
      (void)ctl_node_event(ctl, id, NODE_EV_FORWARD_ERROR);
    }
    (void)ctl_node_event(
        ctl, id, ctl_probe_health(ctl, nodes[i].addr) ? NODE_EV_PROBE_OK : NODE_EV_PROBE_FAIL);
  }
}

static bool is_dead(node_state s) { return s == NODE_DEAD; }
static bool is_repairing(node_state s) { return s == NODE_REPAIRING; }

static void heal(controller *ctl) {
  static node_ref nodes[CLUSTER_MAX_NODES];
  size_t n = ctl_snapshot(ctl, is_dead, nodes);
  for (size_t i = 0; i != n; i++) {
    ctl_respawn(ctl, nodes[i].id);
  }
  // A node left REPAIRING (repair failed, or cp restarted) gets its repair again.
  n = ctl_snapshot(ctl, is_repairing, nodes);
  for (size_t i = 0; i != n; i++) {
    if (ctl_repair(ctl, nodes[i].id) == 0) {
      (void)ctl_node_event(ctl, nodes[i].id, NODE_EV_REPAIR_DONE);
    }
  }
}

/** Return whether a node is over KV_CP_SCALE_RPS in the last window; restart the window. */
static bool over_rps(controller *ctl) {
  int64_t now = ctl_mono_ms();
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
  bool cooled = ctl_mono_ms() - ctl->last_scale_ms >= (int64_t)ctl->cfg->scale_cooldown_s * 1000;
  if (count < ctl->cfg->max_nodes && all_active && (asked || (over && cooled))) {
    LOGF(ctl, LOG_INFO, "migration: adding a node (%s)", asked ? "admin" : "autoscale");
    ctl_join(ctl, asked);
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
    ctl_reserve_versions(ctl);
    probe_all(ctl);
    heal(ctl);
    pthread_mutex_lock(&ctl->c->mu);
    mig_phase phase = ctl->c->phase;
    int target = ctl->c->target;
    pthread_mutex_unlock(&ctl->c->mu);
    if (phase == MIG_PURGE) {
      ctl_finish_join(ctl, phase, target);
    } else {
      maybe_join(ctl);
    }
  }
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
