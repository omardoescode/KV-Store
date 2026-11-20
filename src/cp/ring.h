#ifndef CP_RING_H
#define CP_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/** Ring positions in (start, end], wrapping past the top; start == end is the whole ring. */
typedef struct {
  uint64_t start;
  uint64_t end;
} ring_range;

/**
 * Write to *out the ranges where the preference list in from and to differ, *n of them. *out is
 * malloc'd (NULL if none) and the caller frees it. Returns 0, or -1 on allocation failure.
 */
int ring_diff(const ring *from, const ring *to, ring_range **out, size_t *n);

/** Return whether key falls in one of the n ranges. */
bool ring_ranges_have(const ring_range *ranges, size_t n, const char *key, size_t key_len);

/**
 * Which rows move when the ring changes from `from` to `to`. readers is indexed by node id: the
 * nodes that serve reads.
 */
typedef struct {
  const ring *from;
  const ring *to;
  const bool *readers;
} xfer;

/**
 * Return the one node that sends key to target: the first reader of key in from, other than
 * target. 0 if target does not hold key in to, or no reader has it.
 */
int xfer_source(const xfer *x, const char *key, size_t len, int target);

/** Return whether node held key in from but not in to, so it deletes key. */
bool xfer_drops(const xfer *x, int node, const char *key, size_t len);

#endif
