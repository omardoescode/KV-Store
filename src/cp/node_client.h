#ifndef CP_NODE_CLIENT_H
#define CP_NODE_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config/config.h"

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
  int64_t version;
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

/** Write rows; a row older than the node's version of its key changes nothing. Returns 0 or -1. */
int node_put_rows(const char *addr, const node_row *rows, size_t n);

/** Remove the keys of rows and their tombstones, whatever their versions. Returns 0 or -1. */
int node_delete_keys(const char *addr, const node_row *rows, size_t n);

/** Pause or resume the tombstone purge of the node. Returns 0 or -1. */
int node_pause_purge(const char *addr, bool paused);

#endif
