/* posix/agent_tools_dispatch.c: the tool-call dispatcher. Each tool's
 * implementation lives in agent_tools.c; this file just routes by name
 * and applies the shared guardrails/snapshot/slop hooks. */
#include "aimee.h"
#include "agent_tools.h"
#include "agent_tools_internal.h"
#include "aimee_home.h"
#include "tool_condense.h"
#include "tool_args_coerce.h"
#include "workspace_provider.h"

/* delegation_active_id is provided by server_compute.c at link time;
 * stub returns NULL when running outside the server (CLI, tests). */
const char *delegation_active_id(void) __attribute__((weak));
const char *delegation_active_id(void)
{
   return NULL;
}

static __thread char g_dispatch_role[64];

void agent_tools_set_dispatch_role(const char *role)
{
   if (role && role[0])
      snprintf(g_dispatch_role, sizeof(g_dispatch_role), "%s", role);
   else
      g_dispatch_role[0] = '\0';
}

const char *agent_tools_dispatch_role(void)
{
   return g_dispatch_role[0] ? g_dispatch_role : NULL;
}

/* Thread-local tool-call lifecycle hook (see agent_tools.h). NULL by default. */
static __thread agent_tool_event_cb_t g_tool_event_cb = NULL;
static __thread void *g_tool_event_ud = NULL;

void agent_tools_set_tool_event_cb(agent_tool_event_cb_t cb, void *ud)
{
   g_tool_event_cb = cb;
   g_tool_event_ud = ud;
}

static void agent_tools_emit_tool_event(const char *phase, const char *name)
{
   if (g_tool_event_cb)
      g_tool_event_cb(phase, name ? name : "", g_tool_event_ud);
}

/* db1_session_write_path_record from db1/session_paths.h — declared
 * locally so the dispatch path doesn't pull the full db1 umbrella. */
int db1_session_write_path_record(const char *session_id, const char *path);
#include "process_mgr.h"
#include "agent_exec.h"
#include "config.h"
#include "db1.h"
#include "kb_client.h"
#include "sandbox.h"
#include "slop_detect.h"
#include "web_search.h"
#include "notes.h"
#include "kb.h"
#include "mcp_client_registry.h"
#include "lifecycle.h"
#include "workspace.h"
#include "diff.h"
#include "dstr.h"
#include "lsp.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

int agent_get_durable_job_id(void) __attribute__((weak));
int db1_agent_job_is_cancelled(int job_id) __attribute__((weak));
int db1_delegation_spawn_stop_reason(const char *delegation_id, char *out, size_t out_sz)
    __attribute__((weak));

int agent_delegation_stop_requested(char *buf, size_t bufsz)
{
   if (!db1_delegation_spawn_stop_reason)
      return 0;
   const char *delegation_id = delegation_active_id();
   char reason[32];
   if (!delegation_id || !delegation_id[0] ||
       db1_delegation_spawn_stop_reason(delegation_id, reason, sizeof(reason)) != 1)
      return 0;
   if (buf && bufsz > 0)
      snprintf(buf, bufsz, "error: delegate %s (%s) before tool execution", reason, delegation_id);
   return 1;
}

static int tool_dispatch_cancelled(char *buf, size_t bufsz)
{
   if (agent_get_durable_job_id && db1_agent_job_is_cancelled)
   {
      int job_id = agent_get_durable_job_id();
      if (job_id > 0 && db1_agent_job_is_cancelled(job_id))
      {
         if (buf && bufsz > 0)
            snprintf(buf, bufsz, "error: delegate cancelled (job #%d) before tool execution",
                     job_id);
         return 1;
      }
   }
   return agent_delegation_stop_requested(buf, bufsz);
}

static char *guardrail_input_json(const char *name, const char *arguments_json)
{
   cJSON *args = cJSON_Parse(arguments_json);
   if (!args)
      return safe_strdup(arguments_json);

   cJSON *mapped = cJSON_CreateObject();
   if (strcmp(name, "bash") == 0)
   {
      cJSON *cmd = cJSON_GetObjectItem(args, "command");
      if (cmd && cJSON_IsString(cmd))
         cJSON_AddStringToObject(mapped, "command", cmd->valuestring);
   }
   else if (strcmp(name, "write_file") == 0)
   {
      cJSON *p = cJSON_GetObjectItem(args, "path");
      if (p && cJSON_IsString(p))
         cJSON_AddStringToObject(mapped, "file_path", p->valuestring);
   }
   else if (strcmp(name, "edit_file") == 0)
   {
      cJSON *p = cJSON_GetObjectItem(args, "path");
      if (p && cJSON_IsString(p))
         cJSON_AddStringToObject(mapped, "file_path", p->valuestring);
   }
   else if (strcmp(name, "execute_script") == 0)
   {
      cJSON *body = cJSON_GetObjectItem(args, "body");
      cJSON *workdir = cJSON_GetObjectItem(args, "workdir");
      if (body && cJSON_IsString(body))
         cJSON_AddStringToObject(mapped, "command", body->valuestring);
      if (workdir && cJSON_IsString(workdir))
         cJSON_AddStringToObject(mapped, "workdir", workdir->valuestring);
   }
   else if (strcmp(name, "read_file") == 0)
   {
      cJSON *p = cJSON_GetObjectItem(args, "path");
      if (p && cJSON_IsString(p))
         cJSON_AddStringToObject(mapped, "file_path", p->valuestring);
   }
   else
   {
      /* Pass through original */
      cJSON_Delete(mapped);
      cJSON_Delete(args);
      return safe_strdup(arguments_json);
   }

   char *json = cJSON_PrintUnformatted(mapped);
   cJSON_Delete(mapped);
   cJSON_Delete(args);
   return json ? json : safe_strdup(arguments_json);
}

static int command_word_matches(const char *s, const char *word)
{
   size_t len = strlen(word);
   if (strncmp(s, word, len) != 0)
      return 0;
   return s[len] == '\0' || isspace((unsigned char)s[len]);
}

static int command_uses_aimee_stale_context(const char *command)
{
   if (!command || !command[0])
      return 0;

   const char *p = command;
   while ((p = strstr(p, "aimee ")) != NULL)
   {
      const char *sub = p + strlen("aimee ");
      if (strncmp(sub, "index ", 6) == 0)
      {
         const char *idx = sub + 6;
         if (!command_word_matches(idx, "overview") && !command_word_matches(idx, "find") &&
             !command_word_matches(idx, "list") && !command_word_matches(idx, "structure") &&
             !command_word_matches(idx, "callers") && !command_word_matches(idx, "blast-radius"))
            return 1;
      }
      else if (strncmp(sub, "memory ", 7) == 0)
      {
         const char *mem = sub + 7;
         if (!command_word_matches(mem, "search") && !command_word_matches(mem, "list") &&
             !command_word_matches(mem, "get") && !command_word_matches(mem, "read"))
            return 1;
      }
      else if (command_word_matches(sub, "search") || command_word_matches(sub, "docs") ||
               command_word_matches(sub, "mcp") ||
               (strncmp(sub, "kb ", 3) == 0 && command_word_matches(sub + 3, "search")))
      {
         return 1;
      }
      p = sub;
   }
   return 0;
}

