#include "cp/resolve.h"

#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

int resolve_addr(const char *host_port, char addr[DRIVER_ADDR_LEN]) {
  const char *colon = strrchr(host_port, ':');
  char host[DRIVER_ADDR_LEN];
  size_t host_len = colon ? (size_t)(colon - host_port) : 0;
  if (!colon || host_len == 0 || host_len >= sizeof(host)) {
    return -1;
  }
  memcpy(host, host_port, host_len);
  host[host_len] = '\0';
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
  struct addrinfo *ai = NULL;
  if (getaddrinfo(host, colon + 1, &hints, &ai) != 0) {
    return -1;
  }
  char ip[INET_ADDRSTRLEN];
  int rc = getnameinfo(ai->ai_addr, ai->ai_addrlen, ip, sizeof(ip), NULL, 0, NI_NUMERICHOST);
  freeaddrinfo(ai);
  if (rc != 0) {
    return -1;
  }
  (void)snprintf(addr, DRIVER_ADDR_LEN, "%s:%s", ip, colon + 1);
  return 0;
}
