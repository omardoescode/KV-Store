#ifndef KV_HANDLERS_H
#define KV_HANDLERS_H

#include "http/http.h"
#include "kv_store.h"
#include "log/log.h"

/**
 * Register GET/PUT/DELETE handlers for /kv/<key> on server, backed by store.
 * Each request logs one line "<method> <path> <status>" to log, which must outlive server.
 */
void kv_register_routes(http_server *server, kv_store *store, logger *log);

#endif /* KV_HANDLERS_H */
