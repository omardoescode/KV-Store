#ifndef CP_HCLIENT_H
#define CP_HCLIENT_H

#include <stddef.h>

/** A blocking HTTP/1.1 client, one connection per request. For the controller thread. */

typedef struct {
  const char *method;
  const char *path;
  /** Extra header lines, each "name: value\r\n", or NULL. */
  const char *headers;
  const void *body;
  size_t body_len;
  /** Limit for the whole request. */
  int timeout_ms;
} hclient_req;

typedef struct {
  int status;
  /** malloc'd and NUL-terminated; the caller frees it. */
  char *body;
  size_t len;
} hclient_resp;

/**
 * Send req to addr ("host:port", "[v6]:port" or "unix:/path") and read the response, plain or
 * chunked. Returns 0 with *out filled, or -1 on a connect, I/O, timeout or parse error.
 */
int hclient_do(const char *addr, const hclient_req *req, hclient_resp *out);

#endif
