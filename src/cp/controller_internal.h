#ifndef CP_CONTROLLER_INTERNAL_H
#define CP_CONTROLLER_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "cp/controller.h"
#include "cp/driver.h"
#include "cp/hclient.h"

enum {
  BATCH_ROWS = 500,
  DUMP_PAGE = 1000,
  SPAWN_WAIT_MS = 15000,
  DRAIN_TIMEOUT_MS = 30000,
  START_SPAWN_TRIES = 5,
  // Versions this far ahead of the clock are reserved in the store, in ms.
  VERSION_RESERVE_MS = 60000,
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

typedef struct {
  int id;
  char addr[DRIVER_ADDR_LEN];
} node_ref;

int64_t ctl_mono_ms(void);
void ctl_sleep_ms(int ms);

/** Send a request to a node. Returns the status, or -1. Frees the body unless out is given. */
int ctl_call(const char *addr, const hclient_req *req, hclient_resp *out);
bool ctl_probe_health(controller *ctl, const char *addr);
/** Probe addr until it answers or SPAWN_WAIT_MS passes. */
bool ctl_wait_healthy(controller *ctl, const char *addr);
/** Resolve the host of "host:port" to a numeric "ip:port" in out. Returns 0 or -1. */
int ctl_resolve(const char *in, char out[DRIVER_ADDR_LEN]);

int ctl_save_node(controller *ctl, int id);
/** Apply ev to node id, persist a change, and return the actions. */
unsigned ctl_node_event(controller *ctl, int id, node_event ev);
int ctl_save_migration(controller *ctl, mig_phase phase, int target);
/** Copy the nodes for which want(state) holds into out (CLUSTER_MAX_NODES). Returns the count. */
size_t ctl_snapshot(controller *ctl, bool (*want)(node_state), node_ref *out);
/** Pause or resume the tombstone purge on every live node. */
void ctl_set_purge_all(controller *ctl, bool paused);
/** Move the stored version mark ahead when the versions in use come near it. */
void ctl_reserve_versions(controller *ctl);

/** Decides per dumped row whether it goes to the destination. */
typedef bool (*row_filter)(controller *ctl, const char *key, size_t key_len, void *arg);

/**
 * Flush src, dump all its rows, and send those filter keeps to dst at path (/_internal/batch
 * sends rows, /_internal/delete-batch sends keys). Returns 0 or -1.
 */
int ctl_copy_rows(controller *ctl, const char *src, row_filter filter, void *arg, const char *dst,
                  const char *path);
/** Return the first node of key's preference list in r that serves reads and is not skip. */
int ctl_first_reader(controller *ctl, const ring *r, const char *key, size_t key_len, int skip);
/** Copy the keys of node id from the replicas that serve reads. Returns 0 or -1. */
int ctl_repair(controller *ctl, int id);
/** Spawn node id again with its data and repair it. */
void ctl_respawn(controller *ctl, int id);

/** Add one node. asked: POST /_cp/nodes asked for it, so an aborted join asks again. */
void ctl_join(controller *ctl, bool asked);
/** Run the phases from `from` (FLIP, DRAIN or PURGE) to IDLE. */
void ctl_finish_join(controller *ctl, mig_phase from, int target);
/** Abort or finish a join that an earlier cp left in phase mig. */
void ctl_restart_migration(controller *ctl, stored_migration mig);

#endif
