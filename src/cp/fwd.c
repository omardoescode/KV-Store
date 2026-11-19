#include "cp/fwd.h"

#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "h2o.h"
#include "http/http.h"
#include "picohttpparser.h"

enum { FWD_MAX_IDLE = 256 };

// Idle connections to one address on one loop.
typedef struct pool {
  struct pool *next;
  char addr[64];
  struct sockaddr_storage sa;
  socklen_t sa_len;
  bool resolved;
  h2o_linklist_t idle;
  size_t idle_count;
} pool;

typedef struct {
  h2o_socket_t *sock;
  pool *pool;
  h2o_linklist_t link;
} conn;

typedef struct {
  pool *pool;
  conn *conn;
  bool reused;
  bool retried;
  fwd_cb cb;
  void *ctx;
  h2o_timer_t timer;
  // Zero-delay timer that reports an early error after fwd_send returns.
  h2o_timer_t later;
  size_t len;
  char data[];
} op;

static __thread pool *pools;

static pool *pool_for(const char *addr) {
  for (pool *p = pools; p; p = p->next) {
    if (strcmp(p->addr, addr) == 0) {
      return p;
    }
  }
  pool *p = calloc(1, sizeof(*p));
  if (!p || strlen(addr) >= sizeof(p->addr)) {
    free(p);
    return NULL;
  }
  memcpy(p->addr, addr, strlen(addr) + 1);
  h2o_linklist_init_anchor(&p->idle);
  const char *colon = strrchr(addr, ':');
  char host[64];
  size_t host_len = colon ? (size_t)(colon - addr) : 0;
  if (host_len >= 2 && addr[0] == '[') {
    memcpy(host, addr + 1, host_len - 2);
    host[host_len - 2] = '\0';
  } else {
    memcpy(host, addr, host_len);
    host[host_len] = '\0';
  }
  struct addrinfo hints = {.ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICHOST};
  struct addrinfo *ai = NULL;
  if (colon && getaddrinfo(host, colon + 1, &hints, &ai) == 0) {
    memcpy(&p->sa, ai->ai_addr, ai->ai_addrlen);
    p->sa_len = ai->ai_addrlen;
    p->resolved = true;
    freeaddrinfo(ai);
  }
  p->next = pools;
  pools = p;
  return p;
}

static void conn_close(conn *c) {
  if (c->sock) {
    h2o_socket_close(c->sock);
  }
  free(c);
}

static void on_idle_event(h2o_socket_t *sock, const char *err) {
  (void)err;
  // The server closed or sent bytes nobody asked for: drop the connection.
  conn *c = sock->data;
  h2o_linklist_unlink(&c->link);
  c->pool->idle_count--;
  conn_close(c);
}

static void conn_release(conn *c) {
  pool *p = c->pool;
  if (p->idle_count >= FWD_MAX_IDLE) {
    conn_close(c);
    return;
  }
  c->sock->data = c;
  h2o_linklist_insert(&p->idle, &c->link);
  p->idle_count++;
  h2o_socket_read_start(c->sock, on_idle_event);
}

static void op_finish(op *o, int status, const char *body, size_t len) {
  if (h2o_timer_is_linked(&o->timer)) {
    h2o_timer_unlink(&o->timer);
  }
  fwd_result res = {status, body, len};
  o->cb(o->ctx, &res);
  free(o);
}

static void start(op *o);

static void op_fail(op *o) {
  if (o->conn) {
    conn_close(o->conn);
    o->conn = NULL;
  }
  if (o->reused && !o->retried) {
    o->retried = true;
    o->reused = false;
    start(o);
    return;
  }
  op_finish(o, -1, NULL, 0);
}

static void on_timeout(h2o_timer_t *t) {
  op *o = H2O_STRUCT_FROM_MEMBER(op, timer, t);
  o->retried = true;
  op_fail(o);
}

static void on_later(h2o_timer_t *t) {
  op *o = H2O_STRUCT_FROM_MEMBER(op, later, t);
  op_finish(o, -1, NULL, 0);
}

/** Return the content-length header value, or 0 if absent. Sets *close for connection: close. */
static size_t content_length(const struct phr_header *h, size_t n, bool *close) {
  size_t len = 0;
  for (size_t i = 0; i != n; i++) {
    if (h[i].name_len == 14 && strncasecmp(h[i].name, "content-length", 14) == 0) {
      len = strtoul(h[i].value, NULL, 10);
    } else if (h[i].name_len == 10 && strncasecmp(h[i].name, "connection", 10) == 0 &&
               h[i].value_len == 5 && strncasecmp(h[i].value, "close", 5) == 0) {
      *close = true;
    }
  }
  return len;
}

