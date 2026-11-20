#include "kv_handlers.h"
#include "kv_store.h"
#include "vclock/vclock.h"
#include "json/json.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * Log one line "<method> <path> <status>" after the handler responds, e.g.
 * "GET /key_144 200". The path is percent-decoded, so it writes each byte outside printable
 * ASCII, and '\', as "\xHH": a decoded newline must not start a forged log line.
 */
static void log_requests(http_response *response, const http_request *request, http_next next,
                         http_chain *chain, void *ctx) {
  next(response, request, chain);
  const logger *log = ctx;
  http_str method = http_req_method(request);
  http_str path = http_req_path(request);
  static const char hex[] = "0123456789abcdef";
  char escaped[200];
  size_t n = 0;
  for (size_t i = 0; i != path.len && n + 4 < sizeof(escaped); i++) {
    unsigned char c = (unsigned char)path.ptr[i];
    if (c < 0x20 || c >= 0x7f || c == '\\') {
      escaped[n++] = '\\';
      escaped[n++] = 'x';
      escaped[n++] = hex[c >> 4U];
      escaped[n++] = hex[c & 0xfU];
    } else {
      escaped[n++] = (char)c;
    }
  }
  log->log(log->ctx, LOG_INFO, "%.*s %.*s %d", (int)method.len, method.ptr, (int)n, escaped,
           http_resp_status(response));
}

/**
 * Return a NUL-terminated copy of the "{key}" path parameter and set *len to its length.
 * On error, respond 400 (key holds a NUL byte or is not UTF-8) or 500 and return NULL.
 */
static char *key_copy(http_response *response, const http_request *request, size_t *len) {
  http_str key = http_req_param(request, HTTP_STR("key"));
  // kv_store takes C strings, so "a%00b" would silently become key "a".
  if (memchr(key.ptr, '\0', key.len) != NULL) {
    static const char message[] = "key must not contain NUL\n";
    http_write(response, 400, message, sizeof(message) - 1);
    return NULL;
  }
  // The key column is SQLite TEXT, which must be UTF-8.
  if (!json_valid_utf8((const unsigned char *)key.ptr, key.len)) {
    static const char message[] = "key must be UTF-8\n";
    http_write(response, 400, message, sizeof(message) - 1);
    return NULL;
  }
  char *buf = malloc(key.len + 1);
  if (!buf) {
    http_write(response, 500, NULL, 0);
    return NULL;
  }
  memcpy(buf, key.ptr, key.len);
  buf[key.len] = '\0';
  *len = key.len;
  return buf;
}

/**
 * Set *out from header name, a decimal at most max, or to 0 if absent. Returns 0, or -1 if the
 * header is empty or not such a decimal.
 */
static int header_uint(const http_request *request, http_str name, uint64_t max, uint64_t *out) {
  http_str h = http_req_header(request, name);
  uint64_t v = 0;
  for (size_t i = 0; i != h.len; i++) {
    unsigned digit = (unsigned)(unsigned char)h.ptr[i] - '0';
    if (digit > 9 || v > (max - digit) / 10) {
      return -1;
    }
    v = (v * 10) + digit;
  }
  *out = v;
  return h.ptr != NULL && h.len == 0 ? -1 : 0;
}

/**
 * Set *stamp from the x-kv-clock, x-kv-ts and x-kv-coord headers that cp sends; an absent one
 * gives the empty clock or 0. On a malformed one, respond 400 and return -1.
 */
static int stamp_of(http_response *response, const http_request *request, vstamp *stamp) {
  http_str clock = http_req_header(request, HTTP_STR("x-kv-clock"));
  uint64_t coord = 0;
  if (vclock_decode(clock.ptr, clock.len, &stamp->clock) != 0 ||
      header_uint(request, HTTP_STR("x-kv-ts"), UINT64_MAX, &stamp->ts) != 0 ||
      header_uint(request, HTTP_STR("x-kv-coord"), INT32_MAX, &coord) != 0) {
    static const char message[] =
        "want x-kv-clock \"id:n,...\", x-kv-ts uint64 and x-kv-coord int32 >= 0\n";
    http_write(response, 400, message, sizeof(message) - 1);
    return -1;
  }
  stamp->coord = (int)coord;
  return 0;
}

