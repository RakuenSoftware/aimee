#include "cJSON.h"
#include "platform_test_util.h"
#include "server_native_delegate_source.h"
#include "server.h"
#include "../server/workflow_control_bus.h"
#include "headers/module_json_call.h"
#include <aimee/core/event_bus/module_protocol.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *handle = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static char workspace[512], purpose[32];
static cJSON *sent;
static int mint_calls, revoked, bus_calls, deny;
static int reply_status = 200;
static aimee_module_call_result_t call_result = AIMEE_MODULE_CALL_OK;
static int submissions, submit_result, notifications;
static char fixture_home[512];

int server_native_delegate_source_mint_for_workspace(const char *why, const char *cwd, char out[65],
                                                     char *err, size_t cap)
{
   (void)err;
   (void)cap;
   mint_calls++;
   snprintf(workspace, sizeof workspace, "%s", cwd ? cwd : "");
   snprintf(purpose, sizeof purpose, "%s", why);
   if (deny || !cwd || !cwd[0])
      return -1;
   snprintf(out, 65, "%s", handle);
   return 0;
}
int server_native_delegate_source_revoke(const char *value)
{
   assert(!strcmp(value, handle));
   revoked++;
   return 0;
}
int obs_bus_module_available(uint32_t kind)
{
   (void)kind;
   return 1;
}
const char *aimee_module_call_result_name(aimee_module_call_result_t result)
{
   (void)result;
   return "fixture";
}
cJSON *aimee_module_json_call_raw(uint32_t kind, uint32_t stage, const char *body, size_t len,
                                  size_t max, int timeout, aimee_module_call_result_t *result)
{
   (void)stage;
   (void)max;
   (void)timeout;
   assert(strlen(body) == len);
   bus_calls++;
   cJSON_Delete(sent);
   sent = cJSON_Parse(body);
   assert(sent);
   *result = call_result;
   if (call_result != AIMEE_MODULE_CALL_OK)
      return NULL;
   if (kind != AIMEE_WORKFLOWS_EVENT_CONTROL)
      return cJSON_Parse("{\"approved\":false}");
   cJSON *reply = cJSON_CreateObject();
   assert(reply);
   cJSON_AddNumberToObject(reply, "status", reply_status);
   cJSON_AddStringToObject(reply, "body", "{}");
   return reply;
}
const char *run_cmd_get_cwd(void)
{
   return fixture_home;
}
int roundtable_review_resolve_panel(const char *requested, char *out, size_t cap, int *timeout)
{
   (void)requested;
   snprintf(out, cap, "review");
   *timeout = 100;
   return 0;
}
int server_send_error(server_conn_t *conn, const char *message, const char *details)
{
   (void)conn;
   (void)message;
   (void)details;
   return -1;
}
int server_send_response(server_conn_t *conn, cJSON *reply)
{
   (void)conn;
   (void)reply;
   return 0;
}

const char *aimee_home(void)
{
   return fixture_home;
}
int wfe_autonomy_killed(void)
{
   return 0;
}
double wfe_autonomy_default_max_cost_usd(void)
{
   return 0;
}
void wfe_scheduler_notify(void)
{
   notifications++;
}
void router_autonomous_pick(const char *text, char *wf, size_t wn, char *src, size_t sn, char *raw,
                            size_t rn, char *tag, size_t tn, int *clamped)
{
   (void)text;
   (void)wf;
   (void)wn;
   (void)src;
   (void)sn;
   (void)raw;
   (void)rn;
   (void)tag;
   (void)tn;
   (void)clamped;
   assert(!"unexpected model router call");
}
void router_autonomous_audit(const char *id, const char *chosen, const char *src, const char *raw,
                             int clamped, const char *tag)
{
   (void)id;
   (void)chosen;
   (void)src;
   (void)raw;
   (void)clamped;
   (void)tag;
   assert(!"unexpected router audit");
}
int wfe_work_item_resolve(const char *workflow, const char *repo, char name[64], char version[65],
                          char start[64], char approved[512], char id[80], char *err, size_t cap)
{
   (void)workflow;
   (void)repo;
   (void)err;
   (void)cap;
   snprintf(name, 64, "build");
   snprintf(version, 65, "version");
   snprintf(start, 64, "start");
   snprintf(approved, 512, "/resolved/approved/repository");
   snprintf(id, 80, "issuer-fixture");
   return 0;
}
int db1_work_item_submit_capped(const char *id, const char *repo, const char *proposal,
                                const char *workflow, const char *version, const char *start,
                                const char *submitter, int active, int rate, int seconds,
                                const char *execution_scope)
{
   (void)id;
   (void)proposal;
   (void)workflow;
   (void)version;
   (void)start;
   (void)submitter;
   (void)active;
   (void)rate;
   (void)seconds;
   assert(!strcmp(repo, "/resolved/approved/repository"));
   assert(!strcmp(execution_scope, handle));
   submissions++;
   return submit_result;
}
int db1_work_item_set_terminal(const char *id, const char *state)
{
   (void)id;
   (void)state;
   return 0;
}
int db1_work_item_set_cost_cap(const char *id, double cap)
{
   (void)id;
   (void)cap;
   return 0;
}

