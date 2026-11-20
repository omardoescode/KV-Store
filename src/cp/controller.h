#ifndef CP_CONTROLLER_H
#define CP_CONTROLLER_H

#include "cp/cluster.h"
#include "cp/store.h"
#include "log/log.h"

/**
 * The controller thread of cp: probes nodes, spawns dead ones again and repairs them, runs one
 * join at a time (autoscale or POST /_cp/nodes), and writes every transition to the store before
 * its action.
 */
typedef struct controller controller;

/** store is the one c writes to; cfg and both must outlive the controller. */
controller *controller_new(cluster *c, cp_store *store, const cp_config *cfg, logger log);

/**
 * Rebuild the cluster from the store and the driver, spawn the initial nodes of an empty
 * cluster, and continue or abort a migration left by an earlier cp. Returns 0, or -1 after 5
 * failed spawns or a store error.
 */
int controller_start_up(controller *ctl);

/** Start the controller thread. Returns 0 or -1. */
int controller_run(controller *ctl);

/** Stop and join the controller thread, then free ctl. */
void controller_free(controller *ctl);

#endif
