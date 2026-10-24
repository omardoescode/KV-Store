#ifndef HTTP_HTTP_H
#define HTTP_HTTP_H

#include <stddef.h>

typedef struct http_server http_server;
typedef struct http_request http_request;
typedef struct http_response http_response;
typedef struct http_group http_group;
typedef struct http_chain http_chain;

// Not NUL-terminated. Request strings are valid only until the handler returns.
typedef struct {
  const char *ptr;
  size_t len;
} http_str;

#define HTTP_STR(literal) ((http_str){(literal), sizeof(literal) - 1})

typedef enum { HTTP_GET, HTTP_HEAD, HTTP_POST, HTTP_PUT, HTTP_PATCH, HTTP_DELETE } http_method;

// Runs on an event loop thread: must not block, must call http_write once before it returns.
typedef void (*http_handler)(http_response *w, const http_request *r, void *arg);

// Runs the rest of the chain: the next middleware, or the handler.
typedef void (*http_next)(http_response *w, const http_request *r, http_chain *chain);

/**
 * Runs on an event loop thread around a handler: must not block, and must either call
 * next(w, r, chain) once or call http_write once before it returns. After next returns,
 * the response is written and http_resp_status gives its status.
 */
typedef void (*http_middleware)(http_response *w, const http_request *r, http_next next,
                                http_chain *chain, void *ctx);

/** Allocate and initialize a server; register routes before starting it. */
http_server *http_server_new(void);

/** Free a server and its routes; call only when no event loop of it runs. */
void http_server_free(http_server *s);

/**
 * Register handler for method on path, passing arg to each call. path must start with '/'.
 * A "{name}" segment matches one non-empty path segment; read it with http_req_param.
 * Names must be non-empty and unique in path; literal segments must not contain braces.
 * At most 32 segments. A malformed path aborts the process with a message.
 * The request path is split on '/', then each segment is percent-decoded and compared;
 * an invalid escape or a "." or ".." segment gets 400.
 * Paths match exactly: "/" matches only "/", and "/a/" does not match "/a" or "/{name}".
 * At the first position where two routes differ, a static segment wins over "{name}".
 * All calls must come before http_listen_and_serve.
 */
void http_handle(http_server *s, http_method method, const char *path, http_handler handler,
                 void *arg);

/**
 * Return a group, owned by s, whose routes get prefix in front of their path.
 * prefix must start with '/' and not end with '/'; a malformed prefix aborts the process.
 * The route path "/" of group "/kv" is "/kv/", which does not match "/kv".
 */
http_group *http_group_new(http_server *s, const char *prefix);

/** Like http_handle, on the prefix of g plus path; only g's routes run g's middleware. */
void http_group_handle(http_group *g, http_method method, const char *path, http_handler handler,
                       void *arg);

/**
 * Add middleware mw, passing ctx to each call, to every route of g, also routes added before.
 * Middleware runs in the order added, after the middleware of the server.
 * All calls must come before http_listen_and_serve.
 */
void http_group_use(http_group *g, http_middleware mw, void *ctx);

/** Like http_group_use, for every route of s; it runs before any group middleware. */
void http_use(http_server *s, http_middleware mw, void *ctx);

/**
 * Return the CPU count that this process can use: the smaller of the CPU affinity and the
 * cgroup CPU quota on Linux, the online CPU count on other systems.
 */
int http_default_threads(void);

/**
 * Serve addr ("host:port") with threads event loops, one on the calling thread.
 * Requires threads >= 1. Returns 0 after http_server_stop, when every loop has stopped;
 * returns -1 on setup failure or if an event loop fails. Joins all its threads before it returns.
 */
int http_listen_and_serve(http_server *s, const char *addr, int threads);

/**
 * Make http_listen_and_serve stop accepting connections, wait up to 5 s for open requests to
 * finish, and return 0. Async-signal-safe; any thread, also before the server starts.
 */
void http_server_stop(http_server *s);

/** Return the borrowed request method, valid until the handler returns. */
http_str http_req_method(const http_request *r);
/** Return the borrowed request normalized path, valid until the handler returns. */
http_str http_req_path(const http_request *r);
/**
 * Return the path segment that matched the "{name}" route segment, or {NULL, 0} if absent.
 * The value is percent-decoded, borrowed, and valid until the handler returns.
 */
http_str http_req_param(const http_request *r, http_str name);
/** Return the borrowed request body, valid until the handler returns. */
http_str http_req_body(const http_request *r);
/**
 * Return the first header matching lowercase name, or {NULL, 0} if absent.
 * The returned value is borrowed and valid until the handler returns.
 */
http_str http_req_header(const http_request *r, http_str name);

/**
 * Set or replace a response header, copying value.
 * name must be lowercase and stay valid until http_write, e.g. a string literal.
 */
void http_set_header(http_response *w, http_str name, http_str value);
/** Return the status of the response, or 0 if it is not written yet. */
int http_resp_status(const http_response *w);
/** Send status and len bytes of body as the final response; call once per request. */
void http_write(http_response *w, int status, const void *body, size_t len);

#endif
