#include "http/http.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "h2o.h"
#include "http/router.h"

typedef struct {
  http_middleware fn;
  void *ctx;
} middleware;

typedef struct {
  middleware *items;
  size_t count;
} middleware_list;

struct http_group {
  http_server *server;
  char *prefix;
  middleware_list middleware;
  http_group *next;
};

// The route arg: the handler arg and the group of the route, or NULL.
typedef struct binding {
  void *arg;
  const http_group *group;
  struct binding *next;
} binding;

struct http_server {
  h2o_globalconf_t config;
  router routes;
  middleware_list middleware;
  http_group *groups;
  binding *bindings;
  // http_server_stop writes to stop_pipe[1]; each event loop polls its own dup of stop_pipe[0],
  // which stays readable once written, so a stop before the loops start is not lost.
  int stop_pipe[2];
};

// The rest of the middleware chain of one request: server middleware, then group middleware.
struct http_chain {
  const http_server *server;
  const route *rt;
  size_t next;
};

struct http_request {
  h2o_req_t *req;
  const route *rt;
  // The decoded path segment that matched each route segment.
  const http_str *values;
};

typedef struct {
  h2o_handler_t super;
  http_server *server;
} dispatcher;

enum { GATE_WAIT, GATE_RUN, GATE_ABORT };

enum { HTTP_SHUTDOWN_GRACE_MS = 5000 };

// Holds new threads until all of them exist, so that a failed start can stop them.
typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t cond;
  int state;
} start_gate;

typedef struct {
  http_server *server;
  int fd;
  pthread_t tid;
  start_gate *gate;
  h2o_socket_t *listener;
  h2o_socket_t *stop;
  int stopping;
  h2o_context_t ctx;
  h2o_accept_ctx_t accept_ctx;
} worker;

static const h2o_iovec_t method_names[] = {
    [HTTP_GET] = {H2O_STRLIT("GET")},     [HTTP_HEAD] = {H2O_STRLIT("HEAD")},
    [HTTP_POST] = {H2O_STRLIT("POST")},   [HTTP_PUT] = {H2O_STRLIT("PUT")},
    [HTTP_PATCH] = {H2O_STRLIT("PATCH")}, [HTTP_DELETE] = {H2O_STRLIT("DELETE")},
};

/** Return the http_method of name, or -1 if it is not supported. */
static int method_of(h2o_iovec_t name) {
  for (int i = 0; i != (int)(sizeof(method_names) / sizeof(method_names[0])); i++) {
    if (h2o_memis(name.base, name.len, method_names[i].base, method_names[i].len)) {
      return i;
    }
  }
  return -1;
}

/** Run middleware number c->next of the chain, or the handler after the last one. */
static void run_chain(http_response *w, const http_request *r, http_chain *c) {
  const binding *b = c->rt->arg;
  size_t i = c->next++;
  const middleware_list *list = &c->server->middleware;
  if (i >= list->count && b->group != NULL) {
    i -= list->count;
    list = &b->group->middleware;
  }
  if (i < list->count) {
    list->items[i].fn(w, r, run_chain, c, list->items[i].ctx);
    return;
  }
  c->rt->fn(w, r, b->arg);
}

/** Send 405 with an Allow header that lists the methods in mask. */
static void send_405(h2o_req_t *req, unsigned mask) {
  char allow[64];
  size_t len = 0;
  for (size_t i = 0; i != sizeof(method_names) / sizeof(method_names[0]); i++) {
    if (mask & (1U << i)) {
      len += (size_t)snprintf(allow + len, sizeof(allow) - len, "%s%.*s", len ? ", " : "",
                              (int)method_names[i].len, method_names[i].base);
    }
  }
  h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_ALLOW, NULL,
                 h2o_strdup(&req->pool, allow, len).base, len);
  h2o_send_error_generic(req, 405, "Method Not Allowed", "method not allowed",
                         H2O_SEND_ERROR_KEEP_HEADERS);
}

/**
 * Dispatch to the first route that matches path and method; HEAD falls back to the GET route.
 * Send 405 if only other methods match the path; return -1 if no route matches it.
 */
