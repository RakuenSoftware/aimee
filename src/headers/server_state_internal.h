#ifndef SERVER_STATE_INTERNAL_H
#define SERVER_STATE_INTERNAL_H
#include <stddef.h>
#include "cJSON.h"
#include "server.h"
/* Cross-TU declarations for the server_state cluster (server_state.c + the .c
 * files split out of it). Formerly file-local statics shared by textual .inc. */
/* promoted cross-TU (former .inc statics) */
/* 0: local user, 1: explicit KB, -1: invalid selector. Never inferred from IDs. */
int server_memory_store_selection(const cJSON *req);
int send_and_free(server_conn_t *conn, cJSON *resp);
int workspace_rpc_args(cJSON *req, char **argv, int max);
void ws_git_line(const char *const argv[], char *out, size_t outsz);

/* Did a workspace scan actually index the project? Pure decision, split out so it
 * can be tested without a live kb — it reports success to the user, and it was
 * getting that wrong.
 *
 * `rc` is kb_client_index_scan's return; `skipped`, `inspected` and `files` come
 * from its result. Returns 1 when the project is genuinely indexed, else 0. */
int server_workspace_scan_indexed(int rc, int skipped, int inspected, int files);

/* cJSON stores numbers as doubles. Reject fractional and unrepresentable IDs
 * instead of truncating them into a different row's integer primary key.
 * Promoted from a server_state.c static so the typed-fact handlers in
 * server_facts.c validate ids the same way rather than growing a second,
 * subtly-different guard. 0 on success (*out filled), -1 on a bad id. */
int memory_request_positive_id(cJSON *req, const char *field, int64_t *out);

#endif /* SERVER_STATE_INTERNAL_H */