static char *current_code_role_policy_error(const char *role, const char *detail)
{
   char err[256];
   snprintf(err, sizeof(err), "error: %s delegates may only use current-checkout evidence; %s",
            role ? role : "this", detail);
   return safe_strdup(err);
}

/* Surgical edit: replace old_string with new_string in an existing file.
 * Reads the whole file (raw, untruncated — tool_read_file caps at 4 KB and
 * would corrupt a round-trip), checks old_string is present and unique (unless
 * replace_all), then writes through tool_write_file, which re-resolves the
 * path, enforces the parent-write guard, and returns the structured diff. */
char *tool_edit_file(const char *path, const char *old_string, const char *new_string,
                     int replace_all)
{
   if (!path || !path[0])
      return safe_strdup("error: missing 'path' parameter");
   if (!old_string || !old_string[0])
      return safe_strdup("error: missing or empty 'old_string' parameter");
   if (!new_string)
      new_string = "";

   char cwd_path[MAX_PATH_LEN];
   const char *actual_path = path_in_thread_cwd(path, cwd_path, sizeof(cwd_path));

   /* Read through the workspace provider (shared = direct fs). The write back
    * already routes through the provider via tool_write_file below. */
   const workspace_provider_t *ws = workspace_provider_active();
   ws_stat_t st;
   ws->stat(ws, actual_path, &st);
   if (!st.exists)
   {
      char errbuf[512];
      snprintf(errbuf, sizeof(errbuf), "error: cannot open %s", actual_path);
      return safe_strdup(errbuf);
   }
   if (st.size >= 8 * 1024 * 1024)
      return safe_strdup("error: file too large to edit (limit 8MB); use write_file instead");

   char *content = NULL;
   size_t rd = 0;
   if (ws->read_all(ws, actual_path, &content, &rd) != 0)
   {
      char errbuf[512];
      snprintf(errbuf, sizeof(errbuf), "error: cannot open %s", actual_path);
      return safe_strdup(errbuf);
   }
   (void)rd;

   /* Count non-overlapping occurrences of old_string. */
   size_t old_len = strlen(old_string);
   size_t count = 0;
   for (const char *p = content; (p = strstr(p, old_string)) != NULL; p += old_len)
      count++;

   if (count == 0)
   {
      free(content);
      return safe_strdup("error: old_string not found in file; read the file and copy the exact "
                         "text (including whitespace and indentation) into old_string");
   }
   if (count > 1 && !replace_all)
   {
      free(content);
      char errbuf[256];
      snprintf(errbuf, sizeof(errbuf),
               "error: old_string occurs %zu times; add surrounding context to make it unique, "
               "or set replace_all=true to replace every occurrence",
               count);
      return safe_strdup(errbuf);
   }

   size_t new_len = strlen(new_string);
   size_t reps = replace_all ? count : 1;
   size_t content_len = strlen(content);
   char *out = malloc(content_len + reps * new_len - reps * old_len + 1);
   if (!out)
   {
      free(content);
      return safe_strdup("error: out of memory");
   }

   char *dst = out;
   const char *src = content;
   size_t done = 0;
   const char *m;
   while (done < reps && (m = strstr(src, old_string)) != NULL)
   {
      size_t prefix = (size_t)(m - src);
      memcpy(dst, src, prefix);
      dst += prefix;
      memcpy(dst, new_string, new_len);
      dst += new_len;
      src = m + old_len;
      done++;
   }
   size_t rem = strlen(src);
   memcpy(dst, src, rem);
   dst += rem;
   *dst = '\0';

   char *result = tool_write_file(path, out);
   free(content);
   free(out);
   return result;
}

/* Delegation conversation: request input from parent agent.
 * delegation_request_input is provided by server_compute.c when running as delegate.
 * Default stub returns NULL; the server overrides this at link time. */

static char *td_bash(cJSON *args, const char *name, const char *dispatch_cwd,
                     const char *dispatch_sid, int timeout_ms)
{
   cJSON *cmd = cJSON_GetObjectItem(args, "command");
   if (!cmd || !cJSON_IsString(cmd))
      return safe_strdup("error: missing 'command' parameter");

   /* Detached workspace (turn bound to a serving client): marshal the shell
    * command — with the thread-local cwd — over the reverse-channel so it runs
    * on the CLIENT's working tree, not the server's filesystem. tool_bash's
    * local fork/exec + read-only fast-paths only apply co-located, and would
    * otherwise fail (the client's cwd does not exist on the server). The shared
    * provider keeps using tool_bash below. */
   const workspace_provider_t *ws = workspace_provider_active();
   if (ws && ws->kind == WS_PROVIDER_DETACHED && ws->exec_shell)
   {
      int exit_code = -1;
      char *out = ws->exec_shell(ws, cmd->valuestring, &exit_code);
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "stdout", out ? out : "");
      cJSON_AddStringToObject(r, "stderr", "");
      cJSON_AddNumberToObject(r, "exit_code", exit_code);
      free(out);
      char *result = cJSON_PrintUnformatted(r);
      cJSON_Delete(r);
      return result ? result : safe_strdup("{}");
   }

   return tool_bash(cmd->valuestring, timeout_ms);
}

static char *td_execute_script(cJSON *args, const char *name, const char *dispatch_cwd,
                               const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *lang = cJSON_GetObjectItem(args, "language");
   cJSON *body = cJSON_GetObjectItem(args, "body");
   cJSON *tout = cJSON_GetObjectItem(args, "timeout_secs");
   cJSON *wd = cJSON_GetObjectItem(args, "workdir");
   cJSON *env = cJSON_GetObjectItem(args, "env");
   char *env_json = cJSON_IsObject(env) ? cJSON_PrintUnformatted(env) : NULL;
   if (!lang || !cJSON_IsString(lang) || !body || !cJSON_IsString(body))
   {
      result = safe_strdup("error: missing 'language' or 'body' parameter");
   }
   else
   {
      int secs = (tout && cJSON_IsNumber(tout)) ? tout->valueint : 120;
      const char *dir = (wd && cJSON_IsString(wd)) ? wd->valuestring : NULL;
      result = tool_execute_script(lang->valuestring, body->valuestring, secs, dir, env_json);
   }
   free(env_json);

   return result;
}

/* tool_output_get (P2): resolve a spill ref to the full raw output aimee condensed —
 * the single first-class recovery handle. */
static char *td_tool_output_get(cJSON *args, const char *name, const char *dispatch_cwd,
                                const char *dispatch_sid, int timeout_ms)
{
   (void)name;
   (void)dispatch_cwd;
   (void)dispatch_sid;
   (void)timeout_ms;
   cJSON *r = cJSON_GetObjectItem(args, "ref");
   if (!r || !cJSON_IsString(r))
      return safe_strdup("error: missing 'ref' parameter");
   char spill_dir[600];
   const char *home = aimee_home();
   if (!home || !home[0] ||
       snprintf(spill_dir, sizeof spill_dir, "%s/tool-spills", home) >= (int)sizeof spill_dir)
      return safe_strdup("error: spill store unavailable");
   char err[64];
   char *full = tool_condense_recall(spill_dir, r->valuestring, err, sizeof err);
   if (!full)
   {
      char msg[128];
      snprintf(msg, sizeof msg, "error: %s", err[0] ? err : "not found");
      return safe_strdup(msg);
   }
   return full;
}

