#include "cp/controller_internal.h"

#include <string.h>

typedef struct {
  int source;
  int target;
} join_arg;

static bool copy_filter(controller *ctl, const char *key, size_t key_len, void *arg) {
  join_arg *a = arg;
  pthread_mutex_lock(&ctl->c->mu);
  bool keep = (bool)(ctl->c->next && ring_has(ctl->c->next, a->target, key, key_len) &&
                     ctl_first_reader(ctl, ctl->c->ring, key, key_len, a->target) == a->source);
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

static void ctl_abort_join(controller *ctl, int target, bool retry) {
  (void)ctl_save_migration(ctl, MIG_ABORT, target);
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
  (void)ctl_save_node(ctl, target);
  ctl_set_purge_all(ctl, false);
  (void)ctl_save_migration(ctl, MIG_IDLE, 0);
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
  ctl_set_purge_all(ctl, true);
  static node_ref sources[CLUSTER_MAX_NODES];
  size_t n = ctl_snapshot(ctl, node_serves_reads, sources);
  int rc = 0;
  for (size_t i = 0; i != n && rc == 0; i++) {
    join_arg a = {sources[i].id, target};
    rc = ctl_copy_rows(ctl, sources[i].addr, copy_filter, &a, addr, "/_internal/batch");
  }
  pthread_mutex_lock(&ctl->c->mu);
  bool lost_write = ctl->c->double_write_failed;
  pthread_mutex_unlock(&ctl->c->mu);
  if (rc == 0 && (lost_write || !ctl_probe_health(ctl, addr))) {
    rc = -1;
  }
  // Copied rows must be durable on the target before PURGE deletes them from the old owners.
  hclient_req flush = {
      "POST", "/_internal/flush", NULL, NULL, 0, ctl->cfg->forward_timeout_ms * 5,
  };
  if (rc == 0 && ctl_call(addr, &flush, NULL) != 200) {
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
  (void)ctl_save_node(ctl, target);
}

/** PURGE: delete from each old node the keys it no longer holds. Returns 0 or -1. */
static int purge_phase(controller *ctl, int target) {
  driver_fault_point("PURGE", target);
  static node_ref olds[CLUSTER_MAX_NODES];
  size_t n = ctl_snapshot(ctl, node_serves_reads, olds);
  int rc = 0;
  for (size_t i = 0; i != n && rc == 0; i++) {
    if (olds[i].id == target) {
      continue;
    }
    rc = ctl_copy_rows(ctl, olds[i].addr, purge_filter, &olds[i].id, olds[i].addr,
                       "/_internal/delete-batch");
  }
  if (rc == 0) {
    ctl_set_purge_all(ctl, false);
  }
  return rc;
}

void ctl_finish_join(controller *ctl, mig_phase from, int target) {
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
    if (ctl_save_migration(ctl, phase, phase == MIG_IDLE ? 0 : target) != 0) {
      return;
    }
  }
  pthread_mutex_lock(&ctl->c->mu);
  ring_free(ctl->c->prev);
  ctl->c->prev = NULL;
  pthread_mutex_unlock(&ctl->c->mu);
  ctl->last_scale_ms = ctl_mono_ms();
  LOGF(ctl, LOG_INFO, "migration: node %d joined", target);
}

void ctl_join(controller *ctl, bool asked) {
  pthread_mutex_lock(&ctl->c->mu);
  int target = 1;
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    if (ctl->c->nodes[id].exists) {
      target = id + 1;
    }
  }
  ctl->c->add_requested = false;
  pthread_mutex_unlock(&ctl->c->mu);
  if (target > CLUSTER_MAX_NODES || ctl_save_migration(ctl, MIG_SPAWN, target) != 0) {
    return;
  }
  driver_fault_point("SPAWN", target);
  pthread_mutex_lock(&ctl->c->mu);
  ctl->c->nodes[target] = (cnode){.exists = true, .sm = {NODE_SPAWNING, 0, false}};
  pthread_mutex_unlock(&ctl->c->mu);
  (void)ctl_save_node(ctl, target);
  char raw[DRIVER_ADDR_LEN];
  char addr[DRIVER_ADDR_LEN];
  if (driver_spawn(target, raw) != 0 || ctl_resolve(raw, addr) != 0 ||
      !ctl_wait_healthy(ctl, addr)) {
    LOGF(ctl, LOG_ERROR, "migration: spawn of node %d failed", target);
    ctl_abort_join(ctl, target, asked);
    return;
  }
  pthread_mutex_lock(&ctl->c->mu);
  memcpy(ctl->c->nodes[target].addr, addr, sizeof(addr));
  pthread_mutex_unlock(&ctl->c->mu);
  (void)ctl_node_event(ctl, target, NODE_EV_SPAWN_OK);
  if (ctl_save_migration(ctl, MIG_COPY, target) != 0 || copy_phase(ctl, target) != 0 ||
      ctl_save_migration(ctl, MIG_FLIP, target) != 0) {
    ctl_abort_join(ctl, target, asked);
    return;
  }
  ctl_finish_join(ctl, MIG_FLIP, target);
}

void ctl_restart_migration(controller *ctl, stored_migration mig) {
  cluster *c = ctl->c;
  mig_result r = mig_step(mig.phase, MIG_EV_RESTART);
  if ((r.actions & MIG_ACT_ABORT) != 0) {
    LOGF(ctl, LOG_WARN, "start: aborting the join of node %d left in %s", mig.target,
         mig_phase_name(mig.phase));
    pthread_mutex_lock(&c->mu);
    c->nodes[mig.target].in_ring = false;
    pthread_mutex_unlock(&c->mu);
    ctl_abort_join(ctl, mig.target, false);
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
    if (ctl_save_migration(ctl, MIG_DRAIN, mig.target) == 0) {
      ctl_finish_join(ctl, MIG_DRAIN, mig.target);
    }
  } else {
    ctl_finish_join(ctl, mig.phase, mig.target);
  }
}
