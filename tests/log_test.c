#include "log/async_log.h"
#include "log/log_ring.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static void push(log_ring *r, char c) {
  log_record *rec = log_ring_reserve(r);
  assert(rec != NULL);
  rec->text[0] = c;
  rec->len = 1;
  log_ring_commit(r);
}

static char pop(log_ring *r) {
  const log_record *rec = log_ring_peek(r);
  assert(rec != NULL && rec->len == 1);
  char c = rec->text[0];
  log_ring_release(r);
  return c;
}

static void test_ring_wraparound(void) {
  log_ring r;
  assert(log_ring_init(&r, 4) == 0);
  assert(log_ring_peek(&r) == NULL);
  for (int round = 0; round != 5; round++) {
    push(&r, 'a');
    push(&r, 'b');
    push(&r, 'c');
    assert(pop(&r) == 'a');
    assert(pop(&r) == 'b');
    assert(pop(&r) == 'c');
    assert(log_ring_peek(&r) == NULL);
  }
  assert(log_ring_take_dropped(&r) == 0);
  log_ring_free(&r);
  printf("  test_ring_wraparound OK\n");
}

static void test_ring_full_drops(void) {
  log_ring r;
  assert(log_ring_init(&r, 4) == 0);
  push(&r, '1');
  push(&r, '2');
  push(&r, '3');
  push(&r, '4');
  assert(log_ring_reserve(&r) == NULL);
  assert(log_ring_reserve(&r) == NULL);
  assert(log_ring_take_dropped(&r) == 2);
  assert(log_ring_take_dropped(&r) == 0);

  assert(pop(&r) == '1');
  push(&r, '5');
  assert(log_ring_reserve(&r) == NULL);
  assert(pop(&r) == '2');
  assert(pop(&r) == '3');
  assert(pop(&r) == '4');
  assert(pop(&r) == '5');
  assert(log_ring_peek(&r) == NULL);
  assert(log_ring_take_dropped(&r) == 1);
  log_ring_free(&r);
  printf("  test_ring_full_drops OK\n");
}

static void test_ring_rejects_bad_capacity(void) {
  log_ring r;
  assert(log_ring_init(&r, 0) == -1);
  assert(log_ring_init(&r, 3) == -1);
  printf("  test_ring_rejects_bad_capacity OK\n");
}

static void *log_from_thread(void *arg) {
  logger *log = arg;
  log->log(log->ctx, LOG_INFO, "from %s", "thread");
  return NULL;
}

static void test_async_log_writes_lines(void) {
  FILE *out = tmpfile();
  assert(out != NULL);
  async_log *l = async_log_new(out, 1);
  assert(l != NULL);
  logger log = async_log_logger(l);
  log.log(log.ctx, LOG_INFO, "GET %s %d", "/kv/a", 200);
  log.log(log.ctx, LOG_ERROR, "line %d", 2);

  // The only ring belongs to this thread, so the other thread's line is dropped.
  pthread_t tid;
  assert(pthread_create(&tid, NULL, log_from_thread, &log) == 0);
  assert(pthread_join(tid, NULL) == 0);
  async_log_close(l);

  rewind(out);
  char line[256];
  assert(fgets(line, sizeof(line), out) != NULL);
  // "YYYY-MM-DDTHH:MM:SS.mmm " prefix.
  assert(strlen(line) > 24 && line[4] == '-' && line[10] == 'T' && line[19] == '.');
  assert(strcmp(line + 24, "INFO GET /kv/a 200\n") == 0);
  assert(fgets(line, sizeof(line), out) != NULL);
  assert(strcmp(line + 24, "ERROR line 2\n") == 0);
  assert(fgets(line, sizeof(line), out) != NULL);
  assert(strcmp(line + 24, "WARN dropped 1 lines\n") == 0);
  assert(fgets(line, sizeof(line), out) == NULL);
  (void)fclose(out);
  printf("  test_async_log_writes_lines OK\n");
}

static void test_async_log_truncates_long_lines(void) {
  FILE *out = tmpfile();
  assert(out != NULL);
  async_log *l = async_log_new(out, 1);
  assert(l != NULL);
  logger log = async_log_logger(l);
  char big[400];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  log.log(log.ctx, LOG_DEBUG, "%s", big);
  async_log_close(l);

  rewind(out);
  char line[512];
  assert(fgets(line, sizeof(line), out) != NULL);
  assert(strncmp(line + 24, "DEBUG ", 6) == 0);
  assert(strlen(line + 30) == 231 + 1);
  (void)fclose(out);
  printf("  test_async_log_truncates_long_lines OK\n");
}

int main(void) {
  printf("log tests:\n");
  test_ring_wraparound();
  test_ring_full_drops();
  test_ring_rejects_bad_capacity();
  test_async_log_writes_lines();
  test_async_log_truncates_long_lines();
  printf("all log tests passed\n");
  return 0;
}
