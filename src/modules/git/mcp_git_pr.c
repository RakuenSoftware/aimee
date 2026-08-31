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
#include "dstr.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

/* How much of a failed job's log to bring back. Enough for a compiler error list
 * or a failed assertion with context; small enough that several failures in one
 * response stay readable. */
#define PR_LOG_TAIL_BYTES 6000

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

static cJSON *mcp_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static cJSON *mcp_error(const char *fmt, ...)
{
   char buf[1024];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   return mcp_text(buf);
}

/* --- the base a PR targets when the caller named none ------------------------
 *
 * Default: the session's durable feature branch (aimee/feat/<slug>), NOT the
 * repository's default branch. A feature's slices then accumulate on one branch and
 * reach the default branch through a SINGLE reviewed PR, instead of every slice
 * opening its own PR against the trunk. pr_base_mode=default_branch restores the old
 * behaviour, and an explicit `base` always wins over both. */

static int get_origin_repo_slug(char *buf, size_t len); /* defined below, with the git helpers */

static void pr_capture_line(const char *cmd, char *out, size_t out_len)
{
   out[0] = '\0';
   int rc = 0;
   char *o = mcp_git_run(cmd, &rc);
   if (rc == 0 && o)
   {
      snprintf(out, out_len, "%s", o);
      out[strcspn(out, "\r\n")] = '\0';
   }
   free(o);
}

/* The repository's real default branch (origin/HEAD -> "main"). Returns 0 with `out`
 * filled, -1 when it cannot be resolved -- there is no "main" guess, because opening a
 * PR against the wrong trunk is worse than surfacing the failure. */
static int pr_repo_default_branch(char *out, size_t out_len)
{
   char head_ref[256];
   pr_capture_line("git symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null", head_ref,
                   sizeof(head_ref));
   if (!head_ref[0])
      return -1;
   const char *name = strncmp(head_ref, "origin/", 7) == 0 ? head_ref + 7 : head_ref;
   if (!name[0])
      return -1;
   snprintf(out, out_len, "%s", name);
   return 0;
}

/* Create the feature branch on the remote when it is not there yet, so a PR has
 * something to target. It is cut from the repo's default branch by pushing that ref
 * straight to the new name -- no checkout, no working-tree change, so this is safe to
 * run from a session sitting on its own branch. Already-exists is success. */
static int pr_ensure_feature_branch(const char *feat, char *err, size_t err_len)
{
   char *esc = shell_quote(feat);
   if (!esc)
      return -1;
   char cmd[768];
   int rc = 0;

   snprintf(cmd, sizeof(cmd), "git ls-remote --exit-code --heads origin %s 2>/dev/null", esc);
   char *out = mcp_git_run(cmd, &rc);
   free(out);
   if (rc == 0)
   {
      free(esc);
      return 0; /* already published */
   }

   char def[256];
   if (pr_repo_default_branch(def, sizeof(def)) != 0)
   {
      free(esc);
      snprintf(err, err_len,
               "cannot resolve the repository default branch to cut the feature branch from");
      return -1;
   }
   char *esc_def = shell_quote(def);
   if (!esc_def)
   {
      free(esc);
      return -1;
   }

   /* Fetch first: cutting the feature branch from a stale local copy of the trunk would
    * silently start the feature behind. */
   snprintf(cmd, sizeof(cmd), "git fetch origin %s 2>&1", esc_def);
   out = mcp_git_run(cmd, &rc);
   free(out);
   if (rc != 0)
   {
      snprintf(err, err_len, "cannot fetch origin/%s to cut the feature branch from", def);
      free(esc);
      free(esc_def);
      return -1;
   }

   /* CREATE-ONLY. `--force-with-lease=<ref>:` with an empty expected value requires the
    * remote ref NOT to exist, which closes the window between the ls-remote above and
    * this push: a concurrent session that created the feature branch first keeps its
    * commits instead of having the branch yanked back to the trunk. Losing that race is
    * success, not failure -- the branch we needed exists either way -- so a rejection
    * here is re-checked rather than reported. */
   char lease_raw[512], spec_raw[768];
   snprintf(lease_raw, sizeof(lease_raw), "--force-with-lease=refs/heads/%s:", feat);
   snprintf(spec_raw, sizeof(spec_raw), "refs/remotes/origin/%s:refs/heads/%s", def, feat);
   char *lease = shell_quote(lease_raw);
   char *spec = shell_quote(spec_raw);
   snprintf(cmd, sizeof(cmd), "git push %s origin %s 2>&1", lease, spec);
   free(lease);
   free(spec);
   out = mcp_git_run(cmd, &rc);
   if (rc != 0)
   {
      free(out);
      snprintf(cmd, sizeof(cmd), "git ls-remote --exit-code --heads origin %s 2>/dev/null", esc);
      out = mcp_git_run(cmd, &rc);
      free(out);
      free(esc);
      free(esc_def);
      if (rc == 0)
         return 0; /* someone else created it first: the branch is there, which is all we need */
      snprintf(err, err_len, "cannot create feature branch %s", feat);
      return -1;
   }
   free(out);
   free(esc);
   free(esc_def);
   return rc == 0 ? 0 : -1;
}

