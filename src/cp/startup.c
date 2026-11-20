#include "cp/controller_internal.h"

#include <stdio.h>
#include <string.h>

static int spawn_initial(controller *ctl, int id, int *failures) {
  for (;;) {
    char raw[DRIVER_ADDR_LEN];
    char addr[DRIVER_ADDR_LEN];
    if (driver_spawn(id, raw) == 0 && ctl_resolve(raw, addr) == 0 && ctl_wait_healthy(ctl, addr)) {
      return cluster_put_node(ctl->c, id, addr, (node_sm){NODE_ACTIVE, 0, true}, true);
    }
    if (++*failures >= START_SPAWN_TRIES) {
      (void)fprintf(stderr, "cp: spawn of node %d failed %d times, exiting\n", id, *failures);
      return -1;
    }
    LOGF(ctl, LOG_WARN, "start: spawn of node %d failed, retrying", id);
    ctl_sleep_ms((int)(200U << (unsigned)*failures));
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
        running = ctl_resolve(live[j].addr, s.addr) == 0;
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
    char addr[DRIVER_ADDR_LEN];
    if (ctl_resolve(live[i].addr, addr) == 0) {
      (void)cluster_put_node(ctl->c, live[i].id, addr, (node_sm){NODE_ACTIVE, 0, true}, true);
    }
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
  ctl_restart_migration(ctl, mig);
  cluster_reserve_versions(ctl->c);
  return 0;
}
