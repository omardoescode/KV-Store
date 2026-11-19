#ifndef CP_FWD_H
#define CP_FWD_H

#include <stddef.h>

/**
 * An async HTTP/1.1 client for the event loop threads of cp, with keep-alive connections per
 * loop and node address. Call only on an event loop thread (http_current_loop() != NULL).
 */

typedef struct {
  const char *method;
  const char *path;
  /** Extra header lines, each "name: value\r\n", or NULL. */
  const char *headers;
  const void *body;
  size_t body_len;
  int timeout_ms;
} fwd_req;

typedef struct {
  /** HTTP status, or -1 on connect, I/O, timeout or parse error. */
  int status;
  /** Borrowed for the callback only. */
  const char *body;
  size_t len;
} fwd_result;

typedef void (*fwd_cb)(void *ctx, const fwd_result *res);

/**
 * Send req to addr ("ip:port", numeric) and call cb once on this loop thread, never before
 * fwd_send returns. A request on a reused connection that fails before any response byte is sent
 * once more on a new connection.
 */
void fwd_send(const char *addr, const fwd_req *req, fwd_cb cb, void *ctx);

#endif
