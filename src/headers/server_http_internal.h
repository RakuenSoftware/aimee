#ifndef SERVER_HTTP_INTERNAL_H
#define SERVER_HTTP_INTERNAL_H
#include <stdint.h>
#include "server_http.h"
#include <stddef.h>
#include <stdatomic.h>
#include "cJSON.h"
#include "persona.h"
/* Cross-TU declarations for the server_http cluster (server_http.c + the .c files
 * split out of it: conn_worker / sse / response / config_routes). Formerly these
 * were file-local statics shared by textual .inc inclusion. */
/* promoted cross-TU (former .inc statics) */
int conn_offload(int fd, int is_tcp, int is_tls);
cJSON *persona_to_json(const persona_t *p);
void request_id_header(char *dst, size_t n, const char *request_id);
void retrieval_event_header(char *dst, size_t n);
int route_persona_current(char *resp, int cap);
int route_persona_remove(const char *name, char *resp, int cap);
int route_persona_show(const char *name, char *resp, int cap);
int route_persona_upsert(const char *url_name, const char *body, char *resp, int cap);
int route_personas_list(char *resp, int cap);
int route_role_template_remove(const char *name, char *resp, int cap);
int route_role_template_show(const char *name, char *resp, int cap);
int route_role_template_upsert(const char *name, const char *body, char *resp, int cap);
int route_role_templates_list(char *resp, int cap);
int route_roundtables_list(char *resp, int cap);
int route_roundtable_show(const char *name, char *resp, int cap);
int route_roundtable_upsert(const char *url_name, const char *body, char *resp, int cap);
int route_roundtable_remove(const char *name, char *resp, int cap);
int route_roundtable_set_active(const char *body, char *resp, int cap);
void send_rate_limited(int fd, int retry_after, const char *request_id);
void send_response(int fd, int status, const char *body, const char *request_id);
void handle_session_events(int fd, const char *id_in, const char *request_id);

extern atomic_int g_conn_live;
#define CONN_LIVE_MAX 64
typedef struct
{
   int fd;
   int is_tcp;
   int is_tls;
} conn_job_t;
void handle_conn(int fd, int is_tcp);
/* promoted cross-TU (former .inc statics) */
int emit(char *resp, int cap, cJSON *obj);
int err_json(char *resp, int cap, int status, const char *msg);
int write_all_fd(int fd, const char *buf, int len);
void write_sse_headers(int fd, const char *request_id);

/* promoted cross-TU (former .inc statics) */
int loopback_rpc(const char *body, int body_len, char *resp, int resp_cap, uint32_t conn_caps);
int route_capabilities(char *resp, int cap);
int route_completion(server_http_completion_fn fn, const char *body, char *resp, int cap);
int route_health(char *resp, int cap);
int route_json_provider(server_http_json_provider fn, char *resp, int cap, const char *what);
int route_models(char *resp, int cap);
int route_native_post(server_http_completion_fn fn, const char *body, char *resp, int cap,
                      const char *unavailable_msg);
int route_runs_get(const char *id, char *resp, int cap);
int route_runs_stop(const char *id, char *resp, int cap);
int route_session_attach(const char *session_id, const char *body, char *resp, int cap);
int route_session_detach(const char *session_id, const char *body, char *resp, int cap);
int route_session_persona_get(const char *session_id, char *resp, int cap);
int route_session_persona_set(const char *session_id, const char *body, char *resp, int cap);
int route_session_primary_clear(const char *session_id, char *resp, int cap);
int route_session_primary_get(const char *session_id, char *resp, int cap);
int route_session_primary_set(const char *session_id, const char *body, char *resp, int cap);
int route_sessions_list(char *resp, int cap);
int route_version(char *resp, int cap);
uint32_t v1_route_caps_lookup(const char *method, const char *path);
int v1_route_dispatch(const char *method, const char *path, const char *body, int body_len,
                      char *resp, int resp_cap);
int v1_route_is_local_only(const char *method, const char *path);

extern server_http_json_provider g_agents_provider;
extern server_http_completion_fn g_chat_handler;
extern server_http_completion_fn g_completion_handler;
extern server_http_completion_fn g_count_tokens_handler;
extern server_http_json_provider g_curiosity_provider;
extern server_http_json_provider g_dashboard_memory_provider;
extern server_http_json_provider g_dashboard_reminders_provider;
extern server_http_completion_fn g_embeddings_handler;
extern server_http_completion_fn g_kb_search_handler;
extern server_http_json_provider g_kb_status_provider;
extern server_http_completion_fn g_memory_recall_handler;
extern server_http_completion_fn g_messages_handler;
extern server_http_json_provider g_notes_list_provider;
extern server_http_completion_fn g_notes_search_handler;
extern server_http_completion_fn g_responses_handler;
extern server_http_json_provider g_roadmap_provider;
extern _Thread_local uint32_t g_rpc_conn_caps;
extern server_http_json_provider g_rules_provider;
extern server_http_completion_fn g_runs_handler;
#define SHTTP_RESP_MAX (256 * 1024)

/* Per-request context handed to a /v1 route handler (shared so route handlers can
 * live in their own translation unit, e.g. server_ci_route.c). `id` holds the
 * extracted dynamic path segment for RM_PREFIX routes, or "" for fixed routes. */
typedef struct
{
   const char *method;
   const char *path;
   const char *body;
   int body_len;
   const char *id;
   const char *op; /* matched row's NDJSON method twin (for rh_dispatch_op), or NULL */
} route_req_t;

typedef int (*route_handler_fn)(const route_req_t *rq, char *resp, int cap);

/* PC2: CI webhook route handler (defined in server_ci_route.c). */
int rh_dev_ci_event(const route_req_t *rq, char *resp, int cap);

/* Workflow Actions lifecycle + project-file-browser route adapters + the shared
 * unsigned-long query-param helper — defined in server_http_config_routes.c
 * (relocated out of server_http_routes.c to stay under the line-check ceiling).
 * Referenced by the route table in server_http_routes.c. */
long rh_query_long(const char *key, long dflt);
int rh_wf_item_pause(const route_req_t *rq, char *resp, int cap);
int rh_wf_item_resume(const route_req_t *rq, char *resp, int cap);
int rh_wf_item_stop(const route_req_t *rq, char *resp, int cap);
int rh_wf_item_delete(const route_req_t *rq, char *resp, int cap);
int rh_wf_repo_tree(const route_req_t *rq, char *resp, int cap);
int rh_wf_repo_file(const route_req_t *rq, char *resp, int cap);

#endif /* SERVER_HTTP_INTERNAL_H */
