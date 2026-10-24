#ifndef HTTP_ROUTER_H
#define HTTP_ROUTER_H

#include <stddef.h>

#include "http/http.h"

#define ROUTER_MAX_SEGMENTS 32

typedef struct {
  // The parameter name of a "{name}" segment, else the literal segment text.
  http_str text;
  int is_param;
} route_segment;

typedef struct {
  // Owns the text of segments.
  char *pattern;
  route_segment segments[ROUTER_MAX_SEGMENTS];
  size_t segment_count;
  http_method method;
  http_handler fn;
  void *arg;
} route;

typedef struct {
  // Sorted so that, at the first position where two routes differ in segment kind, the route
  // with the literal segment comes first.
  route *routes;
  size_t route_count;
  // The segment count of the longest route.
  size_t max_segments;
} router;

typedef enum { ROUTER_OK, ROUTER_NOT_FOUND, ROUTER_BAD_REQUEST } router_status;

/**
 * Add a route for method on pattern; return 0, or -1 if pattern is malformed or allocation
 * fails. pattern must start with '/'; each segment is literal text without braces or "{name}",
 * with a non-empty name that is unique in the pattern. At most ROUTER_MAX_SEGMENTS segments.
 */
int router_add(router *rt, http_method method, const char *pattern, http_handler fn, void *arg);

/** Free all routes of rt and leave it empty. */
void router_free(router *rt);

/**
 * Split path on '/' into at most rt->max_segments segments, then percent-decode each one.
 * "/" has no segments; otherwise each '/' starts one, so "/a/" has "a" and "".
 * A segment without '%' points into path; a decoded one points into buf, which needs path.len
 * bytes and may be NULL if path has no '%'.
 * Return ROUTER_BAD_REQUEST for an invalid escape or a "." or ".." segment, ROUTER_NOT_FOUND
 * if path does not start with '/' or has more segments than the longest route; segments after
 * that limit are not examined.
 */
router_status router_split(const router *rt, http_str path, char *buf, http_str *segments,
                           size_t *count);

/**
 * Return the first route for method whose segments match segments, or NULL.
 * A literal segment matches equal text; a "{name}" segment matches any non-empty text.
 */
const route *router_match(const router *rt, int method, const http_str *segments, size_t count);

/** Return a mask with bit (1U << method) set for each method that has a route matching segments. */
unsigned router_allowed(const router *rt, const http_str *segments, size_t count);

#endif
