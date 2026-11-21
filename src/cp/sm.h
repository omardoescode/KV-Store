#ifndef CP_SM_H
#define CP_SM_H

#include <stdbool.h>

// Pure state machines of cp: (state, event) -> (state, actions). No I/O; the controller runs
// the actions and feeds back their results as events.

typedef enum {
  NODE_SPAWNING,
  NODE_SYNCING,
  NODE_ACTIVE,
  NODE_SUSPECT,
  NODE_DEAD,
  NODE_REPAIRING,
  NODE_STATE_COUNT,
} node_state;

typedef enum {
  NODE_EV_SPAWN_OK,
  NODE_EV_SPAWN_FAIL,
  NODE_EV_SYNC_DONE,
  NODE_EV_PROBE_OK,
  NODE_EV_PROBE_FAIL,
  NODE_EV_FORWARD_ERROR,
  NODE_EV_RESPAWN,
  NODE_EV_REPAIR_DONE,
  NODE_EV_REPAIR_FAIL,
  NODE_EVENT_COUNT,
} node_event;

#define NODE_ACT_SPAWN 0x1U
#define NODE_ACT_RESPAWN 0x2U
#define NODE_ACT_REPAIR 0x4U

typedef struct {
  node_state state;
  /** Failed probes in a row. */
  int fails;
  /** The node was spawned before: a spawn brings back its volume, which needs a repair. */
  bool rejoin;
} node_sm;

typedef struct {
  node_sm next;
  unsigned actions;
} node_result;

/**
 * Apply ev to cur. probe_fails failed probes in a row make a node DEAD. An event that does not
 * apply to the state changes nothing.
 */
node_result node_step(node_sm cur, node_event ev, int probe_fails);

/** Return whether a node in state s may answer reads. */
bool node_serves_reads(node_state s);
/** Return whether a node in state s gets every write of its keys. */
bool node_takes_writes(node_state s);

const char *node_state_name(node_state s);
/** Return the state named name, or -1. */
int node_state_from_name(const char *name);

/**
 * Phases of a join (one new node). Before FLIP a failure aborts; from FLIP the join finishes.
 * COPY: cp double-writes moving keys to the target and copies the older rows to it.
 */
typedef enum {
  MIG_IDLE,
  MIG_SPAWN,
  MIG_COPY,
  MIG_FLIP,
  MIG_DRAIN,
  MIG_PURGE,
  MIG_ABORT,
  MIG_PHASE_COUNT,
} mig_phase;

typedef enum {
  MIG_EV_START,
  MIG_EV_SPAWN_OK,
  MIG_EV_COPY_DONE,
  MIG_EV_FLIP_DONE,
  MIG_EV_DRAIN_DONE,
  MIG_EV_PURGE_DONE,
  MIG_EV_PURGE_FAIL,
  /** The target, a source or a replica died; a failed spawn counts too. */
  MIG_EV_NODE_DIED,
  MIG_EV_ABORT_DONE,
  /** cp started with this phase in its state store. */
  MIG_EV_RESTART,
  MIG_EVENT_COUNT,
} mig_event;

#define MIG_ACT_SPAWN_TARGET 0x1U
#define MIG_ACT_COPY 0x2U
#define MIG_ACT_FLIP 0x4U
#define MIG_ACT_DRAIN 0x8U
#define MIG_ACT_PURGE 0x10U
#define MIG_ACT_ABORT 0x20U

typedef struct {
  mig_phase next;
  unsigned actions;
} mig_result;

mig_result mig_step(mig_phase cur, mig_event ev);

const char *mig_phase_name(mig_phase p);
/** Return the phase named name, or -1. */
int mig_phase_from_name(const char *name);

#endif