static int on_req(h2o_handler_t *self, h2o_req_t *req) {
  const http_server *s = ((dispatcher *)self)->server;
  http_str path = {req->path.base, req->query_at != SIZE_MAX ? req->query_at : req->path.len};
  char *buf = memchr(path.ptr, '%', path.len) != NULL
                  ? h2o_mem_alloc_pool(&req->pool, char, path.len)
                  : NULL;
  http_str values[ROUTER_MAX_SEGMENTS];
  size_t count = 0;
  router_status status = router_split(&s->routes, path, buf, values, &count);
  if (status == ROUTER_BAD_REQUEST) {
    h2o_send_error_400(req, "Bad Request", "invalid path", 0);
    return 0;
  }
  if (status != ROUTER_OK) {
    return -1;
  }
  int method = method_of(req->method);
  const route *rt = router_match(&s->routes, method, values, count);
  if (rt == NULL && method == HTTP_HEAD) {
    rt = router_match(&s->routes, HTTP_GET, values, count);
  }
  if (rt == NULL) {
    unsigned allowed = router_allowed(&s->routes, values, count);
    if (allowed == 0) {
      return -1;
    }
    if (allowed & (1U << (unsigned)HTTP_GET)) {
      allowed |= 1U << (unsigned)HTTP_HEAD;
    }
    send_405(req, allowed);
    return 0;
  }
  struct http_request r = {req, rt, values};
  http_chain c = {s, rt, 0};
  run_chain((http_response *)req, &r, &c);
  return 0;
}

