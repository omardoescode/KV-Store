#include "http/router.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static http_str str(const char *s) { return (http_str){s, strlen(s)}; }

static int str_is(http_str s, const char *want) {
  return s.len == strlen(want) && memcmp(s.ptr, want, s.len) == 0;
}

static void handler_a(http_response *w, const http_request *r, void *arg) {
  (void)w;
  (void)r;
  (void)arg;
}

/** Return the pattern of the route that path selects for GET, or NULL if none. */
static const char *match(const router *rt, const char *path) {
  char buf[256];
  http_str segments[ROUTER_MAX_SEGMENTS];
  size_t count = 0;
  if (router_split(rt, str(path), buf, segments, &count) != ROUTER_OK) {
    return NULL;
  }
  const route *found = router_match(rt, HTTP_GET, segments, count);
  return found != NULL ? found->pattern : NULL;
}

static void test_param_precedence(void) {
  router rt = {0};
  assert(router_add(&rt, HTTP_GET, "/{a}/{b}", handler_a, NULL) == 0);
  assert(router_add(&rt, HTTP_GET, "/{x}", handler_a, NULL) == 0);
  assert(router_add(&rt, HTTP_GET, "/{k}/meta", handler_a, NULL) == 0);

  assert(strcmp(match(&rt, "/foo/meta"), "/{k}/meta") == 0);
  assert(strcmp(match(&rt, "/foo/bar"), "/{a}/{b}") == 0);
  assert(strcmp(match(&rt, "/foo"), "/{x}") == 0);
  router_free(&rt);
  printf("  test_param_precedence OK\n");
}

static void test_literal_precedence_any_order(void) {
  const char *patterns[] = {"/kv/{key}", "/{a}/{b}", "/kv/stats", "/{a}/stats",
                            "/ping",     "/{x}",     "/"};
  size_t n = sizeof(patterns) / sizeof(patterns[0]);
  for (size_t shift = 0; shift != n; shift++) {
    router rt = {0};
    for (size_t i = 0; i != n; i++) {
      assert(router_add(&rt, HTTP_GET, patterns[(i + shift) % n], handler_a, NULL) == 0);
    }
    assert(strcmp(match(&rt, "/kv/stats"), "/kv/stats") == 0);
    assert(strcmp(match(&rt, "/kv/k1"), "/kv/{key}") == 0);
    assert(strcmp(match(&rt, "/db/stats"), "/{a}/stats") == 0);
    assert(strcmp(match(&rt, "/db/other"), "/{a}/{b}") == 0);
    assert(strcmp(match(&rt, "/ping"), "/ping") == 0);
    assert(strcmp(match(&rt, "/pong"), "/{x}") == 0);
    assert(strcmp(match(&rt, "/"), "/") == 0);
    router_free(&rt);
  }
  printf("  test_literal_precedence_any_order OK\n");
}

static void test_exact_segments(void) {
  router rt = {0};
  assert(router_add(&rt, HTTP_GET, "/kv/{key}", handler_a, NULL) == 0);
  assert(match(&rt, "/kv") == NULL);
  assert(match(&rt, "/kv/") == NULL);
  assert(match(&rt, "/kv/a/") == NULL);
  assert(match(&rt, "/kv/a/b") == NULL);
  assert(match(&rt, "") == NULL);
  assert(match(&rt, "kv/a") == NULL);
  assert(strcmp(match(&rt, "/kv/a"), "/kv/{key}") == 0);

  char buf[16];
  http_str segments[ROUTER_MAX_SEGMENTS];
  size_t count = 0;
  assert(router_split(&rt, str("/kv/a"), buf, segments, &count) == ROUTER_OK);
  assert(router_match(&rt, HTTP_PUT, segments, count) == NULL);
  assert(router_allowed(&rt, segments, count) == 1U << HTTP_GET);
  assert(router_add(&rt, HTTP_DELETE, "/kv/{key}", handler_a, NULL) == 0);
  assert(router_allowed(&rt, segments, count) == ((1U << HTTP_GET) | (1U << HTTP_DELETE)));
  assert(router_split(&rt, str("/x/a"), buf, segments, &count) == ROUTER_OK);
  assert(router_allowed(&rt, segments, count) == 0);
  router_free(&rt);
  printf("  test_exact_segments OK\n");
}