/* --- promotion: the feature branch -> the repository's default branch -----------
 *
 * A feature branch is COMPLETE when nothing is still queued to land on it: every PR
 * that targeted it has merged (or closed) and none is open. At that point the feature
 * is what wants review as a whole, so it gets ONE PR into the default branch. Opened as
 * a DRAFT, matching the existing rule that a protected base is never targeted by a PR
 * that looks ready to merge itself.
 *
 * This runs off the merge that made the branch complete rather than off a poll: the
 * merge is the event, and polling would either lag or hammer the forge. */

/* 1 iff no OPEN PR still targets `feature`. Returns -1 when the listing failed -- the
 * caller must treat that as "cannot tell" and NOT promote, because promoting a feature
 * whose slices are still in flight publishes an incomplete change for review. */
static int pr_feature_is_complete(const char *principal, const char *slug, const char *feature)
{
   git_pr_list_item_t rows[100];
   int n = 0;
   char err[512];
   err[0] = '\0';
   if (git_pr_list_open_via_api_slug(principal, slug, (int)(sizeof(rows) / sizeof(rows[0])), rows,
                                     &n, err, sizeof(err)) != 0)
      return -1;
   for (int i = 0; i < n; i++)
      if (strcmp(rows[i].base, feature) == 0)
         return 0; /* something is still queued to land on the feature */
   return 1;
}

/* Open the feature -> default-branch PR when the feature has just become complete.
 * Best-effort by design: it reports what it did into `note` and never turns a
 * successful merge into a failure. */
static void pr_promote_feature(const char *principal, const char *slug, const char *feature,
                               char *note, size_t note_len)
{
   note[0] = '\0';
   if (!config_feature_auto_promote())
      return;
   if (!feature || strncmp(feature, "aimee/feat/", 11) != 0)
      return; /* only aimee-managed feature branches promote */

   int complete = pr_feature_is_complete(principal, slug, feature);
   if (complete != 1)
   {
      if (complete < 0)
         snprintf(note, note_len,
                  "\npromote: skipped — could not list open PRs, so whether %s is complete is "
                  "unknown",
                  feature);
      return;
   }

   char def[256];
   if (pr_repo_default_branch(def, sizeof(def)) != 0)
   {
      snprintf(note, note_len,
               "\npromote: skipped — %s is complete but the repository default branch could not "
               "be resolved",
               feature);
      return;
   }

   char url[1024], err[512];
   url[0] = '\0';
   err[0] = '\0';
   int existing = 0;
   if (git_pr_find_open_via_api_slug(principal, slug, feature, def, url, sizeof(url), &existing,
                                     err, sizeof(err)) == 1)
   {
      snprintf(note, note_len, "\npromote: %s -> %s already open: %s", feature, def, url);
      return;
   }

   char title[512];
   snprintf(title, sizeof(title), "%s", feature + 11); /* the slug reads as the feature name */
   char body[512];
   snprintf(body, sizeof(body),
            "Every PR targeting `%s` has landed, so the feature is up for review as a whole "
            "against `%s`.\n\nOpened as a draft: mark it ready when the feature is done.\n",
            feature, def);

   if (git_pr_create_via_api_slug(principal, slug, feature, def, title, body, 1, url, sizeof(url),
                                  err, sizeof(err)) != 0)
      snprintf(note, note_len,
               "\npromote: %s is complete but the PR into %s could not be "
               "opened: %.200s",
               feature, def, err[0] ? err : "unknown");
   else
      snprintf(note, note_len, "\npromote: %s is complete — opened draft PR into %s: %s", feature,
               def, url);
}

/* Resolve the base for this session's PR and make sure it exists on the remote.
 * Returns 0 with `out` filled, -1 with `err` filled. */
static int pr_resolve_base(char *out, size_t out_len, char *err, size_t err_len)
{
   err[0] = '\0';

   /* No github origin means no PR at all, and create's own refusal names that clearly.
    * Report it here rather than a feature-branch failure that would only be the
    * symptom -- a checkout with no forge has no base to resolve either. */
   char slug[264];
   if (get_origin_repo_slug(slug, sizeof(slug)) != 0)
   {
      snprintf(err, err_len, "cannot resolve a github.com origin for this checkout");
      return -1;
   }

   /* The feature branch is read from db1 (keyed repo+session), NOT from a file under
    * the checkout: this runs on aimee-server, which for a detached or mirrored
    * workspace cannot see the checkout's directory at all. */
   char feat[256];
   if (strcmp(config_pr_base_mode(), "default_branch") != 0)
   {
      int found = feature_branch_for_session(feat, sizeof(feat));
      if (found == 0 && feat[0])
      {
         if (pr_ensure_feature_branch(feat, err, err_len) != 0)
            return -1;
         snprintf(out, out_len, "%s", feat);
         return 0;
      }
      /* "Could not tell" is NOT "nothing named". Falling through on an unreachable
       * store would open the PR against the trunk while this session has a feature
       * branch it cannot see -- silently retargeting the PR, which is the whole
       * failure this feature exists to remove. Only a definite absence falls back. */
      if (found < 0)
      {
         snprintf(err, err_len,
                  "cannot read this session's feature branch, so the base is unknown. Pass base "
                  "explicitly, or set pr_base_mode=default_branch to target the repository "
                  "default branch");
         return -1;
      }
   }

   /* No feature branch for this session -> the repository's default branch, which is
    * what every PR targeted before feature targeting existed. Deliberately NOT a
    * refusal: a session that never named a feature must still be able to open a PR,
    * and failing closed here would break every caller that worked yesterday. Name one
    * with base=aimee/feat/<slug> (or answer the prompt `aimee git pr` shows on a
    * terminal) and it sticks for the rest of the session. */
   if (pr_repo_default_branch(out, out_len) != 0)
   {
      snprintf(err, err_len, "cannot resolve the repository default branch (pass base explicitly)");
      return -1;
   }
   return 0;
}

