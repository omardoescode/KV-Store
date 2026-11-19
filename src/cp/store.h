#ifndef CP_STORE_H
#define CP_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cp/driver.h"
#include "cp/sm.h"

/** cp state in SQLite: nodes, the migration record and the version mark. Controller thread. */
typedef struct cp_store cp_store;

typedef struct {
  int id;
  char addr[DRIVER_ADDR_LEN];
  node_sm sm;
  /** The node owns keys: it is in the ring. */
  bool in_ring;
} stored_node;

typedef struct {
  mig_phase phase;
  /** The joining node, 0 if none. */
  int target;
} stored_migration;

cp_store *cp_store_open(const char *path);
void cp_store_close(cp_store *s);

/** Insert or replace node n. Returns 0 or -1. */
int cp_store_put_node(cp_store *s, const stored_node *n);
/** Remove node id. Returns 0 or -1. */
int cp_store_del_node(cp_store *s, int id);
/** Write up to max nodes, by id, to out. Returns the count, or -1. */
int cp_store_nodes(cp_store *s, stored_node *out, size_t max);

int cp_store_put_migration(cp_store *s, stored_migration m);
/** Read the migration record; IDLE with target 0 if none. Returns 0 or -1. */
int cp_store_migration(cp_store *s, stored_migration *out);

int cp_store_put_version_mark(cp_store *s, uint64_t mark);
/** Read the version mark, 0 if none. Returns 0 or -1. */
int cp_store_version_mark(cp_store *s, uint64_t *out);

#endif
