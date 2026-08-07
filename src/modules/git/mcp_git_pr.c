/* mcp_git_pr.c: MCP git PR handlers */
#include "aimee.h"
#include "cJSON.h"
#include "config.h"
#include "guardrails.h"
#include "git_verify.h"
#include "git_pr_api.h"   /* git_pr_create_via_api_slug */
#include "agent_config.h" /* agent_get_request_vault_principal */
#include "mcp_git.h"
#include "util.h"
#include "branch_ownership.h"
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
                      "(create/view/list/edit/checks/merge_status/merge)");

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

      /* MCP stdio dispatch is synchronous. A watcher or sleep/poll loop blocks every
       * unrelated tool on that session, and cancelling the caller does not cancel the
       * server-side operation. Keep this action snapshot-only; callers poll it with
       * their own bounded scheduling instead of occupying the MCP request lane. */
      if ((jwait && cJSON_IsTrue(jwait)) || watch_checks)
         return mcp_text("error: blocking PR check waits are disabled; call action=checks with "
                         "wait=false and poll with a bounded client-side interval");

      char slug[264];
      if (get_origin_repo_slug(slug, sizeof(slug)) != 0)
         return mcp_text("error: cannot resolve a github.com origin for this checkout");

      git_pr_check_t rows[100];
      int n = 0;
      char err[512];
      err[0] = '\0';
      if (git_pr_checks_via_api_slug(agent_get_request_vault_principal(), slug, jnum->valueint,
                                     (int)(sizeof(rows) / sizeof(rows[0])), rows, &n, err,
                                     sizeof(err)) != 0)
         return mcp_error("error: pr checks failed: %s", err[0] ? err : "unknown");
      if (n == 0)
         return mcp_text("(no checks output)");

      /* gh printed one TAB-separated row per check and left a trailing empty
       * column, so each line ends with a tab before the newline. Reproduced
       * exactly: callers parse this by field. */
      size_t cap = (size_t)n * (sizeof(rows[0].name) + sizeof(rows[0].status) +
                                sizeof(rows[0].elapsed) + sizeof(rows[0].url) + 8) +
                   1;
      char *text = malloc(cap);
      if (!text)
         return mcp_text("error: out of memory rendering checks");
      size_t pos = 0;
      for (int i = 0; i < n && pos < cap; i++)
         pos += (size_t)snprintf(text + pos, cap - pos, "%s\t%s\t%s\t%s\t\n", rows[i].name,
                                 rows[i].status, rows[i].elapsed, rows[i].url);
      cJSON *r = mcp_text(text);
      free(text);
      return r;
   }

   if (strcmp(action, "merge_status") == 0)
   {
      cJSON *jnum = cJSON_GetObjectItemCaseSensitive(args, "number");
      if (!cJSON_IsNumber(jnum))
         return mcp_text("error: 'number' parameter is required for merge_status");

      char slug[264];
      if (get_origin_repo_slug(slug, sizeof(slug)) != 0)
         return mcp_text("error: cannot resolve a github.com origin for this checkout");

      git_pr_info_t info;
      char err[512];
      err[0] = '\0';
      if (git_pr_info_via_api_slug(agent_get_request_vault_principal(), slug, jnum->valueint, &info,
                                   err, sizeof(err)) != 0)
         return mcp_error("error: pr merge_status failed: %s", err[0] ? err : "unknown");

      /* gh's mergeable was a GraphQL enum; REST gives a tri-state bool, which
       * git_pr_info_t already carries as 1/0/-1. Same three words out. */
      const char *state = info.merged ? "MERGED" : (info.open ? "OPEN" : "CLOSED");
      const char *mergeable =
          info.mergeable == 1 ? "MERGEABLE" : (info.mergeable == 0 ? "CONFLICTING" : "UNKNOWN");

      char result[1600];
      int pos = snprintf(result, sizeof(result), "PR #%d: %s", jnum->valueint, state);
      if (info.merged_at[0] && pos > 0 && (size_t)pos < sizeof(result))
         pos +=
             snprintf(result + pos, sizeof(result) - (size_t)pos, " (merged %s)", info.merged_at);
      if (pos > 0 && (size_t)pos < sizeof(result))
         snprintf(result + pos, sizeof(result) - (size_t)pos,
                  " - %s\nmergeable: %s\nmerge_state: %s\nurl: %s", info.title, mergeable,
                  info.merge_state[0] ? info.merge_state : "UNKNOWN", info.html_url);
      return mcp_text(result);
   }

   if (strcmp(action, "merge") == 0)
   {
      /* Policy-aware merge executor (authoring-pipeline #50). The caller passes
       * the PR number, optional merge_method (merge|squash|rebase, default
       * merge), and optional expected_head_sha for drift safety (gh refuses the
       * merge if the head moved). Captures executor/command/exit/output and the
       * resulting merge SHA so the ledger has full evidence.
       *
       * There is deliberately NO admin/bypass option: a merge that requires an
       * admin override of branch protection is HUMAN-ONLY (operator ruling
       * 2026-07-15). A protection-blocked merge fails here and parks for a
       * human; it is never forced through. */
      cJSON *jnum = cJSON_GetObjectItemCaseSensitive(args, "number");
      if (!cJSON_IsNumber(jnum))
         return mcp_text("error: 'number' parameter is required for merge");
      int pr_num = jnum->valueint;

      cJSON *jmethod = cJSON_GetObjectItemCaseSensitive(args, "merge_method");
      cJSON *jauto = cJSON_GetObjectItemCaseSensitive(args, "auto");
      int auto_merge = jauto && cJSON_IsTrue(jauto);
      const char *mflag = "--merge";
      if (cJSON_IsString(jmethod))
      {
         if (strcmp(jmethod->valuestring, "squash") == 0)
            mflag = "--squash";
         else if (strcmp(jmethod->valuestring, "rebase") == 0)
            mflag = "--rebase";
      }
      char match[160] = {0};
      cJSON *jhead = cJSON_GetObjectItemCaseSensitive(args, "expected_head_sha");
      if (cJSON_IsString(jhead) && jhead->valuestring[0])
      {
         /* only allow a hex SHA to flow into the shell command. */
         const char *h = jhead->valuestring;
         int ok = 1;
         for (const char *p = h; *p; p++)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
            {
               ok = 0;
               break;
            }
         if (ok && h[0])
            snprintf(match, sizeof(match), " --match-head-commit %s", h);
      }

      /* CI must be fully green before the merge (operator ruling 2026-07-15). Still
       * its own `gh pr checks` read, relying on that command's 0/1/8 tri-state exit
       * code. action=checks NO LONGER shares this path -- it reads the Checks API
       * in-process -- so the two are now independent and this one inherits the `gh`
       * problem: `gh` in the aimee-server image has no credential, and mcp_git_run
       * hands children the token on a memfd rather than as GH_TOKEN, so this gate
       * only works from a DETACHED workspace where the client holds its own creds.
       * Migrating it means replacing the exit-code verdict with git_pr_ci_permits_merge
       * over git_pr_ci_via_api_slug, which is a change to a MERGE gate and wants its
       * own review rather than riding along with the read migrations.
       *
       * Fails CLOSED on anything it cannot positively classify. In particular the
       * verdict is taken from gh's EXIT CODE, never inferred from parsed counters:
       * a zero count means "no rows parsed", which is equally true of genuinely
       * zero checks and of output we failed to understand (or a NULL from an alloc
       * failure) — merging on that would be a fail-open. Only gh's own explicit
       * "no checks reported" is accepted as genuinely zero, which the operator
       * ruling says may merge (nothing to fail). */
      {
         char ccmd[128];
         snprintf(ccmd, sizeof(ccmd), "gh pr checks %d 2>&1", pr_num);
         int crc = 0;
         char *cout = mcp_git_run(ccmd, &crc);
         const char *why = NULL;
         if (!cout)
            why = "could not read CI status (no output)";
         else if (strstr(cout, "no checks reported"))
            why = NULL; /* genuinely zero checks -> nothing to fail -> merge */
         else if (crc == 0)
            why = NULL; /* gh: every check passed */
         else if (crc == 8 && auto_merge)
            why = NULL; /* branch protection keeps an auto-merge pending until green */
         else if (crc == 8)
            why = "CI has not finished; re-try once checks settle";
         else if (crc == 1)
            why = "CI is not green (at least one check failed)";
         else
            why = "could not read CI status";
         if (why)
         {
            char msg[320];
            snprintf(msg, sizeof(msg),
                     "error: merge blocked — %s. A merge requires fully green CI.", why);
            free(cout);
            return mcp_text(msg);
         }
         free(cout);
      }

      char cmd[512];
      snprintf(cmd, sizeof(cmd), "gh pr merge %d %s%s%s 2>&1", pr_num, mflag, match,
               auto_merge ? " --auto" : "");
      int rc = 0;
      char *out = mcp_git_run(cmd, &rc);
      cJSON *res = cJSON_CreateObject();
      cJSON_AddStringToObject(res, "executor", "git_pr");
      cJSON_AddStringToObject(res, "command", cmd);
      cJSON_AddNumberToObject(res, "exit_code", rc);
      cJSON_AddStringToObject(res, "output", out ? out : "");
      free(out);
      if (rc == 0 && auto_merge)
      {
         /* Success means the request was accepted, not necessarily that GitHub merged
          * it synchronously. Do not manufacture merge evidence for a queued PR. */
         cJSON_AddBoolToObject(res, "auto_merge_enabled", 1);
         cJSON_AddBoolToObject(res, "merged", 0);
      }
      else if (rc == 0)
      {
         /* recover the merge commit SHA for the ledger. */
         char vcmd[128];
         snprintf(vcmd, sizeof(vcmd), "gh pr view %d --json mergeCommit -q .mergeCommit.oid 2>&1",
                  pr_num);
         int vrc = 0;
         char *vout = mcp_git_run(vcmd, &vrc);
         if (vout)
         {
            char *nl = strchr(vout, '\n');
            if (nl)
               *nl = '\0';
            cJSON_AddStringToObject(res, "merge_sha", vrc == 0 ? vout : "");
            free(vout);
         }
         cJSON_AddBoolToObject(res, "merged", 1);
      }
      else
      {
         cJSON_AddBoolToObject(res, "merged", 0);
      }
      char *s = cJSON_PrintUnformatted(res);
      cJSON_Delete(res);
      cJSON *r = mcp_text(s ? s : "{\"merged\":false}");
      free(s);
      return r;
   }

   if (strcmp(action, "view") == 0)
   {
      cJSON *jnum = cJSON_GetObjectItemCaseSensitive(args, "number");
      if (!cJSON_IsNumber(jnum))
         return mcp_text("error: 'number' parameter is required for view");

      char slug[264];
      if (get_origin_repo_slug(slug, sizeof(slug)) != 0)
         return mcp_text("error: cannot resolve a github.com origin for this checkout");

      git_pr_info_t info;
      char err[512];
      err[0] = '\0';
      if (git_pr_info_via_api_slug(agent_get_request_vault_principal(), slug, jnum->valueint, &info,
                                   err, sizeof(err)) != 0)
         return mcp_error("error: pr view failed: %s", err[0] ? err : "unknown");

      /* gh reported OPEN/CLOSED/MERGED; the REST API splits that into state plus a
       * merged flag. Reassemble it so the rendered output is unchanged. */
      const char *state = info.merged ? "MERGED" : (info.open ? "OPEN" : "CLOSED");

      char result[1600];
      int pos = snprintf(result, sizeof(result), "PR #%d: %s\ntitle: %s\nbase: %s <- %s\nurl: %s",
                         jnum->valueint, state, info.title, info.base, info.head, info.html_url);
      if (info.merged_at[0] && pos > 0 && (size_t)pos < sizeof(result))
         snprintf(result + pos, sizeof(result) - (size_t)pos, "\nmerged: %s", info.merged_at);
      return mcp_text(result);
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

      /* Standing directive: no AI attribution in PR bodies (in-place strip is
       * shrink-only, so the cJSON-owned buffer is safe). */
      if (has_body)
         strip_ai_attribution(jbody->valuestring);

      char repo_slug[256];
      if (get_origin_repo_slug(repo_slug, sizeof(repo_slug)) != 0)
         return mcp_text("error: could not determine GitHub repository from origin remote");

      /* Build the PATCH command in a heap buffer sized to the escaped fields.
       * A PR body is user-controlled and shell_escape can expand it up to ~4x,
       * so it easily exceeds any fixed buffer; the `pos += snprintf` accumulation
       * would then run pos past the end and wrap (cap - pos) to a huge size_t on
       * the next write — an out-of-bounds (stack) write. Sizing the buffer to
       * fit avoids both the overflow and silently truncating a long body. */
      const char *principal = agent_get_request_vault_principal();
      char err[512];
      err[0] = '\0';
      if (git_pr_edit_via_api_slug(principal, repo_slug, jnum->valueint,
                                   has_title ? jtitle->valuestring : NULL,
                                   has_body ? jbody->valuestring : NULL,
                                   has_base ? jbase->valuestring : NULL, err, sizeof(err)) != 0)
         return mcp_error("error: pr edit failed: %s", err[0] ? err : "unknown");

      /* Read back what the edit produced, as the gh --template did. A failure here
       * means the PATCH landed but the confirmation did not: say so rather than
       * reporting the edit itself as failed. */
      git_pr_info_t info;
      err[0] = '\0';
      if (git_pr_info_via_api_slug(principal, repo_slug, jnum->valueint, &info, err, sizeof(err)) !=
          0)
         return mcp_error("updated PR, but reading it back failed: %s", err[0] ? err : "unknown");

      char result[1600];
      int pos =
          snprintf(result, sizeof(result), "updated PR #%d\ntitle: %s\nbase: %s <- %s\nurl: %s",
                   jnum->valueint, info.title, info.base, info.head, info.html_url);
      if (info.merged_at[0] && pos > 0 && (size_t)pos < sizeof(result))
         snprintf(result + pos, sizeof(result) - (size_t)pos, "\nmerged: %s", info.merged_at);
      return mcp_text(result);
   }

   if (strcmp(action, "list") == 0)
   {
      char slug[264];
      if (get_origin_repo_slug(slug, sizeof(slug)) != 0)
         return mcp_text("error: cannot resolve a github.com origin for this checkout");

      git_pr_list_item_t rows[20];
      int n = 0;
      char err[512];
      err[0] = '\0';
      if (git_pr_list_open_via_api_slug(agent_get_request_vault_principal(), slug,
                                        (int)(sizeof(rows) / sizeof(rows[0])), rows, &n, err,
                                        sizeof(err)) != 0)
         return mcp_error("error: pr list failed: %s", err[0] ? err : "unknown");
      if (n == 0)
         return mcp_text("(no open PRs)");

      /* One line per PR, as the gh template rendered them. Sized for 20 rows of
       * the struct's own maxima plus the fixed decoration. */
      size_t cap =
          (size_t)n * (sizeof(rows[0].title) + sizeof(rows[0].head) + sizeof(rows[0].state) + 32) +
          1;
      char *text = malloc(cap);
      if (!text)
         return mcp_text("error: out of memory rendering PR list");
      size_t pos = 0;
      for (int i = 0; i < n && pos < cap; i++)
         pos += (size_t)snprintf(text + pos, cap - pos, "#%d [%s] %s: %s\n", rows[i].number,
                                 rows[i].state, rows[i].head, rows[i].title);
      cJSON *r = mcp_text(text);
      free(text);
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

      /* Standing directive: no AI attribution in PR bodies (in-place strip is
       * shrink-only, so the cJSON-owned buffer is safe). */
      if (cJSON_IsString(jbody))
         strip_ai_attribution(jbody->valuestring);

      const char *base = cJSON_IsString(jbase) ? jbase->valuestring : "main";

      /* Resolve the repository through mcp_git_run, the SAME runner every other git
       * command here goes through, and hand the slug to the API. The API's
       * repo_dir-based entry points run git in aimee-server's own process, which is
       * wrong for this tool: a DETACHED workspace keeps the checkout on the client,
       * so the server cannot see that path and every create failed with "no origin
       * remote" (#2386, reverted in #2391). */
      char slug[264];
      if (get_origin_repo_slug(slug, sizeof(slug)) != 0)
         return mcp_text("error: cannot resolve a github.com origin for this checkout");

      /* HEAD is the session branch (aimee/session/<id>) in worktree mode, not the
       * branch the work belongs to, so name the owned branch explicitly. Otherwise
       * ask the runner which branch is checked out -- again not this process. */
      char head[256];
      if (mcp_git_get_worktree())
      {
         if (branch_own_get_session_branch(head, sizeof(head)) != 0)
            return mcp_text("error: in worktree mode but no owned branch found. "
                            "Use git_branch action=create to create and register a branch first.");
      }
      else
      {
         int hrc;
         char *hout = mcp_git_run("git rev-parse --abbrev-ref HEAD 2>/dev/null", &hrc);
         if (hrc != 0 || !hout || !hout[0])
         {
            free(hout);
            return mcp_text("error: cannot determine the current branch");
         }
         size_t hl = strlen(hout);
         while (hl > 0 && isspace((unsigned char)hout[hl - 1]))
            hout[--hl] = '\0';
         if (!hout[0] || strcmp(hout, "HEAD") == 0)
         {
            free(hout);
            return mcp_text("error: not on a branch (detached HEAD)");
         }
         snprintf(head, sizeof(head), "%s", hout);
         free(hout);
      }

      char url[1024];
      char err[512];
      url[0] = '\0';
      err[0] = '\0';
      if (git_pr_create_via_api_slug(agent_get_request_vault_principal(), slug, head, base,
                                     jtitle->valuestring,
                                     cJSON_IsString(jbody) ? jbody->valuestring : "", 0, url,
                                     sizeof(url), err, sizeof(err)) != 0)
         return mcp_error("error: pr create failed: %s", err[0] ? err : "unknown");

      char result[1280];
      snprintf(result, sizeof(result), "created: \"%s\"\nurl: %s\nbase: %s", jtitle->valuestring,
               url, base);
      return mcp_text(result);
   }

   return mcp_text(
       "error: unknown action. Use create/view/list/edit/checks/watch/merge_status/merge/wait");
}
