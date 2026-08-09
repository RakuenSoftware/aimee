/* mcp_git_integrate.c: the operations that bring one line of history into
 * another — merge, rebase, cherry-pick, revert — and `sync`, which is the whole
 * "get my branch current with its base" errand in one call.
 *
 * These four are one operation with four names. Each can stop mid-flight on a
 * conflict, each leaves the repository in a state that needs continuing or
 * abandoning, and each needs an author identity when it finally commits. So they
 * share one driver (integrate_run) and differ only by the row in OPS.
 *
 * The point of modeling them rather than passing argv through is that AIMEE does
 * the work the caller would otherwise have to reason about:
 *   - a remote-looking ref is fetched first, so `merge origin/main` cannot
 *     silently merge a stale copy;
 *   - the editor is disabled, so nothing ever blocks waiting on one;
 *   - a conflict is REPORTED as the list of conflicted files and, by default,
 *     ABORTED, so the caller is never handed a half-merged tree it has to know
 *     how to clean up;
 *   - a continuation commits with the vaulted operator identity, the same as
 *     git_commit;
 *   - the result says what changed (commits, files, HEAD) instead of echoing
 *     git's prose.
 */
#include "aimee.h"
#include "branch_ownership.h"
#include "cJSON.h"
#include "mcp_git.h"
#include "util.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INTEGRATE_OUT_MAX 8192
#define CONFLICT_LIST_MAX 4096

static cJSON *mcp_text(const char *text)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *item = cJSON_CreateObject();
   cJSON_AddStringToObject(item, "type", "text");
   cJSON_AddStringToObject(item, "text", text);
   cJSON_AddItemToArray(arr, item);
   return arr;
}

static cJSON *mcp_textf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static cJSON *mcp_textf(const char *fmt, ...)
{
   char buf[INTEGRATE_OUT_MAX];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   return mcp_text(buf);
}

/* See mcp_git.h. The git tools report failure in the text they return — the
 * leading word is "error" or "conflict" — and composed operations (sync, pr
 * action=ready) have to branch on it. One definition of the convention, here,
 * rather than a substring check per caller. */
int mcp_git_response_failed(cJSON *resp)
{
   cJSON *item = resp ? cJSON_GetArrayItem(resp, 0) : NULL;
   cJSON *text = item ? cJSON_GetObjectItem(item, "text") : NULL;
   if (!cJSON_IsString(text))
      return 1;
   return strncmp(text->valuestring, "error", 5) == 0 ||
          strncmp(text->valuestring, "conflict", 8) == 0;
}

/* One row per operation. `start` and the continuation words are git's own
 * spelling; everything else about how the operation is driven is shared. */
typedef struct
{
   const char *name;        /* aimee command name, for messages */
   const char *start;       /* printf with one %s: the escaped target ref */
   const char *needs_ref;   /* what to call the ref in the "required" error */
   const char *inprog_ref;  /* ref that exists while the op is stopped mid-flight */
   const char *cont;        /* git words to continue */
   const char *abort;       /* git words to abort */
   const char *skip;        /* git words to skip a commit, NULL if not applicable */
   int commits_on_continue; /* continuing creates a commit -> needs an identity */
} integrate_op_t;

static const integrate_op_t OPS[] = {
    {"merge", "merge --no-edit '%s'", "ref (the branch or commit to merge in)", "MERGE_HEAD",
     "merge --continue", "merge --abort", NULL, 1},
    {"rebase", "rebase '%s'", "base (the branch to rebase onto)", "REBASE_HEAD",
     "rebase --continue", "rebase --abort", "rebase --skip", 1},
    {"cherry_pick", "cherry-pick '%s'", "ref (the commit to cherry-pick)", "CHERRY_PICK_HEAD",
     "cherry-pick --continue", "cherry-pick --abort", "cherry-pick --skip", 1},
    {"revert", "revert --no-edit '%s'", "ref (the commit to revert)", "REVERT_HEAD",
     "revert --continue", "revert --abort", "revert --skip", 1},
};

/* Never let git open an editor: these run unattended, and a blocked editor looks
 * exactly like a hang. core.editor covers merge/revert messages, and
 * sequence.editor covers the rebase todo list. */
#define GIT_NO_EDITOR "-c core.editor=true -c sequence.editor=true"

static int run_ok(const char *cmd)
{
   int rc = 0;
   free(mcp_git_run(cmd, &rc));
   return rc == 0;
}

