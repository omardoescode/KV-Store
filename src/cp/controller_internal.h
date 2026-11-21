#ifndef CP_CONTROLLER_INTERNAL_H
#define CP_CONTROLLER_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "cp/controller.h"

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
  int64_t last_anti_entropy_ms;
  int64_t window_start_ms;
  uint64_t window_requests[CLUSTER_MAX_NODES + 1];
};

#define LOGF(ctl, level, ...) (ctl)->log.log((ctl)->log.ctx, (level), __VA_ARGS__)

int64_t ctl_mono_ms(void);

/**
 * Anti-entropy: compare a digest of each ring segment on its live home replicas and copy the
 * winner of each key that differs to the replicas without it, tombstones too. Then purge the
 * tombstones of the segments whose home replicas all agree. Returns 0 or -1.
 */
int ctl_anti_entropy(controller *ctl);
/** Spawn node id, seen DEAD, again with its data and repair it; or repair it, seen REPAIRING. */
void ctl_heal(controller *ctl, int id, node_state state);
/** Add one node. asked: POST /_cp/nodes asked for it, so an aborted join asks again. */
void ctl_join(controller *ctl, bool asked);
/** Run the phases from `from` (FLIP, DRAIN or PURGE) to IDLE. */
void ctl_finish_join(controller *ctl, mig_phase from, int target);

#endif
