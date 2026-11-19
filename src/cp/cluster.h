#ifndef CP_CLUSTER_H
#define CP_CLUSTER_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config/config.h"
#include "cp/driver.h"
#include "cp/ring.h"
#include "cp/sm.h"

/**
 * The view of the cluster that the router (event loop threads) and the controller thread share.
 * All fields are guarded by mu. The router reads it to plan each request; the controller
 * changes it.
 */

enum { CLUSTER_MAX_NODES = 1024 };

typedef struct {
  bool exists;
  char addr[DRIVER_ADDR_LEN];
  node_sm sm;
  bool in_ring;
  /** The router saw a forward to this node fail since the controller last looked. */
  bool forward_error;
  /** Forwards to this node, for its RPS. */
  uint64_t requests;
} cnode;

typedef struct {
  pthread_mutex_t mu;
  const cp_config *cfg;
  /** Indexed by node id; ids start at 1. */
  cnode nodes[CLUSTER_MAX_NODES + 1];
  /** The ring that owns keys now. */
  ring *ring;
  /** During a join before FLIP: the ring with the target. */
  ring *next;
  /** During a join from FLIP: the ring before the join, for PURGE. */
  ring *prev;
  mig_phase phase;
  int target;
  /** COPY: writes of keys that move to the target also go to the target. */
  bool double_write;
  /** A write to the target failed during COPY: the join must abort. */
  bool double_write_failed;
  uint64_t last_version;
  uint64_t version_mark;
  /** Requests take a ticket of the current epoch; a drain waits for older tickets. */
  uint64_t epoch;
  long inflight[2];
  /** POST /_cp/nodes asked for a node. */
  bool add_requested;
} cluster;

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

int cluster_init(cluster *c, const cp_config *cfg);
void cluster_free(cluster *c);

/** Plan a write of key: fails (-1) if no replica takes writes. Takes a ticket on success. */
int cluster_plan_write(cluster *c, const char *key, size_t key_len, plan *out);
/** Plan a read of key: fails (-1) if no replica serves reads. Takes a ticket on success. */
int cluster_plan_read(cluster *c, const char *key, size_t key_len, plan *out);
/** Return the ticket of a finished request. */
void cluster_done(cluster *c, uint64_t epoch);
/** Note a failed forward to node id; join_target: it was the double-write to the target. */
void cluster_forward_failed(cluster *c, int id, bool join_target);

/**
 * Wait until every request that took a ticket before this call finishes. Controller only.
 * Returns 0, or -1 after timeout_ms.
 */
int cluster_drain(cluster *c, int timeout_ms);

/** Build a ring of in-ring nodes, plus extra if not 0. Call with mu held. NULL on error. */
ring *cluster_build_ring(cluster *c, int extra);

#endif