static void capture_line(const char *cmd, char *out, size_t out_len)
{
   out[0] = '\0';
   int rc = 0;
   char *got = mcp_git_run(cmd, &rc);
   if (got && rc == 0)
   {
      snprintf(out, out_len, "%s", got);
      out[strcspn(out, "\r\n")] = '\0';
   }
   free(got);
}

static void short_head(char *out, size_t out_len)
{
   capture_line("git rev-parse --short HEAD 2>/dev/null", out, out_len);
}

/* Files git has left with conflict markers. This is the one piece of state the
 * caller genuinely needs from a stopped operation. */
static int conflicted_files(char *out, size_t out_len)
{
   out[0] = '\0';
   int rc = 0;
   char *got = mcp_git_run("git diff --name-only --diff-filter=U 2>/dev/null", &rc);
   if (!got)
      return 0;
   int n = 0, pos = 0;
   char *line = got;
   while (line && *line)
   {
      char *nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';
      if (*line)
      {
         n++;
         pos = str_appendf(out, pos, (int)out_len, "\n  %s", line);
      }
      line = nl ? nl + 1 : NULL;
   }
   free(got);
   return n;
}

/* Is one of these operations already stopped mid-flight? Returns the op, or NULL.
 * Checked before starting anything: git's own error for this case names a file in
 * .git, which tells the caller nothing about what to do next. */
static const integrate_op_t *operation_in_progress(void)
{
   for (size_t i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++)
   {
      char cmd[128];
      snprintf(cmd, sizeof(cmd), "git rev-parse --verify --quiet %s >/dev/null 2>&1",
               OPS[i].inprog_ref);
      if (run_ok(cmd))
         return &OPS[i];
   }
   return NULL;
}

/* A ref naming a remote-tracking branch is only as current as the last fetch, so
 * fetch it before using it. Silent no-op for a local ref or a raw SHA — that is
 * what makes `merge origin/main` mean what the caller meant. */
static void fetch_if_remote(const char *ref, char *note, size_t note_len)
{
   note[0] = '\0';
   const char *slash = strchr(ref, '/');
   if (!slash || slash == ref)
      return;

   char remote[128];
   size_t rlen = (size_t)(slash - ref);
   if (rlen >= sizeof(remote))
      return;
   memcpy(remote, ref, rlen);
   remote[rlen] = '\0';

   char *esc_remote = shell_escape(remote);
   char check[256];
   snprintf(check, sizeof(check), "git remote get-url '%s' >/dev/null 2>&1", esc_remote);
   if (!run_ok(check))
   {
      free(esc_remote);
      return; /* not a remote name: a branch with a slash, or a path */
   }

   char *esc_branch = shell_escape(slash + 1);
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "git fetch '%s' '%s' 2>&1", esc_remote, esc_branch);
   free(esc_remote);
   free(esc_branch);
   int rc = 0;
   char *out = mcp_git_run(cmd, &rc);
   free(out);
   if (rc == 0)
      snprintf(note, note_len, "fetched %s; ", ref);
   else
      snprintf(note, note_len, "warning: could not fetch %s (using the local copy); ", ref);
}

/* What the operation actually did, in the terms the caller cares about. */
static void describe_change(const char *pre, char *out, size_t out_len)
{
   char post[64] = "";
   short_head(post, sizeof(post));
   if (strcmp(pre, post) == 0)
   {
      snprintf(out, out_len, "already up to date at %s — nothing to do", post);
      return;
   }

   char count_cmd[256], stat_cmd[256], count[64] = "", stat[256] = "";
   snprintf(count_cmd, sizeof(count_cmd), "git rev-list --count '%s'..HEAD 2>/dev/null", pre);
   snprintf(stat_cmd, sizeof(stat_cmd), "git diff --shortstat '%s' HEAD 2>/dev/null", pre);
   capture_line(count_cmd, count, sizeof(count));
   capture_line(stat_cmd, stat, sizeof(stat));

   int pos = snprintf(out, out_len, "%s..%s", pre, post);
   if (count[0])
      pos = str_appendf(out, pos, (int)out_len, ", %s commit(s)", count);
   if (stat[0])
   {
      const char *s = stat;
      while (*s == ' ')
         s++;
      pos = str_appendf(out, pos, (int)out_len, ", %s", s);
   }
   (void)pos;
}

/* Continue / abort / skip a stopped operation. Continuing commits, so it needs
 * the same vaulted identity git_commit uses — without it the merge commit would
 * have no author. */