/** Allocate and initialize a server with a default host and no Server response header. */
http_server *http_server_new(void) {
  http_server *s = h2o_mem_alloc(sizeof(*s));
  s->routes = (router){NULL, 0, 0};
  s->middleware = (middleware_list){NULL, 0};
  s->groups = NULL;
  s->bindings = NULL;
  if (pipe2(s->stop_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
    h2o_fatal("pipe2: %s", strerror(errno));
  }
  h2o_config_init(&s->config);
  // No "Server" header: H2O formats it with sprintf on each response.
  s->config.server_name = (h2o_iovec_t){NULL, 0};

  // One catch-all H2O path: H2O does not fall back to a parent path when no handler of the
  // most specific path takes the request, so on_req selects the route by path and method.
  h2o_hostconf_t *host =
      h2o_config_register_host(&s->config, h2o_iovec_init(H2O_STRLIT("default")), 65535);
  h2o_pathconf_t *pathconf = h2o_config_register_path(host, "/", 0);
  dispatcher *d = (dispatcher *)h2o_create_handler(pathconf, sizeof(*d));
  d->super.on_req = on_req;
  d->server = s;
  return s;
}

/** Free the routes, groups, middleware, and H2O configuration of a server no loop runs. */
void http_server_free(http_server *s) {
  router_free(&s->routes);
  while (s->bindings != NULL) {
    binding *b = s->bindings;
    s->bindings = b->next;
    free(b);
  }
  while (s->groups != NULL) {
    http_group *g = s->groups;
    s->groups = g->next;
    free(g->prefix);
    free(g->middleware.items);
    free(g);
  }
  free(s->middleware.items);
  h2o_config_dispose(&s->config);
  close(s->stop_pipe[0]);
  close(s->stop_pipe[1]);
  free(s);
}

void http_server_stop(http_server *s) {
  int saved = errno;
  (void)!write(s->stop_pipe[1], "", 1);
  errno = saved;
}

/** Register handler with a binding to group; abort with a message if path is malformed. */
static void add_route(http_server *s, http_method method, const char *path, http_handler handler,
                      void *arg, const http_group *group) {
  binding *b = h2o_mem_alloc(sizeof(*b));
  *b = (binding){arg, group, s->bindings};
  s->bindings = b;
  if (router_add(&s->routes, method, path, handler, b) != 0) {
    h2o_fatal("http_handle: invalid route pattern \"%s\"", path);
  }
}

/** Register a method handler on a path pattern; abort with a message if it is malformed. */
void http_handle(http_server *s, http_method method, const char *path, http_handler handler,
                 void *arg) {
  add_route(s, method, path, handler, arg, NULL);
}

/** Allocate a group with a copy of prefix; abort with a message if prefix is malformed. */
http_group *http_group_new(http_server *s, const char *prefix) {
  size_t len = strlen(prefix);
  if (len < 2 || prefix[0] != '/' || prefix[len - 1] == '/') {
    h2o_fatal("http_group_new: invalid prefix \"%s\"", prefix);
  }
  http_group *g = h2o_mem_alloc(sizeof(*g));
  *g = (http_group){s, h2o_strdup(NULL, prefix, len).base, {NULL, 0}, s->groups};
  s->groups = g;
  return g;
}

/** Register handler on the group prefix followed by path. */
void http_group_handle(http_group *g, http_method method, const char *path, http_handler handler,
                       void *arg) {
  if (path[0] != '/') {
    h2o_fatal("http_group_handle: invalid route pattern \"%s\"", path);
  }
  char *full = h2o_concat(NULL, h2o_iovec_init(g->prefix, strlen(g->prefix)),
                          h2o_iovec_init(path, strlen(path)))
                   .base;
  add_route(g->server, method, full, handler, arg, g);
  free(full);
}

/** Append mw to list, aborting if allocation fails. */
static void middleware_add(middleware_list *list, http_middleware mw, void *ctx) {
  list->items = h2o_mem_realloc(list->items, sizeof(*list->items) * (list->count + 1));
  list->items[list->count++] = (middleware){mw, ctx};
}

/** Append mw to the middleware of g. */
void http_group_use(http_group *g, http_middleware mw, void *ctx) {
  middleware_add(&g->middleware, mw, ctx);
}

/** Append mw to the middleware of s. */
void http_use(http_server *s, http_middleware mw, void *ctx) {
  middleware_add(&s->middleware, mw, ctx);
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

/** Stop accepting and ask open connections to close once their requests finish. */
static void on_stop(h2o_socket_t *stop, const char *err) {
  (void)err;
  worker *w = stop->data;
  h2o_socket_close(stop);
  w->stop = NULL;
  h2o_socket_read_stop(w->listener);
  h2o_socket_close(w->listener);
  w->listener = NULL;
  h2o_context_request_shutdown(&w->ctx);
  w->stopping = 1;
}

static size_t open_conns(const h2o_context_t *ctx) {
  return ctx->_conns.num_conns.idle + ctx->_conns.num_conns.active + ctx->_conns.num_conns.shutdown;
}

/**
 * Initialize a worker context and accept loop, run it until http_server_stop and its connections
 * close or a loop fails, then free the context. Return w if stopped, NULL on failure.
 */
static void *run_worker(void *arg) {
  worker *w = arg;
  pthread_setname_np(pthread_self(), "kv-loop");
  int stop_fd = fcntl(w->server->stop_pipe[0], F_DUPFD_CLOEXEC, 0);
  if (stop_fd == -1) {
    close(w->fd);
    http_server_stop(w->server);
    return NULL;
  }
  h2o_evloop_t *loop = h2o_evloop_create();
  h2o_context_init(&w->ctx, loop, &w->server->config);
  w->accept_ctx.ctx = &w->ctx;
  w->accept_ctx.hosts = w->server->config.hosts;

  w->listener = h2o_evloop_socket_create(loop, w->fd, H2O_SOCKET_FLAG_DONT_READ);
  w->listener->data = w;
  h2o_socket_read_start(w->listener, on_accept);
  w->stop = h2o_evloop_socket_create(loop, stop_fd, H2O_SOCKET_FLAG_DONT_READ);
  w->stop->data = w;
  h2o_socket_read_start(w->stop, on_stop);

  int failed = 0;
  uint64_t deadline = 0;
  while (!w->stopping || (open_conns(&w->ctx) != 0 && h2o_now(loop) < deadline)) {
    // A signal can interrupt the wait; that is not a reason to stop the loop.
    if (h2o_evloop_run(loop, w->stopping ? 100 : INT32_MAX) != 0 && errno != EINTR) {
      failed = 1;
      break;
    }
    if (w->stopping && deadline == 0) {
      deadline = h2o_now(loop) + HTTP_SHUTDOWN_GRACE_MS;
    }
  }
  if (failed) {
    http_server_stop(w->server);
  }
  if (w->stop != NULL) {
    h2o_socket_close(w->stop);
  }
  if (w->listener != NULL) {
    h2o_socket_close(w->listener);
  }
  // A connection still open after the grace period keeps the loop: disposing would free under it.
  if (open_conns(&w->ctx) == 0) {
    h2o_context_dispose(&w->ctx);
    h2o_evloop_destroy(loop);
    // Free the thread-local recycled buffers and pools of H2O; now 0 asks for a full clear.
    h2o_cleanup_thread(0, NULL);
  }
  return failed ? NULL : w;
}

/** Wait at the start gate, then run the worker, or return NULL if the start was aborted. */
static void *worker_thread(void *arg) {
  worker *w = arg;
  pthread_mutex_lock(&w->gate->mu);
  while (w->gate->state == GATE_WAIT) {
    pthread_cond_wait(&w->gate->cond, &w->gate->mu);
  }
  int state = w->gate->state;
  pthread_mutex_unlock(&w->gate->mu);
  return state == GATE_RUN ? run_worker(w) : NULL;
}

/** Release all threads that wait at the gate with the given state. */
static void open_gate(start_gate *gate, int state) {
  pthread_mutex_lock(&gate->mu);
  gate->state = state;
  pthread_cond_broadcast(&gate->cond);
  pthread_mutex_unlock(&gate->mu);
}

#ifdef __linux__
/** Return the CPU quota of the cgroup (v2) of this process, e.g. `docker --cpus`; 0 if none. */
static long cgroup_cpu_quota(void) {
  char line[4096];
  FILE *f = fopen("/proc/self/cgroup", "r");
  if (f == NULL) {
    return 0;
  }
  const char *group = NULL;
  while (group == NULL && fgets(line, sizeof(line), f) != NULL) {
    if (strncmp(line, "0::", 3) == 0) {
      char *newline = strchr(line, '\n');
      if (newline != NULL) {
        *newline = '\0';
      }
      group = line + 3;
    }
  }
  (void)fclose(f);
  if (group == NULL) {
    return 0;
  }

  char *path =
      h2o_concat(NULL, h2o_iovec_init(H2O_STRLIT("/sys/fs/cgroup")),
                 h2o_iovec_init(group, strlen(group)), h2o_iovec_init(H2O_STRLIT("/cpu.max")))
          .base;
  f = fopen(path, "r");
  free(path);
  if (f == NULL) {
    return 0;
  }
  const char *read = fgets(line, sizeof(line), f);
  (void)fclose(f);
  if (read == NULL) {
    return 0;
  }

  // "<quota> <period>", or "max <period>" for no limit.
  char *end;
  long quota = strtol(line, &end, 10);
  long period = strtol(end, NULL, 10);
  if (end == line || quota <= 0 || period <= 0) {
    return 0;
  }
  return (quota + period - 1) / period;
}
#endif

/** Return the CPU count that this process can use, at least 1. */
int http_default_threads(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
#ifdef __linux__
  cpu_set_t set;
  if (sched_getaffinity(0, sizeof(set), &set) == 0) {
    n = CPU_COUNT(&set);
  }
  long quota = cgroup_cpu_quota();
  if (quota != 0 && quota < n) {
    n = quota;
  }
#endif
  return n > 0 ? (int)n : 1;
}

/** Resolve "host:port" or "[v6]:port" for a passive TCP socket; return NULL and set errno on error.
 */
static struct addrinfo *resolve(const char *addr) {
  const char *colon = strrchr(addr, ':');
  if (colon == NULL) {
    errno = EINVAL;
    return NULL;
  }
  size_t host_len = (size_t)(colon - addr);
  if (host_len >= 2 && addr[0] == '[' && addr[host_len - 1] == ']') {
    addr++;
    host_len -= 2;
  }
  char *host = h2o_strdup(NULL, addr, host_len).base;
  struct addrinfo hints = {.ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE};
  struct addrinfo *ai = NULL;
  int gai = getaddrinfo(*host != '\0' ? host : NULL, colon + 1, &hints, &ai);
  free(host);
  if (gai != 0) {
    if (gai != EAI_SYSTEM) {
      (void)fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai));
      errno = EINVAL;
    }
    return NULL;
  }
  return ai;
}

