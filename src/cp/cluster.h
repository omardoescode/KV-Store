#ifndef CP_CLUSTER_H
#define CP_CLUSTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config/config.h"
#include "cp/driver.h"
#include "cp/ring.h"
#include "cp/sm.h"
#include "cp/store.h"
#include "log/log.h"

/**
 * The nodes, rings and join phase of cp. The router (event loop threads) plans requests with it;
 * the controller thread changes it. Each change is written to the store before it applies, so a
 * new cp starts from the last change.
 */
typedef struct cluster cluster;

enum { CLUSTER_MAX_NODES = 1024 };

typedef struct {
  int id;
  char addr[DRIVER_ADDR_LEN];
} plan_target;

/** Where one request goes. */
typedef struct {
  size_t count;
  /** Writes: every live replica, then the join target. Reads: replicas that serve reads. */
  plan_target targets[RING_MAX_REPLICAS + 1];
  /** Writes: how many of targets are replicas whose response counts; the target comes after. */
  size_t replicas;
  uint64_t version;
  uint64_t epoch;
} plan;

typedef struct {
  bool exists;
  char addr[DRIVER_ADDR_LEN];
  node_sm sm;
  bool in_ring;
  /** Forwards to this node, for its RPS. */
  uint64_t requests;
} cluster_node;

/** A copy of the cluster. */
typedef struct {
  /** Indexed by node id; ids start at 1. */
  cluster_node nodes[CLUSTER_MAX_NODES + 1];
  int count;
  mig_phase phase;
  int target;
  /** POST /_cp/nodes asked for a node. */
  bool add_requested;
  /**
   * The ring that owns keys; during a join before FLIP the ring with the target (next), from
   * FLIP to IDLE the ring before the join (prev). NULL if none. They stay valid until the
   * controller changes the cluster again; only the controller thread may read them.
   */
  const ring *ring;
  const ring *next;
  const ring *prev;
} cluster_view;

/** Read the version mark from store. Returns NULL on error. */
cluster *cluster_new(const cp_config *cfg, cp_store *store, logger log);
void cluster_free(cluster *c);

/** Plan a write of key: fails (-1) if no replica takes writes. Takes a ticket on success. */
int cluster_plan_write(cluster *c, const char *key, size_t key_len, plan *out);
/** Plan a read of key: fails (-1) if no replica serves reads. Takes a ticket on success. */
int cluster_plan_read(cluster *c, const char *key, size_t key_len, plan *out);
/** Return the ticket of a finished request. */
void cluster_done(cluster *c, uint64_t epoch);
/** Note a failed forward to node id; join_target: it was the double-write to the target. */
void cluster_forward_failed(cluster *c, int id, bool join_target);

/** POST /_cp/nodes: ask the controller for one more node. False at KV_CP_MAX_NODES. */
bool cluster_request_add(cluster *c);

void cluster_snapshot(cluster *c, cluster_view *out);

// The rest is for the controller thread only.

/**
 * Wait until every request that took a ticket before this call finishes. Returns 0, or -1
 * after timeout_ms.
 */
int cluster_drain(cluster *c, int timeout_ms);

/** Insert or replace node id. Not during a join. Returns 0 or -1. */
int cluster_put_node(cluster *c, int id, const char *addr, node_sm sm, bool in_ring);
/** Remove node id. Returns 0 or -1. */
int cluster_drop_node(cluster *c, int id);
/** Apply ev to node id and return the actions. */
unsigned cluster_node_event(cluster *c, int id, node_event ev);
/** A spawn of node id answers at addr: apply NODE_EV_SPAWN_OK and return the actions. */
unsigned cluster_spawned(cluster *c, int id, const char *addr);
/** Return and clear whether a forward to node id failed since the last call. */
bool cluster_take_forward_error(cluster *c, int id);
/** Move the stored version mark ahead when the versions in use come near it. */
void cluster_reserve_versions(cluster *c);

/** Set the join phase. IDLE drops the rings of the join. Returns 0 or -1. */
int cluster_set_phase(cluster *c, mig_phase phase, int target);
/** Choose the id of a new node, clear the add request and set phase SPAWN. Returns it, or -1. */
int cluster_begin_join(cluster *c);
/** COPY: writes of keys that move to target go to target too. Returns 0 or -1. */
int cluster_begin_copy(cluster *c, int target);
/** Return whether a write to the target failed since cluster_begin_copy. */
bool cluster_copy_lost_write(cluster *c);
/** FLIP: the ring with target owns keys; the ring before the join stays as prev. */
void cluster_flip(cluster *c, int target);
/** Stop writes to target; plans are as before the join. */
void cluster_abort_join(cluster *c, int target);

#endif
