#include "cp/sm.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

typedef struct {
  node_state from;
  int fails;
  bool rejoin;
  node_event ev;
  node_state to;
  int to_fails;
  unsigned actions;
} node_case;

// Probe fail limit 3 in all cases.
static const node_case node_cases[] = {
    {NODE_SPAWNING, 0, false, NODE_EV_SPAWN_OK, NODE_SYNCING, 0, 0},
    {NODE_SPAWNING, 0, true, NODE_EV_SPAWN_OK, NODE_REPAIRING, 0, NODE_ACT_REPAIR},
    {NODE_SPAWNING, 0, false, NODE_EV_SPAWN_FAIL, NODE_DEAD, 0, NODE_ACT_RESPAWN},
    {NODE_SPAWNING, 0, true, NODE_EV_SPAWN_FAIL, NODE_DEAD, 0, NODE_ACT_RESPAWN},
    {NODE_SYNCING, 0, false, NODE_EV_SYNC_DONE, NODE_ACTIVE, 0, 0},
    {NODE_SYNCING, 0, false, NODE_EV_PROBE_FAIL, NODE_SYNCING, 1, 0},
    {NODE_SYNCING, 2, false, NODE_EV_PROBE_FAIL, NODE_DEAD, 0, NODE_ACT_RESPAWN},
    {NODE_SYNCING, 2, false, NODE_EV_PROBE_OK, NODE_SYNCING, 0, 0},
    {NODE_ACTIVE, 0, false, NODE_EV_PROBE_OK, NODE_ACTIVE, 0, 0},
    {NODE_ACTIVE, 0, false, NODE_EV_PROBE_FAIL, NODE_SUSPECT, 1, 0},
    {NODE_ACTIVE, 0, false, NODE_EV_FORWARD_ERROR, NODE_SUSPECT, 1, 0},
    {NODE_SUSPECT, 1, false, NODE_EV_PROBE_OK, NODE_ACTIVE, 0, 0},
    {NODE_SUSPECT, 1, false, NODE_EV_PROBE_FAIL, NODE_SUSPECT, 2, 0},
    {NODE_SUSPECT, 2, false, NODE_EV_PROBE_FAIL, NODE_DEAD, 0, NODE_ACT_RESPAWN},
    {NODE_SUSPECT, 1, false, NODE_EV_FORWARD_ERROR, NODE_SUSPECT, 1, 0},
    {NODE_ACTIVE, 0, true, NODE_EV_MISSED_WRITE, NODE_REPAIRING, 0, 0},
    {NODE_SUSPECT, 2, false, NODE_EV_MISSED_WRITE, NODE_REPAIRING, 2, 0},
    {NODE_DEAD, 0, false, NODE_EV_RESPAWN, NODE_SPAWNING, 0, NODE_ACT_SPAWN},
    {NODE_REPAIRING, 0, true, NODE_EV_REPAIR_DONE, NODE_ACTIVE, 0, 0},
    {NODE_REPAIRING, 0, true, NODE_EV_PROBE_FAIL, NODE_REPAIRING, 1, 0},
    {NODE_REPAIRING, 2, true, NODE_EV_PROBE_FAIL, NODE_DEAD, 0, NODE_ACT_RESPAWN},
    {NODE_REPAIRING, 0, true, NODE_EV_REPAIR_FAIL, NODE_REPAIRING, 0, NODE_ACT_REPAIR},
    {NODE_REPAIRING, 2, true, NODE_EV_PROBE_OK, NODE_REPAIRING, 0, 0},
};

static const node_case *find_node_case(node_state from, node_event ev) {
  for (size_t i = 0; i != sizeof(node_cases) / sizeof(node_cases[0]); i++) {
    if (node_cases[i].from == from && node_cases[i].ev == ev) {
      return &node_cases[i];
    }
  }
  return NULL;
}

static void test_node_transitions(void) {
  for (size_t i = 0; i != sizeof(node_cases) / sizeof(node_cases[0]); i++) {
    const node_case *c = &node_cases[i];
    node_result r = node_step((node_sm){c->from, c->fails, c->rejoin}, c->ev, 3);
    assert(r.next.state == c->to);
    assert(r.next.fails == c->to_fails);
    assert(r.actions == c->actions);
  }
  printf("  test_node_transitions OK\n");
}

static void test_node_other_pairs_change_nothing(void) {
  for (int s = 0; s != NODE_STATE_COUNT; s++) {
    for (int e = 0; e != NODE_EVENT_COUNT; e++) {
      if (find_node_case((node_state)s, (node_event)e)) {
        continue;
      }
      node_sm cur = {(node_state)s, 1, true};
      node_result r = node_step(cur, (node_event)e, 3);
      assert(r.next.state == cur.state && r.next.fails == 1 && r.next.rejoin);
      assert(r.actions == 0);
    }
  }
  printf("  test_node_other_pairs_change_nothing OK\n");
}

