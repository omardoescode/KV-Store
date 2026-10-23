#include "kv_handlers.h"
#include "kv_store.h"
#include "json/json.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE *log_file = NULL;

/**
 * Append one line "<timestamp> <op> <key> <status>" to the log, e.g.
 * "2026-10-22T21:15:03.412 GET key_144 200".
 */
static void log_op(const char *op, http_str key, int status) {
  if (!log_file) {
    return;
  }

  struct timespec current;
  clock_gettime(CLOCK_REALTIME, &current);

  struct tm local_time;
  localtime_r(&current.tv_sec, &local_time);
  char when[32];
  if (strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%S", &local_time) == 0) {
    when[0] = '\0';
  }

  // One fprintf per line: stdio locks the FILE per call, so lines from
  // concurrent threads never interleave. %.*s prints exactly key.len bytes
  // (the key isn't NUL-terminated).
  (void)fprintf(log_file, "%s.%03ld %s %.*s %d\n", when, current.tv_nsec / 1000000L, op,
                (int)key.len, key.ptr, status);
}

/**
 * Log the operation, then send the response.
 */
static void reply(http_response *response, const char *op, http_str key, int status,
                  const void *body, size_t len) {
  log_op(op, key, status);
  http_write(response, status, body, len);
}

/**
 * Set *key to the "{key}" path parameter.
 * Return 0 or -1 if there is no key.
 */
static int key_from_path(const http_request *request, http_str *key) {
  http_str param = http_req_param(request, HTTP_STR("key"));
  if (!param.ptr) {
    return -1;
  }
  *key = param;
  return 0;
}

/**
 * Route the HTTP GET request to kv_store to then access the hashmap.
 */
static void kv_get_handler(http_response *response, const http_request *request, void *arg) {
  kv_store *store = arg;

  // Extract key from the URL.
  http_str key = HTTP_STR("");
  if (key_from_path(request, &key) != 0) {
    static const char message[] = "Missing key\n";
    reply(response, "GET", key, 400, message, sizeof(message) - 1);
    return;
  }

  // Make a NUL-terminated copy.
  char *key_buf = malloc(key.len + 1);
  if (!key_buf) {
    reply(response, "GET", key, 500, NULL, 0);
    return;
  }
  memcpy(key_buf, key.ptr, key.len);
  key_buf[key.len] = '\0';

  // Get the value.
  void *value = NULL;
  size_t value_len = 0;
  int get_value = kv_store_get(store, key_buf, &value, &value_len);

  if (get_value == 0) { // key not found
    free(key_buf);
    static const char message[] = "Key not found.\n";
    reply(response, "GET", key, 404, message, sizeof(message) - 1);
    return;
  }
  if (get_value < 0) { // internal error
    free(key_buf);
    reply(response, "GET", key, 500, NULL, 0);
    return;
  }

  // Build the key-value dict.
  json_builder *b = json_builder_new();
  json_node *r = json_new_obj(b);
  json_put(b, r, "key", json_new_str(b, key_buf, key.len));
  json_put(b, r, "value", json_new_str(b, value, value_len));
  size_t len;
  char *bytes = json_to_bytes(r, &len);

  // Send out one response
  if (bytes != NULL) {
    http_set_header(response, HTTP_STR("content-type"), HTTP_STR("application/json"));
    reply(response, "GET", key, 200, bytes, len);
  } else {
    reply(response, "GET", key, 500, NULL, 0);
  }

  free(bytes);
  json_builder_free(b);
  free(value);
  free(key_buf);
}

/**
 * Route the HTTP PUT request to kv_store to then access the hashmap.
 */
static void kv_put_handler(http_response *response, const http_request *request, void *arg) {
  kv_store *store = arg;

  // Extract key from the URL.
  http_str key = HTTP_STR("");
  if (key_from_path(request, &key) != 0) {
    static const char message[] = "Missing key\n";
    http_write(response, 400, message, sizeof(message) - 1);
    return;
  }

  // Make a NUL-terminated copy.
  char *key_buf = malloc(key.len + 1);
  if (!key_buf) {
    http_write(response, 500, NULL, 0);
    return;
  }
  memcpy(key_buf, key.ptr, key.len);
  key_buf[key.len] = '\0';

  // Parse the body using value.
  http_str body = http_req_body(request);
  json_doc *doc = json_parse(body.ptr, body.len);
  size_t value_len;
  const char *value = json_str(json_get(json_doc_root(doc), "value"), &value_len);
  if (value == NULL) {
    json_doc_free(doc);
    static const char message[] = "want {\"value\": string}\n";
    reply(response, "PUT", key, 400, message, sizeof(message) - 1);
    free(key_buf);
    return;
  }

  // Put the value.
  int put_value = kv_store_put(store, key_buf, value, value_len);
  json_doc_free(doc);
  free(key_buf);

  if (put_value != 0) {
    reply(response, "PUT", key, 500, NULL, 0);
    return;
  }
  reply(response, "PUT", key, 200, NULL, 0);
}

/**
 * Route the HTTP DEL request to kv_store to then access the hashmap.
 */
static void kv_del_handler(http_response *response, const http_request *request, void *arg) {
  kv_store *store = arg;

  // Extract key from the URL.
  http_str key = HTTP_STR("");
  if (key_from_path(request, &key) != 0) {
    static const char message[] = "Missing key\n";
    http_write(response, 400, message, sizeof(message) - 1);
    return;
  }

  // Make a NUL-terminated copy.
  char *key_buf = malloc(key.len + 1);
  if (!key_buf) {
    http_write(response, 500, NULL, 0);
    return;
  }
  memcpy(key_buf, key.ptr, key.len);
  key_buf[key.len] = '\0';

  // Delete the value.
  int del_value = kv_store_del(store, key_buf);
  free(key_buf);

  if (del_value == 0) { // key not found
    static const char message[] = "Key not found.\n";
    reply(response, "DEL", key, 404, message, sizeof(message) - 1);
    return;
  }
  if (del_value < 0) {
    reply(response, "DEL", key, 500, NULL, 0);
    return;
  }
  reply(response, "DEL", key, 200, NULL, 0);
}

void kv_register_routes(http_server *server, kv_store *store, FILE *log) {
  log_file = log;
  http_handle(server, HTTP_GET, "/kv/{key}", kv_get_handler, store);
  http_handle(server, HTTP_PUT, "/kv/{key}", kv_put_handler, store);
  http_handle(server, HTTP_DELETE, "/kv/{key}", kv_del_handler, store);
}
