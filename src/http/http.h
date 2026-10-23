#ifndef HTTP_HTTP_H
#define HTTP_HTTP_H

#include <stddef.h>

typedef struct http_server http_server;
typedef struct http_request http_request;
typedef struct http_response http_response;

// Not NUL-terminated. Request strings are valid only until the handler returns.
typedef struct {
  const char *ptr;
  size_t len;
} http_str;

#define HTTP_STR(literal) ((http_str){(literal), sizeof(literal) - 1})

typedef enum { HTTP_GET, HTTP_HEAD, HTTP_POST, HTTP_PUT, HTTP_PATCH, HTTP_DELETE } http_method;

// Runs on an event loop thread: must not block, must call http_write once before it returns.
typedef void (*http_handler)(http_response *w, const http_request *r, void *arg);

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
 * Return the CPU count that this process can use: the smaller of the CPU affinity and the
 * cgroup CPU quota on Linux, the online CPU count on other systems.
 */
int http_default_threads(void);

/**
 * Serve addr ("host:port") with threads event loops, one on the calling thread.
 * Requires threads >= 1; returns -1 on failure or if the calling event loop stops.
 */
int http_listen_and_serve(http_server *s, const char *addr, int threads);

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
/** Send status and len bytes of body as the final response; call once per request. */
void http_write(http_response *w, int status, const void *body, size_t len);

#endif
