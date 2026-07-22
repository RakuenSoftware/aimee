/* cmd_hooks.c: the `aimee hooks` pre/post tool hook command.
 *
 * Session-start / launch / wrapup live in cmd_session_lifecycle.c; scope-aware
 * section building (shared with that file) lives in cmd_hooks_scope.c. */
#include "aimee.h"
#include "db1.h"
#include "headers/cmd_hooks_scope.h"
#include "memory_redirect.h"
#include "headers/plugin.h"
#include "platform_process.h"
#include "agent_config.h"
#include "agent_coord.h"
#include "agent_eval.h"
#include "log.h"
#include "wfe_binding.h"     /* db1_wfe_binding_get -- S2 native-tool gate */
#include "wfe_store.h"       /* db1_work_item_get (delivered==accepted) */
#include "wfe_enforce.h"     /* the enforce dial -> deny (hard) vs warn (soft) */
#include "wfe_native_gate.h" /* wfe_native_tool_externalizes / wfe_is_shell_tool */
#include "audit_action.h"
#include "trace_analysis.h"
#include "workspace.h"
#include "commands.h"
#include "cmd_hooks_platform.h"
#include "platform_random.h"
#include "slop_detect.h"
#include "git_verify.h"
#include "cJSON.h"
#include "headers/util.h"
#include "headers/conversation_context.h"
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Returns 1 if the tool writes file content that can be slop-scanned. */
static int is_write_tool(const char *tool)
{
   return strcmp(tool, "Write") == 0 || strcmp(tool, "Edit") == 0 ||
          strcmp(tool, "MultiEdit") == 0 || strcmp(tool, "write_file") == 0;
}

/* Worktree isolation helpers (aimee_path_is_main_clone / aimee_main_clone_edits_allowed)
 * are shared with the client hook dispatch — see headers/util.h. */

/* Emit slop findings to stderr (advisory). */
static void slop_emit_stderr(const char *file_path, slop_finding_t *findings, int n)
{
   fprintf(stderr, "\naimee: slop advisory (%d finding(s) in %s):\n", n,
           file_path ? file_path : "<content>");
   for (int i = 0; i < n; i++)
      fprintf(stderr, "  %s:%d [%s] %s\n", file_path ? file_path : "<content>",
              findings[i].line_number, slop_category_label(findings[i].category),
              findings[i].excerpt);
   if (file_path)
      fprintf(stderr, "  (run 'aimee slop %s' for full report)\n", file_path);
}

/* Build a JSON array of slop findings (caller owns returned object). */
static cJSON *slop_findings_to_json(const char *file_path, slop_finding_t *findings, int n)
{
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      if (file_path)
         cJSON_AddStringToObject(obj, "file", file_path);
      cJSON_AddNumberToObject(obj, "line", findings[i].line_number);
      cJSON_AddStringToObject(obj, "category", slop_category_label(findings[i].category));
      cJSON_AddStringToObject(obj, "excerpt", findings[i].excerpt);
      cJSON_AddItemToArray(arr, obj);
   }
   return arr;
}

static int hook_client_uses_pretool_json(void)
{
   const char *client = getenv("AIMEE_HOOK_CLIENT");
   if (client)
      return strcmp(client, "claude") == 0 || strcmp(client, "codex") == 0 ||
             strcmp(client, "copilot") == 0;
   return getenv("CODEX_THREAD_ID") || getenv("CODEX_SANDBOX") || getenv("CODEX_HOME") ||
          getenv("CODEX_CWD") || getenv("CLAUDE_SESSION_ID");
}

static int hook_client_supports_updated_input(void)
{
   const char *client = getenv("AIMEE_HOOK_CLIENT");
   if (client)
      return strcmp(client, "claude") == 0;
   return getenv("CLAUDE_SESSION_ID") != NULL;
}

static void emit_pretool_deny_json(const char *reason)
{
   cJSON *out = cJSON_CreateObject();
   cJSON *hook_out = cJSON_AddObjectToObject(out, "hookSpecificOutput");
   cJSON_AddStringToObject(hook_out, "hookEventName", "PreToolUse");
   cJSON_AddStringToObject(hook_out, "permissionDecision", "deny");
   cJSON_AddStringToObject(hook_out, "permissionDecisionReason",
                           reason && reason[0] ? reason : "Blocked by aimee guardrails.");

   char *js = cJSON_PrintUnformatted(out);
   if (js)
   {
      fputs(js, stdout);
      fputc('\n', stdout);
      free(js);
   }
   cJSON_Delete(out);
}