/**
 * Bind host:port and serve with threads event loops, including the calling thread.
 * Requires threads >= 1. Returns 0 after http_server_stop once every loop has stopped, or -1
 * on setup failure or if an event loop fails. All threads and listeners of this call are
 * stopped and closed before it returns.
 */
int http_listen_and_serve(http_server *s, const char *addr, int threads) {
  if (threads < 1) {
    errno = EINVAL;
    return -1;
  }
  struct addrinfo *ai = resolve(addr);
  if (ai == NULL) {
    return -1;
  }

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

  // workers[0] runs on this thread.
  start_gate gate = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, GATE_WAIT};
  int started = 1;
  int err = 0;
  for (; started != threads; started++) {
    workers[started].gate = &gate;
    err = pthread_create(&workers[started].tid, NULL, worker_thread, &workers[started]);
    if (err != 0) {
      break;
    }
  }
  if (err != 0) {
    open_gate(&gate, GATE_ABORT);
    for (int i = 1; i != started; i++) {
      pthread_join(workers[i].tid, NULL);
    }
    for (int i = 0; i != threads; i++) {
      close(workers[i].fd);
    }
    free(workers);
    errno = err;
    return -1;
  }

  (void)signal(SIGPIPE, SIG_IGN);
  open_gate(&gate, GATE_RUN);
  int status = run_worker(&workers[0]) != NULL ? 0 : -1;
  for (int i = 1; i != threads; i++) {
    void *ret = NULL;
    pthread_join(workers[i].tid, &ret);
    if (ret == NULL) {
      status = -1;
    }
  }
  free(workers);
  return status;
}