static void test_split_decodes_each_segment(void) {
  router rt = {0};
  assert(router_add(&rt, HTTP_GET, "/{a}/{b}/{c}/{d}", handler_a, NULL) == 0);
  char buf[128];
  http_str segments[ROUTER_MAX_SEGMENTS];
  size_t count = 0;

  assert(router_split(&rt, str("/"), buf, segments, &count) == ROUTER_OK);
  assert(count == 0);

  assert(router_split(&rt, str("/kv/a%2Fb"), buf, segments, &count) == ROUTER_OK);
  assert(count == 2);
  assert(str_is(segments[0], "kv"));
  assert(str_is(segments[1], "a/b"));

  assert(router_split(&rt, str("/kv/ab%41"), buf, segments, &count) == ROUTER_OK);
  assert(count == 2);
  assert(str_is(segments[1], "abA"));

  assert(router_split(&rt, str("/k%76/%7e%20x"), buf, segments, &count) == ROUTER_OK);
  assert(count == 2);
  assert(str_is(segments[0], "kv"));
  assert(str_is(segments[1], "~ x"));

  assert(router_split(&rt, str("/a//b/"), buf, segments, &count) == ROUTER_OK);
  assert(count == 4);
  assert(str_is(segments[0], "a"));
  assert(str_is(segments[1], ""));
  assert(str_is(segments[2], "b"));
  assert(str_is(segments[3], ""));

  assert(router_split(&rt, str("/plain"), NULL, segments, &count) == ROUTER_OK);
  assert(str_is(segments[0], "plain"));
  router_free(&rt);
  printf("  test_split_decodes_each_segment OK\n");
}

static void test_split_rejects_bad_paths(void) {
  router rt = {0};
  assert(router_add(&rt, HTTP_GET, "/{a}/{b}/{c}", handler_a, NULL) == 0);
  char buf[128];
  http_str segments[ROUTER_MAX_SEGMENTS];
  size_t count = 0;
  assert(router_split(&rt, str("/kv/a%"), buf, segments, &count) == ROUTER_BAD_REQUEST);
  assert(router_split(&rt, str("/kv/a%4"), buf, segments, &count) == ROUTER_BAD_REQUEST);
  assert(router_split(&rt, str("/kv/a%zz"), buf, segments, &count) == ROUTER_BAD_REQUEST);
  assert(router_split(&rt, str("/kv/%G1/x"), buf, segments, &count) == ROUTER_BAD_REQUEST);
  assert(router_split(&rt, str("/kv/.."), buf, segments, &count) == ROUTER_BAD_REQUEST);
  assert(router_split(&rt, str("/./kv"), buf, segments, &count) == ROUTER_BAD_REQUEST);
  assert(router_split(&rt, str("/kv/%2E%2e"), buf, segments, &count) == ROUTER_BAD_REQUEST);
  assert(router_split(&rt, str(""), buf, segments, &count) == ROUTER_NOT_FOUND);
  assert(router_split(&rt, str("*"), buf, segments, &count) == ROUTER_NOT_FOUND);

  router_free(&rt);
  printf("  test_split_rejects_bad_paths OK\n");
}