static cJSON *integrate_resume(const integrate_op_t *op, const char *action)
{
   const integrate_op_t *live = operation_in_progress();
   if (!live)
      return mcp_textf("error: no %s is in progress, so there is nothing to %s", op->name, action);
   if (live != op)
      return mcp_textf("error: a %s is in progress, not a %s — use command=%s action=%s",
                       live->name, op->name, live->name, action);

   const char *words = NULL;
   if (strcmp(action, "continue") == 0)
      words = op->cont;
   else if (strcmp(action, "abort") == 0)
      words = op->abort;
   else if (strcmp(action, "skip") == 0)
      words = op->skip;
   if (!words)
      return mcp_textf("error: %s does not support action=%s", op->name, action);

   char pre[64] = "";
   short_head(pre, sizeof(pre));

   if (strcmp(action, "continue") == 0)
   {
      char still[CONFLICT_LIST_MAX];
      int n = conflicted_files(still, sizeof(still));
      if (n > 0)
         return mcp_textf("error: %d file(s) still have conflict markers; resolve and stage them "
                          "before continuing:%s",
                          n, still);
   }

   char ident[768] = "";
   if (strcmp(action, "continue") == 0 && op->commits_on_continue)
   {
      int have = mcp_git_identity_flags(ident, sizeof(ident));
      if (have <= 0)
         return mcp_text(mcp_git_identity_error(have));
   }

   char cmd[1024];
   snprintf(cmd, sizeof(cmd), "git %s %s %s 2>&1", ident, GIT_NO_EDITOR, words);
   int rc = 0;
   char *out = mcp_git_run(cmd, &rc);

   if (rc != 0)
   {
      char still[CONFLICT_LIST_MAX];
      int n = conflicted_files(still, sizeof(still));
      cJSON *r;
      if (n > 0)
         r = mcp_textf("error: %s --%s stopped again on %d conflicted file(s):%s\n%s", op->name,
                       action, n, still, out ? out : "");
      else
         r = mcp_textf("error: %s --%s failed: %s", op->name, action, out ? out : "unknown");
      free(out);
      return r;
   }
   free(out);

   if (strcmp(action, "abort") == 0)
      return mcp_textf("%s aborted; the tree is back at %s with no changes from it", op->name, pre);

   char change[512];
   describe_change(pre, change, sizeof(change));
   return mcp_textf("%s completed (%s): %s", op->name, action, change);
}

