#ifndef CP_CONTROLLER_INTERNAL_H
#define CP_CONTROLLER_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "cp/controller.h"
#include "cp/driver.h"

enum {
  SPAWN_WAIT_MS = 15000,
  DRAIN_TIMEOUT_MS = 30000,
  START_SPAWN_TRIES = 5,
};

struct controller {
  cluster *c;
  const cp_config *cfg;
  cp_store *store;
  logger log;
  pthread_t thread;
  pthread_mutex_t mu;
  pthread_cond_t wake;
  bool stop;
  bool running;
  int64_t last_scale_ms;
  int64_t window_start_ms;
  uint64_t window_requests[CLUSTER_MAX_NODES + 1];
};

#define LOGF(ctl, level, ...) (ctl)->log.log((ctl)->log.ctx, (level), __VA_ARGS__)

int64_t ctl_mono_ms(void);
void ctl_sleep_ms(int ms);

/** Probe addr until it answers or SPAWN_WAIT_MS passes. */
bool ctl_wait_healthy(const char *addr);
/** Resolve the host of "host:port" to a numeric "ip:port" in out. Returns 0 or -1. */
int ctl_resolve(const char *in, char out[DRIVER_ADDR_LEN]);

/** Pause or resume the tombstone purge on every live node. */
void ctl_pause_purge(controller *ctl, bool paused);

/**
 * Move the rows of node (at addr) that x moves: with target, flush node and send to dst the rows
 * that node is the source of; with target 0, delete from node the keys it drops. Returns 0 or -1.
 */
int ctl_move_rows(controller *ctl, const xfer *x, int node, const char *addr, int target,
                  const char *dst);
/** Spawn node id, seen DEAD, again with its data and repair it; or repair it, seen REPAIRING. */
void ctl_heal(controller *ctl, int id, node_state state);

/** Add one node. asked: POST /_cp/nodes asked for it, so an aborted join asks again. */
void ctl_join(controller *ctl, bool asked);
/** Run the phases from `from` (FLIP, DRAIN or PURGE) to IDLE. */
void ctl_finish_join(controller *ctl, mig_phase from, int target);
/** Abort or finish a join that an earlier cp left in phase mig. */
void ctl_restart_migration(controller *ctl, stored_migration mig);

#endif
