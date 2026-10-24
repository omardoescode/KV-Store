#include "http/router.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/** Return the value of hex digit c, or -1 if c is not one. */
static int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

/**
 * Return 1 if a must be tried before b: at the first position where the segment kinds differ,
 * the literal one wins; if one route is a prefix of the other by kind, the shorter one wins.
 * Routes of different lengths never match the same path, so that last rule only keeps the
 * order a strict weak ordering.
 */
static int route_before(const route *a, const route *b) {
  size_t n = a->segment_count < b->segment_count ? a->segment_count : b->segment_count;
  for (size_t i = 0; i != n; i++) {
    if (a->segments[i].is_param != b->segments[i].is_param) {
      return !a->segments[i].is_param;
    }
  }
  return a->segment_count < b->segment_count;
}

/** Return 1 if seg has no stray braces and, as a parameter, a non-empty name new to r. */
static int segment_valid(const route *r, route_segment seg) {
  if ((seg.is_param && seg.text.len == 0) || memchr(seg.text.ptr, '{', seg.text.len) != NULL ||
      memchr(seg.text.ptr, '}', seg.text.len) != NULL) {
    return 0;
  }
  for (size_t i = 0; seg.is_param && i != r->segment_count; i++) {
    const route_segment *prev = &r->segments[i];
    if (prev->is_param && prev->text.len == seg.text.len &&
        memcmp(prev->text.ptr, seg.text.ptr, seg.text.len) == 0) {
      return 0;
    }
  }
  return 1;
}

/** Parse pattern into a route and insert it by precedence; return -1 if it is malformed. */
int router_add(router *rt, http_method method, const char *pattern, http_handler fn, void *arg) {
  if (pattern[0] != '/') {
    return -1;
  }
  route r = {.method = method, .fn = fn, .arg = arg};
  r.pattern = strdup(pattern);
  if (r.pattern == NULL) {
    return -1;
  }
  size_t len = strlen(r.pattern);
  const char *p = r.pattern + 1;
  const char *end = r.pattern + len;
  while (len > 1) {
    if (r.segment_count == ROUTER_MAX_SEGMENTS) {
      free(r.pattern);
      return -1;
    }
    const char *slash = memchr(p, '/', (size_t)(end - p));
    const char *seg_end = slash != NULL ? slash : end;
    route_segment seg = {{p, (size_t)(seg_end - p)}, 0};
    if (seg.text.len >= 2 && p[0] == '{' && seg_end[-1] == '}') {
      seg = (route_segment){{p + 1, seg.text.len - 2}, 1};
    }
    if (!segment_valid(&r, seg)) {
      free(r.pattern);
      return -1;
    }
    r.segments[r.segment_count++] = seg;
    if (slash == NULL) {
      break;
    }
    p = slash + 1;
  }

  route *routes = realloc(rt->routes, sizeof(*routes) * (rt->route_count + 1));
  if (routes == NULL) {
    free(r.pattern);
    return -1;
  }
  rt->routes = routes;
  if (r.segment_count > rt->max_segments) {
    rt->max_segments = r.segment_count;
  }
  size_t i = rt->route_count++;
  for (; i != 0 && route_before(&r, &routes[i - 1]); i--) {
    routes[i] = routes[i - 1];
  }
  routes[i] = r;
  return 0;
}

/** Free the patterns and the route array of rt, then reset it to empty. */
void router_free(router *rt) {
  for (size_t i = 0; i != rt->route_count; i++) {
    free(rt->routes[i].pattern);
  }
  free(rt->routes);
  rt->routes = NULL;
  rt->route_count = 0;
  rt->max_segments = 0;
}

/** Percent-decode raw into out; return the decoded length, or -1 for an invalid escape. */
static ptrdiff_t percent_decode(http_str raw, char *out) {
  const char *end = raw.ptr + raw.len;
  ptrdiff_t n = 0;
  for (const char *q = raw.ptr; q != end; n++) {
    if (*q != '%') {
      out[n] = *q++;
      continue;
    }
    int hi = end - q >= 3 ? hex_value(q[1]) : -1;
    int lo = hi != -1 ? hex_value(q[2]) : -1;
    if (lo == -1) {
      return -1;
    }
    out[n] = (char)((hi * 16) + lo);
    q += 3;
  }
  return n;
}

/** Split path on raw '/' first, so that "%2F" stays inside its segment, then decode it. */
router_status router_split(const router *rt, http_str path, char *buf, http_str *segments,
                           size_t *count) {
  if (path.len == 0 || path.ptr[0] != '/') {
    return ROUTER_NOT_FOUND;
  }
  *count = 0;
  const char *p = path.ptr + 1;
  const char *end = path.ptr + path.len;
  while (path.len > 1) {
    if (*count == rt->max_segments) {
      return ROUTER_NOT_FOUND;
    }
    const char *slash = memchr(p, '/', (size_t)(end - p));
    const char *seg_end = slash != NULL ? slash : end;
    http_str seg = {p, (size_t)(seg_end - p)};
    if (memchr(seg.ptr, '%', seg.len) != NULL) {
      // Same offset as in path: a decoded segment is never longer, so segments cannot overlap.
      char *out = buf + (p - path.ptr);
      ptrdiff_t n = percent_decode(seg, out);
      if (n == -1) {
        return ROUTER_BAD_REQUEST;
      }
      seg = (http_str){out, (size_t)n};
    }
    // h2o resolves dot segments in path_normalized; reject them so both views agree.
    if ((seg.len == 1 && seg.ptr[0] == '.') ||
        (seg.len == 2 && seg.ptr[0] == '.' && seg.ptr[1] == '.')) {
      return ROUTER_BAD_REQUEST;
    }
    segments[(*count)++] = seg;
    if (slash == NULL) {
      break;
    }
    p = slash + 1;
  }
  return ROUTER_OK;
}

/** Return the first route, in precedence order, for method with segments; NULL if none. */
static bool route_matches(const route *candidate, const http_str *segments, size_t count) {
  if (candidate->segment_count != count) {
    return false;
  }
  for (size_t i = 0; i != count; i++) {
    const route_segment *seg = &candidate->segments[i];
    if (seg->is_param ? segments[i].len == 0
                      : seg->text.len != segments[i].len ||
                            memcmp(seg->text.ptr, segments[i].ptr, segments[i].len) != 0) {
      return false;
    }
  }
  return true;
}

const route *router_match(const router *rt, int method, const http_str *segments, size_t count) {
  for (size_t r = 0; r != rt->route_count; r++) {
    const route *candidate = &rt->routes[r];
    if ((int)candidate->method == method && route_matches(candidate, segments, count)) {
      return candidate;
    }
  }
  return NULL;
}

unsigned router_allowed(const router *rt, const http_str *segments, size_t count) {
  unsigned mask = 0;
  for (size_t r = 0; r != rt->route_count; r++) {
    if (route_matches(&rt->routes[r], segments, count)) {
      mask |= 1U << rt->routes[r].method;
    }
  }
  return mask;
}
