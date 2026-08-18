/* test_index_investigate_status.c: index.investigate must tell an unreachable
 * knowledge service apart from an index that simply has no evidence.
 *
 * Both used to arrive as the same empty packet, so the call every model is told
 * to make FIRST reported an outage as "no evidence" and sent the model off to
 * search the tree by hand. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"
#include "cJSON.h"
#include "json_fluent.h"
#include "kb_client.h"

/* The kb answer is scripted off the query text, so one batch can mix a healthy
 * query with a dead one -- the case that proves the verdict is per row. */
static const char *EVIDENCE = "{\"status\":\"ok\","
                              "\"answerability\":{\"decision\":\"answerable\"},"
                              "\"results\":[{\"project\":\"p\",\"file_path\":\"src.c\","
                              "\"span\":{\"kind\":\"line\",\"line_start\":10,\"line_end\":12},"
                              "\"snippet\":\"int a(void);\"}]}";
static const char *NO_EVIDENCE = "{\"status\":\"ok\","
                                 "\"answerability\":{\"decision\":\"no_answer\"},"
                                 "\"results\":[]}";

static kb_client_result_status_t g_last = KB_CLIENT_RESULT_OK;

static char *scripted(const char *query, int *status_out)
{
   if (strstr(query, "down"))
   {
      g_last = KB_CLIENT_RESULT_UNAVAILABLE;
      if (status_out)
         *status_out = 503;
      return NULL;
   }
   if (strstr(query, "quiet"))
   {
      g_last = KB_CLIENT_RESULT_EMPTY;
      if (status_out)
         *status_out = 200;
      return strdup(NO_EVIDENCE);
   }
   g_last = KB_CLIENT_RESULT_OK;
   if (status_out)
      *status_out = 200;
   return strdup(EVIDENCE);
}

char *kb_client_code_context(const char *q, const char *s, const char *p, int *st)
{
   (void)s;
   (void)p;
   return scripted(q, st);
}

char *kb_client_code_hybrid_scoped(const char *q, const char *s, const char *p, int all_projects,
                                   int max_results, int *st)
{
   (void)s;
   (void)p;
   (void)all_projects;
   (void)max_results;
   return scripted(q, st);
}

kb_client_result_status_t kb_client_last_result_status(void)
{
   return g_last;
}

const char *kb_client_result_status_name(kb_client_result_status_t status)
{
   switch (status)
   {
   case KB_CLIENT_RESULT_OK:
      return "ok";
   case KB_CLIENT_RESULT_EMPTY:
      return "empty";
   case KB_CLIENT_RESULT_ABSTAINED:
      return "abstained";
   case KB_CLIENT_RESULT_STALE:
      return "stale";
   case KB_CLIENT_RESULT_UNAUTHORIZED:
      return "unauthorized";
   default:
      return "unavailable";
   }
}

char *kb_client_last_result_json(const char *message)
{
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "status", kb_client_result_status_name(g_last));
   cJSON_AddStringToObject(o, "message", message);
   char *j = cJSON_PrintUnformatted(o);
   cJSON_Delete(o);
   return j;
}

/* --- server seams: capture what the handler decided to send --- */
static cJSON *g_sent = NULL;

int server_send_response(server_conn_t *conn, cJSON *resp)
{
   (void)conn;
   cJSON_Delete(g_sent);
   g_sent = cJSON_Duplicate(resp, 1);
   return 0;
}

int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   (void)conn;
   (void)request_id;
   cJSON_Delete(g_sent);
   g_sent = cJSON_CreateObject();
   cJSON_AddStringToObject(g_sent, "status", "error");
   cJSON_AddStringToObject(g_sent, "message", message);
   return 0;
}

int server_active_project_from_cwd(const char *cwd, char *out, size_t outlen)
{
   (void)cwd;
   snprintf(out, outlen, "p");
   return 0;
}

/* include_code is off in every case here, so the span reader is never asked. */
cJSON *code_span_read(const char *project, const char *project_root, const char *file_path,
                      int line_start, int line_end, int max_lines)
{
   (void)project;
   (void)project_root;
   (void)file_path;
   (void)line_start;
   (void)line_end;
   (void)max_lines;
   return NULL;
}

int config_code_span_max_lines(void)
{
   return 200;
}

/* The rest of the translation unit's kb surface: never called from investigate,
 * present so the link resolves. */