static char *td_read_file(cJSON *args, const char *name, const char *dispatch_cwd,
                          const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *p = cJSON_GetObjectItem(args, "path");
   if (!p || !cJSON_IsString(p))
   {
      result = safe_strdup("error: missing 'path' parameter");
   }
   else
   {
      cJSON *off = cJSON_GetObjectItem(args, "offset");
      cJSON *lim = cJSON_GetObjectItem(args, "limit");
      int offset = (off && cJSON_IsNumber(off)) ? off->valueint : 0;
      int limit = (lim && cJSON_IsNumber(lim)) ? lim->valueint : 0;
      result = tool_read_file(p->valuestring, offset, limit);

      /* Record the read in the session state for read-before-write tracking. */
      if (result && strncmp(result, "error:", 6) != 0)
      {
         session_state_t rs;
         session_state_load(&rs, dispatch_sid);
         char abs_path[MAX_PATH_LEN];
         normalize_path(p->valuestring, dispatch_cwd, abs_path, sizeof(abs_path));
         session_record_read(&rs, abs_path);
         session_state_save(&rs, dispatch_sid);
      }
   }

   return result;
}

static char *td_edit_file(cJSON *args, const char *name, const char *dispatch_cwd,
                          const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *p = cJSON_GetObjectItem(args, "path");
   cJSON *o = cJSON_GetObjectItem(args, "old_string");
   cJSON *nw = cJSON_GetObjectItem(args, "new_string");
   cJSON *ra = cJSON_GetObjectItem(args, "replace_all");
   if (!p || !cJSON_IsString(p))
      result = safe_strdup("error: missing 'path' parameter");
   else if (!o || !cJSON_IsString(o))
      result = safe_strdup("error: missing 'old_string' parameter");
   else
   {
      const char *new_str = (nw && cJSON_IsString(nw)) ? nw->valuestring : "";
      int replace_all = (ra && cJSON_IsBool(ra)) ? cJSON_IsTrue(ra) : 0;
      if (agent_tools_parent_write_guard_blocks(p->valuestring, dispatch_cwd))
      {
         result = safe_strdup("error: write blocked: parent worktree is read-only for delegates");
      }
      else if (agent_tools_session_isolation_blocks(p->valuestring, dispatch_cwd))
      {
         result = safe_strdup("error: write blocked: require_session_worktree is enabled and this "
                              "target is outside an aimee-managed worktree (.aimee/worktrees/...)");
      }
      else
      {
         /* Auto-snapshot: record pre-edit state in the persistent rewind DB */
         auto_snapshot_record(p->valuestring);
         result = tool_edit_file(p->valuestring, o->valuestring, new_str, replace_all);
      }
      /* Record the write under the active delegation id (mirrors write_file). */
      if (result && strncmp(result, "error:", 6) != 0)
      {
         char abs_path[MAX_PATH_LEN];
         normalize_path(p->valuestring, dispatch_cwd, abs_path, sizeof(abs_path));
         const char *write_key = delegation_active_id();
         (void)db1_session_write_path_record(write_key ? write_key : dispatch_sid, abs_path);
      }
   }

   return result;
}

static char *td_write_file(cJSON *args, const char *name, const char *dispatch_cwd,
                           const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *p = cJSON_GetObjectItem(args, "path");
   cJSON *c = cJSON_GetObjectItem(args, "content");
   if (!p || !cJSON_IsString(p))
      result = safe_strdup("error: missing 'path' parameter");
   else
   {
      const char *content_str = c && cJSON_IsString(c) ? c->valuestring : "";
      if (agent_tools_parent_write_guard_blocks(p->valuestring, dispatch_cwd))
      {
         result = safe_strdup("error: write blocked: parent worktree is read-only for delegates");
      }
      else if (agent_tools_session_isolation_blocks(p->valuestring, dispatch_cwd))
      {
         result = safe_strdup("error: write blocked: require_session_worktree is enabled and this "
                              "target is outside an aimee-managed worktree (.aimee/worktrees/...)");
      }
      else
      {
         /* Auto-snapshot: record pre-write state in the persistent rewind DB */
         auto_snapshot_record(p->valuestring);
         result = tool_write_file(p->valuestring, content_str);
      }
      /* Record the write under the active delegation id (when running
       * as a delegate) or the dispatch session id otherwise. Pairs with
       * read tracking on the read_file path so db1_session_stale_reads
       * can warn the parent when a child writes a file the parent had
       * already read. Skipped on tool_write_file error. */
      if (result && strncmp(result, "error:", 6) != 0)
      {
         char abs_path[MAX_PATH_LEN];
         normalize_path(p->valuestring, dispatch_cwd, abs_path, sizeof(abs_path));
         const char *write_key = delegation_active_id();
         (void)db1_session_write_path_record(write_key ? write_key : dispatch_sid, abs_path);
      }
      /* Append recovery hint on write failure */
      if (result && strncmp(result, "error:", 6) == 0)
      {
         size_t rlen = strlen(result);
         const char *hint = "\nRecovery: read the file with read_file before retrying.";
         size_t hlen = strlen(hint);
         char *augmented = malloc(rlen + hlen + 1);
         if (augmented)
         {
            memcpy(augmented, result, rlen);
            memcpy(augmented + rlen, hint, hlen + 1);
            free(result);
            result = augmented;
         }
      }
      else if (result && content_str[0])
      {
         /* Advisory slop scan on written content. */
         slop_finding_t slop[16];
         int nslop = slop_detect_buf(content_str, 0, slop, 16);
         if (nslop > 0)
         {
            /* Preserve structured write results while surfacing advisories. */
            char *augmented = append_write_slop_advisory(result, slop, nslop);
            if (augmented)
            {
               free(result);
               result = augmented;
            }
         }

         /* Post-edit LSP diagnostic refresh: if an LSP server is active for
          * this file's extension, fetch any new errors/warnings and append
          * them to the result. Capped at 6 entries so the context stays tight. */
         {
            config_t lsp_cfg;
            config_load(&lsp_cfg);
            char ws[MAX_PATH_LEN] = "";
            if (workspace_active_root(&lsp_cfg, dispatch_cwd, ws, sizeof(ws)) != 0)
               snprintf(ws, sizeof(ws), "%s", dispatch_cwd);
            lsp_diag_t lsp_diags[6];
            int nlsp = lsp_manager_diagnostics(ws, p->valuestring, lsp_diags, 6);
            if (nlsp > 0)
            {
               /* Filter to errors and warnings only */
               int filtered = 0;
               lsp_diag_t kept[6];
               for (int li = 0; li < nlsp && filtered < 6; li++)
               {
                  if (lsp_diags[li].severity == LSP_SEV_ERROR ||
                      lsp_diags[li].severity == LSP_SEV_WARNING)
                     kept[filtered++] = lsp_diags[li];
               }
               if (filtered > 0)
               {
                  /* Build compact diagnostic string and attach to JSON result. */
                  char diag_buf[1024];
                  char *dp = diag_buf;
                  size_t dleft = sizeof(diag_buf);
                  for (int li = 0; li < filtered && dleft > 2; li++)
                  {
                     int n = snprintf(dp, dleft, "%s%s:%d [%s] %s", li > 0 ? "; " : "",
                                      kept[li].file[0] ? kept[li].file : p->valuestring,
                                      kept[li].line + 1, lsp_severity_label(kept[li].severity),
                                      kept[li].message);
                     if (n > 0 && (size_t)n < dleft)
                     {
                        dp += n;
                        dleft -= (size_t)n;
                     }
                  }

                  cJSON *jres = cJSON_Parse(result);
                  if (jres && cJSON_IsObject(jres))
                  {
                     cJSON_AddStringToObject(jres, "lsp_diagnostics", diag_buf);
                     char *augmented = cJSON_PrintUnformatted(jres);
                     if (augmented)
                     {
                        free(result);
                        result = augmented;
                     }
                  }
                  cJSON_Delete(jres);
               }
            }
         }
      }
   }

   return result;
}

