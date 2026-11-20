#ifndef CP_RESOLVE_H
#define CP_RESOLVE_H

#include "cp/driver.h"

/** Resolve "host:port" to an IPv4 "ip:port" in addr. Returns 0, or -1 if it does not resolve. */
int resolve_addr(const char *host_port, char addr[DRIVER_ADDR_LEN]);

#endif