static void emit_pretool_rewrite_unsupported_json(int rewrite_rc, const char *rewritten)
{
   char reason[2048];
   snprintf(reason, sizeof(reason),
            "BLOCKED: Aimee would rewrite this %s to the session worktree, but this client does "
            "not support PreToolUse input rewrites. Retry with rewritten %s: %s",
            rewrite_rc == 3 ? "command" : "path", rewrite_rc == 3 ? "command" : "path",
            rewritten && rewritten[0] ? rewritten : "<unknown>");
   emit_pretool_deny_json(reason);
}

/* S2 pre-delivery externalization gate for the primary CLI's NATIVE tools (tracks
 * 2+3). If this session is bound to an enforced work-item whose gate.deliver has NOT
 * passed (state != "accepted") and this tool call is a KNOWN externalization
 * (wfe_native_tool_externalizes -- including a Bash command that runs git push / a
 * remote curl), DENY under a hard dial (emit deny + exit) or LOG under advisory/soft
 * (the warn-soak that measures false positives before a hard flip). Fail-CLOSED on
 * the externalizing surface: if the binding is unreadable under a hard global dial,
 * deny. Best-effort RISK-REDUCTION, not a hermetic seal (see wfe_native_gate.h).
 *
 * FAIL POLICY (consult #984 [6][14][23]): a DB fault (binding UNREADABLE) fails
 * CLOSED under a hard dial -- we cannot tell if a binding exists, so deny. A cleanly
 * ABSENT binding (bg==0) is ALLOWED -- that session genuinely has no enforced
 * work-item to gate. Distinct cases: "unknown" != "none". The gate therefore depends
 * on the aimee session id reaching the hook (AIMEE_SESSION_ID env, stamped into the
 * tmux CLI): a session spawned OUTSIDE that stamp resolves no binding and is allowed
 * -- an inherent limitation of a hook that trusts its environment (same class as the
 * classifier's documented bypasses; the full seal is a sandbox). On DENY the hook
 * emits the deny JSON and exit(0)s -- the Claude Code PreToolUse protocol for a
 * blocked tool (same channel as the memory-interception deny). */
/* The require_aimee_git dial (default ON). An unreadable config reads as ENFORCING:
 * a guard that fails open is not a guard. */
static int require_aimee_git_on(void)
{
   config_t cfg;
   if (config_load(&cfg) != 0)
      return 1;
   return cfg.require_aimee_git;
}

