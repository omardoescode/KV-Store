#include "http/http.h"

#include <errno.h>
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

typedef struct {
  h2o_iovec_t path;
  http_method method;
  http_handler fn;
  void *arg;
} route;

struct http_server {
  h2o_globalconf_t config;
  // Sorted by path length, longest first, so that the first match is the most specific.
  route *routes;
  size_t route_count;
};

typedef struct {
  h2o_handler_t super;
  http_server *server;
} dispatcher;

enum { GATE_WAIT, GATE_RUN, GATE_ABORT };

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

/** Return 1 if path is the route path or is below it. */
static int path_matches(const route *rt, h2o_iovec_t path) {
  return path.len >= rt->path.len && memcmp(path.base, rt->path.base, rt->path.len) == 0 &&
         (rt->path.base[rt->path.len - 1] == '/' || path.len == rt->path.len ||
          path.base[rt->path.len] == '/');
}

/** Dispatch to the most specific route that matches path and method; return -1 if none. */
static int on_req(h2o_handler_t *self, h2o_req_t *req) {
  const http_server *s = ((dispatcher *)self)->server;
  int method = method_of(req->method);
  for (size_t i = 0; i != s->route_count; i++) {
    const route *rt = &s->routes[i];
    if ((int)rt->method == method && path_matches(rt, req->path_normalized)) {
      rt->fn((http_response *)req, (const http_request *)req, rt->arg);
      return 0;
    }
  }
  return -1;
}

/** Allocate and initialize a server with a default host and no Server response header. */
http_server *http_server_new(void) {
  http_server *s = h2o_mem_alloc(sizeof(*s));
  s->routes = NULL;
  s->route_count = 0;
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

/**
 * Register a method handler on a path, keeping routes sorted from longest path to shortest.
 * Call before serving; the path also matches descendants and arg is passed to the handler.
 */
void http_handle(http_server *s, http_method method, const char *path, http_handler handler,
                 void *arg) {
  route rt = {h2o_strdup(NULL, path, SIZE_MAX), method, handler, arg};
  s->routes = h2o_mem_realloc(s->routes, sizeof(*s->routes) * (s->route_count + 1));

  size_t i = s->route_count++;
  for (; i != 0 && s->routes[i - 1].path.len < rt.path.len; i--) {
    s->routes[i] = s->routes[i - 1];
  }
  s->routes[i] = rt;
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

  w->listener = h2o_evloop_socket_create(w->ctx.loop, w->fd, H2O_SOCKET_FLAG_DONT_READ);
  w->listener->data = w;
  h2o_socket_read_start(w->listener, on_accept);

  // A signal can interrupt the wait; that is not a reason to stop the loop.
  while (h2o_evloop_run(w->ctx.loop, INT32_MAX) == 0 || errno == EINTR) {
  }
  return NULL;
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

/**
 * Bind host:port and serve with threads event loops, including the calling thread.
 * Requires threads >= 1; returns -1 on setup failure or if the calling event loop stops.
 * On failure, all threads and listeners of this call are stopped and closed.
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