/**
 * Route the HTTP GET request to kv_store to then access the hashmap.
 */
static void kv_get_handler(http_response *response, const http_request *request, void *arg) {
  kv_store *store = arg;
  size_t key_len;
  char *key = key_copy(response, request, &key_len);
  if (!key) {
    return;
  }

  // Get the value.
  void *value = NULL;
  size_t value_len = 0;
  int get_value = kv_store_get(store, key, &value, &value_len);

  if (get_value == 0) { // key not found
    free(key);
    static const char message[] = "Key not found.\n";
    http_write(response, 404, message, sizeof(message) - 1);
    return;
  }
  if (get_value < 0) { // internal error
    free(key);
    http_write(response, 500, NULL, 0);
    return;
  }

  json_builder *b = json_builder_new();
  json_node *r = json_new_obj(b);
  // An empty value comes back as NULL, and json_new_str(b, NULL, 0) fails.
  const char *value_str = value ? value : "";
  size_t len = 0;
  char *bytes = json_put(b, r, "value", json_new_str(b, value_str, value_len))
                    ? json_to_bytes(r, &len)
                    : NULL;

  if (bytes != NULL) {
    http_set_header(response, HTTP_STR("content-type"), HTTP_STR("application/json"));
    http_write(response, 200, bytes, len);
  } else {
    http_write(response, 500, NULL, 0);
  }

  free(bytes);
  json_builder_free(b);
  free(value);
  free(key);
}

typedef struct {
  http_deferred *d;
  int status;
  const char *message;
} commit_wait;

static void on_committed(void *ctx, int status) {
  commit_wait *w = ctx;
  int code = status == 0 ? w->status : 500;
  const char *message = status == 0 ? w->message : "";
  http_resume(w->d, code, NULL, 0, message, strlen(message));
  free(w);
}

/**
 * Respond status with the static message, after the change is committed if sync and the change
 * was applied (200 or 404).
 */
static void respond_change(http_response *response, kv_store *store, bool sync, int status,
                           const char *message) {
  if (!sync || (status != 200 && status != 404)) {
    http_write(response, status, message, strlen(message));
    return;
  }
  commit_wait *w = malloc(sizeof(*w));
  if (!w) {
    http_write(response, 500, NULL, 0);
    return;
  }
  *w = (commit_wait){http_defer(response), status, message};
  if (kv_store_flush(store, false, on_committed, w) != 0) {
    http_resume(w->d, 500, NULL, 0, NULL, 0);
    free(w);
  }
}

/** Handle POST /<key> {"value": string}, with the stamp headers from cp. */
static void kv_post(http_response *response, const http_request *request, kv_store *store,
                    bool sync) {
  vstamp stamp;
  if (stamp_of(response, request, &stamp) != 0) {
    return;
  }
  size_t key_len;
  char *key = key_copy(response, request, &key_len);
  if (!key) {
    return;
  }

  http_str body = http_req_body(request);
  json_doc *doc = json_parse(body.ptr, body.len);
  size_t value_len;
  const char *value = json_str(json_get(json_doc_root(doc), "value"), &value_len);
  if (value == NULL) {
    json_doc_free(doc);
    free(key);
    static const char message[] = "want {\"value\": string}\n";
    http_write(response, 400, message, sizeof(message) - 1);
    return;
  }

  int put_value = kv_store_put(store, key, value, value_len, &stamp);
  json_doc_free(doc);
  free(key);
  int code = 200;
  if (put_value == -2) {
    code = 503;
  } else if (put_value != 0) {
    code = 500;
  }
  respond_change(response, store, sync, code, "");
}

/** Handle DELETE /<key>, with the stamp headers from cp. */
static void kv_del(http_response *response, const http_request *request, kv_store *store,
                   bool sync) {
  vstamp stamp;
  if (stamp_of(response, request, &stamp) != 0) {
    return;
  }
  size_t key_len;
  char *key = key_copy(response, request, &key_len);
  if (!key) {
    return;
  }

  int del_value = kv_store_del(store, key, &stamp);
  free(key);

  if (del_value == 0) {
    respond_change(response, store, sync, 404, "Key not found.\n");
    return;
  }
  int code = 200;
  if (del_value == -2) {
    code = 503;
  } else if (del_value < 0) {
    code = 500;
  }
  respond_change(response, store, sync, code, "");
}