/** Return a borrowed method string, valid only until the request handler returns. */
http_str http_req_method(const http_request *r) {
  const h2o_req_t *req = r->req;
  return (http_str){req->method.base, req->method.len};
}

/** Return the borrowed normalized path, valid only until the request handler returns. */
http_str http_req_path(const http_request *r) {
  const h2o_req_t *req = r->req;
  return (http_str){req->path_normalized.base, req->path_normalized.len};
}

/**
 * Return the path segment that matched the "{name}" route segment, or {NULL, 0} if the route
 * has no such parameter. The value is borrowed and valid only until the handler returns.
 */
http_str http_req_param(const http_request *r, http_str name) {
  for (size_t i = 0; i != r->rt->segment_count; i++) {
    const route_segment *seg = &r->rt->segments[i];
    if (seg->is_param && h2o_memis(seg->text.ptr, seg->text.len, name.ptr, name.len)) {
      return r->values[i];
    }
  }
  return (http_str){NULL, 0};
}

/** Return the borrowed request body, valid only until the request handler returns. */
http_str http_req_body(const http_request *r) {
  const h2o_req_t *req = r->req;
  return (http_str){req->entity.base, req->entity.len};
}

/**
 * Find the first header with the lowercase name; return {NULL, 0} when absent.
 * The returned value is borrowed and valid only until the request handler returns.
 */
http_str http_req_header(const http_request *r, http_str name) {
  const h2o_req_t *req = r->req;
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

/** Return the status set by http_write; H2O starts each request with status 0. */
int http_resp_status(const http_response *w) { return ((const h2o_req_t *)w)->res.status; }

/** Send the final response with status and len bytes of body; call once per request. */
void http_write(http_response *w, int status, const void *body, size_t len) {
  h2o_req_t *req = (h2o_req_t *)w;
  req->res.status = status;
  req->res.reason = reason_of(status);
  req->res.content_length = len;
  h2o_send_inline(req, body, len);
}