#include "../server/workflow_control_bus.c"
#include "../server/roundtable_review_bus.c"
#include "../server/server_dev_submit.c"

int main(void)
{
   char temporary[256];
   assert(snprintf(temporary, sizeof temporary, "%s/aimee-source-issuer-XXXXXX",
                   platform_tmpdir()) < (int)sizeof temporary);
   assert(mkdtemp(temporary));
   snprintf(fixture_home, sizeof fixture_home, "%s", temporary);
   char response[256];
   const char *body = "{\"proposal_md\":\"real "
                      "proposal\",\"repo\":\"/owned/project\",\"execution_scope\":\"forged\"}";
   assert(workflow_control_request("POST", "/v1/dev/submit", "", body, (int)strlen(body), "actor",
                                   0, response, sizeof response) == 200);
   assert(mint_calls == 1 && bus_calls == 1 && revoked == 0);
   assert(!strcmp(workspace, "/owned/project") && !strcmp(purpose, "workflow"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(sent, "execution_scope")),
                  handle));
   /* The body is preserved as business data, never elevated into the outer scope. */
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(sent, "body")), body));
   assert(workflow_control_request("GET", "/v1/workflow/items", "", NULL, 0, "actor", 0, response,
                                   sizeof response) == 200);
   assert(mint_calls == 1 && !cJSON_HasObjectItem(sent, "execution_scope"));
   body = "{\"workspace\":\"/owned/trigger\",\"proposal\":\"candidate\"}";
   assert(workflow_control_request("POST", "/v1/trigger/fire", "", body, (int)strlen(body), "actor",
                                   0, response, sizeof response) == 200);
   assert(!strcmp(workspace, "/owned/trigger"));
   deny = 1;
   int before = bus_calls;
   body = "{\"proposal_md\":\"real proposal\",\"repo\":\"/owned/project\"}";
   assert(workflow_control_request("POST", "/v1/dev/submit", "", body, (int)strlen(body), "actor",
                                   0, response, sizeof response) == 403);
   assert(bus_calls == before);
   char review_root[600], review_alias[600], process_cwd[4096];
   snprintf(review_root, sizeof review_root, "%s/review", fixture_home);
   snprintf(review_alias, sizeof review_alias, "%s/review-alias", fixture_home);
   assert(!mkdir(review_root, 0700) && !symlink(review_root, review_alias));
   assert(getcwd(process_cwd, sizeof process_cwd) && !chdir(fixture_home));
   cJSON *review = cJSON_Parse("{\"artifact\":\"public diff\",\"workdir\":\"./review-alias\","
                               "\"execution_scope\":\"forged\"}");
   assert(handle_roundtable_review(NULL, NULL, review) == -1 && bus_calls == before);
   deny = 0;
   assert(handle_roundtable_review(NULL, NULL, review) == 0);
   assert(!strcmp(workspace, review_root) && !strcmp(purpose, "roundtable"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(sent, "workdir")),
                  review_root));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(sent, "execution_scope")),
                  handle));
   assert(revoked == 1);
   cJSON *canonical = cJSON_Parse(
       "{\"version\":1,\"messages\":[{\"role\":\"user\",\"blocks\":["
       "{\"type\":\"text\",\"text\":\"original authored task\"},"
       "{\"type\":\"text\",\"text\":\"private evidence\",\"context\":{\"model_visible\":false}}]}],"
       "\"max_tokens\":321,\"metadata\":{\"task_id\":\"source\"}}");
   assert(canonical && cJSON_AddItemToObject(review, "request", canonical));
   cJSON *brief = cJSON_Parse("{\"questions\":[\"Does authorization hold?\"]}");
   assert(brief && cJSON_AddItemToObject(review, "brief", brief));
   char *review_body = build_review_body(review, "panel", "scope", "/workspace");
   cJSON *projected = review_body ? cJSON_Parse(review_body) : NULL;
   assert(projected && cJSON_Compare(canonical, cJSON_GetObjectItem(projected, "request"), 1));
   assert(cJSON_Compare(brief, cJSON_GetObjectItem(projected, "brief"), 1));
   assert(!cJSON_HasObjectItem(projected, "original_request"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(projected, "artifact")), "public diff"));
   free(review_body);
   cJSON_Delete(projected);
   cJSON_DeleteItemFromObjectCaseSensitive(review, "artifact");
   review_body = build_review_body(review, "panel", "scope", "/workspace");
   projected = review_body ? cJSON_Parse(review_body) : NULL;
   assert(projected && !cJSON_HasObjectItem(projected, "artifact"));
   assert(cJSON_Compare(canonical, cJSON_GetObjectItem(projected, "request"), 1));
   free(review_body);
   cJSON_Delete(projected);
   cJSON_ReplaceItemInObjectCaseSensitive(review, "workdir", cJSON_CreateString("./missing"));
   int calls_before = bus_calls, mints_before = mint_calls;
   assert(handle_roundtable_review(NULL, NULL, review) == -1);
   assert(bus_calls == calls_before && mint_calls == mints_before);
   cJSON_DeleteItemFromObjectCaseSensitive(review, "workdir");
   assert(handle_roundtable_review(NULL, NULL, review) == 0);
   assert(!strcmp(workspace, fixture_home));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(sent, "workdir")),
                  fixture_home));
   assert(revoked == 2);
   assert(!chdir(process_cwd) && !unlink(review_alias) && !rmdir(review_root));
   cJSON_Delete(review);
   cJSON_Delete(sent);
   sent = NULL;
   /* Legacy C intake must bind the resolver's approved repository, not the
    * caller's unvalidated alias, and admission failure retires its capability. */
   char workflows[600], proposals[640], file[700];
   snprintf(workflows, sizeof workflows, "%s/workflows", fixture_home);
   assert(!mkdir(workflows, 0700));
   snprintf(proposals, sizeof proposals, "%s/proposals", workflows);
   cJSON *response_json = NULL;
   char error[256];
   assert(dev_submit_run("a public proposal", "build", "caller-alias", "actor", &response_json,
                         error, sizeof error) == 200);
   assert(submissions == 1 && notifications == 1 && revoked == 2);
   assert(!strcmp(workspace, "/resolved/approved/repository"));
   cJSON_Delete(response_json);
   submit_result = 1;
   assert(dev_submit_run("another proposal", "build", "caller-alias", "actor", NULL, error,
                         sizeof error) == 429);
   assert(submissions == 2 && notifications == 1 && revoked == 3);
   deny = 1;
   assert(dev_submit_run("another proposal", "build", "caller-alias", "actor", NULL, error,
                         sizeof error) == 403);
   assert(submissions == 2 && notifications == 1);
   deny = 0;
   static const char *bad[] = {"{}", "{\"repo\":\"/owned/project\"}", "{\"proposal_md\":\"text\"}",
                               "{\"proposal_md\":42,\"repo\":\"/owned/project\"}",
                               "{\"proposal_md\":\"   \",\"repo\":\"/owned/project\"}"};
   int issued_before = mint_calls, dispatched_before = bus_calls;
   for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
      assert(workflow_control_request("POST", "/v1/dev/submit", "", bad[i], (int)strlen(bad[i]),
                                      "actor", 0, response, sizeof response) == 400);
   assert(mint_calls == issued_before && bus_calls == dispatched_before);
   const char *valid = "{\"proposal_md\":\"work\",\"repo\":\"/owned/project\"}";
   int retired_before = revoked;
   reply_status = 400;
   assert(workflow_control_request("POST", "/v1/dev/submit", "", valid, (int)strlen(valid), "actor",
                                   0, response, sizeof response) == 400);
   assert(revoked == retired_before + 1 && mint_calls == issued_before + 1);
   retired_before = revoked;
   reply_status = 200;
   call_result = AIMEE_MODULE_CALL_DEADLINE_EXCEEDED;
   assert(workflow_control_request("POST", "/v1/dev/submit", "", valid, (int)strlen(valid), "actor",
                                   0, response, sizeof response) == 502);
   /* Unknown dispatch outcome: work may have been admitted, so retain authority. */
   assert(revoked == retired_before && mint_calls == issued_before + 2);
   call_result = AIMEE_MODULE_CALL_OK;
   cJSON_Delete(sent);
   sent = NULL;
   snprintf(file, sizeof file, "%s/issuer-fixture.md", proposals);
   assert(!unlink(file) && !rmdir(proposals) && !rmdir(workflows) && !rmdir(fixture_home));
   puts("native source issuers: ok");
   return 0;
}