static void kv_post_handler(http_response *response, const http_request *request, void *arg) {
  kv_post(response, request, arg, false);
}

static void kv_post_sync_handler(http_response *response, const http_request *request, void *arg) {
  kv_post(response, request, arg, true);
}

static void kv_del_handler(http_response *response, const http_request *request, void *arg) {
  kv_del(response, request, arg, false);
}

static void kv_del_sync_handler(http_response *response, const http_request *request, void *arg) {
  kv_del(response, request, arg, true);
}

static void on_flushed(void *ctx, int status) {
  http_resume(ctx, status == 0 ? 200 : 500, NULL, 0, NULL, 0);
}

/** POST /_internal/flush: respond once every change queued before it is committed. */
static void flush_handler(http_response *response, const http_request *request, void *arg) {
  (void)request;
  http_deferred *d = http_defer(response);
  if (kv_store_flush(arg, true, on_flushed, d) != 0) {
    http_resume(d, 500, NULL, 0, NULL, 0);
  }
}

static void write_message(http_response *response, int status, const char *message) {
  http_write(response, status, message, strlen(message));
}

/** Return whether key is a valid store key: non-empty UTF-8 without NUL. */
static bool valid_key(const char *key, size_t len) {
  if (len == 0 || memchr(key, '\0', len) != NULL) {
    return false;
  }
  return json_valid_utf8((const unsigned char *)key, len);
}

static void health_handler(http_response *response, const http_request *request, void *arg) {
  (void)request;
  (void)arg;
  http_write(response, 200, NULL, 0);
}

typedef struct {
  json_builder *b;
  json_node *items;
  // The key of the last row, copied: row strings die with the callback.
  char *last;
} dump_page;

/** Add "value", "clock", "ts", "coord" and "deleted" of an entry to obj. Returns false on error. */
static bool put_entry(json_builder *b, json_node *obj, const void *value, size_t value_len,
                      const vstamp *stamp, bool deleted) {
  char clock[VCLOCK_TEXT_MAX];
  size_t clock_len = vclock_encode(&stamp->clock, clock);
  return (bool)(json_put(b, obj, "value", json_new_str(b, value ? value : "", value_len)) &&
                json_put(b, obj, "clock", json_new_str(b, clock, clock_len)) &&
                json_put(b, obj, "ts", json_new_int(b, (int64_t)stamp->ts)) &&
                json_put(b, obj, "coord", json_new_int(b, stamp->coord)) &&
                json_put(b, obj, "deleted", json_new_bool(b, deleted)));
}

/** Respond 200 with the JSON root, or 500 if it is NULL or does not serialize. */
static void write_json(http_response *response, const json_node *root) {
  size_t len = 0;
  char *bytes = root ? json_to_bytes(root, &len) : NULL;
  if (bytes) {
    http_set_header(response, HTTP_STR("content-type"), HTTP_STR("application/json"));
    http_write(response, 200, bytes, len);
  } else {
    http_write(response, 500, NULL, 0);
  }
  free(bytes);
}

/**
 * GET /_internal/get?key=<key>: the entry of key, a tombstone too, as
 * {"value","clock","ts","coord","deleted"}; 404 if the node has none.
 */
static void get_entry_handler(http_response *response, const http_request *request, void *arg) {
  http_str key = http_req_query(request, HTTP_STR("key"));
  char *copy = key.ptr && valid_key(key.ptr, key.len) ? strndup(key.ptr, key.len) : NULL;
  if (!copy) {
    write_message(response, 400, "want ?key=<non-empty UTF-8 key>\n");
    return;
  }
  kv_record r;
  int rc = kv_store_read(arg, copy, &r);
  free(copy);
  if (rc != 1) {
    http_write(response, rc == 0 ? 404 : 500, NULL, 0);
    return;
  }
  json_builder *b = json_builder_new();
  json_node *root = json_new_obj(b);
  bool ok = put_entry(b, root, r.value, r.value_len, &r.stamp, r.deleted);
  write_json(response, ok ? root : NULL);
  json_builder_free(b);
  free(r.value);
}

