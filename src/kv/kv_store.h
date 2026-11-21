#ifndef KV_STORE_H
#define KV_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "hashmap/hashmap.h"
#include "log/log.h"
#include "vclock/vclock.h"

typedef struct kv_store kv_store;

/** Called once by kv_store_flush: status 0 when committed, -1 when the store dropped changes. */
typedef void (*kv_flush_fn)(void *ctx, int status);

typedef struct {
  const char *db_path;
  /** >= 1. */
  int flush_interval_ms;
  /** >= 1. */
  size_t max_pending;
  /** >= 1. Seconds after its write (its stamp ts) that kv_store_purge may remove a tombstone. */
  long tombstone_ttl_s;
} kv_store_options;

/** One row given by kv_store_dump; the strings are valid only during the callback. */
typedef struct {
  const char *key;
  const void *value;
  size_t value_len;
  vstamp stamp;
  bool deleted;
} kv_dump_row;

/** The entry of one key, a tombstone too, given by kv_store_read. */
typedef struct {
  /** malloc'd, NULL if empty; the caller frees it. */
  void *value;
  size_t value_len;
  vstamp stamp;
  bool deleted;
} kv_record;

/** Called per row by kv_store_dump; nonzero stops the dump with an error. */
typedef int (*kv_dump_fn)(void *ctx, const kv_dump_row *row);

/**
 * Load every row of the SQLite database at opt->db_path into memory, upgrading an older database
 * (its version column becomes the ts of an empty clock), then start a writer thread that commits
 * queued changes every flush_interval_ms. The writer thread reports failed commits to log; it keeps
 * the changes and retries them, but drops a change that cannot commit whatever the database state.
 * Put and delete fail while max_pending changes wait for the database. Returns NULL on error.
 */
kv_store *kv_store_new(const kv_store_options *opt, logger log);

/**
 * Stop the writer thread after it commits the queued changes, then free the store. Returns 0, or
 * -1 if the writer dropped changes it could not commit.
 */
int kv_store_free(kv_store *store);

/**
 * Copy key's value into a new buffer the caller must free. Returns 1 on found, 0 on missing or
 * deleted, -1 on error.
 */
int kv_store_get(kv_store *store, const char *key, void **out_value, size_t *out_len);

/**
 * Read key's entry, a tombstone too, into *out. Returns 1 on found, 0 on missing, -1 on error.
 */
int kv_store_read(kv_store *store, const char *key, kv_record *out);

/**
 * Store a copy of value under the key with stamp (NULL: the empty clock, ts 0, coord 0) and queue
 * it for the database, by the winner rule of vstamp_pick against the stored entry: the stored
 * entry keeps the winner, with the merge of both clocks. Does no IO.
 * Returns 0 on success or ignore, -1 on error, -2 if max_pending changes wait (the map is
 * unchanged).
 */
int kv_store_put(kv_store *store, const char *key, const void *value, size_t value_len,
                 const vstamp *stamp);

/**
 * Replace the key with a tombstone of stamp like kv_store_put. Does no IO.
 * Returns 1 if a value was removed or the stored entry won, 0 if no value was there, -1 on
 * error, -2 if max_pending changes wait (the map is unchanged).
 */
int kv_store_del(kv_store *store, const char *key, const vstamp *stamp);

/**
 * Remove key and its tombstone, with no clock check, and queue the removal. Does no IO.
 * Returns 1 if removed, 0 if missing, -1 on error, -2 if max_pending changes wait.
 */
int kv_store_remove(kv_store *store, const char *key);

/**
 * Call fn for up to limit committed rows, tombstones too, with key > after, in key order.
 * Reads the database, so a change shows only after its commit (see kv_store_flush).
 * Returns the row count, or -1 on error.
 */
int kv_store_dump(kv_store *store, const char *after, size_t limit, kv_dump_fn fn, void *ctx);

/**
 * For the n ring segments that end at the increasing positions ends (segment i holds the key
 * positions in (ends[i - 1], ends[i]], segment 0 wrapping past the top), set hashes[i] to the XOR
 * of a hash of each entry's key, stamp and deleted flag, tombstones too. Equal entries give equal
 * hashes on every node. Reads the map, so changes not committed yet count. Returns 0 or -1.
 */
int kv_store_digest(kv_store *store, const uint64_t *ends, size_t n, uint64_t *hashes);

/**
 * Call fn for each entry, tombstones too, whose key position is in (start, end], wrapping past
 * the top; start == end is the whole ring. Reads the map; fn must not call kv_store. Returns the
 * entry count, or -1 on error.
 */
int kv_store_range(kv_store *store, uint64_t start, uint64_t end, kv_dump_fn fn, void *ctx);

/**
 * Remove the tombstones whose stamp ts is older than the TTL from the segments i with purge[i], of
 * the n segments that end at ends (see kv_store_digest), and queue the removals. cp calls it once
 * every home replica of a segment holds the same entries, so no replica brings a purged key back.
 * Does no IO. Returns the count removed, or -1 on error.
 */
int kv_store_purge(kv_store *store, const uint64_t *ends, const bool *purge, size_t n);

/**
 * Call fn(ctx, status) once every change queued before this call is committed: at once on the
 * calling thread if none waits, else on the writer thread. now asks for a commit before the flush
 * interval ends. Returns 0, or -1 (fn not called) on error or after kv_store_free started.
 */
int kv_store_flush(kv_store *store, bool now, kv_flush_fn fn, void *ctx);

#endif /* KV_STORE_H */