static char *td_list_files(cJSON *args, const char *name, const char *dispatch_cwd,
                           const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *p = cJSON_GetObjectItem(args, "path");
   if (!p || !cJSON_IsString(p))
   {
      result = safe_strdup("error: missing 'path' parameter");
   }
   else
   {
      cJSON *pat = cJSON_GetObjectItem(args, "pattern");
      result =
          tool_list_files(p->valuestring, (pat && cJSON_IsString(pat)) ? pat->valuestring : NULL);
   }

   return result;
}

static char *td_verify(cJSON *args, const char *name, const char *dispatch_cwd,
                       const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *ct = cJSON_GetObjectItem(args, "check_type");
   cJSON *tgt = cJSON_GetObjectItem(args, "target");
   cJSON *exp = cJSON_GetObjectItem(args, "expected");
   if (!ct || !cJSON_IsString(ct) || !tgt || !cJSON_IsString(tgt))
      result = safe_strdup("error: missing 'check_type' or 'target'");
   else
      result = tool_verify(ct->valuestring, tgt->valuestring,
                           (exp && cJSON_IsString(exp)) ? exp->valuestring : NULL);

   return result;
}

static char *td_git_log(cJSON *args, const char *name, const char *dispatch_cwd,
                        const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *p = cJSON_GetObjectItem(args, "path");
   cJSON *n = cJSON_GetObjectItem(args, "count");
   if (!p || !cJSON_IsString(p))
      result = safe_strdup("error: missing 'path' parameter");
   else
      result = tool_git_log(p->valuestring, (n && cJSON_IsNumber(n)) ? n->valueint : 10);

   return result;
}

static char *td_grep(cJSON *args, const char *name, const char *dispatch_cwd,
                     const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *p = cJSON_GetObjectItem(args, "path");
   cJSON *pat = cJSON_GetObjectItem(args, "pattern");
   cJSON *mx = cJSON_GetObjectItem(args, "max_results");
   if (!p || !cJSON_IsString(p) || !pat || !cJSON_IsString(pat))
      result = safe_strdup("error: missing 'path' or 'pattern' parameter");
   else
      result = tool_grep(p->valuestring, pat->valuestring,
                         (mx && cJSON_IsNumber(mx)) ? mx->valueint : 50);

   return result;
}

static char *td_git_diff(cJSON *args, const char *name, const char *dispatch_cwd,
                         const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *p = cJSON_GetObjectItem(args, "path");
   cJSON *r = cJSON_GetObjectItem(args, "ref");
   if (!p || !cJSON_IsString(p))
      result = safe_strdup("error: missing 'path' parameter");
   else
      result = tool_git_diff(p->valuestring, (r && cJSON_IsString(r)) ? r->valuestring : NULL);

   return result;
}

static char *td_git_status(cJSON *args, const char *name, const char *dispatch_cwd,
                           const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *p = cJSON_GetObjectItem(args, "path");
   if (!p || !cJSON_IsString(p))
      result = safe_strdup("error: missing 'path' parameter");
   else
      result = tool_git_status(p->valuestring);

   return result;
}

static char *td_env_get(cJSON *args, const char *name, const char *dispatch_cwd,
                        const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *n = cJSON_GetObjectItem(args, "name");
   if (!n || !cJSON_IsString(n))
      result = safe_strdup("error: missing 'name' parameter");
   else
      result = tool_env_get(n->valuestring);

   return result;
}

static char *td_test(cJSON *args, const char *name, const char *dispatch_cwd,
                     const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *p = cJSON_GetObjectItem(args, "path");
   cJSON *c = cJSON_GetObjectItem(args, "check");
   if (!p || !cJSON_IsString(p))
      result = safe_strdup("error: missing 'path' parameter");
   else
      result = tool_test(p->valuestring, (c && cJSON_IsString(c)) ? c->valuestring : NULL);

   return result;
}

static char *td_request_input(cJSON *args, const char *name, const char *dispatch_cwd,
                              const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *q = cJSON_GetObjectItem(args, "question");
   if (!q || !cJSON_IsString(q))
      result = safe_strdup("error: missing 'question' parameter");
   else
      result = tool_request_input(q->valuestring);

   return result;
}

static char *td_code_search(cJSON *args, const char *name, const char *dispatch_cwd,
                            const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *q = cJSON_GetObjectItem(args, "query");
   cJSON *p = cJSON_GetObjectItem(args, "project");
   cJSON *mx = cJSON_GetObjectItem(args, "max_results");
   if (!q || !cJSON_IsString(q))
      result = safe_strdup("error: missing 'query' parameter");
   else
      result = tool_code_search(q->valuestring, (p && cJSON_IsString(p)) ? p->valuestring : NULL,
                                (mx && cJSON_IsNumber(mx)) ? mx->valueint : 50);

   return result;
}

static char *td_find_symbol(cJSON *args, const char *name, const char *dispatch_cwd,
                            const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *id = cJSON_GetObjectItem(args, "identifier");
   if (!id || !cJSON_IsString(id))
   {
      result = safe_strdup("error: missing 'identifier' parameter");
   }
   else
   {
      result = tool_find_symbol(id->valuestring);
   }

   return result;
}

static char *td_search_memory(cJSON *args, const char *name, const char *dispatch_cwd,
                              const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *q = cJSON_GetObjectItem(args, "query");
   if (!q || !cJSON_IsString(q))
   {
      result = safe_strdup("error: missing 'query' parameter");
   }
   else
   {
      memory_t facts[20];
      int count = kb_client_memory_find_facts(q->valuestring, 20, facts, 20);
      char buf[8192];
      int pos = 0;
      if (count <= 0)
         pos += snprintf(buf, sizeof(buf), "No facts found for '%s'", q->valuestring);
      else
      {
         pos += snprintf(buf, sizeof(buf), "Found %d fact(s):\n\n", count);
         for (int i = 0; i < count && pos < (int)sizeof(buf) - 512; i++)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "- **%s** [%s/%s]: %s\n", facts[i].key,
                            facts[i].tier, facts[i].kind, facts[i].content);
      }
      result = safe_strdup(buf);
   }

   return result;
}