static int add_dump_row(void *ctx, const kv_dump_row *row) {
  dump_page *page = ctx;
  json_node *item = json_new_obj(page->b);
  bool ok =
      (bool)(json_put(page->b, item, "key", json_new_str(page->b, row->key, strlen(row->key))) &&
             put_entry(page->b, item, row->value, row->value_len, &row->stamp, row->deleted) &&
             json_push(page->items, item));
  free(page->last);
  page->last = strdup(row->key);
  return ok && page->last ? 0 : -1;
}

/**
 * GET /_internal/dump?cursor=<key>&limit=<n>: committed rows with key > cursor in key order,
 * tombstones too, as {"items":[{key,value,clock,ts,coord,deleted}],"next":<last key, or null at
 * the end>}.
 */
static void dump_handler(http_response *response, const http_request *request, void *arg) {
  kv_store *store = arg;
  http_str cursor = http_req_query(request, HTTP_STR("cursor"));
  http_str limit_str = http_req_query(request, HTTP_STR("limit"));
  size_t limit = 1000;
  if (limit_str.ptr) {
    limit = 0;
    for (size_t i = 0; i != limit_str.len && limit <= 100000; i++) {
      unsigned digit = (unsigned)(unsigned char)limit_str.ptr[i] - '0';
      limit = digit > 9 ? SIZE_MAX : (limit * 10) + digit;
    }
  }
  if (limit < 1 || limit > 100000) {
    write_message(response, 400, "limit must be in [1, 100000]\n");
    return;
  }
  char *after = strndup(cursor.ptr ? cursor.ptr : "", cursor.len);
  dump_page page = {json_builder_new(), NULL, NULL};
  json_node *root = json_new_obj(page.b);
  page.items = json_new_arr(page.b);
  int count = after && page.items ? kv_store_dump(store, after, limit, add_dump_row, &page) : -1;
  json_node *next = (size_t)count == limit && page.last
                        ? json_new_str(page.b, page.last, strlen(page.last))
                        : json_new_null(page.b);
  bool ok = (bool)(count >= 0 && json_put(page.b, root, "items", page.items) &&
                   json_put(page.b, root, "next", next));
  write_json(response, ok ? root : NULL);
  free(page.last);
  free(after);
  json_builder_free(page.b);
}

/** Map a kv_store change result to an HTTP status: -2 is 503, other negatives 500. */
static int status_of(int rc) {
  if (rc == -2) {
    return 503;
  }
  return rc < 0 ? 500 : 200;
}

/** Return the array field name of the JSON object body, or NULL. */
static const json_val *body_array(const json_doc *doc, const char *name) {
  const json_val *arr = json_get(json_doc_root(doc), name);
  return json_type_of(arr) == JSON_ARRAY ? arr : NULL;
}

/** Read the "clock", "ts" and "coord" of a batch item into *stamp. Returns false if malformed. */
static bool item_stamp(const json_val *item, vstamp *stamp) {
  size_t clock_len = 0;
  const char *clock = json_str(json_get(item, "clock"), &clock_len);
  const json_val *ts = json_get(item, "ts");
  const json_val *coord = json_get(item, "coord");
  if (!clock || vclock_decode(clock, clock_len, &stamp->clock) != 0 ||
      json_type_of(ts) != JSON_NUMBER || json_int(ts) < 0 || json_type_of(coord) != JSON_NUMBER ||
      json_int(coord) < 0 || json_int(coord) > INT32_MAX) {
    return false;
  }
  stamp->ts = (uint64_t)json_int(ts);
  stamp->coord = (int)json_int(coord);
  return true;
}

/**
 * POST /_internal/batch {"items":[{key,value,clock,ts,coord,deleted}]}: puts and tombstones, each
 * applied by the winner rule. All items are checked before any is applied.
 */
