#ifndef CP_DRIVER_H
#define CP_DRIVER_H

#include <stddef.h>

#include "config/config.h"
#include "log/log.h"

/**
 * Starts and stops storage nodes. One implementation is linked into cp: driver_docker.c
 * (containers through the Docker API) or driver_process.c (local processes, for tests).
 * Called from the controller thread only.
 */

enum { DRIVER_ADDR_LEN = 64 };

typedef struct {
  int id;
  /** "ip:port" that cp forwards to. */
  char addr[DRIVER_ADDR_LEN];
} driver_node;

/** Prepare the driver. Returns 0, or -1 after logging why. */
int driver_init(const cp_config *cfg, logger log);

/**
 * Start node id, or start it again with its old data, and write its "ip:port" to addr. Returns
 * once the node runs (it may not answer yet), 0 or -1.
 */
int driver_spawn(int id, char addr[DRIVER_ADDR_LEN]);

/** Stop node id and remove it; its data stays. Returns 0 or -1. */
int driver_stop(int id);

/** Write up to max running nodes to out. Returns the count, or -1. */
int driver_list(driver_node *out, size_t max);

/**
 * Fault hook for tests: called when phase starts (a migration or node phase name); target is
 * the migration target or the repaired node, or 0. Only the process driver acts on it.
 */
void driver_fault_point(const char *phase, int target);

#endif