/* Write a PR title and body from the commits this branch has that `base` does
 * not. Returns 0, or -1 when the branch has no such commits (nothing to open).
 *
 * Deterministic on purpose — no model call. The commit subjects ARE the summary,
 * so a title the caller would have written costs a turn to produce and says the
 * same thing. One commit lends its subject verbatim (and its body, which is
 * usually the rationale worth reading); several get the shared conventional-commit
 * prefix where they agree, and the body lists them with the diffstat. */
static int pr_derive_from_commits(const char *base, char *title, size_t title_len, char *body,
                                  size_t body_len)
{
   title[0] = '\0';
   body[0] = '\0';

   /* Prefer the remote's copy of the base: a stale local base would attribute
    * commits to this branch that are already merged. */
   char remote_ref[600];
   snprintf(remote_ref, sizeof(remote_ref), "origin/%s", base);
   char *quoted_remote_ref = shell_quote(remote_ref);
   char range[512];
   int rc = 0;
   snprintf(range, sizeof(range), "git rev-parse --verify --quiet %s >/dev/null 2>&1",
            quoted_remote_ref);
   free(quoted_remote_ref);
   int have_remote_base = 0;
   {
      char *probe = mcp_git_run(range, &rc);
      free(probe);
      have_remote_base = (rc == 0);
   }
   char base_ref[600];
   if (have_remote_base && strncmp(base, "origin/", 7) != 0)
      snprintf(base_ref, sizeof(base_ref), "origin/%s", base);
   else
      snprintf(base_ref, sizeof(base_ref), "%s", base);

   char cmd[1024];
   char revision_range[700];
   snprintf(revision_range, sizeof(revision_range), "%s..HEAD", base_ref);
   char *quoted_range = shell_quote(revision_range);
   snprintf(cmd, sizeof(cmd), "git log --reverse --format='%%s' %s 2>/dev/null", quoted_range);
   char *subjects = mcp_git_run(cmd, &rc);
   if (rc != 0 || !subjects || !subjects[0])
   {
      free(subjects);
      free(quoted_range);
      return -1;
   }

   /* Count and collect. */
   int n = 0;
   char first[512] = "";
   int bpos = 0;
   char *line = subjects;
   while (line && *line)
   {
      char *nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';
      if (*line)
      {
         n++;
         if (n == 1)
            snprintf(first, sizeof(first), "%s", line);
         bpos = str_appendf(body, bpos, (int)body_len, "- %s\n", line);
      }
      line = nl ? nl + 1 : NULL;
   }
   free(subjects);
   if (n == 0)
   {
      free(quoted_range);
      return -1;
   }

   if (n == 1)
   {
      /* One commit: its subject is the title, and its own body is the rationale. */
      snprintf(title, title_len, "%s", first);
      snprintf(cmd, sizeof(cmd), "git log -1 --format='%%b' HEAD 2>/dev/null");
      char *msg_body = mcp_git_run(cmd, &rc);
      body[0] = '\0';
      if (rc == 0 && msg_body && msg_body[0])
         snprintf(body, body_len, "%s", msg_body);
      free(msg_body);
      bpos = (int)strlen(body);
   }
   else
   {
      /* Several: keep the conventional-commit type when every subject agrees on
       * one, so the PR title reads like the commits it contains. */
      char prefix[64] = "";
      const char *colon = strchr(first, ':');
      if (colon && colon - first < (long)sizeof(prefix) - 1)
      {
         size_t plen = (size_t)(colon - first);
         memcpy(prefix, first, plen);
         prefix[plen] = '\0';
         /* Only a type/scope prefix, not any sentence with a colon in it. */
         for (char *p = prefix; *p; p++)
            if (!islower((unsigned char)*p) && *p != '(' && *p != ')' && *p != '-' && *p != '/' &&
                *p != '_')
            {
               prefix[0] = '\0';
               break;
            }
      }
      char branch[256] = "";
      get_current_branch(branch, sizeof(branch));
      const char *topic = branch;
      const char *slash = strrchr(branch, '/');
      if (slash && slash[1])
         topic = slash + 1;
      if (prefix[0])
         snprintf(title, title_len, "%s: %s (%d commits)", prefix, topic, n);
      else
         snprintf(title, title_len, "%s (%d commits)", topic, n);
   }

   /* The diffstat is the one thing the subjects do not say: how big this is. */
   free(quoted_range);
   snprintf(revision_range, sizeof(revision_range), "%s...HEAD", base_ref);
   quoted_range = shell_quote(revision_range);
   snprintf(cmd, sizeof(cmd), "git diff --stat %s 2>/dev/null", quoted_range);
   free(quoted_range);
   char *stat = mcp_git_run(cmd, &rc);
   if (rc == 0 && stat && stat[0])
      str_appendf(body, bpos, (int)body_len, "\n%s", stat);
   free(stat);
   return 0;
}

