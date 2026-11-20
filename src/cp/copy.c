#include "cp/controller_internal.h"

#include "cp/node_client.h"

enum { BATCH_ROWS = 500 };

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
  cluster_reserve_versions(m->ctl->c);
  static node_row batch[BATCH_ROWS];
  size_t count = 0;
  for (size_t i = 0; i != n; i++) {
    const node_row *r = &rows[i];
    bool moves = false;
    if (m->target != 0) {
      moves = xfer_source(m->x, r->key, r->key_len, m->target) == m->node;
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

int ctl_move_rows(controller *ctl, const xfer *x, int node, const char *addr, int target,
                  const char *dst) {
  if (node_flush(addr) != 0) {
    return -1;
  }
  move m = {ctl, x, node, addr, target, dst};
  return node_dump(addr, move_page, &m);
}

static bool is_live(node_state s) { return (bool)(node_takes_writes(s) || s == NODE_SYNCING); }

void ctl_pause_purge(controller *ctl, bool paused) {
  static cluster_view v;
  cluster_snapshot(ctl->c, &v);
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    if (v.nodes[id].exists && is_live(v.nodes[id].sm.state)) {
      (void)node_pause_purge(v.nodes[id].addr, paused);
    }
  }
}

/** Copy the keys of node id from the replicas that serve reads. Returns 0 or -1. */
static int repair(controller *ctl, int id) {
  driver_fault_point("REPAIR", id);
  // Writes that started before the node was REPAIRING did not go to it.
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
  xfer x = {v.ring, v.ring, readers};
  int rc = 0;
  for (int src = 1; src <= CLUSTER_MAX_NODES && rc == 0; src++) {
    if (readers[src]) {
      rc = ctl_move_rows(ctl, &x, src, v.nodes[src].addr, id, v.nodes[id].addr);
    }
  }
  ctl_pause_purge(ctl, false);
  return rc;
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
  if (ctl_spawn_ready(id, addr) != 0) {
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
    ctl_sleep_ms(ctl->cfg->probe_interval_ms);
  }
}
