/* test_roundtable_pipeline_ctl.c: controller-level state transitions for the
 * roundtable authoring pipeline. Drives the REAL handle_pipeline_advance /
 * handle_pipeline_gate handlers against a temp ledger, stubbing only the heavy
 * externals (roundtable submit / git / agent registry / response sink) that the
 * exercised paths do not invoke. Covers:
 *   - DRAFT completion stores the skeleton + transitions drafting->proposal_review
 *     (#1), and proposal REVIEW then consumes the stored proposal file with no
 *     caller-supplied artifact.
 *   - an unanswered gate past its TTL is abandoned (never passed) even when
 *     pipeline.gate is called directly (#2). */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sqlite3.h>

#include "config.h" /* defines MAX_PATH_LEN used by agent_types.h */
#include "agent_config.h"
#include "cJSON.h"
#include "mcp_git.h"
#include "roundtable_pipeline.h"
#include "server.h"
#include "server_http.h"
#include "server_pipeline.h"

int db1_init(const char *path);
void db1_shutdown(void);

/* ---- captured response sink (stubs for server_send_*) ---- */
static char g_resp[8192];
static int g_is_error;
static char g_err[512];

int server_send_response(server_conn_t *conn, cJSON *resp)
{
   (void)conn;
   g_is_error = 0;
   char *s = cJSON_PrintUnformatted(resp);
   snprintf(g_resp, sizeof(g_resp), "%s", s ? s : "");
   free(s);
   return 0;
}
int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   (void)conn;
   (void)request_id;
   g_is_error = 1;
   snprintf(g_err, sizeof(g_err), "%s", message ? message : "");
   g_resp[0] = '\0';
   return 0;
}

/* ---- stubs for externals the exercised paths don't call ---- */
static int g_submits;
int server_http_submit_op_run(const char *op_method, const char *body_json, uint32_t conn_caps,
                              char *resp, int cap)
{
   (void)op_method;
   (void)body_json;
   (void)conn_caps;
   g_submits++;
   snprintf(resp, (size_t)cap, "{\"id\":\"oprun_stub\",\"status\":\"queued\"}");
   return 200;
}
cJSON *handle_git_pr(cJSON *args)
{
   (void)args;
   return NULL;
}
char *mcp_git_run(const char *cmd, int *exit_code)
{
   (void)cmd;
   if (exit_code)
      *exit_code = 0;
   return strdup("");
}
int agent_load_config(agent_config_t *cfg)
{
   (void)cfg;
   return -1;
}
agent_t *agent_find(agent_config_t *cfg, const char *name)
{
   (void)cfg;
   (void)name;
   return NULL;
}
int model_context_window(const char *model_id)
{
   (void)model_id;
   return 0;
}
int openai_runs_store_request_cancel(const char *run_id)
{
   (void)run_id;
   return 0;
}

/* ---- harness ---- */
static char tmp_db[256];
static char tmp_home[256];

static cJSON *advance_req(int id)
{
   cJSON *r = cJSON_CreateObject();
   cJSON_AddNumberToObject(r, "pipeline_id", id);
   return r;
}

static int call_advance(int id)
{
   server_conn_t conn;
   memset(&conn, 0, sizeof(conn));
   cJSON *req = advance_req(id);
   handle_pipeline_advance(NULL, &conn, req);
   cJSON_Delete(req);
   return 0;
}

static const char *resp_action(void)
{
   static char act[64];
   act[0] = '\0';
   cJSON *r = cJSON_Parse(g_resp);
   if (r)
   {
      cJSON *a = cJSON_GetObjectItemCaseSensitive(r, "action");
      if (cJSON_IsString(a))
         snprintf(act, sizeof(act), "%s", a->valuestring);
      cJSON_Delete(r);
   }
   return act;
}

static void setup(void)
{
   snprintf(tmp_home, sizeof(tmp_home), "/tmp/test_rtp_ctl_home_%d", (int)getpid());
   setenv("AIMEE_HOME", tmp_home, 1);
   snprintf(tmp_db, sizeof(tmp_db), "/tmp/test_rtp_ctl_%d.sqlite", (int)getpid());
   unlink(tmp_db);
   assert(db1_init(tmp_db) == 0);
}

static void teardown(void)
{
   db1_shutdown();
   unlink(tmp_db);
}

