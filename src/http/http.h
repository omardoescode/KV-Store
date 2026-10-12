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

http_server *http_server_new(void);

// A path matches itself and all paths below it.
// All calls must come before http_listen_and_serve.
void http_handle(http_server *s, http_method method, const char *path, http_handler handler,
                 void *arg);

// addr: "host:port". Runs `threads` event loops, one of them on the calling thread.
// Returns -1 on failure, does not return on success.
int http_listen_and_serve(http_server *s, const char *addr, int threads);

http_str http_req_method(const http_request *r);
http_str http_req_path(const http_request *r);
http_str http_req_body(const http_request *r);
// name must be lowercase. Returns {NULL, 0} if the header is absent.
http_str http_req_header(const http_request *r, http_str name);

// name must be lowercase and must stay valid until http_write, e.g. a string literal.
void http_set_header(http_response *w, http_str name, http_str value);
void http_write(http_response *w, int status, const void *body, size_t len);

#endif