static void s2_native_gate_pretool(const char *sid, const char *tool_name, const char *tool_input)
{
   /* Deliberately NOT gated on `sid`: the unconditional rules below (admin-merge
    * override, no-git-in-a-shell) apply to every caller, and a session aimee spawned
    * resolves no sid at all (provider_cli_adapter does not stamp one), so requiring
    * one here would skip exactly the delegates these rules exist to cover. Only the
    * binding-dependent S2 decision needs a sid, and it checks for one itself. */
   if (!tool_name || !tool_name[0])
      return;

   /* Pull the shell command (Bash etc.) out of tool_input for inspection. Prefer the
    * parsed {"command":...} member, but FALL BACK to the raw tool_input string when
    * it is not a JSON object, has no command member, or parsing/alloc fails -- the
    * command text is still present there and the classifier's substring match catches
    * it. This closes a bypass where tool_input is a raw string, not an object
    * (consult #984 [16][25][31]), and is robust to malformed JSON / OOM ([5][9][26]). */
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
      emit_pretool_deny_json(
          "aimee: merging with an admin override of branch protection is human-only. "
          "Open the PR and let a human decide; aimee's own merge paths have no bypass either.");
      exit(0);
   }

   /* No git/gh in a shell: every git and forge action goes through aimee's git_*
    * tools, which run on aimee-server where the forge credential stays in-process.
    * Also checked before the binding lookup -- an unbound delegate is exactly the
    * case this must cover, and the binding path would ALLOW it. */
   if (shell_git)
   {
      audit_log("s2-native-gate", "DENY-shell-git sid=%s tool=%s", (sid && sid[0]) ? sid : "-",
                tool_name);
      emit_pretool_deny_json(
          "aimee: delegates do not run git or gh directly — use aimee's git tools, which "
          "execute on aimee-server: git_status, git_log, git_diff_summary, git_branch, "
          "git_commit, git_push, git_pr, git_verify. (Operator: require_aimee_git: false "
          "in aimee.yaml opts out.)");
      exit(0);
   }

   if (!externalizes)
      return; /* not an externalizing tool -> never gated */
   if (!sid || !sid[0])
      return; /* no session -> no binding to resolve; the S2 decision needs one */

   char wi[80] = "", stage[16] = "";
   int bg = db1_wfe_binding_get(sid, wi, sizeof wi, stage, sizeof stage);
   if (bg < 0)
   {
      /* Binding unreadable (DB fault): fail CLOSED on the externalizing surface
       * under a hard GLOBAL dial -- a security gate that fails open is not a gate. */
      if (wfe_enforce_stage_parse(getenv("AIMEE_WORKFLOW_ENFORCE_STAGE")) == WFE_ENFORCE_HARD)
      {
         audit_log("s2-native-gate", "DENY-failclosed sid=%s tool=%s (binding unreadable)", sid,
                   tool_name);
         emit_pretool_deny_json("aimee S2: enforcement state is temporarily unavailable; this "
                                "externalizing action is blocked (fail-closed).");
         exit(0);
      }
      return;
   }
   int bound = (bg == 1 && wi[0]);

   /* gate.deliver passing transitions the run to "accepted"; before that the guard
    * holds, once accepted it lifts. */
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
      emit_pretool_deny_json(
          "aimee S2: this session is bound to an enforced work-item that has not passed "
          "gate.deliver -- externalizing actions (push / PR / publish / network egress) are "
          "blocked until the change is reviewed and delivered.");
      exit(0);
   case WFE_NATIVE_WARN:
      /* advisory/soft = warn-soak: record the would-deny (measures false positives)
       * but do NOT block -- this calibrates the gate before a hard flip. */
      audit_log("s2-native-gate", "WOULD-DENY sid=%s tool=%s wi=%s stage=%s", sid, tool_name, wi,
                stage[0] ? stage : "?");
      LOG_WARN("s2-native-gate",
               "would deny externalizing tool '%s' pre-gate.deliver (stage=%s, wi=%s)", tool_name,
               stage[0] ? stage : "?", wi);
      break;
   case WFE_NATIVE_ALLOW:
      break;
   }
}

/* --- cmd_hooks --- */

