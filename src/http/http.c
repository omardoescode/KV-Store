#include "http/http.h"

#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "h2o.h"

struct http_server {
  h2o_globalconf_t config;
  h2o_hostconf_t *host;
};

typedef struct {
  h2o_handler_t super;
  h2o_iovec_t method;
  http_handler fn;
  void *arg;
} route;

typedef struct {
  http_server *server;
  int fd;
  h2o_context_t ctx;
  h2o_accept_ctx_t accept_ctx;
} worker;

static const h2o_iovec_t method_names[] = {
    [HTTP_GET] = {H2O_STRLIT("GET")},     [HTTP_HEAD] = {H2O_STRLIT("HEAD")},
    [HTTP_POST] = {H2O_STRLIT("POST")},   [HTTP_PUT] = {H2O_STRLIT("PUT")},
    [HTTP_PATCH] = {H2O_STRLIT("PATCH")}, [HTTP_DELETE] = {H2O_STRLIT("DELETE")},
};

/** Dispatch matching methods to the route handler; return -1 to try the next handler. */
static int on_req(h2o_handler_t *self, h2o_req_t *req) {
  route *rt = (route *)self;
  if (!h2o_memis(req->method.base, req->method.len, rt->method.base, rt->method.len)) {
    return -1;
  }
  rt->fn((http_response *)req, (const http_request *)req, rt->arg);
  return 0;
}

/** Allocate and initialize a server with a default host and no Server response header. */
http_server *http_server_new(void) {
  http_server *s = h2o_mem_alloc(sizeof(*s));
  h2o_config_init(&s->config);
  // No "Server" header: H2O formats it with sprintf on each response.
  s->config.server_name = (h2o_iovec_t){NULL, 0};
  s->host = h2o_config_register_host(&s->config, h2o_iovec_init(H2O_STRLIT("default")), 65535);
  return s;
}

/**
 * Register a method handler on a path, reusing its path configuration for fallthrough.
 * Call before serving; the path also matches descendants and arg is passed to the handler.
 */
void http_handle(http_server *s, http_method method, const char *path, http_handler handler,
                 void *arg) {
  // Handlers on one pathconf fall through to the next on -1, separate pathconfs do not.
  h2o_pathconf_t *pathconf = NULL;
  for (size_t i = 0; i != s->host->paths.size; i++) {
    h2o_pathconf_t *candidate = s->host->paths.entries[i];
    if (h2o_memis(candidate->path.base, candidate->path.len, path, strlen(path))) {
      pathconf = candidate;
      break;
    }
  }
  if (pathconf == NULL) {
    pathconf = h2o_config_register_path(s->host, path, 0);
  }

  route *rt = (route *)h2o_create_handler(pathconf, sizeof(*rt));
  rt->super.on_req = on_req;
  rt->method = method_names[method];
  rt->fn = handler;
  rt->arg = arg;
}

/** Drain pending connections into the worker context, ignoring listener error callbacks. */
static void on_accept(h2o_socket_t *listener, const char *err) {
  if (err != NULL) {
    return;
  }
  worker *w = listener->data;
  h2o_socket_t *sock;
  // Drain the backlog: one wakeup can carry many connections.
  while ((sock = h2o_evloop_socket_accept(listener)) != NULL) {
    h2o_accept(&w->accept_ctx, sock);
  }
}

/** Open a reusable TCP listener for ai; return its descriptor or -1 after closing on failure. */
static int listen_on(const struct addrinfo *ai) {
  int on = 1;
  int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd == -1) {
    return -1;
  }
  // SO_REUSEPORT: one listener per thread, the kernel spreads connections between them.
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0 ||
      setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) != 0 ||
      bind(fd, ai->ai_addr, ai->ai_addrlen) != 0 || listen(fd, SOMAXCONN) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/** Initialize a worker context and accept loop; return NULL if the event loop stops. */
static void *run_worker(void *arg) {
  worker *w = arg;
  h2o_context_init(&w->ctx, h2o_evloop_create(), &w->server->config);
  w->accept_ctx.ctx = &w->ctx;
  w->accept_ctx.hosts = w->server->config.hosts;

  h2o_socket_t *sock = h2o_evloop_socket_create(w->ctx.loop, w->fd, H2O_SOCKET_FLAG_DONT_READ);
  sock->data = w;
  h2o_socket_read_start(sock, on_accept);

  while (h2o_evloop_run(w->ctx.loop, INT32_MAX) == 0) {
  }
  return NULL;
}

