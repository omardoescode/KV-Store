#include "cp/sm.h"

#include <string.h>

static const char *const node_names[] = {
    "SPAWNING", "SYNCING", "ACTIVE", "SUSPECT", "DEAD", "REPAIRING",
};

static const char *const mig_names[] = {
    "IDLE", "SPAWN", "COPY", "FLIP", "DRAIN", "PURGE", "ABORT",
};

/** Count a failed probe; at the limit the node is DEAD and must be spawned again. */
static node_result probe_failed(node_sm cur, int probe_fails) {
  cur.fails++;
  if (cur.fails >= probe_fails) {
    return (node_result){{NODE_DEAD, 0, cur.rejoin}, NODE_ACT_RESPAWN};
  }
  return (node_result){cur, 0};
}

static node_result node_unchanged(node_sm cur) { return (node_result){cur, 0}; }

static node_result step_spawning(node_sm cur, node_event ev) {
  switch (ev) {
  case NODE_EV_SPAWN_OK:
    if (cur.rejoin) {
      return (node_result){{NODE_REPAIRING, 0, true}, NODE_ACT_REPAIR};
    }
    return (node_result){{NODE_SYNCING, 0, false}, 0};
  case NODE_EV_SPAWN_FAIL:
    return (node_result){{NODE_DEAD, 0, cur.rejoin}, NODE_ACT_RESPAWN};
  default:
    return node_unchanged(cur);
  }
}

static node_result step_live(int probe_fails, node_sm cur, node_event ev) {
  switch (ev) {
  case NODE_EV_PROBE_OK:
    cur.fails = 0;
    if (cur.state == NODE_SUSPECT) {
      cur.state = NODE_ACTIVE;
    }
    return (node_result){cur, 0};
  case NODE_EV_PROBE_FAIL:
    if (cur.state == NODE_ACTIVE) {
      cur.state = NODE_SUSPECT;
    }
    return probe_failed(cur, probe_fails);
  case NODE_EV_FORWARD_ERROR:
    if (cur.state != NODE_ACTIVE) {
      return node_unchanged(cur);
    }
    cur.state = NODE_SUSPECT;
    return probe_failed(cur, probe_fails);
  case NODE_EV_SYNC_DONE:
    if (cur.state != NODE_SYNCING) {
      return node_unchanged(cur);
    }
    return (node_result){{NODE_ACTIVE, 0, cur.rejoin}, 0};
  case NODE_EV_REPAIR_DONE:
    if (cur.state != NODE_REPAIRING) {
      return node_unchanged(cur);
    }
    return (node_result){{NODE_ACTIVE, 0, cur.rejoin}, 0};
  case NODE_EV_REPAIR_FAIL:
    if (cur.state != NODE_REPAIRING) {
      return node_unchanged(cur);
    }
    return (node_result){cur, NODE_ACT_REPAIR};
  default:
    return node_unchanged(cur);
  }
}

node_result node_step(node_sm cur, node_event ev, int probe_fails) {
  switch (cur.state) {
  case NODE_SPAWNING:
    return step_spawning(cur, ev);
  case NODE_DEAD:
    if (ev == NODE_EV_RESPAWN) {
      return (node_result){{NODE_SPAWNING, 0, true}, NODE_ACT_SPAWN};
    }
    return node_unchanged(cur);
  case NODE_SYNCING:
  case NODE_ACTIVE:
  case NODE_SUSPECT:
  case NODE_REPAIRING:
    return step_live(probe_fails, cur, ev);
  default:
    return node_unchanged(cur);
  }
}

bool node_serves_reads(node_state s) { return (bool)(s == NODE_ACTIVE || s == NODE_SUSPECT); }

bool node_takes_writes(node_state s) {
  return (bool)(s == NODE_ACTIVE || s == NODE_SUSPECT || s == NODE_REPAIRING);
}

const char *node_state_name(node_state s) {
  return (unsigned)s < NODE_STATE_COUNT ? node_names[s] : "?";
}

int node_state_from_name(const char *name) {
  for (int i = 0; i != NODE_STATE_COUNT; i++) {
    if (strcmp(node_names[i], name) == 0) {
      return i;
    }
  }
  return -1;
}

/** The phase that follows cur when its work is done, with the actions of that phase. */
static mig_result advance(mig_phase cur) {
  switch (cur) {
  case MIG_SPAWN:
    return (mig_result){MIG_COPY, MIG_ACT_COPY};
  case MIG_COPY:
    return (mig_result){MIG_FLIP, MIG_ACT_FLIP};
  case MIG_FLIP:
    return (mig_result){MIG_DRAIN, MIG_ACT_DRAIN};
  case MIG_DRAIN:
    return (mig_result){MIG_PURGE, MIG_ACT_PURGE};
  default:
    return (mig_result){MIG_IDLE, 0};
  }
}

/** The action that does the work of phase p, run again after a cp restart or a failure. */
static unsigned work_of(mig_phase p) {
  static const unsigned work[] = {
      [MIG_FLIP] = MIG_ACT_FLIP,
      [MIG_DRAIN] = MIG_ACT_DRAIN,
      [MIG_PURGE] = MIG_ACT_PURGE,
      [MIG_ABORT] = MIG_ACT_ABORT,
  };
  return work[p];
}

mig_result mig_step(mig_phase cur, mig_event ev) {
  mig_result same = {cur, 0};
  bool before_flip = (bool)(cur == MIG_SPAWN || cur == MIG_COPY);
  switch (ev) {
  case MIG_EV_START:
    return cur == MIG_IDLE ? (mig_result){MIG_SPAWN, MIG_ACT_SPAWN_TARGET} : same;
  case MIG_EV_SPAWN_OK:
    return cur == MIG_SPAWN ? advance(cur) : same;
  case MIG_EV_COPY_DONE:
    return cur == MIG_COPY ? advance(cur) : same;
  case MIG_EV_FLIP_DONE:
    return cur == MIG_FLIP ? advance(cur) : same;
  case MIG_EV_DRAIN_DONE:
    return cur == MIG_DRAIN ? advance(cur) : same;
  case MIG_EV_PURGE_DONE:
    return cur == MIG_PURGE ? advance(cur) : same;
  case MIG_EV_PURGE_FAIL:
    return cur == MIG_PURGE ? (mig_result){MIG_PURGE, MIG_ACT_PURGE} : same;
  case MIG_EV_NODE_DIED:
    return before_flip ? (mig_result){MIG_ABORT, MIG_ACT_ABORT} : same;
  case MIG_EV_ABORT_DONE:
    return cur == MIG_ABORT ? (mig_result){MIG_IDLE, 0} : same;
  case MIG_EV_RESTART:
    if (before_flip) {
      return (mig_result){MIG_ABORT, MIG_ACT_ABORT};
    }
    return cur == MIG_IDLE ? same : (mig_result){cur, work_of(cur)};
  default:
    return same;
  }
}

const char *mig_phase_name(mig_phase p) {
  return (unsigned)p < MIG_PHASE_COUNT ? mig_names[p] : "?";
}

int mig_phase_from_name(const char *name) {
  for (int i = 0; i != MIG_PHASE_COUNT; i++) {
    if (strcmp(mig_names[i], name) == 0) {
      return i;
    }
  }
  return -1;
}
