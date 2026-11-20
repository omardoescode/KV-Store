#include "cp/controller_internal.h"

#include "cp/node_client.h"

enum { SPAWN_WAIT_MS = 15000 };

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

controller *controller_new(cluster *c, cp_store *store, const cp_config *cfg, logger log) {
  controller *ctl = calloc(1, sizeof(*ctl));
  if (!ctl) {
    return NULL;
  }
  ctl->c = c;
  ctl->cfg = cfg;
  ctl->store = store;
  ctl->log = log;
  pthread_mutex_init(&ctl->mu, NULL);
  pthread_cond_init(&ctl->wake, NULL);
  ctl->last_scale_ms = ctl_mono_ms() - ((int64_t)ctl->cfg->scale_cooldown_s * 1000);
  ctl->window_start_ms = ctl_mono_ms();
  return ctl;
}

int ctl_spawn_ready(int id, char addr[DRIVER_ADDR_LEN]) {
  if (driver_spawn(id, addr) != 0) {
    return -1;
  }
  int64_t deadline = ctl_mono_ms() + SPAWN_WAIT_MS;
  while (ctl_mono_ms() < deadline) {
    if (node_health(addr)) {
      return 0;
    }
    ctl_sleep_ms(50);
  }
  return -1;
}

static bool probed(node_state s) {
  return (bool)(s == NODE_ACTIVE || s == NODE_SUSPECT || s == NODE_REPAIRING);
}

static void probe_all(controller *ctl) {
  static cluster_view v;
  cluster_snapshot(ctl->c, &v);
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    if (!v.nodes[id].exists || !probed(v.nodes[id].sm.state)) {
      continue;
    }
    if (cluster_take_forward_error(ctl->c, id)) {
      (void)cluster_node_event(ctl->c, id, NODE_EV_FORWARD_ERROR);
    }
    (void)cluster_node_event(ctl->c, id,
                             node_health(v.nodes[id].addr) ? NODE_EV_PROBE_OK : NODE_EV_PROBE_FAIL);
  }
}

static void heal(controller *ctl) {
  static cluster_view v;
  cluster_snapshot(ctl->c, &v);
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    if (v.nodes[id].exists && v.nodes[id].sm.state == NODE_DEAD) {
      ctl_heal(ctl, id, NODE_DEAD);
    }
  }
  // A node left REPAIRING (repair failed, or cp restarted) gets its repair again.
  cluster_snapshot(ctl->c, &v);
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    if (v.nodes[id].exists && v.nodes[id].sm.state == NODE_REPAIRING) {
      ctl_heal(ctl, id, NODE_REPAIRING);
    }
  }
}

/** Return whether a node is over KV_CP_SCALE_RPS in the last window; restart the window. */
static bool over_rps(controller *ctl, const cluster_view *v) {
  int64_t now = ctl_mono_ms();
  int64_t elapsed = now - ctl->window_start_ms;
  if (elapsed < (int64_t)ctl->cfg->scale_window_s * 1000) {
    return false;
  }
  bool over = false;
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    uint64_t total = v->nodes[id].requests;
    uint64_t delta = total - ctl->window_requests[id];
    ctl->window_requests[id] = total;
    if (v->nodes[id].exists && ctl->cfg->scale_rps > 0 &&
        (double)delta * 1000.0 / (double)elapsed > (double)ctl->cfg->scale_rps) {
      over = true;
    }
  }
  ctl->window_start_ms = now;
  return over;
}

static void maybe_join(controller *ctl) {
  static cluster_view v;
  cluster_snapshot(ctl->c, &v);
  bool over = over_rps(ctl, &v);
  bool all_active = true;
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    if (v.nodes[id].exists) {
      all_active = (bool)(all_active && v.nodes[id].sm.state == NODE_ACTIVE);
    }
  }
  bool cooled = ctl_mono_ms() - ctl->last_scale_ms >= (int64_t)ctl->cfg->scale_cooldown_s * 1000;
  if (v.count < ctl->cfg->max_nodes && all_active && (v.add_requested || (over && cooled))) {
    LOGF(ctl, LOG_INFO, "migration: adding a node (%s)", v.add_requested ? "admin" : "autoscale");
    ctl_join(ctl, v.add_requested);
  }
}

void controller_tick(controller *ctl) {
  cluster_reserve_versions(ctl->c);
  probe_all(ctl);
  heal(ctl);
  static cluster_view v;
  cluster_snapshot(ctl->c, &v);
  if (v.phase == MIG_PURGE) {
    ctl_finish_join(ctl, v.phase, v.target);
  } else {
    maybe_join(ctl);
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
    controller_tick(ctl);
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
  if (ctl->up) {
    (void)cp_store_put_running(ctl->store, false);
  }
  pthread_mutex_destroy(&ctl->mu);
  pthread_cond_destroy(&ctl->wake);
  free(ctl);
}