static void on_read(h2o_socket_t *sock, const char *err) {
  op *o = sock->data;
  if (err) {
    // A reused connection may have been closed by the server before it saw the request.
    if (sock->input->size != 0) {
      o->retried = true;
    }
    op_fail(o);
    return;
  }
  o->retried = true;
  int minor = 0;
  int status = 0;
  const char *msg = NULL;
  size_t msg_len = 0;
  struct phr_header headers[32];
  size_t header_count = sizeof(headers) / sizeof(headers[0]);
  int head = phr_parse_response(sock->input->bytes, sock->input->size, &minor, &status, &msg,
                                &msg_len, headers, &header_count, 0);
  if (head == -2) {
    return;
  }
  if (head < 0) {
    op_fail(o);
    return;
  }
  bool close = false;
  size_t body_len = content_length(headers, header_count, &close);
  if (sock->input->size < (size_t)head + body_len) {
    return;
  }
  h2o_socket_read_stop(sock);
  conn *c = o->conn;
  o->conn = NULL;
  char *body = malloc(body_len + 1);
  if (body) {
    memcpy(body, sock->input->bytes + head, body_len);
    body[body_len] = '\0';
  }
  h2o_buffer_consume(&sock->input, (size_t)head + body_len);
  if (close || sock->input->size != 0) {
    conn_close(c);
  } else {
    conn_release(c);
  }
  op_finish(o, body ? status : -1, body, body_len);
  free(body);
}

static void on_written(h2o_socket_t *sock, const char *err) {
  op *o = sock->data;
  if (err) {
    op_fail(o);
    return;
  }
  h2o_socket_read_start(sock, on_read);
}

static void send_request(op *o) {
  o->conn->sock->data = o;
  h2o_iovec_t buf = h2o_iovec_init(o->data, o->len);
  h2o_socket_write(o->conn->sock, &buf, 1, on_written);
}

static void on_connect(h2o_socket_t *sock, const char *err) {
  op *o = sock->data;
  if (err) {
    op_fail(o);
    return;
  }
  send_request(o);
}

static void start(op *o) {
  pool *p = o->pool;
  if (!h2o_linklist_is_empty(&p->idle)) {
    conn *c = H2O_STRUCT_FROM_MEMBER(conn, link, p->idle.next);
    h2o_linklist_unlink(&c->link);
    p->idle_count--;
    h2o_socket_read_stop(c->sock);
    o->conn = c;
    o->reused = true;
    send_request(o);
    return;
  }
  conn *c = calloc(1, sizeof(*c));
  const char *err = NULL;
  h2o_socket_t *sock = c ? h2o_socket_connect(http_current_loop(), (struct sockaddr *)&p->sa,
                                              p->sa_len, on_connect, &err)
                         : NULL;
  if (!sock) {
    free(c);
    // Report after fwd_send returns, as the callback must never run inside it.
    h2o_timer_init(&o->later, on_later);
    h2o_timer_link(http_current_loop(), 0, &o->later);
    return;
  }
  c->sock = sock;
  c->pool = p;
  sock->data = o;
  o->conn = c;
}

void fwd_send(const char *addr, const fwd_req *req, fwd_cb cb, void *ctx) {
  char head[512];
  int head_len =
      snprintf(head, sizeof(head), "%s %s HTTP/1.1\r\nhost: kv\r\ncontent-length: %zu\r\n%s\r\n",
               req->method, req->path, req->body_len, req->headers ? req->headers : "");
  size_t len = head_len > 0 ? (size_t)head_len + req->body_len : 0;
  op *o = len && (size_t)head_len < sizeof(head) ? calloc(1, sizeof(*o) + len) : NULL;
  pool *p = pool_for(addr);
  if (!o || !p || !p->resolved) {
    free(o);
    // Error before any I/O: still report it after fwd_send returns.
    o = calloc(1, sizeof(*o));
    if (!o) {
      abort();
    }
    o->cb = cb;
    o->ctx = ctx;
    h2o_timer_init(&o->timer, on_timeout);
    h2o_timer_init(&o->later, on_later);
    h2o_timer_link(http_current_loop(), 0, &o->later);
    return;
  }
  memcpy(o->data, head, (size_t)head_len);
  if (req->body_len) {
    memcpy(o->data + head_len, req->body, req->body_len);
  }
  o->len = len;
  o->pool = p;
  o->cb = cb;
  o->ctx = ctx;
  h2o_timer_init(&o->timer, on_timeout);
  h2o_timer_init(&o->later, on_later);
  h2o_timer_link(http_current_loop(), (uint64_t)req->timeout_ms, &o->timer);
  start(o);
}
