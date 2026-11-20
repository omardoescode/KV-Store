#include "cp/controller_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http/http.h"
#include "json/json.h"

/** Rows for one node, sent in batches of BATCH_ROWS. */
typedef struct {
  controller *ctl;
  const char *addr;
  const char *path;
  json_builder *b;
  json_node *items;
  size_t count;
  int failed;
} sink;

static void sink_reset(sink *s) {
  json_builder_free(s->b);
  s->b = json_builder_new();
  s->items = json_new_arr(s->b);
  s->count = 0;
}

static void sink_flush(sink *s) {
  if (s->count == 0 || s->failed) {
    return;
  }
  json_node *root = json_new_obj(s->b);
  bool batch = strcmp(s->path, "/_internal/batch") == 0;
  size_t len = 0;
  char *body =
      json_put(s->b, root, batch ? "items" : "keys", s->items) ? json_to_bytes(root, &len) : NULL;
  hclient_req req = {"POST", s->path, NULL, body, len, s->ctl->cfg->forward_timeout_ms * 5};
  if (!body || ctl_call(s->addr, &req, NULL) != 200) {
    s->failed = 1;
  }
  free(body);
  sink_reset(s);
}

static void sink_row(sink *s, const json_val *row) {
  size_t key_len = 0;
  const char *key = json_str(json_get(row, "key"), &key_len);
  bool ok;
  if (strcmp(s->path, "/_internal/batch") == 0) {
    size_t value_len = 0;
    const char *value = json_str(json_get(row, "value"), &value_len);
    json_node *item = json_new_obj(s->b);
    ok = (bool)(json_put(s->b, item, "key", json_new_str(s->b, key, key_len)) &&
                json_put(s->b, item, "value", json_new_str(s->b, value ? value : "", value_len)) &&
                json_put(s->b, item, "version",
                         json_new_int(s->b, json_int(json_get(row, "version")))) &&
                json_put(s->b, item, "deleted",
                         json_new_bool(s->b, json_bool(json_get(row, "deleted")))) &&
                json_push(s->items, item));
  } else {
    ok = json_push(s->items, json_new_str(s->b, key, key_len));
  }
  if (!ok) {
    s->failed = 1;
  }
  if (++s->count == BATCH_ROWS) {
    sink_flush(s);
  }
}

static int copy_into(controller *ctl, const xfer *x, int node, const char *src, int target,
                     sink *s) {
  hclient_req flush = {
      "POST", "/_internal/flush", NULL, NULL, 0, ctl->cfg->forward_timeout_ms * 5,
  };
  if (ctl_call(src, &flush, NULL) != 200) {
    return -1;
  }
  char *cursor = strdup("");
  while (cursor) {
    cluster_reserve_versions(ctl->c);
    size_t path_len = strlen(cursor) + 64;
    char *path = malloc(path_len);
    if (!path) {
      free(cursor);
      return -1;
    }
    (void)snprintf(path, path_len, "/_internal/dump?limit=%d&cursor=%s", DUMP_PAGE, cursor);
    hclient_req req = {"GET", path, NULL, NULL, 0, ctl->cfg->forward_timeout_ms * 5};
    hclient_resp resp = {0, NULL, 0};
    int status = ctl_call(src, &req, &resp);
    free(path);
    if (status != 200) {
      free(resp.body);
      free(cursor);
      return -1;
    }
    json_doc *doc = json_parse(resp.body, resp.len);
    free(resp.body);
    const json_val *items = json_get(json_doc_root(doc), "items");
    if (json_type_of(items) != JSON_ARRAY) {
      json_doc_free(doc);
      free(cursor);
      return -1;
    }
    for (size_t i = 0; i != json_len(items); i++) {
      const json_val *row = json_at(items, i);
      size_t key_len = 0;
      const char *key = json_str(json_get(row, "key"), &key_len);
      bool moves = false;
      if (key && target != 0) {
        moves = xfer_source(x, key, key_len, target) == node;
      } else if (key) {
        moves = xfer_drops(x, node, key, key_len);
      }
      if (moves) {
        sink_row(s, row);
      }
    }
    const json_val *next = json_get(json_doc_root(doc), "next");
    size_t next_len = 0;
    const char *next_key = json_str(next, &next_len);
    free(cursor);
    cursor = NULL;
    if (!next_key) {
      json_doc_free(doc);
      sink_flush(s);
      return s->failed ? -1 : 0;
    }
    cursor = malloc((next_len * 3) + 1);
    if (cursor) {
      cursor[http_percent_encode(cursor, next_key, next_len)] = '\0';
    }
    json_doc_free(doc);
  }
  return -1;
}

int ctl_move_rows(controller *ctl, const xfer *x, int node, const char *addr, int target,
                  const char *dst) {
  sink s = {
      ctl,
      target != 0 ? dst : addr,
      target != 0 ? "/_internal/batch" : "/_internal/delete-batch",
      NULL,
      NULL,
      0,
      0,
  };
  sink_reset(&s);
  int rc = copy_into(ctl, x, node, addr, target, &s);
  json_builder_free(s.b);
  return rc;
}

int ctl_repair(controller *ctl, int id) {
  driver_fault_point("REPAIR", id);
  // Writes that started before the node was REPAIRING did not go to it.
  if (cluster_drain(ctl->c, DRAIN_TIMEOUT_MS) != 0) {
    return -1;
  }
  ctl_set_purge_all(ctl, true);
  static cluster_view v;
  cluster_snapshot(ctl->c, &v);
  static bool readers[CLUSTER_MAX_NODES + 1];
  for (int src = 1; src <= CLUSTER_MAX_NODES; src++) {
    readers[src] = (bool)(v.nodes[src].exists && node_serves_reads(v.nodes[src].sm.state));
  }
  xfer x = {v.ring, v.ring, readers};
  int rc = 0;
  for (int src = 1; src <= CLUSTER_MAX_NODES && rc == 0; src++) {
    if (readers[src]) {
      rc = ctl_move_rows(ctl, &x, src, v.nodes[src].addr, id, v.nodes[id].addr);
    }
  }
  ctl_set_purge_all(ctl, false);
  return rc;
}

void ctl_respawn(controller *ctl, int id) {
  unsigned act = cluster_node_event(ctl->c, id, NODE_EV_RESPAWN);
  if ((act & NODE_ACT_SPAWN) == 0) {
    return;
  }
  driver_fault_point("RESPAWN", id);
  char raw[DRIVER_ADDR_LEN];
  char addr[DRIVER_ADDR_LEN];
  if (driver_spawn(id, raw) != 0 || ctl_resolve(raw, addr) != 0 || !ctl_wait_healthy(ctl, addr)) {
    LOGF(ctl, LOG_ERROR, "node %d: spawn failed", id);
    (void)cluster_node_event(ctl->c, id, NODE_EV_SPAWN_FAIL);
    return;
  }
  act = cluster_spawned(ctl->c, id, addr);
  while ((act & NODE_ACT_REPAIR) != 0) {
    if (ctl_repair(ctl, id) == 0) {
      (void)cluster_node_event(ctl->c, id, NODE_EV_REPAIR_DONE);
      return;
    }
    LOGF(ctl, LOG_WARN, "node %d: repair failed", id);
    if (!ctl_probe_health(ctl, addr)) {
      // Leave it REPAIRING: probes find it dead and spawn it again.
      return;
    }
    act = cluster_node_event(ctl->c, id, NODE_EV_REPAIR_FAIL);
    ctl_sleep_ms(ctl->cfg->probe_interval_ms);
  }
}
