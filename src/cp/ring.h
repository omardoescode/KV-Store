#ifndef CP_RING_H
#define CP_RING_H

#include <stddef.h>

/**
 * A consistent-hash ring of node ids with vnodes points per node. A point of node id sits at
 * SipHash("<id>#<i>") under a fixed key; a key sits at SipHash(key). The ring depends only on
 * the set of ids and vnodes, so cp and tests compute the same owners.
 */
typedef struct ring ring;

typedef struct {
  /** Points per node. */
  int vnodes;
  /** Replicas per key: the length of a preference list. */
  int replicas;
} ring_params;

/** Build a ring of n ids (n may be 0). Returns NULL on allocation failure. */
ring *ring_new(const int *ids, size_t n, ring_params params);

void ring_free(ring *r);

/** Most replicas a ring holds per key. */
#define RING_MAX_REPLICAS 16

/**
 * Write the preference list of key to out (RING_MAX_REPLICAS ids): the first at most replicas
 * distinct node ids met walking clockwise from the key's position. out[0] is the primary.
 * Returns the count written.
 */
size_t ring_prefs(const ring *r, const char *key, size_t key_len, int *out);

/** Return whether id is in the preference list of key. */
int ring_has(const ring *r, int id, const char *key, size_t key_len);

#endif