static void batch_handler(http_response *response, const http_request *request, void *arg) {
  kv_store *store = arg;
  http_str body = http_req_body(request);
  json_doc *doc = json_parse(body.ptr, body.len);
  const json_val *items = body_array(doc, "items");
  bool ok = items != NULL;
  vstamp stamp;
  for (size_t i = 0; ok && i != json_len(items); i++) {
    const json_val *item = json_at(items, i);
    size_t key_len = 0;
    const char *key = json_str(json_get(item, "key"), &key_len);
    ok = (bool)(key && valid_key(key, key_len) &&
                json_type_of(json_get(item, "value")) == JSON_STRING && item_stamp(item, &stamp) &&
                json_type_of(json_get(item, "deleted")) == JSON_BOOL);
  }
  if (!ok) {
    json_doc_free(doc);
    write_message(response, 400,
                  "want {\"items\":[{\"key\":string,\"value\":string,\"clock\":string,"
                  "\"ts\":uint,\"coord\":uint,\"deleted\":bool}]}\n");
    return;
  }
  int rc = 0;
  for (size_t i = 0; rc >= 0 && i != json_len(items); i++) {
    const json_val *item = json_at(items, i);
    const char *key = json_str(json_get(item, "key"), NULL);
    size_t value_len = 0;
    const char *value = json_str(json_get(item, "value"), &value_len);
    (void)item_stamp(item, &stamp);
    rc = json_bool(json_get(item, "deleted")) ? kv_store_del(store, key, &stamp)
                                              : kv_store_put(store, key, value, value_len, &stamp);
  }
  json_doc_free(doc);
  http_write(response, status_of(rc), NULL, 0);
}

/** POST /_internal/delete-batch {"keys":[...]}: remove keys and their tombstones, no versions. */
static void delete_batch_handler(http_response *response, const http_request *request, void *arg) {
  kv_store *store = arg;
  http_str body = http_req_body(request);
  json_doc *doc = json_parse(body.ptr, body.len);
  const json_val *keys = body_array(doc, "keys");
  bool ok = keys != NULL;
  for (size_t i = 0; ok && i != json_len(keys); i++) {
    size_t key_len = 0;
    const char *key = json_str(json_at(keys, i), &key_len);
    ok = (bool)(key && valid_key(key, key_len));
  }
  if (!ok) {
    json_doc_free(doc);
    write_message(response, 400, "want {\"keys\":[string]}\n");
    return;
  }
  int rc = 0;
  for (size_t i = 0; rc >= 0 && i != json_len(keys); i++) {
    rc = kv_store_remove(store, json_str(json_at(keys, i), NULL));
  }
  json_doc_free(doc);
  http_write(response, status_of(rc), NULL, 0);
}

/** POST /_internal/purge {"paused":bool}: pause or resume the tombstone purge. */
static void purge_handler(http_response *response, const http_request *request, void *arg) {
  kv_store *store = arg;
  http_str body = http_req_body(request);
  json_doc *doc = json_parse(body.ptr, body.len);
  const json_val *paused = json_get(json_doc_root(doc), "paused");
  if (json_type_of(paused) != JSON_BOOL) {
    json_doc_free(doc);
    write_message(response, 400, "want {\"paused\":bool}\n");
    return;
  }
  kv_store_pause_purge(store, json_bool(paused));
  json_doc_free(doc);
  http_write(response, 200, NULL, 0);
}

void kv_register_routes(http_server *server, kv_store *store, logger *log, bool sync_ack) {
  http_use(server, log_requests, log);
  http_handle(server, HTTP_GET, "/{key}", kv_get_handler, store);
  http_handle(server, HTTP_POST, "/{key}", sync_ack ? kv_post_sync_handler : kv_post_handler,
              store);
  http_handle(server, HTTP_DELETE, "/{key}", sync_ack ? kv_del_sync_handler : kv_del_handler,
              store);
  http_handle(server, HTTP_POST, "/_internal/flush", flush_handler, store);
  http_handle(server, HTTP_GET, "/_internal/health", health_handler, NULL);
  http_handle(server, HTTP_GET, "/_internal/dump", dump_handler, store);
  http_handle(server, HTTP_GET, "/_internal/get", get_entry_handler, store);
  http_handle(server, HTTP_POST, "/_internal/batch", batch_handler, store);
  http_handle(server, HTTP_POST, "/_internal/delete-batch", delete_batch_handler, store);
  http_handle(server, HTTP_POST, "/_internal/purge", purge_handler, store);
}