/* Start one of the four. */
static cJSON *integrate_run(const integrate_op_t *op, cJSON *args)
{
   /* Guards first, and the same ones the other writers use: this rewrites the
    * branch. */
   char branch[256] = "";
   get_current_branch(branch, sizeof(branch));
   if (strcmp(branch, "main") == 0 || strcmp(branch, "master") == 0)
      return mcp_textf("error: %s blocked — writing to the main branch is not allowed. Work on a "
                       "feature branch, and merge to main through command=pr action=merge.",
                       op->name);
   {
      cJSON *blocked = branch_own_guard_for(branch, op->name);
      if (blocked)
         return blocked;
   }

   /* rebase takes its target as `base` (that is what it is), the other three take
    * `ref`. Accept either for both, because the distinction is not worth a failed
    * call. */
   cJSON *jref = cJSON_GetObjectItemCaseSensitive(args, "ref");
   cJSON *jbase = cJSON_GetObjectItemCaseSensitive(args, "base");
   const char *ref = NULL;
   if (strcmp(op->name, "rebase") == 0)
      ref = (cJSON_IsString(jbase) && jbase->valuestring[0])
                ? jbase->valuestring
                : ((cJSON_IsString(jref) && jref->valuestring[0]) ? jref->valuestring : NULL);
   else
      ref = (cJSON_IsString(jref) && jref->valuestring[0])
                ? jref->valuestring
                : ((cJSON_IsString(jbase) && jbase->valuestring[0]) ? jbase->valuestring : NULL);
   if (!ref)
      return mcp_textf("error: '%s' is required for %s", op->needs_ref, op->name);

   const integrate_op_t *live = operation_in_progress();
   if (live)
      return mcp_textf("error: a %s is already in progress. Finish it with command=%s "
                       "action=continue, or back out with command=%s action=abort, before "
                       "starting a %s.",
                       live->name, live->name, live->name, op->name);

   /* A dirty tree turns a conflict into a mess that cannot be cleanly aborted. */
   int rc = 0;
   char *dirty = mcp_git_run("git status --porcelain 2>/dev/null", &rc);
   int is_dirty = (dirty && dirty[0]);
   free(dirty);
   if (is_dirty)
      return mcp_textf("error: the working tree has uncommitted changes, so a %s could not be "
                       "cleanly undone if it conflicts. Commit them (command=commit) or set them "
                       "aside (command=stash action=push) first.",
                       op->name);

   char fetch_note[256];
   fetch_if_remote(ref, fetch_note, sizeof(fetch_note));

   char pre[64] = "";
   short_head(pre, sizeof(pre));

   char *esc_ref = shell_escape(ref);
   char start[512];
   snprintf(start, sizeof(start), op->start, esc_ref);
   free(esc_ref);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd), "git %s %s 2>&1", GIT_NO_EDITOR, start);
   char *out = mcp_git_run(cmd, &rc);

   if (rc == 0)
   {
      char change[512];
      describe_change(pre, change, sizeof(change));
      cJSON *r = mcp_textf("%s %s into %s — %s%s", op->name, ref, branch, fetch_note, change);
      free(out);
      return r;
   }

   /* It failed. A conflict is the interesting case and gets the file list; by
    * default the operation is undone so the caller is never left holding a
    * half-applied tree it has to know how to clean up. */
   char conflicts[CONFLICT_LIST_MAX];
   int n = conflicted_files(conflicts, sizeof(conflicts));
   if (n == 0)
   {
      cJSON *r =
          mcp_textf("error: %s %s failed: %s%s", op->name, ref, fetch_note, out ? out : "unknown");
      free(out);
      return r;
   }
   free(out);

   cJSON *jkeep = cJSON_GetObjectItemCaseSensitive(args, "abort_on_conflict");
   int keep_going = (jkeep && cJSON_IsFalse(jkeep)) ? 1 : 0;

   if (!keep_going)
   {
      char abort_cmd[256];
      snprintf(abort_cmd, sizeof(abort_cmd), "git %s 2>&1", op->abort);
      int arc = 0;
      char *aout = mcp_git_run(abort_cmd, &arc);
      free(aout);
      if (arc != 0)
         return mcp_textf("error: %s %s conflicted on %d file(s) and could NOT be aborted — the "
                          "tree is mid-%s and needs a human:%s",
                          op->name, ref, n, op->name, conflicts);
      return mcp_textf("conflict: %s %s touches %d file(s) that changed here too:%s\n\n"
                       "The %s was aborted, so the tree is unchanged at %s. To resolve them in "
                       "place instead, re-run with abort_on_conflict=false — then edit the files, "
                       "command=add, and command=%s action=continue.",
                       op->name, ref, n, conflicts, op->name, pre, op->name);
   }

   return mcp_textf("conflict: %s %s stopped on %d file(s), left in progress:%s\n\n"
                    "Resolve them, stage with command=add, then command=%s action=continue. "
                    "command=%s action=abort backs the whole %s out.",
                    op->name, ref, n, conflicts, op->name, op->name, op->name);
}

static cJSON *integrate_dispatch(const char *name, cJSON *args)
{
   const integrate_op_t *op = NULL;
   for (size_t i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++)
      if (strcmp(name, OPS[i].name) == 0)
         op = &OPS[i];
   if (!op)
      return mcp_text("error: unknown integrate operation");

   cJSON *jaction = cJSON_GetObjectItemCaseSensitive(args, "action");
   const char *action = cJSON_IsString(jaction) ? jaction->valuestring : NULL;
   if (action && action[0] && strcmp(action, "run") != 0)
   {
      if (strcmp(action, "continue") != 0 && strcmp(action, "abort") != 0 &&
          strcmp(action, "skip") != 0)
         return mcp_textf("error: unknown action '%s' for %s. Use continue, abort or skip (or "
                          "omit action to start one).",
                          action, name);
      return integrate_resume(op, action);
   }
   return integrate_run(op, args);
}

cJSON *handle_git_merge(cJSON *args)
{
   return integrate_dispatch("merge", args);
}
cJSON *handle_git_rebase(cJSON *args)
{
   return integrate_dispatch("rebase", args);
}
cJSON *handle_git_cherry_pick(cJSON *args)
{
   return integrate_dispatch("cherry_pick", args);
}
cJSON *handle_git_revert(cJSON *args)
{
   return integrate_dispatch("revert", args);
}

/* --- git_sync ---
 *
 * The errand, not the mechanics: "make this branch current with the branch it
 * will merge into". That is a fetch, a choice of rebase or merge, conflict
 * handling, and a before/after report — which is four calls and a decision the
 * caller should not have to make, so it is one call here.
 *
 * Rebase by default: a PR branch that rebases onto its base reviews as the work
 * alone, with no merge commits from the base mixed in. */
