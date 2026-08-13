#ifndef SERVER_HTTP_INTERNAL_H
#define SERVER_HTTP_INTERNAL_H
#include <stdint.h>
#include "server_http.h"
#include <stddef.h>
#include <stdatomic.h>
#include "cJSON.h"
#include "persona.h"
#define SHTTP_READ_MAX 8192
/* Cross-TU declarations for the server_http cluster (server_http.c + the .c files
 * split out of it: conn_worker / sse / response / config_routes). Formerly these
 * were file-local statics shared by textual .inc inclusion. */
/* promoted cross-TU (former .inc statics) */
int conn_offload(int fd, int is_tcp, int is_tls, int is_management);
int server_http_management_health_route(const char *method, const char *path);
int server_http_management_route(const char *method, const char *path);
int server_http_management_action_route(const char *method, const char *path);
int server_http_management_read_route(const char *method, const char *path);
int server_http_management_request_syntax_valid(const char *method, const char *path,
                                                const char *request, size_t request_len);
int server_http_management_action_framing_valid(const char *method, const char *path,
                                                const char *request, size_t request_len);
int server_http_management_read_framing_valid(const char *method, const char *path,
                                              const char *request, size_t request_len);
int server_http_remote_writes(void);
int server_http_management_action_begin(void);
int server_http_management_action_allowed(void);
void server_http_management_action_end(void);
void server_http_management_actions_start(void);
void server_http_management_actions_shutdown_begin(void);
void server_http_management_actions_stop_and_wait(void);
int server_http_management_checkpoint_files_valid(const server_http_management_config_t *);
void server_http_management_set_error(const char *error);
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
extern atomic_int g_management_conn_live;
#define CONN_LIVE_MAX            64
#define CONN_MANAGEMENT_LIVE_MAX 16
typedef struct
{
   int fd;
   int is_tcp;
   int is_tls;
   int is_management;
} conn_job_t;
void handle_conn(int fd, int is_tcp, int is_management);
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
int route_ready(char *resp, int cap);
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

/* First slab for a request body. Bodies are overwhelmingly small, so this is the
 * only allocation most requests ever make; http_body_reserve (server_http_body.c)
 * doubles from here as bytes actually arrive, rather than trusting the declared
 * Content-Length. */
#define HTTP_BODY_INITIAL_ALLOC (64u * 1024u)
char *http_read_body(int fd, const char *prefix, int prefix_len, int declared, int *out_len);

/* Narrow request-scoped keepalive used by the P5 management challenge. */
void server_http_keepalive_set(int enabled);
int server_http_keepalive_peek(void);
int server_http_keepalive_take(void);
int server_http_request_framing_valid(const char *request, size_t total);
int server_http_keepalive_route_eligible(const char *path);
int server_http_gzip_route_eligible(const char *path);
uint32_t server_http_enrollment_caps(uint32_t caps, int is_tcp, int mtls_authenticated,
                                     int native_tls, const char *bearer, const char *method,
                                     const char *path);
void server_http_gzip_set(int enabled);
int server_http_gzip_peek(void);

/* One access-log line per served request (server_http_response.c). Demotes the
 * shapes that are noise BY DESIGN — see the definition — so the log stays
 * readable; everything else logs at INFO as before. */
void server_http_log_access(const char *method, const char *path, int status,
                            const char *request_id);

/* PC2: CI webhook route handler (defined in server_ci_route.c). */
int rh_dev_ci_event(const route_req_t *rq, char *resp, int cap);

/* Workflow Actions lifecycle + project-file-browser route adapters + the shared
 * unsigned-long query-param helper — defined in server_http_config_routes.c
 * (relocated out of server_http_routes.c to stay under the line-check ceiling).
 * Referenced by the route table in server_http_routes.c. */
long rh_query_long(const char *key, long dflt);
/* Percent-decoded string query param into out[cap] ("" if absent). */
void rh_query_str(const char *key, char *out, size_t cap);
int rh_wf_item_pause(const route_req_t *rq, char *resp, int cap);
int rh_wf_item_resume(const route_req_t *rq, char *resp, int cap);
int rh_wf_item_stop(const route_req_t *rq, char *resp, int cap);
int rh_wf_item_delete(const route_req_t *rq, char *resp, int cap);
int rh_wf_repo_tree(const route_req_t *rq, char *resp, int cap);
int rh_wf_repo_file(const route_req_t *rq, char *resp, int cap);
/* GitLab/Gitea OAuth device-flow route handlers — defined in
 * server_http_config_routes.c (relocated to keep server_http_routes.c under the
 * line-check ceiling). Referenced by the route table in server_http_routes.c. */
int rh_git_oauth_device_start(const route_req_t *rq, char *resp, int cap);
int rh_git_oauth_device_poll(const route_req_t *rq, char *resp, int cap);
int rh_git_oauth_device_config(const route_req_t *rq, char *resp, int cap);
/* GitHub device + web (redirect) sign-in and the server-orchestrated deploy
 * handlers, also relocated to server_http_config_routes.c. */
int rh_git_oauth_github_start(const route_req_t *rq, char *resp, int cap);
int rh_git_oauth_github_poll(const route_req_t *rq, char *resp, int cap);
int rh_git_oauth_github_config(const route_req_t *rq, char *resp, int cap);
int rh_git_oauth_github_web_start(const route_req_t *rq, char *resp, int cap);
int rh_git_oauth_github_web_callback(const route_req_t *rq, char *resp, int cap);
int rh_deploy_apply(const route_req_t *rq, char *resp, int cap);
int rh_deploy_status(const route_req_t *rq, char *resp, int cap);
int rh_server_forensics(const route_req_t *rq, char *resp, int cap);

/* Webchat git-surface route handlers — defined in server_http_routes_git.c
 * (relocated out of server_http_routes.c to stay under the line-check
 * ceiling). Referenced by the route table in server_http_routes.c. */
int git_surface_enabled(void); /* AIMEE_WEBCHAT_GIT gate shared by every git route */
int rh_workspace_clone(const route_req_t *rq, char *resp, int cap);
int rh_workspace_org_repos(const route_req_t *rq, char *resp, int cap);
int rh_workspace_clone_org(const route_req_t *rq, char *resp, int cap);
int rh_workspace_git(const route_req_t *rq, char *resp, int cap);
int rh_internal_forge_execute(const route_req_t *rq, char *resp, int cap);
int rh_workspace_projects(const route_req_t *rq, char *resp, int cap);
int rh_workspace_projects_delete(const route_req_t *rq, char *resp, int cap);
int rh_workspace_session_dir(const route_req_t *rq, char *resp, int cap);
int rh_workspace_editor(const route_req_t *rq, char *resp, int cap);
int rh_git_credentials(const route_req_t *rq, char *resp, int cap);
int rh_git_sshkey(const route_req_t *rq, char *resp, int cap);

/* Max flag arguments workspace_add_flag_args can write: --provider/--remote/--head. */
#define WS_ADD_FLAG_ARGS_MAX 6

/* Build the `workspace.add` flag arguments for a REST workspace registration.
 * See the definition in workspace_register_args.c. Returns the count written. */
int workspace_add_flag_args(const char *provider, const char *remote, const char *head,
                            const char *out[], int out_cap);

#endif /* SERVER_HTTP_INTERNAL_H */
