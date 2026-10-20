#ifndef HASHMAP_HASHMAP_H
#define HASHMAP_HASHMAP_H

#include <stddef.h>

// A thread-safe in-memory map from NUL-terminated string keys to arbitrary byte-buffer values.
// Keys and values are copied in on put; the map owns its storage until free. Values returned by
// get are freshly allocated copies that the caller must free.
//
// Note: FNV-1a has no seed, so this map is not DoS-hardened against adversarial key collisions
// over the network. A sip-hash swap is a reasonable future upgrade before exposing user input.
typedef struct hashmap hashmap;

/** Allocate an empty map; return NULL on allocation failure. Free with hashmap_free. */
hashmap *hashmap_new(void);
/** Free a map and all its entries. NULL is permitted. Not thread-safe against concurrent ops. */
void hashmap_free(hashmap *m);

/**
 * Insert or overwrite key -> val_len bytes of val. The map copies both; the caller retains the
 * originals and may free them after put returns. A val_len of 0 is allowed and val may be NULL
 * in that case.
 * Return 0 on success, -1 on bad args or allocation failure (map state unchanged on -1).
 */
int hashmap_put(hashmap *m, const char *key, const void *val, size_t val_len);

/**
 * Look up key. On found (return 1), set *out_val to a freshly malloc'd copy that the caller
 * must free, and set *out_len to the stored length. On absent (return 0), set *out_val to NULL
 * and *out_len to 0. Return -1 on bad args or allocation failure.
 * A zero-length value is returned as *out_val=NULL, *out_len=0 with return 1.
 */
int hashmap_get(const hashmap *m, const char *key, void **out_val, size_t *out_len);

/** Remove key. Return 1 if removed, 0 if absent, -1 on bad args. */
int hashmap_del(hashmap *m, const char *key);

/** Return the number of entries in m, or 0 if m is NULL. */
size_t hashmap_len(const hashmap *m);

#endif