static char *td_web_search(cJSON *args, const char *name, const char *dispatch_cwd,
                           const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *q = cJSON_GetObjectItem(args, "query");
   cJSON *mx = cJSON_GetObjectItem(args, "max_results");
   if (!q || !cJSON_IsString(q))
      result = safe_strdup("error: missing 'query' parameter");
   else
      result = web_search(q->valuestring, (mx && cJSON_IsNumber(mx)) ? mx->valueint : 5);

   return result;
}

static char *td_create_note(cJSON *args, const char *name, const char *dispatch_cwd,
                            const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *t = cJSON_GetObjectItem(args, "title");
   cJSON *c = cJSON_GetObjectItem(args, "content");
   cJSON *tg = cJSON_GetObjectItem(args, "tags");
   if (!t || !cJSON_IsString(t) || !c || !cJSON_IsString(c))
      result = safe_strdup("error: missing 'title' or 'content' parameter");
   else
      result = tool_create_note(t->valuestring, c->valuestring,
                                (tg && cJSON_IsString(tg)) ? tg->valuestring : NULL);

   return result;
}

static char *td_list_notes(cJSON *args, const char *name, const char *dispatch_cwd,
                           const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *tg = cJSON_GetObjectItem(args, "tag");
   cJSON *lm = cJSON_GetObjectItem(args, "limit");
   result = tool_list_notes((tg && cJSON_IsString(tg)) ? tg->valuestring : NULL,
                            (lm && cJSON_IsNumber(lm)) ? lm->valueint : 20);

   return result;
}

static char *td_search_notes(cJSON *args, const char *name, const char *dispatch_cwd,
                             const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *q = cJSON_GetObjectItem(args, "query");
   if (!q || !cJSON_IsString(q))
      result = safe_strdup("error: missing 'query' parameter");
   else
      result = tool_search_notes(q->valuestring);

   return result;
}

static char *td_run_background_process(cJSON *args, const char *name, const char *dispatch_cwd,
                                       const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *cmd = cJSON_GetObjectItem(args, "command");
   cJSON *cwd = cJSON_GetObjectItem(args, "cwd");
   if (!cmd || !cJSON_IsString(cmd))
   {
      result = safe_strdup("error: missing 'command' parameter");
   }
   else if (agent_tools_parent_write_guard_root())
   {
      result = safe_strdup(
          "error: background processes are blocked while the parent worktree is read-only");
   }
   else
   {
      char errbuf[256] = "";
      int id = proc_start(cmd->valuestring, (cwd && cJSON_IsString(cwd)) ? cwd->valuestring : NULL,
                          errbuf, sizeof(errbuf));
      if (id < 0)
      {
         result = safe_strdup(errbuf[0] ? errbuf : "error: proc_start failed");
      }
      else
      {
         char buf[64];
         snprintf(buf, sizeof(buf), "{\"id\":%d,\"status\":\"started\"}", id);
         result = safe_strdup(buf);
      }
   }

   return result;
}

static char *td_get_background_output(cJSON *args, const char *name, const char *dispatch_cwd,
                                      const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *jid = cJSON_GetObjectItem(args, "id");
   cJSON *jtail = cJSON_GetObjectItem(args, "tail_lines");
   if (!jid || !cJSON_IsNumber(jid))
   {
      result = safe_strdup("error: missing 'id' parameter");
   }
   else
   {
      int id = jid->valueint;
      int tail = (jtail && cJSON_IsNumber(jtail)) ? jtail->valueint : 50;
      char *out = malloc(131072);
      if (!out)
      {
         result = safe_strdup("error: out of memory");
      }
      else
      {
         proc_get_output(id, tail, out, 131072);
         result = out;
      }
   }

   return result;
}

static char *td_kill_background_process(cJSON *args, const char *name, const char *dispatch_cwd,
                                        const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *jid = cJSON_GetObjectItem(args, "id");
   if (!jid || !cJSON_IsNumber(jid))
   {
      result = safe_strdup("error: missing 'id' parameter");
   }
   else
   {
      int id = jid->valueint;
      if (proc_kill(id) == 0)
      {
         char buf[64];
         snprintf(buf, sizeof(buf), "{\"id\":%d,\"status\":\"killed\"}", id);
         result = safe_strdup(buf);
      }
      else
      {
         char buf[64];
         snprintf(buf, sizeof(buf), "error: process id %d not found or already exited", id);
         result = safe_strdup(buf);
      }
   }

   return result;
}

static char *td_list_background_processes(cJSON *args, const char *name, const char *dispatch_cwd,
                                          const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   char *out = malloc(32768);
   if (!out)
      result = safe_strdup("[]");
   else
   {
      proc_list(out, 32768);
      result = out;
   }

   return result;
}

static char *td_rules_propose(cJSON *args, const char *name, const char *dispatch_cwd,
                              const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *jtext = cJSON_GetObjectItem(args, "text");
   cJSON *jreason = cJSON_GetObjectItem(args, "reason");
   if (!jtext || !cJSON_IsString(jtext) || !jtext->valuestring[0])
   {
      result = safe_strdup("error: missing or empty 'text' parameter");
   }
   else
   {
      const char *reason = (jreason && cJSON_IsString(jreason)) ? jreason->valuestring : "";
      int id = kb_client_collab_rules_propose(jtext->valuestring, reason, "agent");
      if (id >= 0)
      {
         char buf[128];
         snprintf(buf, sizeof(buf),
                  "{\"id\":%d,\"status\":\"proposed\",\"message\":"
                  "\"Rule proposed. Awaiting human approval.\"}",
                  id);
         result = safe_strdup(buf);
      }
      else
      {
         result = safe_strdup("error: could not propose rule");
      }
   }

   return result;
}

static char *td_rules_list(cJSON *args, const char *name, const char *dispatch_cwd,
                           const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   char *json = kb_client_collab_rules_list_active_json();
   result = json ? json : safe_strdup("{\"epoch\":0,\"rules\":[]}");

   return result;
}

static char *td_learning_propose(cJSON *args, const char *name, const char *dispatch_cwd,
                                 const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *jsig = cJSON_GetObjectItem(args, "signal_type");
   if (!jsig || !cJSON_IsString(jsig) || !jsig->valuestring[0])
   {
      result = safe_strdup("error: missing 'signal_type' parameter");
   }
   else
   {
      char *envelope = kb_client_learning_propose_signal_json(args);
      cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
      free(envelope);
      cJSON *status = resp ? cJSON_GetObjectItemCaseSensitive(resp, "status") : NULL;
      if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0)
      {
         cJSON *dispatch = cJSON_GetObjectItemCaseSensitive(resp, "dispatch");
         if (cJSON_IsObject(dispatch))
         {
            cJSON *detached = cJSON_DetachItemViaPointer(resp, dispatch);
            result = detached ? cJSON_PrintUnformatted(detached) : safe_strdup("{}");
            cJSON_Delete(detached);
         }
         else
         {
            result = safe_strdup("{\"signal_id\":0}");
         }
      }
      else
      {
         result = safe_strdup("error: failed to record learning signal");
      }
      cJSON_Delete(resp);
   }

   return result;
}

