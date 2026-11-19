#include "cp/hclient.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "picohttpparser.h"

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((int64_t)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

/** Wait until fd is ready for events or the deadline passes. Returns 0, or -1 on timeout. */
static int wait_fd(int fd, short events, int64_t deadline) {
  for (;;) {
    int64_t left = deadline - now_ms();
    if (left <= 0) {
      return -1;
    }
    struct pollfd p = {fd, events, 0};
    int n = poll(&p, 1, (int)left);
    if (n > 0) {
      return 0;
    }
    if (n < 0 && errno != EINTR) {
      return -1;
    }
  }
}

/** Start a non-blocking connect of fd to sa and wait for it. Returns 0 or -1. */
static int connect_wait(int fd, const struct sockaddr *sa, socklen_t len, int64_t deadline) {
  if (connect(fd, sa, len) == 0) {
    return 0;
  }
  if (errno != EINPROGRESS || wait_fd(fd, POLLOUT, deadline) != 0) {
    return -1;
  }
  int err = 0;
  socklen_t err_len = sizeof(err);
  return getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) == 0 && err == 0 ? 0 : -1;
}

/** Open a non-blocking socket connected to addr. Returns the fd, or -1. */
static int dial(const char *addr, int64_t deadline) {
  if (strncmp(addr, "unix:", 5) == 0) {
    struct sockaddr_un sun = {.sun_family = AF_UNIX};
    if (strlen(addr + 5) >= sizeof(sun.sun_path)) {
      return -1;
    }
    memcpy(sun.sun_path, addr + 5, strlen(addr + 5) + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd >= 0 && connect_wait(fd, (struct sockaddr *)&sun, sizeof(sun), deadline) != 0) {
      close(fd);
      fd = -1;
    }
    return fd;
  }
  const char *colon = strrchr(addr, ':');
  if (!colon) {
    return -1;
  }
  char host[256];
  size_t host_len = (size_t)(colon - addr);
  if (host_len >= 2 && addr[0] == '[') {
    addr++;
    host_len -= 2;
  }
  if (host_len >= sizeof(host)) {
    return -1;
  }
  memcpy(host, addr, host_len);
  host[host_len] = '\0';
  struct addrinfo hints = {.ai_socktype = SOCK_STREAM};
  struct addrinfo *ai = NULL;
  if (getaddrinfo(host, colon + 1, &hints, &ai) != 0) {
    return -1;
  }
  int fd = -1;
  for (struct addrinfo *p = ai; p && fd < 0; p = p->ai_next) {
    fd = socket(p->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd >= 0 && connect_wait(fd, p->ai_addr, p->ai_addrlen, deadline) != 0) {
      close(fd);
      fd = -1;
    }
  }
  freeaddrinfo(ai);
  return fd;
}

static int send_all(int fd, const char *data, size_t len, int64_t deadline) {
  while (len) {
    ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
    if (n > 0) {
      data += n;
      len -= (size_t)n;
    } else if ((n < 0 && errno != EAGAIN && errno != EINTR) ||
               wait_fd(fd, POLLOUT, deadline) != 0) {
      return -1;
    }
  }
  return 0;
}

/** Read until EOF into a malloc buffer. Returns 0 or -1. */
static int read_all(int fd, int64_t deadline, char **out, size_t *out_len) {
  size_t cap = 4096;
  size_t len = 0;
  char *buf = malloc(cap + 1);
  while (buf) {
    if (len == cap) {
      char *bigger = realloc(buf, (cap * 2) + 1);
      if (!bigger) {
        break;
      }
      buf = bigger;
      cap *= 2;
    }
    ssize_t n = recv(fd, buf + len, cap - len, 0);
    if (n > 0) {
      len += (size_t)n;
    } else if (n == 0) {
      buf[len] = '\0'; // NOLINT(clang-analyzer-security.ArrayBound): len <= cap, buf has cap + 1
      *out = buf;
      *out_len = len;
      return 0;
    } else if ((errno != EAGAIN && errno != EINTR) || wait_fd(fd, POLLIN, deadline) != 0) {
      break;
    }
  }
  free(buf);
  return -1;
}

/** Parse the response in raw into out, taking the body from raw. Returns 0 or -1. */
static int parse(char *raw, size_t raw_len, hclient_resp *out) {
  int minor = 0;
  int status = 0;
  const char *msg = NULL;
  size_t msg_len = 0;
  struct phr_header headers[64];
  size_t header_count = sizeof(headers) / sizeof(headers[0]);
  int head =
      phr_parse_response(raw, raw_len, &minor, &status, &msg, &msg_len, headers, &header_count, 0);
  if (head <= 0) {
    return -1;
  }
  int chunked = 0;
  for (size_t i = 0; i != header_count; i++) {
    if (headers[i].name_len == 17 && strncasecmp(headers[i].name, "transfer-encoding", 17) == 0 &&
        headers[i].value_len >= 7 && strncasecmp(headers[i].value, "chunked", 7) == 0) {
      chunked = 1;
    }
  }
  size_t body_len = raw_len - (size_t)head;
  memmove(raw, raw + head, body_len);
  if (chunked) {
    struct phr_chunked_decoder dec = {0};
    dec.consume_trailer = 1;
    if (phr_decode_chunked(&dec, raw, &body_len) < 0) {
      return -1;
    }
  }
  raw[body_len] = '\0';
  *out = (hclient_resp){status, raw, body_len};
  return 0;
}

int hclient_do(const char *addr, const hclient_req *req, hclient_resp *out) {
  int64_t deadline = now_ms() + req->timeout_ms;
  int fd = dial(addr, deadline);
  if (fd < 0) {
    return -1;
  }
  char head[1024];
  int head_len = snprintf(head, sizeof(head),
                          "%s %s HTTP/1.1\r\nhost: kv\r\nconnection: close\r\n"
                          "content-type: application/json\r\ncontent-length: %zu\r\n%s\r\n",
                          req->method, req->path, req->body_len, req->headers ? req->headers : "");
  char *raw = NULL;
  size_t raw_len = 0;
  int rc = -1;
  if (head_len > 0 && (size_t)head_len < sizeof(head) &&
      send_all(fd, head, (size_t)head_len, deadline) == 0 &&
      send_all(fd, req->body, req->body_len, deadline) == 0 &&
      read_all(fd, deadline, &raw, &raw_len) == 0) {
    rc = parse(raw, raw_len, out);
  }
  close(fd);
  if (rc != 0) {
    free(raw);
  }
  return rc;
}