int kb_client_index_find_scoped(const char *preferred_project, int all_projects,
                                const char *identifier, term_hit_t *out, int max)
{
   (void)preferred_project;
   (void)all_projects;
   (void)identifier;
   (void)out;
   (void)max;
   return -1;
}
int kb_client_index_find_project(const char *project, const char *identifier, term_hit_t *out,
                                 int max)
{
   (void)project;
   (void)identifier;
   (void)out;
   (void)max;
   return -1;
}
int kb_client_index_list(project_info_t *out, int max)
{
   (void)out;
   (void)max;
   return -1;
}
int kb_client_index_structure(const char *project, const char *file_path, definition_t *out,
                              int max)
{
   (void)project;
   (void)file_path;
   (void)out;
   (void)max;
   return -1;
}
int kb_client_index_find_callers(const char *project, const char *symbol, caller_hit_t *out,
                                 int max)
{
   (void)project;
   (void)symbol;
   (void)out;
   (void)max;
   return -1;
}
int kb_client_index_find_callers_scoped(const char *preferred_project, int all_projects,
                                        const char *symbol, caller_hit_t *out, int max)
{
   (void)preferred_project;
   (void)all_projects;
   (void)symbol;
   (void)out;
   (void)max;
   return -1;
}
int kb_client_index_blast_radius(const char *project, const char *file_path, blast_radius_t *out)
{
   (void)project;
   (void)file_path;
   (void)out;
   return -1;
}
char *kb_client_index_cross_repo_deps_json(const char *project, const char *direction,
                                           const char *min_tier, int status_ambiguous, int dry_run)
{
   (void)project;
   (void)direction;
   (void)min_tier;
   (void)status_ambiguous;
   (void)dry_run;
   return NULL;
}

/* --- helpers --- */
static cJSON *investigate(cJSON *req)
{
   assert(handle_index_investigate(NULL, NULL, req) == 0);
   cJSON_Delete(req);
   assert(g_sent);
   return g_sent;
}

static const char *str_field(const cJSON *o, const char *key)
{
   const cJSON *s = cJSON_GetObjectItemCaseSensitive(o, key);
   return cJSON_IsString(s) ? s->valuestring : "";
}

static const char *row_status(const cJSON *resp, int i)
{
   const cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "results");
   return str_field(cJSON_GetArrayItem((cJSON *)arr, i), "result_status");
}

static cJSON *req_one(const char *query)
{
   cJSON *r = cJSON_CreateObject();
   cJSON_AddStringToObject(r, "query", query);
   cJSON_AddStringToObject(r, "cwd", "workdir");
   cJSON_AddBoolToObject(r, "include_code", 0);
   return r;
}

static cJSON *req_batch(const char *a, const char *b)
{
   cJSON *r = cJSON_CreateObject();
   cJSON *qs = cJSON_AddArrayToObject(r, "queries");
   cJSON_AddItemToArray(qs, cJSON_CreateString(a));
   cJSON_AddItemToArray(qs, cJSON_CreateString(b));
   cJSON_AddStringToObject(r, "cwd", "workdir");
   cJSON_AddBoolToObject(r, "include_code", 0);
   return r;
}

int main(void)
{
   /* The index answered and had evidence. */
   cJSON *resp = investigate(req_one("who calls the loader"));
   assert(strcmp(str_field(resp, "result_status"), "ok") == 0);
   assert(strcmp(row_status(resp, 0), "ok") == 0);

   /* AVAILABLE BUT NO ANSWER: the index answered, it just has nothing. This
    * must stay distinct from an outage in BOTH directions. */
   resp = investigate(req_one("quiet corner of the tree"));
   assert(strcmp(str_field(resp, "result_status"), "empty") == 0);
   assert(strcmp(row_status(resp, 0), "empty") == 0);

   /* GENUINELY UNAVAILABLE: no query reached the kb. The regression this file
    * exists for -- this used to answer with an ok envelope and an empty result
    * list, indistinguishable from the case above. */
   resp = investigate(req_one("down and unreachable"));
   assert(strcmp(str_field(resp, "status"), "unavailable") == 0);
   assert(strcmp(str_field(resp, "result_status"), "ok") != 0);
   assert(strcmp(str_field(resp, "result_status"), "empty") != 0);
   assert(!cJSON_GetObjectItemCaseSensitive(resp, "results"));

   /* A partial outage is NOT a whole one: the healthy query still answers, and
    * the dead one names its own verdict on its own row. */
   resp = investigate(req_batch("who calls the loader", "down and unreachable"));
   assert(strcmp(str_field(resp, "result_status"), "ok") == 0);
   assert(strcmp(row_status(resp, 0), "ok") == 0);
   assert(strcmp(row_status(resp, 1), "unavailable") == 0);

   /* Every query in the batch dead is an outage, same as the single case. */
   resp = investigate(req_batch("down and unreachable", "also down"));
   assert(strcmp(str_field(resp, "status"), "unavailable") == 0);
   assert(!cJSON_GetObjectItemCaseSensitive(resp, "results"));

   /* A quiet index plus a dead one is still an outage for the dead row only. */
   resp = investigate(req_batch("quiet corner", "down and unreachable"));
   assert(strcmp(str_field(resp, "result_status"), "empty") == 0);
   assert(strcmp(row_status(resp, 0), "empty") == 0);
   assert(strcmp(row_status(resp, 1), "unavailable") == 0);

   cJSON_Delete(g_sent);
   printf("test_index_investigate_status: ok\n");
   return 0;
}
