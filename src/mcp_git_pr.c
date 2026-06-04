/* mcp_git_pr.c: MCP git PR handlers */
#include "aimee.h"
#include "cJSON.h"
#include "headers/config.h"
#include "headers/guardrails.h"
#include "headers/git_verify.h"
#include "headers/mcp_git.h"
#include "headers/util.h"
#include "headers/branch_ownership.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

/* --- Helpers --- */

static cJSON *mcp_text(const char *text)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *item = cJSON_CreateObject();
   cJSON_AddStringToObject(item, "type", "text");
   cJSON_AddStringToObject(item, "text", text);
   cJSON_AddItemToArray(arr, item);
   return arr;
}

static cJSON *mcp_error(const char *fmt, const char *detail)
{
   char buf[1024];
   snprintf(buf, sizeof(buf), fmt, detail);
   return mcp_text(buf);
}

static int checks_state_is_pending(const char *state)
{
   return state && (strcmp(state, "pending") == 0 || strcmp(state, "queued") == 0 ||
                    strcmp(state, "in_progress") == 0 || strcmp(state, "waiting") == 0 ||
                    strcmp(state, "requested") == 0);
}

static int checks_state_is_failed(const char *state)
{
   return state && (strcmp(state, "fail") == 0 || strcmp(state, "failure") == 0 ||
                    strcmp(state, "error") == 0 || strcmp(state, "cancel") == 0 ||
                    strcmp(state, "cancelled") == 0 || strcmp(state, "timed_out") == 0 ||
                    strcmp(state, "action_required") == 0 || strcmp(state, "startup_failure") == 0);
}

static void checks_parse_plain_output(char *out, int *total, int *pending, int *failed, int *passed)
{
   if (total)
      *total = 0;
   if (pending)
      *pending = 0;
   if (failed)
      *failed = 0;
   if (passed)
      *passed = 0;
   if (!out)
      return;

   char *line = out;
   while (line && *line)
   {
      char *nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';

      char state[64] = "";
      char *tab = strchr(line, '\t');
      if (tab)
      {
         char *start = tab + 1;
         char *end = strchr(start, '\t');
         if (!end)
            end = start + strlen(start);
         snprintf(state, sizeof(state), "%.*s", (int)(end - start), start);
      }
      else
      {
         char name[128];
         if (sscanf(line, "%127s %63s", name, state) != 2)
            state[0] = '\0';
      }

      for (char *p = state; *p; p++)
         *p = (char)tolower((unsigned char)*p);

      if (state[0])
      {
         if (total)
            (*total)++;
         if (checks_state_is_pending(state))
         {
            if (pending)
               (*pending)++;
         }
         else if (checks_state_is_failed(state))
         {
            if (failed)
               (*failed)++;
         }
         else if (passed)
         {
            (*passed)++;
         }
      }

      line = nl ? nl + 1 : NULL;
   }
}

static int get_origin_repo_slug(char *buf, size_t len)
{
   if (!buf || len == 0)
      return -1;

   int rc;
   char *out = mcp_git_run("git config --get remote.origin.url 2>/dev/null", &rc);
   if (rc != 0 || !out || !out[0])
   {
      free(out);
      return -1;
   }

   char *url = out;
   while (*url && isspace((unsigned char)*url))
      url++;

   char *end = url + strlen(url);
   while (end > url && isspace((unsigned char)end[-1]))
      *--end = '\0';

   const char *slug = NULL;
   if (strncmp(url, "git@github.com:", 15) == 0)
      slug = url + 15;
   else if (strncmp(url, "https://github.com/", 19) == 0)
      slug = url + 19;
   else if (strncmp(url, "ssh://git@github.com/", 21) == 0)
      slug = url + 21;

   if (!slug || !slug[0])
   {
      free(out);
      return -1;
   }

   snprintf(buf, len, "%s", slug);
   size_t used = strlen(buf);
   if (used >= 4 && strcmp(buf + used - 4, ".git") == 0)
      buf[used - 4] = '\0';

   free(out);
   return 0;
}

/* --- git_pr --- */

