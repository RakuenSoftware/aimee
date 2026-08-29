#ifndef DEC_SERVER_SKILL_H
#define DEC_SERVER_SKILL_H 1

#include "server.h"

int handle_skill_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_skill_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_skill_lint(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_skill_eval(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_skill_eval_exec(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_skill_create(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_skill_edit(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_skill_patch(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_skill_archive(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_skill_pin(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_skill_lifecycle(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_skill_autostub(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

#endif /* DEC_SERVER_SKILL_H */