void cmd_hooks(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("hooks requires 'pre' or 'post'");

   const char *phase = argv[0];
   argc--;
   argv++;

   config_t cfg;
   config_load(&cfg);
   audit_log_open();
   audit_ensure_key(); /* provision the per-action audit key (best-effort) */

   /* Read JSON from stdin -- hook input is small (tool name + args) */
   char input[65536];
   size_t total = 0;
   while (total < sizeof(input) - 1)
   {
      ssize_t n = read(STDIN_FILENO, input + total, sizeof(input) - 1 - total);
      if (n <= 0)
         break;
      total += (size_t)n;
   }
   input[total] = '\0';

   cJSON *json = cJSON_Parse(input);

   char hook_sid[64] = "";
   if (hook_payload_session_id(json, hook_sid, sizeof(hook_sid)))
      session_id_set_override(hook_sid);

   const char *tool_name = "";
   char *tool_input_heap = NULL;
   const char *tool_input = "{}";

   if (json)
   {
      cJSON *tn = cJSON_GetObjectItemCaseSensitive(json, "tool_name");
      if (cJSON_IsString(tn))
         tool_name = tn->valuestring;

      cJSON *ti = cJSON_GetObjectItemCaseSensitive(json, "tool_input");
      if (cJSON_IsString(ti))
         tool_input = ti->valuestring;
      else if (cJSON_IsObject(ti) || cJSON_IsArray(ti))
      {
         tool_input_heap = cJSON_PrintUnformatted(ti);
         tool_input = tool_input_heap;
      }
   }

   /* DB1 owns its own connection; session_state_load/save delegate to DB1. */
   if (db1_init(cfg.db1_path) != 0)
      fatal("cannot open database");

   /* Run enabled plugin hooks for this phase.
    * Plugin hook failures are advisory: warn but don't block execution. */
   {
      const char *event = (strcmp(phase, "pre") == 0) ? "PreToolUse" : "PostToolUse";
      plugin_t plugins[PLUGIN_MAX_PLUGINS];
      int pcount = plugin_registry_load(plugins, PLUGIN_MAX_PLUGINS);
      const char *roots[64];
      for (int ri = 0; ri < cfg.workspace_count && ri < 64; ri++)
         roots[ri] = cfg.workspaces[ri];
      plugin_discover_local(roots, cfg.workspace_count, plugins, &pcount, PLUGIN_MAX_PLUGINS);

      static char hook_cmds[PLUGIN_MAX_PLUGINS * PLUGIN_MAX_HOOKS][512];
      int hook_count = plugin_collect_hooks(plugins, pcount, event, hook_cmds,
                                            PLUGIN_MAX_PLUGINS * PLUGIN_MAX_HOOKS);
      for (int hi = 0; hi < hook_count; hi++)
      {
         char *out = NULL;
         size_t out_len = 0;
         int rc = platform_exec_pipe(hook_cmds[hi], input, strlen(input), &out, &out_len);
         audit_log("plugin-hook", "phase=%s rc=%d hook=%s", event, rc, hook_cmds[hi]);
         if (rc != 0)
            LOG_WARN("plugin-hook", "%s hook '%s' exited with code %d", event, hook_cmds[hi], rc);
         free(out);
      }
   }

   if (strcmp(phase, "pre") == 0)
   {
      /* Load session state from DB1 */
      const char *sid = hook_sid[0] ? hook_sid : session_id();
      session_state_t state;
      session_state_load(&state, sid);

      char cwd[MAX_PATH_LEN];
      if (!getcwd(cwd, sizeof(cwd)))
         cwd[0] = '\0';
      char payload_cwd[MAX_PATH_LEN];
      if (hook_payload_cwd(json, payload_cwd, sizeof(payload_cwd)))
      {
         snprintf(cwd, sizeof(cwd), "%s", payload_cwd);
      }
      if (!is_aimee_worktree_path(cwd))
      {
         const char *pwd = getenv("PWD");
         if (pwd && pwd[0] && is_aimee_worktree_path(pwd))
            snprintf(cwd, sizeof(cwd), "%s", pwd);
      }

      /* Write CWD to session-scoped tracking file so MCP server follows session CWD */
      if (cwd[0])
      {
         char cwd_path[MAX_PATH_LEN];
         snprintf(cwd_path, sizeof(cwd_path), "%s/git-cwd-%s", config_output_dir(), session_id());
         FILE *fp = fopen(cwd_path, "w");
         if (fp)
         {
            fputs(cwd, fp);
            fclose(fp);
         }
      }

      /* Worktree isolation. aimee already isolates its OWN work — every delegate/
       * work item runs in a locked worktree (delegate_checkout.c, wfe_blocks.c).
       * But the PRIMARY session's harness Edit/Write never traverse aimee's /v1
       * gateway, so nothing stopped it from editing the SHARED MAIN CLONE directly
       * — which is how concurrent sessions pile uncommitted work onto one branch.
       * This is the one seam that sees those edits, so enforce it here: a file
       * mutation in a main clone (its .git is a directory; a worktree's is a file)
       * is denied and steered into a dedicated worktree. Delegates are unaffected
       * (their worktree .git is a file). Escape hatch for the branch owner:
       * AIMEE_ALLOW_MAIN_CHECKOUT=1 or a .git/aimee-allow-main-edits marker. */
      {
         /* Key on the file being edited (resolved against cwd), not just cwd. */
         const char *wt_fpath = NULL;
         cJSON *wt_ti = (tool_input && tool_input[0]) ? cJSON_Parse(tool_input) : NULL;
         if (wt_ti)
         {
            cJSON *fp = cJSON_GetObjectItemCaseSensitive(wt_ti, "file_path");
            if (!cJSON_IsString(fp))
               fp = cJSON_GetObjectItemCaseSensitive(wt_ti, "notebook_path");
            if (cJSON_IsString(fp))
               wt_fpath = fp->valuestring;
         }
         int wt_block = is_write_tool(tool_name) && cwd[0] &&
                        aimee_edit_target_in_main_clone(wt_fpath, cwd) &&
                        !aimee_main_clone_edits_allowed(cwd);
         cJSON_Delete(wt_ti);
         if (wt_block)
         {
            char reason[1024];
            snprintf(reason, sizeof(reason),
                     "BLOCKED: %s edits the SHARED MAIN CLONE (%s), not a git worktree — "
                     "concurrent sessions entangle their uncommitted work this way. Isolate "
                     "this task in its own worktree first:\n"
                     "  git -C %s worktree add ../<name>-<task> -b <branch> origin/testing\n"
                     "then work there. (Branch owner override: AIMEE_ALLOW_MAIN_CHECKOUT=1 or "
                     "touch %s/.git/aimee-allow-main-edits.)",
                     tool_name, cwd, cwd, cwd);
            if (hook_client_uses_pretool_json())
            {
               emit_pretool_deny_json(reason);
               exit(0);
            }
            fprintf(stderr, "aimee: %s\n", reason);
            exit(2);
         }
      }

      /* Memory interception: redirect an agent's local memory-file write into
       * the central store, reusing the deny-with-message channel. */
      {
         cJSON *ti = (tool_input && tool_input[0]) ? cJSON_Parse(tool_input) : NULL;
         if (ti)
         {
            /* The thin client resolves the project against the real cwd/git repo
             * and forwards it; required when this server is remote. Sourced from
             * the top-level hook input only — any value nested in tool_input is
             * intentionally ignored (the agent must not pick its own store key). */
            const char *hp = NULL;
            cJSON *hpj = cJSON_GetObjectItemCaseSensitive(json, "harness_project");
            if (cJSON_IsString(hpj) && hpj->valuestring && hpj->valuestring[0])
               hp = hpj->valuestring;
            char mr_msg[1024] = "";
            int mr = memory_redirect_check(tool_name, ti, cwd, hp, mr_msg, sizeof(mr_msg));
            cJSON_Delete(ti);
            if (mr == 2)
            {
               if (hook_client_uses_pretool_json())
               {
                  emit_pretool_deny_json(mr_msg);
                  exit(0);
               }
               fprintf(stderr, "aimee: %s\n", mr_msg);
               exit(2);
            }
         }
      }

      /* S2 pre-delivery externalization gate for the primary CLI's native tools
       * (may emit a deny + exit before the generic guardrail check runs). */
      s2_native_gate_pretool(sid, tool_name, tool_input);

      char msg[1024] = "";
      int rc = pre_tool_check(tool_name, tool_input, &state, config_guardrail_mode(&cfg), cwd, msg,
                              sizeof(msg));

      session_state_save(&state, sid);

      /* Auto-delegation detection (#2):
       * If the tool is a shell tool and the command looks like a remote operation
       * that a sub-agent could handle, suggest delegation on stderr.
       * Check tool_input as raw string (contains the command JSON). */
      if (rc == 0 && is_shell_tool(tool_name) && tool_input && tool_input[0])
      {
         const char *role = NULL;
         if (strstr(tool_input, "ssh ") &&
             (strstr(tool_input, "deploy") || strstr(tool_input, "192.168") ||
              strstr(tool_input, "10.0.") || strstr(tool_input, "10.1.")))
            role = "deploy";
         else if (strstr(tool_input, "curl ") && strstr(tool_input, "/health"))
            role = "validate";
         else if (strstr(tool_input, "systemctl restart") || strstr(tool_input, "systemctl stop"))
            role = "deploy";

         if (role)
         {
            agent_config_t acfg;
            if (agent_load_config(&acfg) == 0)
            {
               int has_tools = 0;
               for (int i = 0; i < acfg.agent_count; i++)
               {
                  if (acfg.agents[i].tools_enabled && agent_has_role(&acfg.agents[i], role))
                     has_tools = 1;
               }
               if (has_tools)
               {
                  fprintf(stderr,
                          "aimee: this looks like a %s task that a sub-agent could handle. "
                          "Consider: aimee delegate %s \"<task>\"\n",
                          role, role);
               }
            }
         }
      }

      /* Worktree path rewrite: emit hookSpecificOutput with updatedInput so
       * Claude Code silently re-targets the tool call at the worktree path.
       * rc==1: edit tool file_path rewrite, rc==3: bash command rewrite. */
      if ((rc == 1 || rc == 3) && msg[0])
      {
         if (!hook_client_supports_updated_input())
         {
            emit_pretool_rewrite_unsupported_json(rc, msg);
            db1_shutdown();
            free(tool_input_heap);
            cJSON_Delete(json);
            exit(0);
         }

         cJSON *orig = cJSON_Parse(tool_input);
         if (orig)
         {
            if (rc == 1)
            {
               cJSON *file_path = cJSON_GetObjectItemCaseSensitive(orig, "file_path");
               if (file_path)
                  cJSON_ReplaceItemInObject(orig, "file_path", cJSON_CreateString(msg));
               else if (cJSON_GetObjectItemCaseSensitive(orig, "path"))
                  cJSON_ReplaceItemInObject(orig, "path", cJSON_CreateString(msg));
               else
                  cJSON_AddStringToObject(orig, "path", msg);
            }
            else
            {
               cJSON *command = cJSON_GetObjectItemCaseSensitive(orig, "command");
               cJSON *cmd = cJSON_GetObjectItemCaseSensitive(orig, "cmd");
               if (command)
                  cJSON_ReplaceItemInObject(orig, "command", cJSON_CreateString(msg));
               else if (cmd)
                  cJSON_ReplaceItemInObject(orig, "cmd", cJSON_CreateString(msg));
               else
                  cJSON_AddStringToObject(orig, "command", msg);
            }

            cJSON *output = cJSON_CreateObject();
            cJSON *hook_out = cJSON_AddObjectToObject(output, "hookSpecificOutput");
            cJSON_AddStringToObject(hook_out, "hookEventName", "PreToolUse");
            cJSON_AddStringToObject(hook_out, "permissionDecision", "allow");
            cJSON_AddItemToObject(hook_out, "updatedInput", orig);

            char *js = cJSON_PrintUnformatted(output);
            if (js)
            {
               fputs(js, stdout);
               fputc('\n', stdout);
               free(js);
            }
            cJSON_Delete(output);
         }
         db1_shutdown();
         free(tool_input_heap);
         cJSON_Delete(json);
         exit(0);
      }

      /* Slop scan on incoming Write/Edit content (advisory, never blocks). */
      {
         slop_finding_t slop[16];
         int nslop = 0;
         const char *scan_content = NULL;
         const char *scan_file = NULL;

         if (is_write_tool(tool_name) && json)
         {
            cJSON *ti_obj = cJSON_GetObjectItemCaseSensitive(json, "tool_input");
            cJSON *parsed = cJSON_IsObject(ti_obj) ? ti_obj : cJSON_Parse(tool_input);
            cJSON *content_item = cJSON_GetObjectItem(parsed, "content");
            if (!content_item)
               content_item = cJSON_GetObjectItem(parsed, "new_string");
            if (content_item && cJSON_IsString(content_item))
               scan_content = content_item->valuestring;
            cJSON *fp_item = cJSON_GetObjectItem(parsed, "file_path");
            if (fp_item && cJSON_IsString(fp_item))
               scan_file = fp_item->valuestring;
            if (!cJSON_IsObject(ti_obj) && parsed)
               cJSON_Delete(parsed);
         }

         if (scan_content)
            nslop = slop_detect_buf(scan_content, 0, slop, 16);

         if (nslop > 0)
         {
            slop_emit_stderr(scan_file, slop, nslop);

            if (ctx->json_output)
            {
               cJSON *j = cJSON_CreateObject();
               cJSON_AddNumberToObject(j, "exit_code", rc);
               if (msg[0])
                  cJSON_AddStringToObject(j, "message", msg);
               cJSON_AddItemToObject(j, "slop_warnings",
                                     slop_findings_to_json(scan_file, slop, nslop));
               emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
               db1_shutdown();
               free(tool_input_heap);
               cJSON_Delete(json);
               exit(rc);
            }
         }
      }

      if (ctx->json_output)
      {
         cJSON *j = cJSON_CreateObject();
         cJSON_AddNumberToObject(j, "exit_code", rc);
         if (msg[0])
            cJSON_AddStringToObject(j, "message", msg);
         emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      }
      else if (rc == 2 && msg[0] && hook_client_uses_pretool_json())
      {
         emit_pretool_deny_json(msg);
      }
      else if (rc != 0 && msg[0])
      {
         fprintf(stderr, "aimee: %s\n", msg);
      }

      db1_shutdown();
      free(tool_input_heap);
      cJSON_Delete(json);
      exit((rc == 2 && msg[0] && hook_client_uses_pretool_json()) ? 0 : rc);
   }
   else if (strcmp(phase, "post") == 0)
   {
      /* Update CWD tracking (catches worktree CWD changes via PostToolUse) */
      {
         char cwd[MAX_PATH_LEN];
         if (getcwd(cwd, sizeof(cwd)) && cwd[0])
         {
            char payload_cwd[MAX_PATH_LEN];
            if (hook_payload_cwd(json, payload_cwd, sizeof(payload_cwd)))
               snprintf(cwd, sizeof(cwd), "%s", payload_cwd);
            if (!is_aimee_worktree_path(cwd))
            {
               const char *pwd = getenv("PWD");
               if (pwd && pwd[0] && is_aimee_worktree_path(pwd))
                  snprintf(cwd, sizeof(cwd), "%s", pwd);
            }
            char cwd_path[MAX_PATH_LEN];
            snprintf(cwd_path, sizeof(cwd_path), "%s/git-cwd-%s", config_output_dir(),
                     session_id());
            FILE *fp = fopen(cwd_path, "w");
            if (fp)
            {
               fputs(cwd, fp);
               fclose(fp);
            }
         }
      }

      {
         const char *sid = session_id();
         session_state_t post_state;
         session_state_load(&post_state, sid);
         post_tool_update(tool_name, tool_input, &post_state);
         post_state.dirty = 1;
         session_state_save(&post_state, sid);
      }

      /* Record tool event for virtual context assembly (no-op when disabled). */
      {
         cJSON *tr = json ? cJSON_GetObjectItemCaseSensitive(json, "tool_response") : NULL;
         cJSON *tr_content = tr ? cJSON_GetObjectItemCaseSensitive(tr, "content") : NULL;
         const char *result_str =
             (tr_content && cJSON_IsString(tr_content)) ? tr_content->valuestring : "";
         int result_bytes = (int)strlen(result_str);
         conv_ctx_record_event(session_id(), tool_name, tool_input, result_str, result_bytes);
      }

      if (ctx->json_output)
      {
         /* Attach structured diff fields from tool_response if present.
          * Hook consumers can read unified_diff / additions / deletions
          * without re-reading the file. */
         cJSON *tr = json ? cJSON_GetObjectItemCaseSensitive(json, "tool_response") : NULL;
         cJSON *tr_content = tr ? cJSON_GetObjectItemCaseSensitive(tr, "content") : NULL;
         const char *tr_str =
             (tr_content && cJSON_IsString(tr_content)) ? tr_content->valuestring : NULL;
         cJSON *diff_j = tr_str ? cJSON_Parse(tr_str) : NULL;
         cJSON *ud = diff_j ? cJSON_GetObjectItem(diff_j, "unified_diff") : NULL;

         if (ud && cJSON_IsString(ud) && ud->valuestring[0])
         {
            cJSON *j = cJSON_CreateObject();
            cJSON_AddStringToObject(j, "status", "ok");
            /* Copy diff fields so hook scripts can consume them */
            cJSON *path_j = cJSON_GetObjectItem(diff_j, "path");
            if (path_j && cJSON_IsString(path_j))
               cJSON_AddStringToObject(j, "path", path_j->valuestring);
            cJSON *summ_j = cJSON_GetObjectItem(diff_j, "summary");
            if (summ_j && cJSON_IsString(summ_j))
               cJSON_AddStringToObject(j, "summary", summ_j->valuestring);
            cJSON *add_j = cJSON_GetObjectItem(diff_j, "additions");
            if (add_j && cJSON_IsNumber(add_j))
               cJSON_AddNumberToObject(j, "additions", add_j->valuedouble);
            cJSON *del_j = cJSON_GetObjectItem(diff_j, "deletions");
            if (del_j && cJSON_IsNumber(del_j))
               cJSON_AddNumberToObject(j, "deletions", del_j->valuedouble);
            cJSON_AddStringToObject(j, "unified_diff", ud->valuestring);
            emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
         }
         else
         {
            emit_ok_ctx(ctx->json_fields, ctx->response_profile);
         }
         cJSON_Delete(diff_j);
      }
   }
   else
   {
      fatal("hooks requires 'pre' or 'post', got: %s", phase);
   }

   db1_shutdown();
   cJSON_Delete(json);
   if (tool_input_heap)
      free(tool_input_heap);
}
