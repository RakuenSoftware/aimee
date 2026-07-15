/* s2_native_gate_hook.c -- see s2_native_gate_hook.h. Server-side S2 native-tool
 * externalization gate for the hooks.pre RPC (tracks 2+3). */
#include "s2_native_gate_hook.h"

#include <stdlib.h>
#include <string.h>

#include "audit_action.h"
#include "cJSON.h"
#include "config.h" /* require_aimee_git */
#include "log.h"
#include "wfe_binding.h"     /* db1_wfe_binding_get */
#include "wfe_enforce.h"     /* the enforce dial */
#include "wfe_native_gate.h" /* classifier + decision */
#include "wfe_store.h"       /* db1_work_item_get (delivered==accepted) */

int hook_send_blocked(server_conn_t *conn, const char *msg, const char *request_id)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "blocked");
   cJSON_AddNumberToObject(resp, "exit_code", 2);
   cJSON_AddStringToObject(resp, "message", msg);
   if (request_id)
      cJSON_AddStringToObject(resp, "request_id", request_id);
   int r = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return r;
}

/* The require_aimee_git dial (default ON). An unreadable config reads as ENFORCING:
 * a guard that fails open is not a guard. */
static int require_aimee_git_on(void)
{
   config_t cfg;
   if (config_load(&cfg) != 0)
      return 1;
   return cfg.require_aimee_git;
}

/* The decision: 2 = deny (fills msg), 0 = allow. */
static int s2_decide(const char *sid, const char *tool_name, const char *tool_input, char *msg,
                     size_t msg_n)
{
   /* Deliberately NOT gated on `sid`: the unconditional rules below (admin-merge
    * override, no-git-in-a-shell) apply to every caller, and a session aimee spawned
    * resolves no sid at all, so requiring one here would skip exactly the delegates
    * these rules exist to cover. Only the binding-dependent decision needs a sid. */
   if (!tool_name || !tool_name[0])
      return 0;

   char *cmd_heap = NULL;
   const char *command = tool_input ? tool_input : "";
   if (wfe_is_shell_tool(tool_name) && tool_input && tool_input[0])
   {
      cJSON *ti = cJSON_Parse(tool_input);
      if (ti)
      {
         cJSON *c = cJSON_GetObjectItemCaseSensitive(ti, "command");
         if (cJSON_IsString(c) && c->valuestring && (cmd_heap = strdup(c->valuestring)))
            command = cmd_heap;
         cJSON_Delete(ti);
      }
   }
   int externalizes = wfe_native_tool_externalizes(tool_name, command);
   int forbidden = wfe_native_tool_forbidden(tool_name, command);
   int shell_git = require_aimee_git_on() && wfe_shell_invokes_git(tool_name, command);
   free(cmd_heap);

   /* Forbidden outright: denied regardless of binding / delivery / enforce stage.
    * Checked before the binding lookup because no binding state can permit it. */
   if (forbidden)
   {
      audit_log("s2-native-gate", "DENY-forbidden sid=%s tool=%s (admin merge override)",
                (sid && sid[0]) ? sid : "-", tool_name);
      snprintf(msg, msg_n,
               "aimee: merging with an admin override of branch protection is human-only. "
               "Open the PR and let a human decide; aimee's own merge paths have no bypass "
               "either.");
      return 2;
   }

   /* No git/gh in a shell: every git and forge action goes through aimee's git_*
    * tools, which run on aimee-server where the forge credential stays in-process.
    * Also checked before the binding lookup -- an unbound delegate is exactly the
    * case this must cover, and the binding path would ALLOW it. */
   if (shell_git)
   {
      audit_log("s2-native-gate", "DENY-shell-git sid=%s tool=%s", (sid && sid[0]) ? sid : "-",
                tool_name);
      snprintf(msg, msg_n,
               "aimee: delegates do not run git or gh directly — use aimee's git tools, which "
               "execute on aimee-server: git_status, git_log, git_diff_summary, git_branch, "
               "git_commit, git_push, git_pr, git_verify. (Operator: require_aimee_git: false "
               "in aimee.yaml opts out.)");
      return 2;
   }

   if (!externalizes)
      return 0;
   /* The binding-dependent decision needs a resolvable session. */
   if (!sid || !sid[0] || strcmp(sid, "unknown") == 0)
      return 0;

   char wi[80] = "", stage[16] = "";
   int bg = db1_wfe_binding_get(sid, wi, sizeof wi, stage, sizeof stage);
   if (bg < 0)
   {
      /* Binding unreadable: fail CLOSED on the externalizing surface under a hard
       * global dial. A cleanly absent binding (bg==0) is allowed ('unknown' != 'none'). */
      if (wfe_enforce_stage_parse(getenv("AIMEE_WORKFLOW_ENFORCE_STAGE")) == WFE_ENFORCE_HARD)
      {
         audit_log("s2-native-gate", "DENY-failclosed sid=%s tool=%s (binding unreadable)", sid,
                   tool_name);
         snprintf(msg, msg_n,
                  "aimee S2: enforcement state is temporarily unavailable; this "
                  "externalizing action is blocked (fail-closed).");
         return 2;
      }
      return 0;
   }
   int bound = (bg == 1 && wi[0]);
   int delivered = 0, stage_hard = 0;
   if (bound)
   {
      db1_work_item_t item;
      delivered = (db1_work_item_get(wi, &item) == 1 && strcmp(item.state, "accepted") == 0);
      stage_hard = (wfe_enforce_stage_parse(stage) == WFE_ENFORCE_HARD);
   }
   switch (wfe_native_gate_decision(externalizes, bound, delivered, stage_hard))
   {
   case WFE_NATIVE_DENY:
      audit_log("s2-native-gate", "DENY sid=%s tool=%s wi=%s stage=hard", sid, tool_name, wi);
      snprintf(msg, msg_n,
               "aimee S2: this session is bound to an enforced work-item that has not passed "
               "gate.deliver -- externalizing actions (push / PR / publish / network egress) are "
               "blocked until the change is reviewed and delivered.");
      return 2;
   case WFE_NATIVE_WARN:
      audit_log("s2-native-gate", "WOULD-DENY sid=%s tool=%s wi=%s stage=%s", sid, tool_name, wi,
                stage[0] ? stage : "?");
      LOG_WARN("s2-native-gate",
               "would deny externalizing tool '%s' pre-gate.deliver (stage=%s, wi=%s)", tool_name,
               stage[0] ? stage : "?", wi);
      return 0;
   case WFE_NATIVE_ALLOW:
   default:
      return 0;
   }
}

int s2_native_gate_hook_pre(server_conn_t *conn, const char *sid, const char *tool_name,
                            const char *tool_input, const char *request_id)
{
   char msg[1024] = "";
   if (s2_decide(sid, tool_name, tool_input, msg, sizeof msg) == 2)
      return hook_send_blocked(conn, msg, request_id);
   return -1;
}
