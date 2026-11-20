#include "cp/controller_internal.h"

#include "cp/node_client.h"

static void abort_join(controller *ctl, int target, bool retry) {
  (void)cluster_set_phase(ctl->c, MIG_ABORT, target);
  driver_fault_point("ABORT", target);
  cluster_abort_join(ctl->c, target);
  // Requests that write to the target finish before it goes.
  (void)cluster_drain(ctl->c, DRAIN_TIMEOUT_MS);
  (void)driver_stop(target);
  (void)cluster_drop_node(ctl->c, target);
  ctl_pause_purge(ctl, false);
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
  ctl_pause_purge(ctl, true);
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
      rc = ctl_move_rows(ctl, &x, src, v.nodes[src].addr, target, addr);
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
    if (src != target && v.nodes[src].exists && node_serves_reads(v.nodes[src].sm.state)) {
      rc = ctl_move_rows(ctl, &x, src, v.nodes[src].addr, 0, NULL);
    }
  }
  if (rc == 0) {
    ctl_pause_purge(ctl, false);
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
  char raw[DRIVER_ADDR_LEN];
  char addr[DRIVER_ADDR_LEN];
  if (driver_spawn(target, raw) != 0 || ctl_resolve(raw, addr) != 0 || !ctl_wait_healthy(addr)) {
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

void ctl_restart_migration(controller *ctl, stored_migration mig) {
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
