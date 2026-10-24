#include "kv_handlers.h"
#include "kv_store.h"
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

/** Return whether s holds well-formed UTF-8: no overlong form, surrogate, or code point > U+10FFFF.
 */
static bool valid_utf8(const unsigned char *s, size_t len) {
  static const uint32_t min_code[] = {0, 0x80, 0x800, 0x10000};
  size_t i = 0;
  while (i != len) {
    uint32_t c = s[i];
    if ((c >= 0x80 && c < 0xc0) || c >= 0xf8) {
      return false;
    }
    size_t extra = (size_t)(c >= 0xc0) + (c >= 0xe0) + (c >= 0xf0);
    if (len - i <= extra) {
      return false;
    }
    uint32_t code = extra ? c & (0x3fU >> extra) : c;
    for (size_t k = 1; k <= extra; k++) {
      if ((s[i + k] & 0xc0U) != 0x80) {
        return false;
      }
      code = code << 6U | (s[i + k] & 0x3fU);
    }
    if (code < min_code[extra] || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
      return false;
    }
    i += extra + 1;
  }
  return true;
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
  if (!valid_utf8((const unsigned char *)key.ptr, key.len)) {
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

/**
 * Route the HTTP POST request to kv_store to then access the hashmap.
 */
static void kv_post_handler(http_response *response, const http_request *request, void *arg) {
  kv_store *store = arg;
  size_t key_len;
  char *key = key_copy(response, request, &key_len);
  if (!key) {
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
  int code = 200;
  if (put_value == -2) {
    code = 503;
  } else if (put_value != 0) {
    code = 500;
  }
  http_write(response, code, NULL, 0);
}

/**
 * Route the HTTP DEL request to kv_store to then access the hashmap.
 */
static void kv_del_handler(http_response *response, const http_request *request, void *arg) {
  kv_store *store = arg;
  size_t key_len;
  char *key = key_copy(response, request, &key_len);
  if (!key) {
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
  int code = 200;
  if (del_value == -2) {
    code = 503;
  } else if (del_value < 0) {
    code = 500;
  }
  http_write(response, code, NULL, 0);
}

void kv_register_routes(http_server *server, kv_store *store, logger *log) {
  http_use(server, log_requests, log);
  http_handle(server, HTTP_GET, "/{key}", kv_get_handler, store);
  http_handle(server, HTTP_POST, "/{key}", kv_post_handler, store);
  http_handle(server, HTTP_DELETE, "/{key}", kv_del_handler, store);
}
