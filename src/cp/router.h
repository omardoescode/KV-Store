#ifndef CP_ROUTER_H
#define CP_ROUTER_H

#include "cp/cluster.h"
#include "http/http.h"
#include "log/log.h"

/**
 * Register the public /<key> handlers and the /_cp/ admin handlers of cp on server. Ops on one
 * key run one at a time, in arrival order; ops on different keys run in parallel. c, cfg and log
 * must outlive server.
 */
void cp_router_register(http_server *server, cluster *c, const cp_config *cfg, logger *log);

/** Free the per-key table; call after the server stopped. */
void cp_router_free(void);

#endif
