#ifndef CP_NODE_CLIENT_H
#define CP_NODE_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config/config.h"
#include "vclock/vclock.h"

/**
 * The /_internal API of a storage node at addr ("ip:port"). One implementation is linked into
 * cp: node_client.c (HTTP); the controller tests link an in-memory fake. Calls block. Controller
 * thread only.
 */

typedef struct {
  const char *key;
  size_t key_len;
  const char *value;
  size_t value_len;
  vstamp stamp;
  /** A tombstone. */
  bool deleted;
} node_row;

/** Take the timeouts from cfg. */
void node_client_init(const cp_config *cfg);

/** Return whether the node answers its health check. */
bool node_health(const char *addr);

/** Make the committed rows of the node durable. Returns 0 or -1. */
int node_flush(const char *addr);

/** Gets each page of a dump; the rows are valid until it returns. Nonzero stops the dump. */
typedef int (*node_page_fn)(void *arg, const node_row *rows, size_t n);

/** Call fn with every committed row, tombstones too, in key order. Returns 0 or -1. */
int node_dump(const char *addr, node_page_fn fn, void *arg);

/**
 * Set hashes[i] to the node's digest of the ring segment that ends at ends[i] (increasing; see
 * ring_segments): equal entries give equal digests. Returns 0 or -1.
 */
int node_digest(const char *addr, const uint64_t *ends, size_t n, uint64_t *hashes);

/**
 * Call fn once with every entry, tombstones too, whose key position is in (start, end]. Returns 0
 * or -1.
 */
int node_range(const char *addr, uint64_t start, uint64_t end, node_page_fn fn, void *arg);

/** Write rows; each applies by the winner rule against the node's entry. Returns 0 or -1. */
int node_put_rows(const char *addr, const node_row *rows, size_t n);

/** Remove the keys of rows and their tombstones, whatever their clocks. Returns 0 or -1. */
int node_delete_keys(const char *addr, const node_row *rows, size_t n);

/**
 * Remove from the node the tombstones past its TTL in the segments i with purge[i], of the n
 * segments that end at ends (see node_digest). Returns 0 or -1.
 */
int node_purge(const char *addr, const uint64_t *ends, const bool *purge, size_t n);

#endif
