#include "cp/store.h"

#include <inttypes.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cp_store {
  sqlite3 *db;
};

cp_store *cp_store_open(const char *path) {
  cp_store *s = calloc(1, sizeof(*s));
  if (!s) {
    return NULL;
  }
  if (sqlite3_open(path, &s->db) != SQLITE_OK ||
      sqlite3_exec(s->db,
                   "PRAGMA journal_mode=WAL;"
                   "PRAGMA synchronous=FULL;"
                   "CREATE TABLE IF NOT EXISTS nodes(id INTEGER PRIMARY KEY, addr TEXT NOT NULL,"
                   " state TEXT NOT NULL, fails INTEGER NOT NULL, rejoin INTEGER NOT NULL,"
                   " in_ring INTEGER NOT NULL);"
                   "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);",
                   NULL, NULL, NULL) != SQLITE_OK) {
    (void)fprintf(stderr, "cp_store: could not open %s: %s\n", path,
                  s->db ? sqlite3_errmsg(s->db) : "out of memory");
    sqlite3_close(s->db);
    free(s);
    return NULL;
  }
  return s;
}

void cp_store_close(cp_store *s) {
  if (s) {
    sqlite3_close(s->db);
    free(s);
  }
}

/** Run one statement with the given text and int binds (texts first). Returns 0 or -1. */
static int run(cp_store *s, const char *sql, const char *const *texts, size_t text_count,
               const int64_t *ints, size_t int_count) {
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(s->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    return -1;
  }
  int rc = SQLITE_OK;
  for (size_t i = 0; i != text_count && rc == SQLITE_OK; i++) {
    rc = sqlite3_bind_text(stmt, (int)i + 1, texts[i], -1, SQLITE_STATIC);
  }
  for (size_t i = 0; i != int_count && rc == SQLITE_OK; i++) {
    rc = sqlite3_bind_int64(stmt, (int)(text_count + i) + 1, ints[i]);
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_step(stmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_OK ? 0 : -1;
}

int cp_store_put_node(cp_store *s, const stored_node *n) {
  const char *texts[] = {n->addr, node_state_name(n->sm.state)};
  int64_t ints[] = {n->id, n->sm.fails, n->sm.rejoin ? 1 : 0, n->in_ring ? 1 : 0};
  return run(s,
             "INSERT OR REPLACE INTO nodes(addr, state, id, fails, rejoin, in_ring)"
             " VALUES(?1, ?2, ?3, ?4, ?5, ?6)",
             texts, 2, ints, 4);
}

int cp_store_del_node(cp_store *s, int id) {
  int64_t ints[] = {id};
  return run(s, "DELETE FROM nodes WHERE id = ?1", NULL, 0, ints, 1);
}

int cp_store_nodes(cp_store *s, stored_node *out, size_t max) {
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(s->db,
                         "SELECT id, addr, state, fails, rejoin, in_ring FROM nodes ORDER BY id",
                         -1, &stmt, NULL) != SQLITE_OK) {
    return -1;
  }
  size_t n = 0;
  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && n != max) {
    stored_node *node = &out[n];
    memset(node, 0, sizeof(*node));
    node->id = sqlite3_column_int(stmt, 0);
    const char *addr = (const char *)sqlite3_column_text(stmt, 1);
    const char *state = (const char *)sqlite3_column_text(stmt, 2);
    (void)snprintf(node->addr, sizeof(node->addr), "%s", addr ? addr : "");
    int st = state ? node_state_from_name(state) : -1;
    node->sm.state = st < 0 ? NODE_DEAD : (node_state)st;
    node->sm.fails = sqlite3_column_int(stmt, 3);
    node->sm.rejoin = sqlite3_column_int(stmt, 4) != 0;
    node->in_ring = sqlite3_column_int(stmt, 5) != 0;
    n++;
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE || rc == SQLITE_ROW ? (int)n : -1;
}

static int put_meta(cp_store *s, const char *key, const char *value) {
  const char *texts[] = {key, value};
  return run(s, "INSERT OR REPLACE INTO meta(key, value) VALUES(?1, ?2)", texts, 2, NULL, 0);
}

/** Copy meta key's value into out, or "" if absent. Returns 0 or -1. */
static int get_meta(cp_store *s, const char *key, char *out, size_t len) {
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(s->db, "SELECT value FROM meta WHERE key = ?1", -1, &stmt, NULL) !=
          SQLITE_OK ||
      sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC) != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return -1;
  }
  int rc = sqlite3_step(stmt);
  const char *v = rc == SQLITE_ROW ? (const char *)sqlite3_column_text(stmt, 0) : NULL;
  (void)snprintf(out, len, "%s", v ? v : "");
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW || rc == SQLITE_DONE ? 0 : -1;
}

int cp_store_put_migration(cp_store *s, stored_migration m) {
  char target[16];
  (void)snprintf(target, sizeof(target), "%d", m.target);
  if (sqlite3_exec(s->db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) {
    return -1;
  }
  if (put_meta(s, "mig_phase", mig_phase_name(m.phase)) != 0 ||
      put_meta(s, "mig_target", target) != 0) {
    (void)sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
  }
  return sqlite3_exec(s->db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

int cp_store_migration(cp_store *s, stored_migration *out) {
  char phase[32];
  char target[16];
  if (get_meta(s, "mig_phase", phase, sizeof(phase)) != 0 ||
      get_meta(s, "mig_target", target, sizeof(target)) != 0) {
    return -1;
  }
  int p = mig_phase_from_name(phase);
  out->phase = p < 0 ? MIG_IDLE : (mig_phase)p;
  out->target = (int)strtol(target, NULL, 10);
  return 0;
}

int cp_store_put_running(cp_store *s, bool running) {
  return put_meta(s, "running", running ? "1" : "0");
}

int cp_store_running(cp_store *s, bool *out) {
  char v[4];
  if (get_meta(s, "running", v, sizeof(v)) != 0) {
    return -1;
  }
  *out = strcmp(v, "1") == 0;
  return 0;
}

int cp_store_put_version_mark(cp_store *s, uint64_t mark) {
  char v[32];
  (void)snprintf(v, sizeof(v), "%" PRIu64, mark);
  return put_meta(s, "version_mark", v);
}

int cp_store_version_mark(cp_store *s, uint64_t *out) {
  char v[32];
  if (get_meta(s, "version_mark", v, sizeof(v)) != 0) {
    return -1;
  }
  *out = strtoull(v, NULL, 10);
  return 0;
}
