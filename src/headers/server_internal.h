#ifndef SERVER_MAIN_INTERNAL_H
#define SERVER_MAIN_INTERNAL_H
#include "server.h"
/* Cross-TU decls split from server.c (was server_*.inc). */
/* promoted cross-TU (former .inc statics) */
int handle_api_disable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_api_enable(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_api_rotate_bearer(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_api_enroll_bearer(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_api_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
/* Attach the kb block to a server.health response (see server_api_status.c). */
void server_health_add_kb(cJSON *resp);
/* Rows one eval.candidates response may carry. Bounds both the request's
 * `limit` and the stack buffer that serves it. */
#define EVAL_CANDIDATES_MAX_ROWS 100
int handle_eval_candidates(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_eval_candidates_update(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_eval_results(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_eval_run(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_insights_overview(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_model_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_model_refresh(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_model_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_provider_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_provider_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_provider_models(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_provider_quota(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_provider_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_provider_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_provider_test(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
void server_seed_config_defaults(void);

#endif