/* DRAFT completion -> proposal_review, then REVIEW consumes the stored file. */
static void test_draft_completion(void)
{
   int pid = 0;
   assert(rtp_run_create("build a thing", NULL, "/r", "testing", &pid) == 0);
   /* drafting state with a captured, valid DRAFT pass whose attempt snapshot
    * carries the skeleton artifact. */
   int pass_id = 0;
   assert(rtp_pass_create(pid, RTP_PHASE_PROPOSAL, RTP_MODE_DRAFT, 1, "h", &pass_id) == 0);
   rtp_pass_t p;
   assert(rtp_pass_get(pass_id, &p) == 0);
   snprintf(p.status, sizeof(p.status), RTP_PASS_CAPTURED);
   assert(rtp_pass_update(&p) == 0);
   int att = 0;
   assert(rtp_attempt_create(pass_id, 1, "oprun_d", &att) == 0);
   rtp_attempt_t a;
   assert(rtp_attempt_get_by_run("oprun_d", &a) == 0);
   a.envelope_valid = 1;
   snprintf(a.capture_status, sizeof(a.capture_status), RTP_CAP_CAPTURED);
   snprintf(a.result_snapshot, sizeof(a.result_snapshot),
            "{\"artifact\":\"# Proposal skeleton\\n## Goal\\n## Scope\\n\",\"best_round\":1}");
   assert(rtp_attempt_update(&a) == 0);

   /* advance: DRAFT must store the skeleton + move to proposal_review (NOT gate). */
   call_advance(pid);
   assert(!g_is_error);
   assert(strcmp(resp_action(), "drafted") == 0);
   rtp_run_t run;
   assert(rtp_run_get(pid, &run) == 0);
   assert(strcmp(run.state, RTP_STATE_PROPOSAL_REVIEW) == 0);
   assert(run.proposal_ref[0] != '\0');
   /* the proposal lands on a dedicated branch + worktree as a real proposal file
    * (the PR content), not an internal origin blob (#1/§1). */
   assert(strcmp(run.head_branch, "roundtable/proposal-1") == 0 ||
          strncmp(run.head_branch, "roundtable/proposal-", 20) == 0);
   assert(run.worktree_path[0] != '\0');
   assert(strstr(run.proposal_ref, "docs/proposals/pending/roundtable-proposal-") != NULL);
   /* the skeleton was written to that working file. */
   FILE *f = fopen(run.proposal_ref, "rb");
   assert(f != NULL);
   char buf[256] = {0};
   fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   assert(strstr(buf, "Proposal skeleton") != NULL);

   /* a subsequent REVIEW advance with NO --artifact must consume the stored
    * proposal file (not error "review needs the artifact"). With a single
    * (unresolved) panel the budget is 0 -> single review pass submitted. */
   g_submits = 0;
   call_advance(pid);
   assert(!g_is_error); /* did NOT demand a caller artifact (#1) */
   assert(g_submits == 1);
   assert(strcmp(resp_action(), "submitted") == 0);
   printf("  draft completion -> proposal_review consumes stored file: ok\n");
}

/* A gate past its TTL is abandoned through pipeline.gate, never passed (#2). */
static void test_gate_ttl_via_gate(void)
{
   int pid = 0;
   assert(rtp_run_create("ttl idea", NULL, "/r", "testing", &pid) == 0);
   assert(rtp_run_set_state(pid, RTP_STATE_GATE1_PENDING, NULL) == 0);
   int gid = 0;
   assert(rtp_gate_create(pid, 1, 7, "deadbeef", &gid) == 0);

   /* backdate the gate so it is past a 1h TTL (second connection to the temp db). */
   sqlite3 *raw = NULL;
   assert(sqlite3_open(tmp_db, &raw) == SQLITE_OK);
   assert(sqlite3_exec(raw,
                       "UPDATE roundtable_pipeline_gates SET created_at = datetime('now','-48 "
                       "hours')",
                       NULL, NULL, NULL) == SQLITE_OK);
   sqlite3_close(raw);

   /* enable the TTL via an aimee.yaml in AIMEE_HOME. */
   char cfgpath[320];
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", tmp_home);
   FILE *cf = fopen(cfgpath, "wb");
   assert(cf != NULL);
   fputs("roundtable:\n  pipeline_gate_ttl_h: 1\n", cf);
   fclose(cf);

   /* resolve the gate with pass: TTL must abandon it FIRST, never merge. */
   server_conn_t conn;
   memset(&conn, 0, sizeof(conn));
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "pipeline_id", pid);
   cJSON_AddStringToObject(req, "verdict", "pass");
   cJSON_AddStringToObject(req, "operator_principal", "anyone");
   handle_pipeline_gate(NULL, &conn, req);
   cJSON_Delete(req);

   assert(g_is_error); /* rejected, not passed */
   assert(strstr(g_err, "TTL") != NULL);
   rtp_run_t run;
   assert(rtp_run_get(pid, &run) == 0);
   assert(strcmp(run.state, RTP_STATE_ABANDONED) == 0);
   /* the gate verdict was never recorded as pass. */
   rtp_gate_t g;
   assert(rtp_gate_get(pid, 1, &g) == 0);
   assert(strcmp(g.verdict, "pass") != 0);
   printf("  gate TTL abandons via pipeline.gate (never passes): ok\n");
}

int main(void)
{
   setup();
   test_draft_completion();
   test_gate_ttl_via_gate();
   teardown();
   printf("test_roundtable_pipeline_ctl: all passed\n");
   return 0;
}
