#ifndef KV_HANDLERS_H
#define KV_HANDLERS_H

#include <stdbool.h>

#include "http/http.h"
#include "kv_store.h"
#include "log/log.h"

/**
 * Register GET/POST/DELETE handlers for /<key> and the /_internal/ handlers for cp on server,
 * backed by store. With sync_ack, POST and DELETE respond after the change is committed.
 * Each request logs one line "<method> <path> <status>" to log, which must outlive server.
 */
void kv_register_routes(http_server *server, kv_store *store, logger *log, bool sync_ack);

#endif /* KV_HANDLERS_H */