static char *td_learning_review(cJSON *args, const char *name, const char *dispatch_cwd,
                                const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   const char *state = "pending";
   const char *sink = NULL;
   int limit = 20;
   cJSON *item = cJSON_GetObjectItem(args, "state");
   if (cJSON_IsString(item) && item->valuestring[0])
      state = item->valuestring;
   item = cJSON_GetObjectItem(args, "sink");
   if (cJSON_IsString(item) && item->valuestring[0])
      sink = item->valuestring;
   item = cJSON_GetObjectItem(args, "limit");
   if (cJSON_IsNumber(item) && item->valueint > 0)
      limit = item->valueint;

   char *json = kb_client_learning_list_proposals_json(state, sink, limit);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
   {
      result = safe_strdup("error: failed to list learning proposals");
   }
   else
   {
      cJSON *proposals = cJSON_GetObjectItemCaseSensitive(resp, "proposals");
      if (cJSON_IsArray(proposals))
      {
         cJSON *detached = cJSON_DetachItemViaPointer(resp, proposals);
         result = detached ? cJSON_PrintUnformatted(detached) : safe_strdup("[]");
         cJSON_Delete(detached);
      }
      else
      {
         result = safe_strdup("[]");
      }
      cJSON_Delete(resp);
   }

   return result;
}

static char *td_clarify_start(cJSON *args, const char *name, const char *dispatch_cwd,
                              const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *jdesc = cJSON_GetObjectItem(args, "description");
   if (!jdesc || !cJSON_IsString(jdesc) || !jdesc->valuestring[0])
   {
      result = safe_strdup("error: missing or empty 'description' parameter");
   }
   else
   {
      config_t cfg;
      if (config_load(&cfg) != 0 || db1_init(cfg.db1_path) != 0)
         result = safe_strdup("error: server storage unavailable");
      else
      {
         clarify_session_t s;
         int id = db1_clarify_start(jdesc->valuestring, &s);
         if (id < 0)
            result = safe_strdup("error: could not start clarification session");
         else
         {
            char *json = db1_clarify_to_json(&s);
            result = json ? json : safe_strdup("{\"error\":\"serialisation failed\"}");
         }
      }
   }

   return result;
}

static char *td_clarify_answer(cJSON *args, const char *name, const char *dispatch_cwd,
                               const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *jid = cJSON_GetObjectItem(args, "session_id");
   cJSON *jans = cJSON_GetObjectItem(args, "answer");
   int sid = (jid && cJSON_IsNumber(jid)) ? jid->valueint : -1;
   if (sid < 1 || !jans || !cJSON_IsString(jans) || !jans->valuestring[0])
   {
      result = safe_strdup("error: require positive 'session_id' and non-empty 'answer'");
   }
   else
   {
      config_t cfg;
      if (config_load(&cfg) != 0 || db1_init(cfg.db1_path) != 0)
         result = safe_strdup("error: server storage unavailable");
      else
      {
         clarify_session_t s;
         int rc = db1_clarify_answer(sid, jans->valuestring, &s);
         if (rc != 0)
            result = safe_strdup("error: could not record answer — session not found or not open");
         else
         {
            char *json = db1_clarify_to_json(&s);
            result = json ? json : safe_strdup("{\"error\":\"serialisation failed\"}");
         }
      }
   }

   return result;
}

static char *td_diagnose_start(cJSON *args, const char *name, const char *dispatch_cwd,
                               const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *jsym = cJSON_GetObjectItem(args, "symptom");
   if (!jsym || !cJSON_IsString(jsym) || !jsym->valuestring[0])
   {
      result = safe_strdup("error: missing or empty 'symptom' parameter");
   }
   else
   {
      config_t cfg;
      if (config_load(&cfg) != 0 || db1_init(cfg.db1_path) != 0)
         result = safe_strdup("error: server storage unavailable");
      else
      {
         int id = db1_diagnose_start(jsym->valuestring);
         if (id < 0)
            result = safe_strdup("error: could not start diagnosis");
         else
         {
            char buf[96];
            snprintf(buf, sizeof(buf), "{\"diagnosis_id\":%d,\"status\":\"active\"}", id);
            result = safe_strdup(buf);
         }
      }
   }

   return result;
}

static char *td_diagnose_observe(cJSON *args, const char *name, const char *dispatch_cwd,
                                 const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *jid = cJSON_GetObjectItem(args, "diagnosis_id");
   cJSON *jcontent = cJSON_GetObjectItem(args, "content");
   cJSON *jsource = cJSON_GetObjectItem(args, "source");
   int diag_id = (jid && cJSON_IsNumber(jid)) ? jid->valueint : -1;
   if (diag_id < 1 || !jcontent || !cJSON_IsString(jcontent) || !jcontent->valuestring[0])
   {
      result = safe_strdup("error: require positive 'diagnosis_id' and non-empty 'content'");
   }
   else
   {
      config_t cfg;
      if (config_load(&cfg) != 0 || db1_init(cfg.db1_path) != 0)
         result = safe_strdup("error: server storage unavailable");
      else
      {
         int id;
         if (strcmp(name, "diagnose_observe") == 0)
         {
            const char *src = (jsource && cJSON_IsString(jsource)) ? jsource->valuestring : "";
            id = db1_diagnose_add_observation(diag_id, jcontent->valuestring, src);
         }
         else
         {
            id = db1_diagnose_add_hypothesis(diag_id, jcontent->valuestring);
         }
         if (id < 0)
            result = safe_strdup("error: could not record item");
         else
         {
            char buf[96];
            snprintf(buf, sizeof(buf), "{\"item_id\":%d}", id);
            result = safe_strdup(buf);
         }
      }
   }

   return result;
}