static void test_split_stops_after_longest_route(void) {
  router rt = {0};
  char buf[128];
  http_str segments[ROUTER_MAX_SEGMENTS];
  size_t count = 0;
  assert(router_split(&rt, str("/"), buf, segments, &count) == ROUTER_OK);
  assert(count == 0);
  assert(router_split(&rt, str("/a"), buf, segments, &count) == ROUTER_NOT_FOUND);

  assert(router_add(&rt, HTTP_GET, "/kv/{key}", handler_a, NULL) == 0);
  assert(router_add(&rt, HTTP_GET, "/ping", handler_a, NULL) == 0);
  assert(rt.max_segments == 2);
  assert(router_split(&rt, str("/kv/a"), buf, segments, &count) == ROUTER_OK);
  assert(router_split(&rt, str("/kv/a/"), buf, segments, &count) == ROUTER_NOT_FOUND);
  assert(router_split(&rt, str("/kv/a/b"), buf, segments, &count) == ROUTER_NOT_FOUND);
  assert(router_split(&rt, str("/kv/a/%zz"), buf, segments, &count) == ROUTER_NOT_FOUND);
  assert(router_split(&rt, str("/kv/a/.."), buf, segments, &count) == ROUTER_NOT_FOUND);
  assert(router_split(&rt, str("/kv/%zz/b"), buf, segments, &count) == ROUTER_BAD_REQUEST);

  char deep[2 * (ROUTER_MAX_SEGMENTS + 1) + 1];
  for (size_t i = 0; i != ROUTER_MAX_SEGMENTS + 1; i++) {
    deep[2 * i] = '/';
    deep[(2 * i) + 1] = 'a';
  }
  deep[sizeof(deep) - 1] = '\0';
  assert(router_add(&rt, HTTP_GET, deep, handler_a, NULL) == -1);
  assert(rt.max_segments == 2);
  assert(router_split(&rt, str(deep), buf, segments, &count) == ROUTER_NOT_FOUND);
  deep[sizeof(deep) - 3] = '\0';
  assert(router_split(&rt, str(deep), buf, segments, &count) == ROUTER_NOT_FOUND);
  assert(router_add(&rt, HTTP_GET, deep, handler_a, NULL) == 0);
  assert(rt.max_segments == ROUTER_MAX_SEGMENTS);
  assert(router_split(&rt, str(deep), buf, segments, &count) == ROUTER_OK);
  assert(count == ROUTER_MAX_SEGMENTS);
  assert(strcmp(match(&rt, deep), deep) == 0);
  deep[sizeof(deep) - 3] = '/';
  assert(router_split(&rt, str(deep), buf, segments, &count) == ROUTER_NOT_FOUND);
  router_free(&rt);
  assert(rt.max_segments == 0);
  printf("  test_split_stops_after_longest_route OK\n");
}

static void test_decoded_values_match(void) {
  router rt = {0};
  assert(router_add(&rt, HTTP_GET, "/kv/{key}", handler_a, NULL) == 0);
  assert(router_add(&rt, HTTP_GET, "/ping", handler_a, NULL) == 0);
  assert(strcmp(match(&rt, "/kv/a%2Fb"), "/kv/{key}") == 0);
  assert(strcmp(match(&rt, "/p%69ng"), "/ping") == 0);
  router_free(&rt);
  printf("  test_decoded_values_match OK\n");
}

static void test_rejects_malformed_patterns(void) {
  const char *bad[] = {"",         "kv",           "/{}",     "/{key",   "/key}",    "/{{key}}",
                       "/{a{b}c}", "/{key}/{key}", "/a{key}", "/{key}b", "/{k}x{y}", "/kv/{}/meta"};
  router rt = {0};
  for (size_t i = 0; i != sizeof(bad) / sizeof(bad[0]); i++) {
    assert(router_add(&rt, HTTP_GET, bad[i], handler_a, NULL) == -1);
  }
  assert(rt.route_count == 0);

  char deep[2 * (ROUTER_MAX_SEGMENTS + 1) + 1];
  for (size_t i = 0; i != ROUTER_MAX_SEGMENTS + 1; i++) {
    deep[2 * i] = '/';
    deep[(2 * i) + 1] = 'a';
  }
  deep[sizeof(deep) - 1] = '\0';
  assert(router_add(&rt, HTTP_GET, deep, handler_a, NULL) == -1);
  deep[sizeof(deep) - 3] = '\0';
  assert(router_add(&rt, HTTP_GET, deep, handler_a, NULL) == 0);

  assert(router_add(&rt, HTTP_GET, "/{a}/{b}", handler_a, NULL) == 0);
  assert(router_add(&rt, HTTP_GET, "/", handler_a, NULL) == 0);
  assert(router_add(&rt, HTTP_GET, "/a/", handler_a, NULL) == 0);
  assert(rt.route_count == 4);
  router_free(&rt);
  printf("  test_rejects_malformed_patterns OK\n");
}

int main(void) {
  printf("http_router tests:\n");
  test_param_precedence();
  test_literal_precedence_any_order();
  test_exact_segments();
  test_split_decodes_each_segment();
  test_split_rejects_bad_paths();
  test_split_stops_after_longest_route();
  test_decoded_values_match();
  test_rejects_malformed_patterns();
  printf("all http_router tests passed\n");
  return 0;
}