/* Would merging this branch into `base` conflict? Names the conflicting files in
 * `files` when it would.
 *
 * A PR opened in that state is worse than no PR: the forge reports it CONFLICTING,
 * review cannot start, CI results are about a merge that will never happen, and
 * somebody has to notice and rebase before any of it means anything. The check is
 * a merge-tree dry run, so it decides this WITHOUT touching the working tree or
 * the branch.
 *
 * Returns 1 for conflicts, 0 for a clean merge, and -1 when it cannot tell (a
 * missing base, no origin, an ancient git) — callers must treat -1 as "proceed",
 * because refusing to open a PR on an inconclusive check would be worse than the
 * problem. */
int mcp_git_conflicts_with_base(const char *base, char *files, size_t files_cap)
{
   if (files && files_cap)
      files[0] = '\0';
   if (!base || !base[0])
      return -1;

   char base_ref[600];
   int rc = 0;
   {
      char probe[700];
      char remote_ref[600];
      snprintf(remote_ref, sizeof(remote_ref), "origin/%s", base);
      char *quoted_remote_ref = shell_quote(remote_ref);
      snprintf(probe, sizeof(probe), "git rev-parse --verify --quiet %s >/dev/null 2>&1",
               quoted_remote_ref);
      free(quoted_remote_ref);
      char *p = mcp_git_run(probe, &rc);
      free(p);
   }
   if (rc == 0 && strncmp(base, "origin/", 7) != 0)
      snprintf(base_ref, sizeof(base_ref), "origin/%s", base);
   else
      snprintf(base_ref, sizeof(base_ref), "%s", base);

   /* Fetch first: a stale local copy of the base would clear a branch that in fact
    * conflicts with what the base has become, which is precisely the case that
    * produces a CONFLICTING PR. */
   if (strncmp(base_ref, "origin/", 7) == 0)
   {
      char fetch_cmd[700];
      char *quoted_branch = shell_quote(base_ref + 7);
      snprintf(fetch_cmd, sizeof(fetch_cmd), "git fetch origin %s 2>&1", quoted_branch);
      free(quoted_branch);
      int frc = 0;
      free(mcp_git_run(fetch_cmd, &frc));
   }

   /* Resolve the base before merging against it. `merge-tree --write-tree` exits
    * 1 BOTH for "these conflict" and for "that ref does not exist", so without
    * this the gate reads an unresolvable base as a conflict and refuses a PR
    * that would have merged cleanly -- the exact inversion this function's
    * contract forbids. A base branch that exists on the forge but not in this
    * checkout is ordinary, so the wrong answer here would be common. */
   {
      char verify[700];
      char commit_ref[700];
      snprintf(commit_ref, sizeof(commit_ref), "%s^{commit}", base_ref);
      char *quoted_commit_ref = shell_quote(commit_ref);
      snprintf(verify, sizeof(verify), "git rev-parse --verify --quiet %s >/dev/null 2>&1",
               quoted_commit_ref);
      free(quoted_commit_ref);
      int vrc = 0;
      free(mcp_git_run(verify, &vrc));
      if (vrc != 0)
         return -1;
   }

   char cmd[1024];
   char *quoted_base_ref = shell_quote(base_ref);
   snprintf(cmd, sizeof(cmd),
            "git merge-tree --write-tree --name-only --no-messages HEAD %s 2>/dev/null",
            quoted_base_ref);
   free(quoted_base_ref);
   int mrc = 0;
   char *out = mcp_git_run(cmd, &mrc);
   /* --write-tree exits 1 with the conflicted paths on stdout, 0 when clean. */
   if (out && mrc == 1)
   {
      /* First line is the merged tree oid; the rest are the conflicted paths. */
      const char *p = strchr(out, '\n');
      if (p && p[1] && files && files_cap)
      {
         int pos = 0;
         const char *line = p + 1;
         while (*line)
         {
            const char *nl = strchr(line, '\n');
            size_t len = nl ? (size_t)(nl - line) : strlen(line);
            if (len)
               pos = str_appendf(files, pos, (int)files_cap, "\n  %.*s", (int)len, line);
            line = nl ? nl + 1 : line + len;
         }
      }
      free(out);
      return 1;
   }
   if (out && mrc == 0)
   {
      free(out);
      return 0;
   }
   free(out);
   return -1; /* merge-tree unavailable or the base could not be resolved */
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

/* --- action=ready: sync, push, open the PR ---
 *
 * "This work is done, put it up for review" is three calls and one piece of
 * knowledge the caller should not need: that rebasing during the sync rewrites
 * history, so the push after it has to be a lease-protected force. Doing them
 * separately also means a failure halfway leaves the caller to work out which
 * step it was. This runs them in order, stops at the first real failure with that
 * step's own explanation (sync already says how to resolve a conflict), and
 * reports each step's outcome on its own line.
 *
 * Idempotent: run it again after more commits and it re-syncs, re-pushes, and
 * says the PR is already open rather than failing. */
static void first_line_of(cJSON *resp, char *out, size_t out_len)
{
   out[0] = '\0';
   cJSON *item = resp ? cJSON_GetArrayItem(resp, 0) : NULL;
   cJSON *text = item ? cJSON_GetObjectItem(item, "text") : NULL;
   if (!cJSON_IsString(text))
      return;
   snprintf(out, out_len, "%s", text->valuestring);
   char *nl = strchr(out, '\n');
   if (nl)
      *nl = '\0';
}

static cJSON *pr_ready(cJSON *args)
{
   cJSON *jbase = cJSON_GetObjectItemCaseSensitive(args, "base");
   const char *base = (cJSON_IsString(jbase) && jbase->valuestring[0]) ? jbase->valuestring : NULL;

   /* Resolve the base ONCE, up front, and use it for the sync as well as the PR. The
    * sync has to rebase onto the branch the PR will target: rebasing a slice onto the
    * trunk while opening it against its feature branch produces a diff full of commits
    * the feature branch does not have yet. */
   char resolved_base[256];
   if (!base)
   {
      char berr[512];
      if (pr_resolve_base(resolved_base, sizeof(resolved_base), berr, sizeof(berr)) != 0)
         return mcp_error("error: %s", berr);
      base = resolved_base;
   }

   char report[4096];
   int pos = 0;

   /* 1. Sync. A conflict here is the caller's to resolve, and sync's own message
    * is the one that explains how — so return it unchanged rather than wrapping. */
   cJSON *sync_args = cJSON_CreateObject();
   if (base)
      cJSON_AddStringToObject(sync_args, "base", base);
   cJSON *sync_resp = handle_git_sync(sync_args);
   int synced_moved = 0;
   {
      char line[1024];
      first_line_of(sync_resp, line, sizeof(line));
      if (mcp_git_response_failed(sync_resp))
      {
         cJSON_Delete(sync_args);
         return sync_resp; /* carries the conflicted files and the way forward */
      }
      synced_moved = (strstr(line, "already current") == NULL);
      pos = str_appendf(report, pos, (int)sizeof(report), "sync: %s\n", line);
   }
   cJSON_Delete(sync_resp);
   cJSON_Delete(sync_args);

   /* 2. Push. A sync that rebased rewrote history, so the push must be
    * lease-protected — which is also safe when nothing moved, so it is
    * unconditional rather than a guess about what sync did. */
   cJSON *push_args = cJSON_CreateObject();
   cJSON_AddBoolToObject(push_args, "force", 1);
   cJSON *push_resp = handle_git_push(push_args);
   {
      char line[1024];
      first_line_of(push_resp, line, sizeof(line));
      if (mcp_git_response_failed(push_resp))
      {
         cJSON_Delete(push_args);
         return push_resp; /* the ownership / merged-PR / verify gates speak for themselves */
      }
      pos = str_appendf(report, pos, (int)sizeof(report), "push: %s\n", line);
   }
   cJSON_Delete(push_resp);
   cJSON_Delete(push_args);

   /* 3. Open the PR. Title and body are derived from the commits unless the
    * caller passed them, so `action=ready` alone is a complete request. */
   cJSON *create_args = cJSON_CreateObject();
   cJSON_AddStringToObject(create_args, "action", "create");
   if (base)
      cJSON_AddStringToObject(create_args, "base", base);
   cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(args, "title");
   cJSON *jbody = cJSON_GetObjectItemCaseSensitive(args, "body");
   if (cJSON_IsString(jtitle) && jtitle->valuestring[0])
      cJSON_AddStringToObject(create_args, "title", jtitle->valuestring);
   if (cJSON_IsString(jbody) && jbody->valuestring[0])
      cJSON_AddStringToObject(create_args, "body", jbody->valuestring);

   cJSON *create_resp = handle_git_pr(create_args);
   {
      char line[1024];
      first_line_of(create_resp, line, sizeof(line));
      cJSON *item = cJSON_GetArrayItem(create_resp, 0);
      cJSON *text = item ? cJSON_GetObjectItem(item, "text") : NULL;
      const char *full = cJSON_IsString(text) ? text->valuestring : "";
      if (mcp_git_response_failed(create_resp) && strstr(full, "already exist"))
         pos = str_appendf(report, pos, (int)sizeof(report),
                           "pr:   already open for this branch — the new commits are on it now "
                           "(command=pr action=list to see it)\n");
      else if (mcp_git_response_failed(create_resp))
      {
         /* Synced and pushed, but the PR did not open: say what DID happen, so the
          * caller does not redo the first two steps. */
         cJSON *r = mcp_error("%s", full[0] ? full : "error: pr create failed");
         cJSON *ritem = cJSON_GetArrayItem(r, 0);
         char merged[4096];
         snprintf(merged, sizeof(merged), "%s%s", report, full);
         cJSON_ReplaceItemInObject(ritem, "text", cJSON_CreateString(merged));
         cJSON_Delete(create_resp);
         cJSON_Delete(create_args);
         return r;
      }
      else
         pos = str_appendf(report, pos, (int)sizeof(report), "pr:   %s\n%s\n", line,
                           full + strlen(line));
   }
   cJSON_Delete(create_resp);
   cJSON_Delete(create_args);
   (void)synced_moved;

   return mcp_text(report);
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

   /* action=failures: why CI is red.
    *
    * action=checks says a job failed and hands back a details_url, which an agent
    * cannot open (no browser, and the shell-git gate blocks `gh`). So the loop
    * "push, read the failure, fix it" was closed: the only way to learn the cause
    * was for a human to paste it in. This answers with the job, the step inside it
    * that failed, and the tail of that job's log — the step name alone is often
    * enough, because an unnamed step reports its command line, which is the gate to
    * run locally. */
   if (strcmp(action, "failures") == 0)
   {
      cJSON *jnum = cJSON_GetObjectItemCaseSensitive(args, "number");
      if (!cJSON_IsNumber(jnum))
         return mcp_text("error: 'number' parameter is required for failures");

      char slug[264];
      if (get_origin_repo_slug(slug, sizeof(slug)) != 0)
         return mcp_text("error: cannot resolve a github.com origin for this checkout");

      /* Logs are fetched for the first few failures only: they are the expensive
       * part, and a run where twenty jobs went red almost always has one cause. */
      cJSON *jcount = cJSON_GetObjectItemCaseSensitive(args, "count");
      int logs_for = (cJSON_IsNumber(jcount) && jcount->valueint > 0) ? jcount->valueint : 3;
      if (logs_for > 10)
         logs_for = 10;

      git_pr_failure_t rows[30];
      int n = 0;
      char err[512] = "";
      if (git_pr_failures_via_api_slug(agent_get_request_vault_principal(), slug, jnum->valueint,
                                       (int)(sizeof(rows) / sizeof(rows[0])), logs_for,
                                       PR_LOG_TAIL_BYTES, rows, &n, err, sizeof(err)) != 0)
         return mcp_error("error: pr failures failed: %s", err[0] ? err : "unknown");

      if (n == 0)
         return mcp_text("no failing checks on this PR's head commit (a check still running is not "
                         "a failure — use action=checks for the full status list)");

      dstr_t res;
      dstr_init(&res);
      dstr_appendf(&res, "%d failing check(s) on PR #%d:\n", n, jnum->valueint);
      for (int i = 0; i < n; i++)
      {
         dstr_appendf(&res, "\n=== %s (%s)\n", rows[i].name[0] ? rows[i].name : "(unnamed check)",
                      rows[i].conclusion);
         if (rows[i].failed_step[0])
            dstr_appendf(&res, "failed at step %d: %s\n", rows[i].failed_step_number,
                         rows[i].failed_step);
         if (rows[i].log_tail && rows[i].log_tail[0])
         {
            /* The tail starts mid-line after a byte-range read; drop the partial
             * first line rather than present a truncated one as if it were whole. */
            const char *tail = rows[i].log_tail;
            const char *nl = strchr(tail, '\n');
            if (nl && nl[1])
               tail = nl + 1;
            dstr_appendf(&res, "--- last %d bytes of the job log ---\n%s\n", PR_LOG_TAIL_BYTES,
                         tail);
         }
         else if (i < logs_for)
            dstr_append_str(&res, "(job log unavailable)\n");
      }
      git_pr_failures_free(rows, n);
      char *text = dstr_steal(&res);
      cJSON *r = mcp_text(text ? text : "error: out of memory rendering failures");
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

   if (strcmp(action, "update_branch") == 0)
   {
      /* Merge the base INTO the PR head -- REST's "Update branch" button.
       *
       * Needed because a base protected with "require branches to be up to date"
       * reports its required checks as merely "expected" while the head is BEHIND:
       * the PR will not merge however green those checks already are.
       *
       * Distinct from command=sync, which is the LOCAL equivalent: sync rebases the
       * checked-out branch onto its base and needs the checkout; this asks GitHub to
       * merge the base into a PR head by number, so it works on a PR that is not
       * checked out here (and on someone else's branch).
       *
       * Deliberately NOT folded into `merge`: this rewrites the contributor's
       * branch and restarts their CI, which is a separate decision from merging.
       * GitHub answers 202 -- accepted, not built -- so callers must poll
       * merge_status before merging rather than treating success as ready. */
      cJSON *jnum = cJSON_GetObjectItemCaseSensitive(args, "number");
      if (!cJSON_IsNumber(jnum))
         return mcp_text("error: 'number' parameter is required for update_branch");

      char expected_head[72] = {0};
      cJSON *jhead = cJSON_GetObjectItemCaseSensitive(args, "expected_head_sha");
      if (cJSON_IsString(jhead) && jhead->valuestring[0])
      {
         /* Same hex-only guard the merge path applies to this field. */
         const char *h = jhead->valuestring;
         int ok = 1;
         for (const char *p = h; *p; p++)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
            {
               ok = 0;
               break;
            }
         if (!ok || !h[0])
            return mcp_text("error: 'expected_head_sha' must be a hex SHA");
         snprintf(expected_head, sizeof(expected_head), "%s", h);
      }

      char slug[264];
      if (get_origin_repo_slug(slug, sizeof(slug)) != 0)
         return mcp_text("error: cannot resolve a github.com origin for this checkout");

      char err[512];
      err[0] = '\0';
      int rc = git_pr_update_branch_via_api_slug(
          agent_get_request_vault_principal(), slug, jnum->valueint,
          expected_head[0] ? expected_head : NULL, err, sizeof(err));
      if (rc < 0)
         return mcp_error("error: pr update_branch failed: %s", err[0] ? err : "unknown");

      char msg[256];
      snprintf(msg, sizeof(msg), "PR #%d: %s", jnum->valueint,
               rc == 1 ? "already up to date with base; nothing to do"
                       : "update queued (202) — CI restarts on the new head; poll "
                         "merge_status before merging");
      return mcp_text(msg);
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
      char expected_head[72] = {0};
      cJSON *jhead = cJSON_GetObjectItemCaseSensitive(args, "expected_head_sha");
      if (cJSON_IsString(jhead) && jhead->valuestring[0])
      {
         /* only allow a hex SHA to flow into the shell command. The same validation
          * guards the API path: it goes into a JSON body rather than a command line,
          * but a caller handing us a non-SHA is a caller bug either way. */
         const char *h = jhead->valuestring;
         int ok = 1;
         for (const char *p = h; *p; p++)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
            {
               ok = 0;
               break;
            }
         if (ok && h[0] && strlen(h) < sizeof(expected_head))
         {
            snprintf(match, sizeof(match), " --match-head-commit %s", h);
            snprintf(expected_head, sizeof(expected_head), "%s", h);
         }
      }

      /* CI must be fully green before the merge (operator ruling 2026-07-15). Now read
       * through the Checks API in-process, so the verdict comes from the vaulted token
       * and the gate works from any workspace -- `gh` in the aimee-server image has no
       * credential at all, which left this gate dead for a SHARED or CONTAINER
       * session.
       *
       * Still fails CLOSED, and the mapping preserves that exactly. Where the gh
       * version read an exit code, this reads git_pr_ci_permits_merge:
       *
       *   gh exit 0  (all passed)          -> SUCCESS  merges
       *   "no checks reported"             -> NONE     merges: nothing to fail
       *   gh exit 8  (pending)             -> PENDING  refuses, unless auto_merge
       *   gh exit 1  (a check failed)      -> FAILURE  refuses
       *   anything unclassifiable         -> ERROR    refuses
       *
       * The last line is the important one: "unknown" is never "pass". The old comment
       * warned against inferring a verdict from parsed counters because a zero count
       * is equally "no rows" and "no checks"; git_pr_ci_via_api_slug removes that
       * ambiguity by reporting NONE and ERROR as distinct values rather than both as
       * an absence. */
      char merge_slug[264];
      if (get_origin_repo_slug(merge_slug, sizeof(merge_slug)) != 0)
         return mcp_text("error: cannot resolve a github.com origin for this checkout");
      const char *principal = agent_get_request_vault_principal();

      {
         char cerr[512];
         cerr[0] = '\0';
         git_pr_ci_t ci = git_pr_ci_via_api_slug(principal, merge_slug, pr_num, cerr, sizeof(cerr));
         const char *why = NULL;
         if (ci == GIT_PR_CI_PENDING && auto_merge)
            why = NULL; /* branch protection keeps an auto-merge pending until green */
         else if (!git_pr_ci_permits_merge(ci))
            why = ci == GIT_PR_CI_PENDING   ? "CI has not finished; re-try once checks settle"
                  : ci == GIT_PR_CI_FAILURE ? "CI is not green (at least one check failed)"
                                            : "could not read CI status";
         /* GIT_PR_CI_NONE permits: no CI reported has nothing to fail (operator
          * ruling). GIT_PR_CI_ERROR does not -- "unknown" is never "pass", which is
          * the same fail-closed rule the gh exit-code version enforced. */
         if (why)
         {
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "error: merge blocked — %s. A merge requires fully green CI.%s%s", why,
                     cerr[0] ? " detail: " : "", cerr[0] ? cerr : "");
            return mcp_text(msg);
         }
      }

      cJSON *res = cJSON_CreateObject();
      cJSON_AddStringToObject(res, "executor", "git_pr");

      if (auto_merge)
      {
         /* Auto-merge stays on `gh` for now. Enabling it is a GraphQL mutation
          * (enablePullRequestAutoMerge) and this module speaks only REST, so moving
          * it means new transport rather than a swapped call. Keeping it here is not
          * a regression -- it is how auto-merge already worked -- but it inherits the
          * `gh` credential problem: no GH_TOKEN reaches the child (FD mode), so this
          * branch only works from a DETACHED workspace where the client holds its own
          * creds. Direct merges below no longer have that limitation. */
         char cmd[512];
         snprintf(cmd, sizeof(cmd), "gh pr merge %d %s%s --auto 2>&1", pr_num, mflag, match);
         int rc = 0;
         char *out = mcp_git_run(cmd, &rc);
         cJSON_AddStringToObject(res, "command", cmd);
         cJSON_AddNumberToObject(res, "exit_code", rc);
         cJSON_AddStringToObject(res, "output", out ? out : "");
         free(out);
         /* Accepted is not merged. Do not manufacture merge evidence for a queued PR. */
         cJSON_AddBoolToObject(res, "auto_merge_enabled", rc == 0 ? 1 : 0);
         cJSON_AddBoolToObject(res, "merged", 0);
      }
      else
      {
         const char *method = strcmp(mflag, "--squash") == 0   ? "squash"
                              : strcmp(mflag, "--rebase") == 0 ? "rebase"
                                                               : "merge";
         char merge_sha[72];
         char merr[512];
         merge_sha[0] = '\0';
         merr[0] = '\0';
         int rc = git_pr_merge_via_api_slug_ex(principal, merge_slug, pr_num, method,
                                               expected_head[0] ? expected_head : NULL, merge_sha,
                                               sizeof(merge_sha), merr, sizeof(merr));

         /* The ledger records the request that was made. There is no shell command
          * now, so state the API call instead of leaving the field empty. */
         char desc[256];
         snprintf(desc, sizeof(desc), "PUT /repos/%s/pulls/%d/merge merge_method=%s%s%s",
                  merge_slug, pr_num, method, expected_head[0] ? " sha=" : "",
                  expected_head[0] ? expected_head : "");
         cJSON_AddStringToObject(res, "command", desc);
         cJSON_AddNumberToObject(res, "exit_code", rc);
         cJSON_AddStringToObject(res, "output", merr);
         cJSON_AddBoolToObject(res, "merged", rc == 0 || rc == 1);
         if (rc == 0 || rc == 1)
            cJSON_AddStringToObject(res, "merge_sha", merge_sha);
         if (rc == 1)
            cJSON_AddBoolToObject(res, "already_merged", 1);
         /* 3 is a content conflict and identical on every retry; 2 is a lost race
          * that a retry wins. Callers that loop must not treat them alike. */
         if (rc == 3)
            cJSON_AddBoolToObject(res, "conflict", 1);
         else if (rc == 2)
            cJSON_AddBoolToObject(res, "retryable", 1);

         /* A merge into a feature branch may have been the LAST one that feature was
          * waiting on. If so the feature is ready for review as a whole, so promote it
          * to a draft PR against the default branch. Best-effort: a promotion failure
          * is reported, never allowed to mask the merge that did succeed. */
         if (rc == 0 || rc == 1)
         {
            git_pr_info_t merged_info;
            char ierr[512];
            ierr[0] = '\0';
            if (git_pr_info_via_api_slug(principal, merge_slug, pr_num, &merged_info, ierr,
                                         sizeof(ierr)) == 0)
            {
               char note[1024];
               pr_promote_feature(principal, merge_slug, merged_info.base, note, sizeof(note));
               if (note[0])
                  cJSON_AddStringToObject(res, "promote", note + 1); /* drop the lead newline */
            }
         }
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
       * A PR body is user-controlled and shell_quote can expand it up to ~4x,
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

      /* An explicit base wins; otherwise this session's feature branch (created on the
       * remote if this is its first PR), or the repo default under
       * pr_base_mode=default_branch. Never a hardcoded "main" -- that opened every PR
       * against a branch the repo may not even have as its trunk. */
      char resolved_base[256];
      const char *base;
      if (cJSON_IsString(jbase) && jbase->valuestring[0])
      {
         base = jbase->valuestring;
         /* An explicitly named feature branch becomes this session's feature: the
          * operator (or the terminal prompt) says it once and the rest of the
          * session's PRs inherit it, which is the whole point of a feature branch. */
         if (strncmp(base, "aimee/feat/", 11) == 0)
            (void)feature_branch_set(base);
      }
      else
      {
         char berr[512];
         if (pr_resolve_base(resolved_base, sizeof(resolved_base), berr, sizeof(berr)) != 0)
            return mcp_error("error: %s", berr);
         base = resolved_base;
      }

      /* Refuse to open a PR that cannot be merged.
       *
       * A CONFLICTING PR blocks its own review: the forge will not merge it, its CI
       * results describe a merge that will never happen, and someone has to spot
       * the state and rebase before any of it counts. Opening one is not a partial
       * success, so this is a refusal and not a warning — with the one command that
       * fixes it. An inconclusive check (-1) proceeds; refusing on "cannot tell"
       * would block more work than it protects. */
      {
         char conflicts[2048];
         if (mcp_git_conflicts_with_base(base, conflicts, sizeof(conflicts)) == 1)
         {
            char buf[2560];
            snprintf(buf, sizeof(buf),
                     "error: not opening this PR — merging it into %s would conflict, so it would "
                     "arrive unmergeable. Conflicting file(s):%s\n\nRun command=sync base=%s "
                     "abort_on_conflict=false, resolve them, command=add, command=rebase "
                     "action=continue, then open the PR (or command=pr action=ready, which does "
                     "the sync and the push for you).",
                     base, conflicts, base);
            return mcp_text(buf);
         }
      }

      /* Title and body are OPTIONAL: the branch's own commits already say what the
       * change is, so aimee derives them rather than making the caller spend a
       * model turn writing prose it can read off the history. An explicit title
       * always wins. */
      char derived_title[512] = "", derived_body[8192] = "";
      if (!cJSON_IsString(jtitle) || !jtitle->valuestring[0])
      {
         if (pr_derive_from_commits(base, derived_title, sizeof(derived_title), derived_body,
                                    sizeof(derived_body)) != 0)
            return mcp_text("error: this branch has no commits that are not already on the base, "
                            "so there is nothing to open a PR for (and no title to derive). Commit "
                            "your work first, or pass 'title' explicitly.");
         if (jtitle)
            cJSON_ReplaceItemInObject(args, "title", cJSON_CreateString(derived_title));
         else
            cJSON_AddStringToObject(args, "title", derived_title);
         jtitle = cJSON_GetObjectItemCaseSensitive(args, "title");

         if (!cJSON_IsString(jbody) || !jbody->valuestring[0])
         {
            if (jbody)
               cJSON_ReplaceItemInObject(args, "body", cJSON_CreateString(derived_body));
            else
               cJSON_AddStringToObject(args, "body", derived_body);
            jbody = cJSON_GetObjectItemCaseSensitive(args, "body");
         }
      }

      /* Standing directive: no AI attribution in PR bodies (in-place strip is
       * shrink-only, so the cJSON-owned buffer is safe). */
      if (cJSON_IsString(jbody))
         strip_ai_attribution(jbody->valuestring);

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

   if (strcmp(action, "ready") == 0)
      return pr_ready(args);

   return mcp_text("error: unknown action. Use "
                   "create/view/list/edit/checks/watch/merge_status/merge/wait/ready");
}