static char *td_diagnose_evidence(cJSON *args, const char *name, const char *dispatch_cwd,
                                  const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *jid = cJSON_GetObjectItem(args, "diagnosis_id");
   cJSON *jhid = cJSON_GetObjectItem(args, "hypothesis_id");
   cJSON *jstance = cJSON_GetObjectItem(args, "stance");
   cJSON *jcontent = cJSON_GetObjectItem(args, "content");
   cJSON *jrank = cJSON_GetObjectItem(args, "rank");
   cJSON *jsource = cJSON_GetObjectItem(args, "source");
   int diag_id = (jid && cJSON_IsNumber(jid)) ? jid->valueint : -1;
   int hyp_id = (jhid && cJSON_IsNumber(jhid)) ? jhid->valueint : -1;
   const char *stance = (jstance && cJSON_IsString(jstance)) ? jstance->valuestring : "";
   if (diag_id < 1 || hyp_id < 1 || !jcontent || !cJSON_IsString(jcontent) ||
       !jcontent->valuestring[0] || !stance[0])
   {
      result = safe_strdup(
          "error: require positive ids, non-empty content, and stance ('for'|'against')");
   }
   else
   {
      const char *kind = (strcmp(stance, "for") == 0)       ? "evidence_for"
                         : (strcmp(stance, "against") == 0) ? "evidence_against"
                                                            : NULL;
      if (!kind)
      {
         result = safe_strdup("error: stance must be 'for' or 'against'");
      }
      else
      {
         int rank = DIAG_RANK_CODE;
         if (jrank && cJSON_IsString(jrank))
         {
            const char *r = jrank->valuestring;
            if (strcmp(r, "direct") == 0)
               rank = DIAG_RANK_DIRECT;
            else if (strcmp(r, "log") == 0 || strcmp(r, "metric") == 0)
               rank = DIAG_RANK_LOG;
            else if (strcmp(r, "code") == 0)
               rank = DIAG_RANK_CODE;
            else if (strcmp(r, "speculation") == 0)
               rank = DIAG_RANK_SPECULATION;
         }
         const char *src = (jsource && cJSON_IsString(jsource)) ? jsource->valuestring : "";
         config_t cfg;
         if (config_load(&cfg) != 0 || db1_init(cfg.db1_path) != 0)
            result = safe_strdup("error: server storage unavailable");
         else
         {
            int id =
                db1_diagnose_add_evidence(diag_id, hyp_id, kind, jcontent->valuestring, src, rank);
            if (id < 0)
               result = safe_strdup("error: could not record evidence");
            else
            {
               char buf[96];
               snprintf(buf, sizeof(buf), "{\"item_id\":%d}", id);
               result = safe_strdup(buf);
            }
         }
      }
   }

   return result;
}

static char *td_diagnose_status(cJSON *args, const char *name, const char *dispatch_cwd,
                                const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *jid = cJSON_GetObjectItem(args, "diagnosis_id");
   int diag_id = (jid && cJSON_IsNumber(jid)) ? jid->valueint : -1;
   if (diag_id < 1)
   {
      result = safe_strdup("error: require positive 'diagnosis_id'");
   }
   else
   {
      config_t cfg;
      if (config_load(&cfg) != 0 || db1_init(cfg.db1_path) != 0)
         result = safe_strdup("error: server storage unavailable");
      else
      {
         char *json = db1_diagnose_json_full(diag_id);
         if (!json)
            result = safe_strdup("error: diagnosis not found");
         else
            result = json;
      }
   }

   return result;
}

static char *td_search_docs(cJSON *args, const char *name, const char *dispatch_cwd,
                            const char *dispatch_sid, int timeout_ms)
{
   char *result = NULL;
   cJSON *q = cJSON_GetObjectItem(args, "query");
   cJSON *mx = cJSON_GetObjectItem(args, "max_results");
   if (!q || !cJSON_IsString(q) || !q->valuestring[0])
   {
      result = safe_strdup("error: missing 'query' parameter");
   }
   else
   {
      config_t cfg;
      config_load(&cfg);
      int max = (mx && cJSON_IsNumber(mx)) ? mx->valueint : 3;
      char *envelope = kb_client_search_json(NULL, q->valuestring,
                                             config_embedding_command(&cfg, NULL), max, NULL);
      /* The knowledge service returns {"status":"ok","result":"<text>"}; unwrap so the
       * tool sees the same body shape kb_search() used to return. */
      cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
      free(envelope);
      cJSON *body = resp ? cJSON_GetObjectItemCaseSensitive(resp, "result") : NULL;
      if (cJSON_IsString(body) && body->valuestring[0])
         result = safe_strdup(body->valuestring);
      else
         result = safe_strdup("error: knowledge search unavailable");
      cJSON_Delete(resp);
   }

   return result;
}

static char *dispatch_tool_call_ctx_inner(const char *name, const char *arguments_json,
                                          int timeout_ms);

/* Public entry: fire the tool-event hook (no-op unless a worker installed one)
 * around the actual dispatch, so streaming/run consumers see started/completed
 * without the dispatcher's many return paths needing to care. */
char *dispatch_tool_call_ctx(const char *name, const char *arguments_json, int timeout_ms)
{
   agent_tools_emit_tool_event("started", name);
   char *result = dispatch_tool_call_ctx_inner(name, arguments_json, timeout_ms);
   agent_tools_emit_tool_event("completed", name);
   return result;
}