cJSON *handle_git_pr(cJSON *args)
{
   cJSON *jaction = cJSON_GetObjectItemCaseSensitive(args, "action");
   if (!cJSON_IsString(jaction))
      return mcp_text("error: 'action' parameter is required "
                      "(create/view/list/edit/checks/watch/merge_status)");

   const char *action = jaction->valuestring;
   int watch_checks = 0;

   if (strcmp(action, "watch") == 0)
      watch_checks = 1;

   if (strcmp(action, "checks") == 0 || strcmp(action, "watch") == 0)
   {
      cJSON *jnum = cJSON_GetObjectItemCaseSensitive(args, "number");
      cJSON *jwatch = cJSON_GetObjectItemCaseSensitive(args, "watch");
      cJSON *jwait = cJSON_GetObjectItemCaseSensitive(args, "wait");
      if (!cJSON_IsNumber(jnum))
         return mcp_text("error: 'number' parameter is required for checks/watch");
      if (jwatch && cJSON_IsTrue(jwatch))
         watch_checks = 1;

      /* --wait: poll until all checks settle, return clean pass/fail summary */
      if (jwait && cJSON_IsTrue(jwait))
      {
         int pr_num = jnum->valueint;
         for (int i = 0; i < 40; i++) /* up to 10 min at 15 s intervals */
         {
            if (i > 0)
               sleep(15);
            char wcmd[256];
            snprintf(wcmd, sizeof(wcmd), "gh pr checks %d 2>&1", pr_num);
            int wrc;
            char *wout = mcp_git_run(wcmd, &wrc);
            if (wrc != 0 && wrc != 1 && wrc != 8)
            {
               cJSON *r = mcp_error("error: gh pr checks failed: %s", wout ? wout : "unknown");
               free(wout);
               return r;
            }

            int total = 0, pending = 0, failed = 0, passed = 0;
            checks_parse_plain_output(wout, &total, &pending, &failed, &passed);
            free(wout);
            if (pending == 0)
            {
               char res[256];
               if (failed > 0)
                  snprintf(res, sizeof(res), "checks complete: %d/%d failed, %d passed", failed,
                           total, passed);
               else
                  snprintf(res, sizeof(res), "checks complete: all %d passed", total);
               return mcp_text(res);
            }
         }
         return mcp_text("error: timed out waiting for checks (10 minutes)");
      }

      char cmd[256];
      snprintf(cmd, sizeof(cmd), "gh pr checks %d%s 2>&1", jnum->valueint,
               watch_checks ? " --watch" : "");

      int rc;
      char *out = mcp_git_run(cmd, &rc);
      if (rc != 0 && rc != 1 && rc != 8)
      {
         cJSON *r = mcp_error("error: gh pr checks failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }

      cJSON *r = mcp_text(out && out[0] ? out : "(no checks output)");
      free(out);
      return r;
   }

   if (strcmp(action, "merge_status") == 0)
   {
      cJSON *jnum = cJSON_GetObjectItemCaseSensitive(args, "number");
      if (!cJSON_IsNumber(jnum))
         return mcp_text("error: 'number' parameter is required for merge_status");

      char cmd[256];
      snprintf(cmd, sizeof(cmd),
               "gh pr view %d --json state,mergedAt,title,mergeable,mergeStateStatus,url "
               "--template 'PR #%d: {{.state}}{{if .mergedAt}} (merged {{.mergedAt}})"
               "{{end}} - {{.title}}\\nmergeable: {{.mergeable}}\\n"
               "merge_state: {{.mergeStateStatus}}\\nurl: {{.url}}' 2>&1",
               jnum->valueint, jnum->valueint);

      int rc;
      char *out = mcp_git_run(cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: gh pr view failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }
      cJSON *r = mcp_text(out ? out : "unknown");
      free(out);
      return r;
   }

   if (strcmp(action, "view") == 0)
   {
      cJSON *jnum = cJSON_GetObjectItemCaseSensitive(args, "number");
      if (!cJSON_IsNumber(jnum))
         return mcp_text("error: 'number' parameter is required for view");

      char cmd[256];
      snprintf(cmd, sizeof(cmd),
               "gh pr view %d --json title,state,url,baseRefName,headRefName,mergedAt "
               "--template 'PR #%d: {{.state}}\\ntitle: {{.title}}\\n"
               "base: {{.baseRefName}} <- {{.headRefName}}\\nurl: {{.url}}"
               "{{if .mergedAt}}\\nmerged: {{.mergedAt}}{{end}}' 2>&1",
               jnum->valueint, jnum->valueint);

      int rc;
      char *out = mcp_git_run(cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: gh pr view failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }
      cJSON *r = mcp_text(out ? out : "unknown");
      free(out);
      return r;
   }

   if (strcmp(action, "edit") == 0)
   {
      cJSON *jnum = cJSON_GetObjectItemCaseSensitive(args, "number");
      cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(args, "title");
      cJSON *jbody = cJSON_GetObjectItemCaseSensitive(args, "body");
      cJSON *jbase = cJSON_GetObjectItemCaseSensitive(args, "base");
      int has_title = cJSON_IsString(jtitle);
      int has_body = cJSON_IsString(jbody);
      int has_base = cJSON_IsString(jbase) && jbase->valuestring[0];

      if (!cJSON_IsNumber(jnum))
         return mcp_text("error: 'number' parameter is required for edit");
      if (!has_title && !has_body && !has_base)
         return mcp_text("error: edit requires at least one of title/body/base");

      char repo_slug[256];
      if (get_origin_repo_slug(repo_slug, sizeof(repo_slug)) != 0)
         return mcp_text("error: could not determine GitHub repository from origin remote");

      /* Build the PATCH command in a heap buffer sized to the escaped fields.
       * A PR body is user-controlled and shell_escape can expand it up to ~4x,
       * so it easily exceeds any fixed buffer; the `pos += snprintf` accumulation
       * would then run pos past the end and wrap (cap - pos) to a huge size_t on
       * the next write — an out-of-bounds (stack) write. Sizing the buffer to
       * fit avoids both the overflow and silently truncating a long body. */
      char *esc_title = has_title ? shell_escape(jtitle->valuestring) : NULL;
      char *esc_body = has_body ? shell_escape(jbody->valuestring) : NULL;
      char *esc_base = has_base ? shell_escape(jbase->valuestring) : NULL;
      size_t cmdcap = strlen(repo_slug) + 160 + (esc_title ? strlen(esc_title) : 0) +
                      (esc_body ? strlen(esc_body) : 0) + (esc_base ? strlen(esc_base) : 0);
      char *cmd = malloc(cmdcap);
      if (!cmd)
      {
         free(esc_title);
         free(esc_body);
         free(esc_base);
         return mcp_text("error: out of memory building gh command");
      }
      int pos =
          snprintf(cmd, cmdcap, "gh api -X PATCH repos/%s/pulls/%d", repo_slug, jnum->valueint);
      if (esc_title)
         pos += snprintf(cmd + pos, cmdcap - (size_t)pos, " -f title='%s'", esc_title);
      if (esc_body)
         pos += snprintf(cmd + pos, cmdcap - (size_t)pos, " -f body='%s'", esc_body);
      if (esc_base)
         pos += snprintf(cmd + pos, cmdcap - (size_t)pos, " -f base='%s'", esc_base);
      snprintf(cmd + pos, cmdcap - (size_t)pos, " 2>&1");
      free(esc_title);
      free(esc_body);
      free(esc_base);

      int rc;
      char *out = mcp_git_run(cmd, &rc);
      free(cmd);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: gh api pull update failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }
      free(out);

      char view_cmd[512];
      snprintf(view_cmd, sizeof(view_cmd),
               "gh pr view %d --json title,state,url,baseRefName,headRefName,mergedAt "
               "--template 'updated PR #%d\\ntitle: {{.title}}\\n"
               "base: {{.baseRefName}} <- {{.headRefName}}\\nurl: {{.url}}"
               "{{if .mergedAt}}\\nmerged: {{.mergedAt}}{{end}}' 2>&1",
               jnum->valueint, jnum->valueint);

      out = mcp_git_run(view_cmd, &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: gh pr view failed after edit: %s", out ? out : "unknown");
         free(out);
         return r;
      }

      cJSON *r = mcp_text(out ? out : "updated");
      free(out);
      return r;
   }

   if (strcmp(action, "list") == 0)
   {
      int rc;
      char *out = mcp_git_run(
          "gh pr list --limit 20 --json number,title,state,headRefName "
          "--template '{{range .}}#{{.number}} [{{.state}}] {{.headRefName}}: {{.title}}\n{{end}}' "
          "2>&1",
          &rc);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: gh pr list failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }
      cJSON *r = mcp_text(out && out[0] ? out : "(no open PRs)");
      free(out);
      return r;
   }

   if (strcmp(action, "create") == 0)
   {
      /* Fetch branch once — used for ownership and merged-PR checks */
      char branch[256] = "";
      get_current_branch(branch, sizeof(branch));
      {
         cJSON *blocked = branch_own_guard_for(branch, "pr create");
         if (blocked)
            return blocked;
      }

      /* Merged-PR enforcement: block creating PRs from branches with merged PRs */
      if (check_branch_has_merged_pr_for(branch))
         return mcp_text("error: branch already has a merged PR. "
                         "Create a new branch for new work.");

      /* Verify gate. verify_gate_blocks honors scope (current project only
       * unless cross-project verify is enabled) and the global verify master
       * switch, and never auto-generates config for an out-of-scope/unconfigured
       * repo. */
      {
         char verify_msg[256];
         if (verify_gate_blocks(run_cmd_get_cwd(), NULL, verify_msg, sizeof(verify_msg)))
         {
            char buf[512];
            snprintf(buf, sizeof(buf), "error: PR creation blocked: %s", verify_msg);
            return mcp_text(buf);
         }
      }

      cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(args, "title");
      cJSON *jbody = cJSON_GetObjectItemCaseSensitive(args, "body");
      cJSON *jbase = cJSON_GetObjectItemCaseSensitive(args, "base");

      if (!cJSON_IsString(jtitle) || !jtitle->valuestring[0])
         return mcp_text("error: 'title' parameter is required for create");

      char *esc_title = shell_escape(jtitle->valuestring);
      char *esc_body = shell_escape(cJSON_IsString(jbody) ? jbody->valuestring : "");
      const char *base = cJSON_IsString(jbase) ? jbase->valuestring : "main";
      char *esc_base = shell_escape(base);

      /* In worktree mode, gh pr create infers the branch from HEAD, which is the
       * session branch (aimee/session/<id>). Pass --head explicitly with the owned branch. */
      /* Size the command buffer to the escaped fields (see the edit path above):
       * a fixed buffer silently truncates a long PR body, corrupting the created
       * PR's description (and risks the same accumulation overflow). */
      char *esc_head = NULL;
      if (mcp_git_get_worktree())
      {
         char owned_branch[256];
         if (branch_own_get_session_branch(owned_branch, sizeof(owned_branch)) != 0)
         {
            free(esc_title);
            free(esc_body);
            free(esc_base);
            return mcp_text("error: in worktree mode but no owned branch found. "
                            "Use git_branch action=create to create and register a branch first.");
         }
         esc_head = shell_escape(owned_branch);
      }
      size_t cmdcap = 160 + strlen(esc_title) + strlen(esc_body) + strlen(esc_base) +
                      (esc_head ? strlen(esc_head) : 0);
      char *cmd = malloc(cmdcap);
      if (!cmd)
      {
         free(esc_title);
         free(esc_body);
         free(esc_base);
         free(esc_head);
         return mcp_text("error: out of memory building gh command");
      }
      if (esc_head)
         snprintf(cmd, cmdcap, "gh pr create --title '%s' --body '%s' --base '%s' --head '%s' 2>&1",
                  esc_title, esc_body, esc_base, esc_head);
      else
         snprintf(cmd, cmdcap, "gh pr create --title '%s' --body '%s' --base '%s' 2>&1", esc_title,
                  esc_body, esc_base);
      free(esc_head);
      free(esc_title);
      free(esc_body);
      free(esc_base);

      int rc;
      char *out = mcp_git_run(cmd, &rc);
      free(cmd);
      if (rc != 0)
      {
         cJSON *r = mcp_error("error: gh pr create failed: %s", out ? out : "unknown");
         free(out);
         return r;
      }

      /* Output from gh pr create is typically just the URL */
      char result[1024];
      if (out)
      {
         /* Trim trailing newline */
         size_t len = strlen(out);
         while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
            out[--len] = '\0';
         snprintf(result, sizeof(result), "created: \"%s\"\nurl: %s\nbase: %s", jtitle->valuestring,
                  out, base);
      }
      else
      {
         snprintf(result, sizeof(result), "created: \"%s\" (base: %s)", jtitle->valuestring, base);
      }
      free(out);
      return mcp_text(result);
   }

   return mcp_text(
       "error: unknown action. Use create/view/list/edit/checks/watch/merge_status/wait");
}
