#ifndef TESTS_CP_FAKE_H
#define TESTS_CP_FAKE_H

#include <stdbool.h>
#include <stdint.h>

// Controls of the in-memory nodes behind tests/node_client_fake.c and tests/driver_fake.c.
// A node is known by its id; its data stays when it stops, like a volume.

void fake_reset(void);
/** Write key on node id with a version newer than any before. */
void fake_put(int id, const char *key, const char *value);
/** Write key on node id as the first write coordinated by node coord, with timestamp ts. */
void fake_put_stamp(int id, const char *key, const char *value, int coord, uint64_t ts);
/** Return the value of key on node id, or NULL if it has no live row. */
const char *fake_get(int id, const char *key);
/** The node stops answering; driver_spawn starts it again with its data. */
void fake_kill(int id);
bool fake_purge_paused(int id);
/** At the next driver_fault_point(phase, ...), kill node victim, or the target if victim is 0. */
void fake_fault(const char *phase, int victim);

/** Used by driver_fake.c. */
const char *fake_addr(int id);
void fake_start(int id);
bool fake_running(int id);

#endif