static char *dispatch_tool_call_ctx_inner(const char *name, const char *arguments_json,
                                          int timeout_ms)
{
   cJSON *args = cJSON_Parse(arguments_json);
   if (!args)
      return safe_strdup("error: invalid arguments JSON");

   char cancel_msg[128];
   if (tool_dispatch_cancelled(cancel_msg, sizeof(cancel_msg)))
   {
      cJSON_Delete(args);
      return safe_strdup(cancel_msg);
   }

   /* --- Argument normalization: resolve common aliases --- */
   {
      static const struct
      {
         const char *from;
         const char *to;
      } aliases[] = {{"filepath", "path"}, {"file_path", "path"}, {"file", "path"},
                     {"filename", "path"}, {"file_name", "path"}, {"cmd", "command"},
                     {"dir", "path"},      {"directory", "path"}, {"msg", "message"},
                     {NULL, NULL}};
      for (int i = 0; aliases[i].from; i++)
      {
         cJSON *f = cJSON_GetObjectItem(args, aliases[i].from);
         if (f && !cJSON_GetObjectItem(args, aliases[i].to))
         {
            cJSON *det = cJSON_DetachItemFromObject(args, aliases[i].from);
            if (det)
               cJSON_AddItemToObject(args, aliases[i].to, det);
         }
      }
      /* Coerce string integers: "5" → 5 for offset/limit/count fields */
      static const char *int_fields[] = {"offset", "limit", "count", "max_results", NULL};
      for (int i = 0; int_fields[i]; i++)
      {
         cJSON *f = cJSON_GetObjectItem(args, int_fields[i]);
         if (f && cJSON_IsString(f))
         {
            char *end = NULL;
            long v = strtol(f->valuestring, &end, 10);
            if (end && *end == '\0' && f->valuestring != end)
               cJSON_ReplaceItemInObject(args, int_fields[i], cJSON_CreateNumber(v));
         }
      }

      /* Schema-driven coercion (small/local model providers emit ints/bools
       * as strings, scalars where arrays are expected, JSON strings where
       * objects are expected). Runs after the targeted int-field block so
       * tools whose schema isn't in the registry still benefit from the
       * fallback. */
      cJSON *schema = agent_tool_get_schema_cached(name);
      if (schema)
      {
         cJSON *coerced = tool_args_coerce(schema, args);
         if (coerced && coerced != args)
         {
            cJSON_Delete(args);
            args = coerced;
         }
      }
   }

   const char *active_role = agent_tools_dispatch_role();
   if (!agent_tools_tool_allowed_for_role(active_role, name))
   {
      cJSON_Delete(args);
      return current_code_role_policy_error(
          active_role, "indexed, memory, docs, notes, and remote MCP tools are "
                       "disabled for this role");
   }
   if (agent_tools_role_current_code_only(active_role))
   {
      const char *command = NULL;
      if (strcmp(name, "bash") == 0)
      {
         cJSON *cmd = cJSON_GetObjectItem(args, "command");
         if (cJSON_IsString(cmd))
            command = cmd->valuestring;
      }
      else if (strcmp(name, "execute_script") == 0)
      {
         cJSON *body = cJSON_GetObjectItem(args, "body");
         if (cJSON_IsString(body))
            command = body->valuestring;
      }
      if (command_uses_aimee_stale_context(command))
      {
         cJSON_Delete(args);
         return current_code_role_policy_error(
             active_role, "mutating or broad aimee context commands are disabled for this role");
      }
   }

   /* --- Guardrail enforcement for ALL tool execution paths --- */
   /* dispatch_sid and cwd are kept in scope so read_file can update read-tracking below. */
   const char *dispatch_sid = session_id();
   char dispatch_cwd[MAX_PATH_LEN];
   {
      /* Prefer the thread-local CWD set by run_cmd_set_cwd() — delegates set this
       * to their isolated worktree path before running, so path-tool arguments
       * and guardrail checks resolve relative paths inside the delegate worktree
       * instead of the server process CWD. Fall back to getcwd() for sessions
       * that have not set a thread-local CWD (CLI, direct HTTP callers, etc.). */
      const char *tl_cwd = run_cmd_get_cwd();
      if (tl_cwd && tl_cwd[0])
         snprintf(dispatch_cwd, sizeof(dispatch_cwd), "%s", tl_cwd);
      else if (!getcwd(dispatch_cwd, sizeof(dispatch_cwd)))
         dispatch_cwd[0] = '\0';
   }

   {
      config_t cfg;
      config_load(&cfg);
      /* DB1 backs session_state now. In production, aimee-server / CLI main
       * already called db1_init; this is idempotent. Keeping the call here
       * so delegate subprocesses that reach dispatch without going through
       * a main-opened DB1 still persist read-before-write tracking. */
      db1_init(cfg.db1_path);

      session_state_t state;
      session_state_load(&state, dispatch_sid);

      /* Mark as delegate so orchestrator self-discipline checks are suppressed.
       * aimee delegate agents are supposed to edit implementation files directly;
       * the orchestrator coordination reminders do not apply to them. */
      state.is_delegate = 1;

      char *gr_input = guardrail_input_json(name, arguments_json);

      char msg[1024] = "";
      int rc = pre_tool_check(name, gr_input, &state, config_guardrail_mode(&cfg), dispatch_cwd,
                              msg, sizeof(msg));

      session_state_save(&state, dispatch_sid);
      free(gr_input);

      if (rc == 1 && msg[0])
      {
         /* Worktree path rewrite: update file_path in args and continue */
         cJSON *fp_arg = cJSON_GetObjectItem(args, "file_path");
         if (!fp_arg)
            fp_arg = cJSON_GetObjectItem(args, "path");
         if (fp_arg)
            cJSON_SetValuestring(fp_arg, msg);
      }
      else if (rc != 0)
      {
         /* Tool blocked by guardrails */
         cJSON_Delete(args);
         char err[1200];
         snprintf(err, sizeof(err), "error: guardrail blocked: %s", msg);
         return safe_strdup(err);
      }
   }

   char *result = NULL;

   if (strcmp(name, "bash") == 0)
      result = td_bash(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "execute_script") == 0)
      result = td_execute_script(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "read_file") == 0)
      result = td_read_file(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "tool_output_get") == 0)
      result = td_tool_output_get(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "edit_file") == 0)
      result = td_edit_file(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "write_file") == 0)
      result = td_write_file(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "list_files") == 0)
      result = td_list_files(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "verify") == 0)
      result = td_verify(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "git_log") == 0)
      result = td_git_log(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "grep") == 0)
      result = td_grep(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "git_diff") == 0)
      result = td_git_diff(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "git_status") == 0)
      result = td_git_status(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "env_get") == 0)
      result = td_env_get(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "test") == 0)
      result = td_test(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "request_input") == 0)
      result = td_request_input(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "code_search") == 0)
      result = td_code_search(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "find_symbol") == 0)
      result = td_find_symbol(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "search_memory") == 0)
      result = td_search_memory(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "web_search") == 0)
      result = td_web_search(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "create_note") == 0)
      result = td_create_note(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "list_notes") == 0)
      result = td_list_notes(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "search_notes") == 0)
      result = td_search_notes(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "run_background_process") == 0)
      result = td_run_background_process(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "get_background_output") == 0)
      result = td_get_background_output(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "kill_background_process") == 0)
      result = td_kill_background_process(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "list_background_processes") == 0)
      result = td_list_background_processes(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "rules_propose") == 0)
      result = td_rules_propose(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "rules_list") == 0)
      result = td_rules_list(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "learning_propose") == 0)
      result = td_learning_propose(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "learning_review") == 0)
      result = td_learning_review(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "clarify_start") == 0)
      result = td_clarify_start(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "clarify_answer") == 0)
      result = td_clarify_answer(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "diagnose_start") == 0)
      result = td_diagnose_start(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "diagnose_observe") == 0 || strcmp(name, "diagnose_hypothesize") == 0)
      result = td_diagnose_observe(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "diagnose_evidence") == 0)
      result = td_diagnose_evidence(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "diagnose_status") == 0)
      result = td_diagnose_status(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strcmp(name, "search_docs") == 0)
      result = td_search_docs(args, name, dispatch_cwd, dispatch_sid, timeout_ms);
   else if (strchr(name, ':') != NULL)
   {
      cJSON *remote_result = NULL;
      char err_buf[256] = "";
      if (mcp_client_registry_call_tool(name, args, timeout_ms, &remote_result, err_buf,
                                        sizeof(err_buf)) != 0)
      {
         char err[384];
         snprintf(err, sizeof(err), "error: remote mcp tool failed: %s",
                  err_buf[0] ? err_buf : "unknown error");
         result = safe_strdup(err);
      }
      else
      {
         result = cJSON_PrintUnformatted(remote_result);
         if (!result)
            result = safe_strdup("{\"error\":\"failed to serialize remote tool result\"}");
         cJSON_Delete(remote_result);
      }
   }
   else
   {
      char err[256];
      const char *suggestion = tool_suggest(name);
      if (suggestion)
         snprintf(err, sizeof(err), "error: unknown tool '%s'. Did you mean '%s'?", name,
                  suggestion);
      else
         snprintf(err, sizeof(err), "error: unknown tool '%s'", name);
      result = safe_strdup(err);
   }

   cJSON_Delete(args);

   /* Apply compaction to all non-bash tool results. The bash tool already
    * compacts stdout and stderr individually before building its JSON result,
    * so applying compaction again here would double-compact it. */
   if (result && strcmp(name, "bash") != 0)
   {
      char *compacted = agent_compress_tool_result(result, strlen(result), name);
      free(result);
      result = compacted;
   }

   return result;
}

char *dispatch_tool_call(const char *name, const char *arguments_json, int timeout_ms)
{
   return dispatch_tool_call_ctx(name, arguments_json, timeout_ms);
}