/**
 * Bind host:port and serve with threads event loops, including the calling thread.
 * Requires threads >= 1; returns -1 on setup failure or if the calling event loop stops.
 */
int http_listen_and_serve(http_server *s, const char *addr, int threads) {
  const char *colon = strrchr(addr, ':');
  if (colon == NULL || threads < 1) {
    return -1;
  }
  char *host = h2o_strdup(NULL, addr, (size_t)(colon - addr)).base;
  struct addrinfo hints = {.ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE};
  struct addrinfo *ai = NULL;
  int gai = getaddrinfo(*host != '\0' ? host : NULL, colon + 1, &hints, &ai);
  free(host);
  if (gai != 0) {
    return -1;
  }

  // Bind all listeners first, so that a failure returns before a thread starts.
  worker *workers = calloc((size_t)threads, sizeof(*workers));
  if (workers == NULL) {
    freeaddrinfo(ai);
    return -1;
  }
  for (int i = 0; i != threads; i++) {
    workers[i].server = s;
    workers[i].fd = listen_on(ai);
    if (workers[i].fd == -1) {
      while (i-- != 0) {
        close(workers[i].fd);
      }
      freeaddrinfo(ai);
      free(workers);
      return -1;
    }
  }
  freeaddrinfo(ai);

  (void)signal(SIGPIPE, SIG_IGN);
  for (int i = 1; i != threads; i++) {
    pthread_t tid;
    if (pthread_create(&tid, NULL, run_worker, &workers[i]) != 0) {
      return -1;
    }
  }
  run_worker(&workers[0]);
  return -1;
}

/** Return a borrowed method string, valid only until the request handler returns. */
http_str http_req_method(const http_request *r) {
  const h2o_req_t *req = (const h2o_req_t *)r;
  return (http_str){req->method.base, req->method.len};
}

/** Return the borrowed normalized path, valid only until the request handler returns. */
http_str http_req_path(const http_request *r) {
  const h2o_req_t *req = (const h2o_req_t *)r;
  return (http_str){req->path_normalized.base, req->path_normalized.len};
}

/** Return the borrowed request body, valid only until the request handler returns. */
http_str http_req_body(const http_request *r) {
  const h2o_req_t *req = (const h2o_req_t *)r;
  return (http_str){req->entity.base, req->entity.len};
}

/**
 * Find the first header with the lowercase name; return {NULL, 0} when absent.
 * The returned value is borrowed and valid only until the request handler returns.
 */
http_str http_req_header(const http_request *r, http_str name) {
  const h2o_req_t *req = (const h2o_req_t *)r;
  ssize_t i = h2o_find_header_by_str(&req->headers, name.ptr, name.len, -1);
  if (i == -1) {
    return (http_str){NULL, 0};
  }
  return (http_str){req->headers.entries[i].value.base, req->headers.entries[i].value.len};
}

/**
 * Set or replace a response header, copying its value into the request pool.
 * The name must be lowercase and remain valid until http_write is called.
 */
void http_set_header(http_response *w, http_str name, http_str value) {
  h2o_req_t *req = (h2o_req_t *)w;
  h2o_iovec_t copy = h2o_strdup(&req->pool, value.ptr, value.len);
  h2o_set_header_by_str(&req->pool, &req->res.headers, name.ptr, name.len, 1, copy.base, copy.len,
                        1);
}

/** Return a static reason phrase for a supported status, or "Unknown" otherwise. */
static const char *reason_of(int status) {
  switch (status) {
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 204:
    return "No Content";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 409:
    return "Conflict";
  case 500:
    return "Internal Server Error";
  case 503:
    return "Service Unavailable";
  default:
    return "Unknown";
  }
}

/** Send the final response with status and len bytes of body; call once per request. */
void http_write(http_response *w, int status, const void *body, size_t len) {
  h2o_req_t *req = (h2o_req_t *)w;
  req->res.status = status;
  req->res.reason = reason_of(status);
  req->res.content_length = len;
  h2o_send_inline(req, body, len);
}
