#ifndef DEC_KB_HTTP_CODE_H
#define DEC_KB_HTTP_CODE_H 1

#include "cJSON.h"
#include "index.h"
#include <stddef.h>

/* Shared route helpers (defined in kb_http_code.c), used by the graph-feedback
 * route handlers split into kb_http_code_graphfb.c. */
int code_scan_write_error(char *out_buf, int out_cap, const char *message);
int code_scan_handle_phase(cJSON *root, const char *project, const char *root_path, cJSON *files_j,
                           char *out_buf, int out_cap);
int code_method_not_allowed(char *out_buf, int out_cap);
int code_qparam(const char *qs, const char *key, char *out, int outsz);
int code_request_project(const char *query_string, char *project, size_t project_cap, int allow_all,
                         int *all_projects, char *out_buf, int out_cap);
void code_blast_radius_json_fields(cJSON *response, const blast_radius_t *blast);

int handle_post_code_scan(const char *body, char *out_buf, int out_cap);
int handle_post_code_scan_route(const char *method, const char *body, char *out_buf, int out_cap);
int handle_post_code_project_lifecycle_route(const char *method, const char *operation,
                                             const char *body, char *out_buf, int out_cap,
                                             int owner);
int handle_post_code_repo_trust(const char *body, char *out_buf, int out_cap, int owner);
int handle_post_code_repo_trust_route(const char *method, const char *body, char *out_buf,
                                      int out_cap, int owner);
int handle_get_code_projects(const char *query_string, char *out_buf, int out_cap);
int handle_get_code_projects_route(const char *method, const char *query_string, char *out_buf,
                                   int out_cap);
int handle_get_code_find(const char *query_string, char *out_buf, int out_cap);
int handle_get_code_find_route(const char *method, const char *query_string, char *out_buf,
                               int out_cap);
int handle_get_code_blast_radius(const char *query_string, char *out_buf, int out_cap);
int handle_get_code_blast_radius_route(const char *method, const char *query_string, char *out_buf,
                                       int out_cap);
int handle_get_code_structure(const char *query_string, char *out_buf, int out_cap);
int handle_get_code_structure_route(const char *method, const char *query_string, char *out_buf,
                                    int out_cap);
int handle_get_code_search(const char *query_string, char *out_buf, int out_cap);
int handle_get_code_search_route(const char *method, const char *query_string, char *out_buf,
                                 int out_cap);
int handle_get_code_callers(const char *query_string, char *out_buf, int out_cap);
int handle_get_code_callers_route(const char *method, const char *query_string, char *out_buf,
                                  int out_cap);
int handle_get_code_project_stats(const char *query_string, char *out_buf, int out_cap);
int handle_get_code_project_stats_route(const char *method, const char *query_string, char *out_buf,
                                        int out_cap);
int handle_get_code_cross_repo_deps(const char *query_string, char *out_buf, int out_cap);
int handle_get_code_cross_repo_deps_route(const char *method, const char *query_string,
                                          char *out_buf, int out_cap);
int handle_get_code_hybrid(const char *query_string, char *out_buf, int out_cap);
int handle_get_code_hybrid_route(const char *method, const char *query_string, char *out_buf,
                                 int out_cap);
int handle_get_code_context(const char *query_string, char *out_buf, int out_cap);
int handle_get_code_context_route(const char *method, const char *query_string, char *out_buf,
                                  int out_cap);
int handle_get_code_graph_hubs(const char *query_string, char *out_buf, int out_cap);
int handle_get_code_graph_hubs_route(const char *method, const char *query_string, char *out_buf,
                                     int out_cap);
int handle_get_code_graph(const char *query_string, char *out_buf, int out_cap);
int handle_get_code_graph_route(const char *method, const char *query_string, char *out_buf,
                                int out_cap);
int handle_get_code_graph_surprising(const char *query_string, char *out_buf, int out_cap);
int handle_get_code_graph_surprising_route(const char *method, const char *query_string,
                                           char *out_buf, int out_cap);
int handle_get_code_graph_audit(const char *query_string, char *out_buf, int out_cap);
int handle_get_code_graph_audit_route(const char *method, const char *query_string, char *out_buf,
                                      int out_cap);
int handle_get_code_graph_diff(const char *query_string, char *out_buf, int out_cap);
int handle_get_code_graph_diff_route(const char *method, const char *query_string, char *out_buf,
                                     int out_cap);
int handle_get_code_lessons(const char *query_string, char *out_buf, int out_cap);
int handle_get_code_lessons_route(const char *method, const char *query_string, char *out_buf,
                                  int out_cap);
int handle_post_code_lessons_observe(const char *body, char *out_buf, int out_cap);
int handle_post_code_lessons_observe_route(const char *method, const char *body, char *out_buf,
                                           int out_cap);

#endif
