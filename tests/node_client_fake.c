#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cp/node_client.h"
#include "cp_fake.h"
#include "keypos/keypos.h"

enum { FAKE_NODES = 16, FAKE_ROWS = 64, FAKE_LEN = 32, FAKE_PORT = 9100 };

typedef struct {
  char key[FAKE_LEN];
  char value[FAKE_LEN];
  vstamp stamp;
  bool deleted;
} fake_row;

typedef struct {
  bool up;
  size_t n;
  fake_row rows[FAKE_ROWS];
} fake_node;

static fake_node nodes[FAKE_NODES];
static uint64_t writes;

void fake_reset(void) {
  memset(nodes, 0, sizeof(nodes));
  writes = 0;
  fake_fault("", -1);
}

const char *fake_addr(int id) {
  static char addrs[FAKE_NODES][32];
  (void)snprintf(addrs[id], sizeof(addrs[id]), "127.0.0.1:%d", FAKE_PORT + id);
  return addrs[id];
}

static fake_node *node_at(const char *addr) {
  int port = 0;
  int id = sscanf(addr, "127.0.0.1:%d", &port) == 1 ? port - FAKE_PORT : 0;
  if (id < 1 || id >= FAKE_NODES || !nodes[id].up) {
    return NULL;
  }
  return &nodes[id];
}

static fake_row *find(fake_node *n, const char *key, size_t len) {
  for (size_t i = 0; i != n->n; i++) {
    if (strlen(n->rows[i].key) == len && memcmp(n->rows[i].key, key, len) == 0) {
      return &n->rows[i];
    }
  }
  return NULL;
}

static void put(fake_node *n, const node_row *r) {
  fake_row *row = find(n, r->key, r->key_len);
  vstamp winner = r->stamp;
  if (!row) {
    if (n->n == FAKE_ROWS) {
      abort();
    }
    row = &n->rows[n->n++];
    (void)snprintf(row->key, sizeof(row->key), "%.*s", (int)r->key_len, r->key);
  } else {
    int pick = vstamp_pick(&r->stamp, &row->stamp, &winner);
    if (pick < 0) {
      abort();
    }
    if (pick == 1) {
      row->stamp = winner;
      return;
    }
  }
  (void)snprintf(row->value, sizeof(row->value), "%.*s", (int)r->value_len, r->value);
  row->stamp = winner;
  row->deleted = r->deleted;
}

void fake_put(int id, const char *key, const char *value) {
  writes++;
  node_row r = {key, strlen(key), value, strlen(value), {{1, {{1, writes, 0}}}, writes, 1}, false};
  put(&nodes[id], &r);
}

void fake_del(int id, const char *key) {
  writes++;
  node_row r = {key, strlen(key), "", 0, {{1, {{1, writes, 0}}}, writes, 1}, true};
  put(&nodes[id], &r);
}

void fake_put_stamp(int id, const char *key, const char *value, int coord, uint64_t ts) {
  node_row r = {key, strlen(key), value, strlen(value), {{1, {{coord, 1, 0}}}, ts, coord}, false};
  put(&nodes[id], &r);
}

const char *fake_get(int id, const char *key) {
  fake_row *row = find(&nodes[id], key, strlen(key));
  return row && !row->deleted ? row->value : NULL;
}

bool fake_has(int id, const char *key) { return find(&nodes[id], key, strlen(key)) != NULL; }

void fake_kill(int id) { nodes[id].up = false; }

void fake_start(int id) { nodes[id].up = true; }

bool fake_running(int id) { return nodes[id].up; }

void node_client_init(const cp_config *cfg) { (void)cfg; }

bool node_health(const char *addr) { return node_at(addr) != NULL; }

int node_flush(const char *addr) { return node_at(addr) ? 0 : -1; }

int node_dump(const char *addr, node_page_fn fn, void *arg) {
  fake_node *n = node_at(addr);
  if (!n) {
    return -1;
  }
  node_row rows[FAKE_ROWS];
  for (size_t i = 0; i != n->n; i++) {
    const fake_row *r = &n->rows[i];
    rows[i] = (node_row){r->key, strlen(r->key), r->value, strlen(r->value), r->stamp, r->deleted};
  }
  return fn(arg, rows, n->n) == 0 ? 0 : -1;
}

int node_digest(const char *addr, const uint64_t *ends, size_t n, uint64_t *hashes) {
  fake_node *node = node_at(addr);
  if (!node) {
    return -1;
  }
  memset(hashes, 0, n * sizeof(*hashes));
  for (size_t i = 0; i != node->n; i++) {
    const fake_row *r = &node->rows[i];
    size_t len = strlen(r->key);
    hashes[key_segment(key_position(r->key, len), ends, n)] ^=
        key_entry_hash(r->key, len, &r->stamp, r->deleted);
  }
  return 0;
}

int node_range(const char *addr, uint64_t start, uint64_t end, node_page_fn fn, void *arg) {
  fake_node *node = node_at(addr);
  if (!node) {
    return -1;
  }
  node_row rows[FAKE_ROWS];
  size_t count = 0;
  for (size_t i = 0; i != node->n; i++) {
    const fake_row *r = &node->rows[i];
    if (key_in_segment(key_position(r->key, strlen(r->key)), start, end)) {
      rows[count++] =
          (node_row){r->key, strlen(r->key), r->value, strlen(r->value), r->stamp, r->deleted};
    }
  }
  return fn(arg, rows, count) == 0 ? 0 : -1;
}

int node_put_rows(const char *addr, const node_row *rows, size_t n) {
  fake_node *node = node_at(addr);
  if (!node) {
    return -1;
  }
  for (size_t i = 0; i != n; i++) {
    put(node, &rows[i]);
  }
  return 0;
}

int node_delete_keys(const char *addr, const node_row *rows, size_t n) {
  fake_node *node = node_at(addr);
  if (!node) {
    return -1;
  }
  for (size_t i = 0; i != n; i++) {
    fake_row *row = find(node, rows[i].key, rows[i].key_len);
    if (row) {
      *row = node->rows[--node->n];
    }
  }
  return 0;
}

int node_purge(const char *addr, const uint64_t *ends, const bool *purge, size_t n) {
  fake_node *node = node_at(addr);
  if (!node) {
    return -1;
  }
  // Every tombstone is past the TTL.
  for (size_t i = node->n; i-- != 0;) {
    const fake_row *r = &node->rows[i];
    if (r->deleted && purge[key_segment(key_position(r->key, strlen(r->key)), ends, n)]) {
      node->rows[i] = node->rows[--node->n];
    }
  }
  return 0;
}
