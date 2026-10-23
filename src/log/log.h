#ifndef LOG_LOG_H
#define LOG_LOG_H

typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } log_level;

/** A log sink passed to its users: call log with ctx. */
typedef struct {
  void (*log)(void *ctx, log_level level, const char *fmt, ...)
      __attribute__((format(printf, 3, 4)));
  void *ctx;
} logger;

#endif
