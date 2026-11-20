#include "cp/controller_internal.h"

#include "cp/node_client.h"

#include <stdio.h>
#include <string.h>

enum { START_SPAWN_TRIES = 5, BATCH_ROWS = 500 };

static int spawn_initial(controller *ctl, int id, int *failures) {
  for (;;) {
    char addr[DRIVER_ADDR_LEN];
    if (ctl_spawn_ready(id, addr) == 0) {
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

typedef struct {
  const cluster_view *v;
  int src;
} spread;

static int spread_page(void *arg, const node_row *rows, size_t n) {
  const spread *s = arg;
  static node_row batch[BATCH_ROWS];
  for (int dst = 1; dst <= CLUSTER_MAX_NODES; dst++) {
    if (dst == s->src || !s->v->nodes[dst].exists ||
        !node_serves_reads(s->v->nodes[dst].sm.state)) {
      continue;
    }
    size_t count = 0;
    for (size_t i = 0; i != n; i++) {
      if (ring_has(s->v->ring, dst, rows[i].key, rows[i].key_len)) {
        batch[count++] = rows[i];
      }
      if (count == BATCH_ROWS || (count != 0 && i + 1 == n)) {
        if (node_put_rows(s->v->nodes[dst].addr, batch, count) != 0) {
          return -1;
        }
        count = 0;
      }
    }
  }
  return 0;
}

/**
 * A cp that crashed during a write leaves it on some replicas only. Send every row to the other
 * replicas of its key; the newest version wins, so the replicas agree again.
 */
static void reconcile(controller *ctl) {
  LOGF(ctl, LOG_INFO, "start: the last cp did not stop cleanly, reconciling replicas");
  ctl_pause_purge(ctl, true);
  static cluster_view v;
  cluster_snapshot(ctl->c, &v);
  for (int src = 1; src <= CLUSTER_MAX_NODES; src++) {
    spread s = {&v, src};
    if (v.nodes[src].exists && node_serves_reads(v.nodes[src].sm.state) &&
        (node_flush(v.nodes[src].addr) != 0 ||
         node_dump(v.nodes[src].addr, spread_page, &s) != 0)) {
      LOGF(ctl, LOG_WARN, "start: reconcile from node %d failed", src);
    }
  }
  ctl_pause_purge(ctl, false);
}

int controller_start_up(controller *ctl) {
  static stored_node stored[CLUSTER_MAX_NODES];
  static driver_node live[CLUSTER_MAX_NODES];
  int n = cp_store_nodes(ctl->store, stored, CLUSTER_MAX_NODES);
  int n_live = driver_list(live, CLUSTER_MAX_NODES);
  stored_migration mig;
  bool crashed = false;
  if (n < 0 || n_live < 0 || cp_store_migration(ctl->store, &mig) != 0 ||
      cp_store_running(ctl->store, &crashed) != 0 || cp_store_put_running(ctl->store, true) != 0) {
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
  if (crashed) {
    reconcile(ctl);
  }
  ctl->up = true;
  return 0;
}