static void test_node_dead_respawn_sets_rejoin(void) {
  node_result r = node_step((node_sm){NODE_DEAD, 0, false}, NODE_EV_RESPAWN, 3);
  assert(r.next.rejoin);
  r = node_step(r.next, NODE_EV_SPAWN_OK, 3);
  assert(r.next.state == NODE_REPAIRING);
  printf("  test_node_dead_respawn_sets_rejoin OK\n");
}

static void test_node_roles(void) {
  assert(node_serves_reads(NODE_ACTIVE) && node_serves_reads(NODE_SUSPECT));
  assert(!node_serves_reads(NODE_REPAIRING) && !node_serves_reads(NODE_SYNCING));
  assert(!node_serves_reads(NODE_DEAD) && !node_serves_reads(NODE_SPAWNING));
  assert(node_takes_writes(NODE_ACTIVE) && node_takes_writes(NODE_SUSPECT));
  assert(node_takes_writes(NODE_REPAIRING));
  assert(!node_takes_writes(NODE_SYNCING) && !node_takes_writes(NODE_DEAD));
  assert(!node_takes_writes(NODE_SPAWNING));
  printf("  test_node_roles OK\n");
}

typedef struct {
  mig_phase from;
  mig_event ev;
  mig_phase to;
  unsigned actions;
} mig_case;

static const mig_case mig_cases[] = {
    {MIG_IDLE, MIG_EV_START, MIG_SPAWN, MIG_ACT_SPAWN_TARGET},
    {MIG_SPAWN, MIG_EV_SPAWN_OK, MIG_COPY, MIG_ACT_COPY},
    {MIG_SPAWN, MIG_EV_NODE_DIED, MIG_ABORT, MIG_ACT_ABORT},
    {MIG_SPAWN, MIG_EV_RESTART, MIG_ABORT, MIG_ACT_ABORT},
    {MIG_COPY, MIG_EV_COPY_DONE, MIG_FLIP, MIG_ACT_FLIP},
    {MIG_COPY, MIG_EV_NODE_DIED, MIG_ABORT, MIG_ACT_ABORT},
    {MIG_COPY, MIG_EV_RESTART, MIG_ABORT, MIG_ACT_ABORT},
    {MIG_FLIP, MIG_EV_FLIP_DONE, MIG_DRAIN, MIG_ACT_DRAIN},
    {MIG_FLIP, MIG_EV_RESTART, MIG_FLIP, MIG_ACT_FLIP},
    {MIG_DRAIN, MIG_EV_DRAIN_DONE, MIG_PURGE, MIG_ACT_PURGE},
    {MIG_DRAIN, MIG_EV_RESTART, MIG_DRAIN, MIG_ACT_DRAIN},
    {MIG_PURGE, MIG_EV_PURGE_DONE, MIG_IDLE, 0},
    {MIG_PURGE, MIG_EV_PURGE_FAIL, MIG_PURGE, MIG_ACT_PURGE},
    {MIG_PURGE, MIG_EV_RESTART, MIG_PURGE, MIG_ACT_PURGE},
    {MIG_ABORT, MIG_EV_ABORT_DONE, MIG_IDLE, 0},
    {MIG_ABORT, MIG_EV_RESTART, MIG_ABORT, MIG_ACT_ABORT},
};

static const mig_case *find_mig_case(mig_phase from, mig_event ev) {
  for (size_t i = 0; i != sizeof(mig_cases) / sizeof(mig_cases[0]); i++) {
    if (mig_cases[i].from == from && mig_cases[i].ev == ev) {
      return &mig_cases[i];
    }
  }
  return NULL;
}

static void test_mig_transitions(void) {
  for (int p = 0; p != MIG_PHASE_COUNT; p++) {
    for (int e = 0; e != MIG_EVENT_COUNT; e++) {
      const mig_case *c = find_mig_case((mig_phase)p, (mig_event)e);
      mig_result r = mig_step((mig_phase)p, (mig_event)e);
      if (c) {
        assert(r.next == c->to);
        assert(r.actions == c->actions);
      } else {
        // Invalid events, and a dead node after FLIP: the phase goes on unchanged.
        assert(r.next == (mig_phase)p);
        assert(r.actions == 0);
      }
    }
  }
  printf("  test_mig_transitions OK\n");
}

static void test_mig_names(void) {
  assert(mig_phase_from_name("COPY") == MIG_COPY);
  assert(mig_phase_from_name("nope") == -1);
  for (int p = 0; p != MIG_PHASE_COUNT; p++) {
    assert(mig_phase_from_name(mig_phase_name((mig_phase)p)) == p);
  }
  assert(node_state_from_name("SUSPECT") == NODE_SUSPECT);
  for (int s = 0; s != NODE_STATE_COUNT; s++) {
    assert(node_state_from_name(node_state_name((node_state)s)) == s);
  }
  printf("  test_mig_names OK\n");
}

int main(void) {
  printf("cp state machine tests:\n");
  test_node_transitions();
  test_node_other_pairs_change_nothing();
  test_node_dead_respawn_sets_rejoin();
  test_node_roles();
  test_mig_transitions();
  test_mig_names();
  printf("all cp state machine tests passed\n");
  return 0;
}