cJSON *handle_git_sync(cJSON *args)
{
   char branch[256] = "";
   get_current_branch(branch, sizeof(branch));

   /* Resolve the base the same way a human would guess it: what was asked for,
    * else what origin says its default branch is, else main. */
   cJSON *jbase = cJSON_GetObjectItemCaseSensitive(args, "base");
   char base[256] = "";
   if (cJSON_IsString(jbase) && jbase->valuestring[0])
      snprintf(base, sizeof(base), "%s", jbase->valuestring);
   else
   {
      char head_ref[256] = "";
      capture_line("git symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null", head_ref,
                   sizeof(head_ref));
      if (head_ref[0])
         snprintf(base, sizeof(base), "%s", head_ref); /* already origin/<name> */
      else
         snprintf(base, sizeof(base), "origin/main");
   }
   /* A bare branch name means the remote's copy of it — syncing to a stale local
    * base is never what was meant — but only if the remote actually has it. A repo
    * with no origin, or a base that lives only locally, syncs to the local branch
    * rather than failing on a ref that was never going to resolve. */
   if (!strchr(base, '/'))
   {
      char *esc = shell_escape(base);
      char probe[512];
      snprintf(probe, sizeof(probe), "git rev-parse --verify --quiet 'origin/%s' >/dev/null 2>&1",
               esc);
      free(esc);
      if (run_ok(probe))
      {
         char qualified[256];
         snprintf(qualified, sizeof(qualified), "origin/%s", base);
         snprintf(base, sizeof(base), "%s", qualified);
      }
   }

   if (strcmp(branch, "main") == 0 || strcmp(branch, "master") == 0)
      return mcp_textf("error: sync blocked — writing to the main branch is not allowed. Switch "
                       "to a feature branch first (command=switch ref=<branch>).");

   cJSON *jmode = cJSON_GetObjectItemCaseSensitive(args, "mode");
   const char *mode =
       (cJSON_IsString(jmode) && jmode->valuestring[0]) ? jmode->valuestring : "rebase";
   if (strcmp(mode, "rebase") != 0 && strcmp(mode, "merge") != 0)
      return mcp_text("error: mode must be rebase (default) or merge");

   /* Report the gap before and after, so the answer to "am I current?" is in the
    * result rather than a follow-up call. */
   char fetch_note[256];
   fetch_if_remote(base, fetch_note, sizeof(fetch_note));

   char gap_cmd[512], gap[128] = "";
   char *esc_base = shell_escape(base);
   snprintf(gap_cmd, sizeof(gap_cmd), "git rev-list --left-right --count '%s'...HEAD 2>/dev/null",
            esc_base);
   free(esc_base);
   capture_line(gap_cmd, gap, sizeof(gap));

   int behind = 0, ahead = 0;
   if (gap[0] && sscanf(gap, "%d %d", &behind, &ahead) == 2 && behind == 0)
      return mcp_textf("%s is already current with %s (%d commit(s) ahead, 0 behind)", branch, base,
                       ahead);

   cJSON_DeleteItemFromObjectCaseSensitive(args, "base");
   cJSON_AddStringToObject(args, "base", base);
   cJSON_DeleteItemFromObjectCaseSensitive(args, "ref");
   cJSON_AddStringToObject(args, "ref", base);

   cJSON *resp = integrate_dispatch(strcmp(mode, "merge") == 0 ? "merge" : "rebase", args);

   /* Add the gap that was closed — but NEVER in front of a failure. Every caller
    * reads failure off the leading "error"/"conflict" word (mcp_git_response_failed),
    * so prefixing context to a conflicted sync would make it look like it worked.
    * On success the context leads; on failure it trails. */
   cJSON *item = resp ? cJSON_GetArrayItem(resp, 0) : NULL;
   cJSON *text = item ? cJSON_GetObjectItem(item, "text") : NULL;
   if (cJSON_IsString(text))
   {
      char merged[INTEGRATE_OUT_MAX];
      if (mcp_git_response_failed(resp))
         snprintf(merged, sizeof(merged), "%s\n\n(sync %s <- %s: %d behind, %d ahead, via %s)",
                  text->valuestring, branch, base, behind, ahead, mode);
      else
         snprintf(merged, sizeof(merged), "sync %s <- %s (%d behind, %d ahead) via %s\n%s", branch,
                  base, behind, ahead, mode, text->valuestring);
      cJSON_ReplaceItemInObject(item, "text", cJSON_CreateString(merged));
   }
   return resp;
}
