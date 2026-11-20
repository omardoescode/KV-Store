#include "cp/controller_internal.h"

#include <stdio.h>
#include <string.h>

static int spawn_initial(controller *ctl, int id, int *failures) {
  for (;;) {
    char raw[DRIVER_ADDR_LEN];
    char addr[DRIVER_ADDR_LEN];
    if (driver_spawn(id, raw) == 0 && ctl_resolve(raw, addr) == 0 && ctl_wait_healthy(ctl, addr)) {
      pthread_mutex_lock(&ctl->c->mu);
      ctl->c->nodes[id] = (cnode){.exists = true, .sm = {NODE_ACTIVE, 0, true}, .in_ring = true};
      memcpy(ctl->c->nodes[id].addr, addr, sizeof(addr));
      pthread_mutex_unlock(&ctl->c->mu);
      return ctl_save_node(ctl, id);
    }
    if (++*failures >= START_SPAWN_TRIES) {
      (void)fprintf(stderr, "cp: spawn of node %d failed %d times, exiting\n", id, *failures);
      return -1;
    }
    LOGF(ctl, LOG_WARN, "start: spawn of node %d failed, retrying", id);
    ctl_sleep_ms((int)(200U << (unsigned)*failures));
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
        running = ctl_resolve(live[j].addr, node->addr) == 0;
      }
    }
    if (!running || node->sm.state == NODE_SPAWNING) {
      node->sm = (node_sm){NODE_DEAD, 0, true};
    } else if (node->sm.state == NODE_DEAD) {
      node->sm.state = NODE_REPAIRING;
    }
    node->sm.rejoin = true;
    (void)ctl_save_node(ctl, s->id);
  }
  return n;
}

/** Adopt running nodes as the ring when cp has no state (cp.db was lost). */
static void adopt_nodes(controller *ctl, const driver_node *live, int n_live) {
  for (int i = 0; i != n_live; i++) {
    char addr[DRIVER_ADDR_LEN];
    if (ctl_resolve(live[i].addr, addr) == 0) {
      ctl->c->nodes[live[i].id] =
          (cnode){.exists = true, .sm = {NODE_ACTIVE, 0, true}, .in_ring = true};
      memcpy(ctl->c->nodes[live[i].id].addr, addr, sizeof(addr));
      (void)ctl_save_node(ctl, live[i].id);
    }
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
  ctl_restart_migration(ctl, mig);
  ctl_reserve_versions(ctl);
  return 0;
}
