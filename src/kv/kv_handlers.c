#include "kv_handlers.h"
#include "kv_store.h"
#include "json/json.h"
#include <stdlib.h>
#include <string.h>

/**
 * Log one line "<method> <path> <status>" after the handler responds, e.g.
 * "GET /kv/key_144 200".
 */
static void log_requests(http_response *response, const http_request *request, http_next next,
                         http_chain *chain, void *ctx) {
  next(response, request, chain);
  const logger *log = ctx;
  http_str method = http_req_method(request);
  http_str path = http_req_path(request);
  log->log(log->ctx, LOG_INFO, "%.*s %.*s %d", (int)method.len, method.ptr, (int)path.len, path.ptr,
           http_resp_status(response));
}

/**
 * Return a NUL-terminated copy of the "{key}" path parameter and set *len to its length,
 * or NULL if allocation fails.
 */
static char *key_copy(const http_request *request, size_t *len) {
  http_str key = http_req_param(request, HTTP_STR("key"));
  char *buf = malloc(key.len + 1);
  if (buf) {
    memcpy(buf, key.ptr, key.len);
    buf[key.len] = '\0';
    *len = key.len;
  }
  return buf;
}

/**
 * Route the HTTP GET request to kv_store to then access the hashmap.
 */
static void kv_get_handler(http_response *response, const http_request *request, void *arg) {
  kv_store *store = arg;
  size_t key_len;
  char *key = key_copy(request, &key_len);
  if (!key) {
    http_write(response, 500, NULL, 0);
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

  // Build the key-value dict.
  json_builder *b = json_builder_new();
  json_node *r = json_new_obj(b);
  json_put(b, r, "key", json_new_str(b, key, key_len));
  json_put(b, r, "value", json_new_str(b, value, value_len));
  size_t len;
  char *bytes = json_to_bytes(r, &len);

  // Send out one response
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

/**
 * Route the HTTP PUT request to kv_store to then access the hashmap.
 */
static void kv_put_handler(http_response *response, const http_request *request, void *arg) {
  kv_store *store = arg;
  size_t key_len;
  char *key = key_copy(request, &key_len);
  if (!key) {
    http_write(response, 500, NULL, 0);
    return;
  }

  // Parse the body using value.
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

  // Put the value.
  int put_value = kv_store_put(store, key, value, value_len);
  json_doc_free(doc);
  free(key);
  http_write(response, put_value != 0 ? 500 : 200, NULL, 0);
}

/**
 * Route the HTTP DEL request to kv_store to then access the hashmap.
 */
static void kv_del_handler(http_response *response, const http_request *request, void *arg) {
  kv_store *store = arg;
  size_t key_len;
  char *key = key_copy(request, &key_len);
  if (!key) {
    http_write(response, 500, NULL, 0);
    return;
  }

  // Delete the value.
  int del_value = kv_store_del(store, key);
  free(key);

  if (del_value == 0) { // key not found
    static const char message[] = "Key not found.\n";
    http_write(response, 404, message, sizeof(message) - 1);
    return;
  }
  http_write(response, del_value < 0 ? 500 : 200, NULL, 0);
}

void kv_register_routes(http_server *server, kv_store *store, logger *log) {
  http_group *kv = http_group_new(server, "/kv");
  http_group_use(kv, log_requests, log);
  http_group_handle(kv, HTTP_GET, "/{key}", kv_get_handler, store);
  http_group_handle(kv, HTTP_PUT, "/{key}", kv_put_handler, store);
  http_group_handle(kv, HTTP_DELETE, "/{key}", kv_del_handler, store);
}
