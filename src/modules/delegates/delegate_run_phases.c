/* delegate_run_phases.c: self-contained phases lifted out of delegate_worker
 * (server_compute.c) to keep that file under the line cap. Verbatim moves; none
 * touch the worker's thread-local run state (delegation depth/parent, mailbox,
 * session override) — those stay in server_compute.c's delegate_run_ctx.
 *
 *   delegate_file_snapshot / _changed - mtime+size snapshot of a named path
 *   delegate_record_exit_learning     - persist post-run learning + IE event
 *   delegate_detect_noop_write        - flag a write delegate that changed nothing
 */
#include "delegate_run_phases.h"

#include "aimee.h"
#include "agent.h"
#include "agent_config.h"
#include "cmd_agent_delegate_impl.h"
#include "db1/delegate_learning.h"
#include "db1/interaction_events.h"
#include "guardrails.h" /* git_repo_root, is_aimee_worktree_path */
#include "log.h"
#include "util.h"      /* run_cmd, run_cmd_set_cwd */
#include "workspace.h" /* worktree_* helpers */
#include "cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static long delegate_stat_mtime_nsec(const struct stat *st)
{
#if defined(_WIN32)
   (void)st;
   return 0;
#elif defined(__APPLE__) || defined(__MACH__)
   return st->st_mtimespec.tv_nsec;
#else
   return st->st_mtim.tv_nsec;
#endif
}

delegate_file_snapshot_t delegate_file_snapshot(const char *path)
{
   delegate_file_snapshot_t snap;
   memset(&snap, 0, sizeof(snap));
   struct stat st;
   if (path && stat(path, &st) == 0)
   {
      snap.exists = 1;
      snap.mtime_sec = st.st_mtime;
      snap.mtime_nsec = delegate_stat_mtime_nsec(&st);
      snap.size = (long long)st.st_size;
   }
   return snap;
}

static int delegate_file_snapshot_changed(delegate_file_snapshot_t before,
                                          delegate_file_snapshot_t after)
{
   return before.exists != after.exists || before.mtime_sec != after.mtime_sec ||
          before.mtime_nsec != after.mtime_nsec || before.size != after.size;
}

/* Classify a finished delegate run and persist what we learned from it: a
 * delegate_learning row (failure mode / lesson / evidence, tagged with the
 * event agent's provider+model) and a structured IE_DELEGATE_EXIT interaction
 * event. Pure post-run side effect — does not alter rc/result. Extracted
 * verbatim from delegate_worker. */
void delegate_record_exit_learning(const char *sid, const char *role, const agent_result_t *result,
                                   int rc, int max_turns, agent_config_t *acfg,
                                   agent_t *target_agent)
{
   dl_exit_metrics_t dlm;
   memset(&dlm, 0, sizeof(dlm));
   dlm.session_id = sid;
   dlm.role = role;
   dlm.turns = result->turns;
   dlm.tool_calls = result->tool_calls;
   dlm.success = (rc == 0);
   dlm.error = result->error[0] ? result->error : NULL;
   dlm.max_turns_limit = max_turns > 0 ? max_turns : 0; /* 0 = unlimited (default -1) */
   dlm.write_enforce_fired = dlm.error && strstr(dlm.error, "write_enforce") ? 1 : 0;
   dlm.had_writes = dlm.write_enforce_fired ? (strstr(dlm.error, "no Write or Edit") ? 0 : 1)
                                            : (rc == 0 ? 1 : 0);
   dl_classification_t dlc;
   classify_delegate_exit(&dlm, &dlc);
   agent_t *event_agent =
       result->agent_name[0] ? agent_find(acfg, result->agent_name) : target_agent;
   cJSON *evidence = cJSON_Parse(dlc.evidence);
   char *evidence_json = NULL;
   if (cJSON_IsObject(evidence))
   {
      cJSON_AddStringToObject(evidence, "provider", event_agent ? event_agent->provider : "");
      cJSON_AddStringToObject(evidence, "model", event_agent ? event_agent->model : "");
      evidence_json = cJSON_PrintUnformatted(evidence);
   }
   delegate_learning_record(sid, role, dlc.failure_mode, dlc.lesson,
                            evidence_json ? evidence_json : dlc.evidence, dlc.confidence);
   free(evidence_json);
   cJSON_Delete(evidence);

   cJSON *payload = cJSON_CreateObject();
   if (payload)
   {
      cJSON_AddStringToObject(payload, "role", role ? role : "");
      cJSON_AddStringToObject(payload, "failure_mode", dl_failure_mode_to_string(dlc.failure_mode));
      cJSON_AddNumberToObject(payload, "turns", result->turns);
      cJSON_AddNumberToObject(payload, "tool_calls", result->tool_calls);
      cJSON_AddNumberToObject(payload, "confidence", dlc.confidence);
      cJSON_AddStringToObject(payload, "agent", result->agent_name);
      cJSON_AddStringToObject(payload, "provider", event_agent ? event_agent->provider : "");
      cJSON_AddStringToObject(payload, "model", event_agent ? event_agent->model : "");
      char *payload_json = cJSON_PrintUnformatted(payload);
      (void)ie_record(sid, IE_DELEGATE_EXIT, "system", payload_json ? payload_json : "{}",
                      rc == 0 ? "ok" : "error");
      free(payload_json);
      cJSON_Delete(payload);
   }
}

/* Detect a no-op write delegate: a write role that reported success (rc==0) but
 * left its owned files / the worktree / HEAD unchanged. Returns 1 and fills
 * `err` with the "treated as incomplete" message when the run is a no-op; 0
 * otherwise (logging an INFO note for the benign cases — named paths that look
 * unchanged but HEAD/worktree advanced, or a missing HEAD snapshot). Extracted
 * verbatim from delegate_worker; the caller maps a 1 to rc=-1 + result.error. */
int delegate_detect_noop_write(int is_write_role, int allows_writes, int handoff_json, int rc,
                               const char named_paths[][DELEGATE_DRIFT_PATH_MAX],
                               int named_path_count, const delegate_file_snapshot_t *pre_run_files,
                               const char *pre_run_head_sha, const char *worktree_path,
                               const char *cwd, const char *deleg_id, const char *sid,
                               const char *role, char *err, size_t errsz)
{
   /* Treat unchanged named paths as incomplete unless the worktree or HEAD changed. */
   if (is_write_role && allows_writes && named_path_count > 0 && rc == 0)
   {
      const char *check_root = worktree_path[0] ? worktree_path : (cwd[0] ? cwd : ".");
      int any_changed = 0;
      for (int i = 0; i < named_path_count; i++)
      {
         char full[MAX_PATH_LEN];
         if (named_paths[i][0] == '/')
            snprintf(full, sizeof(full), "%s", named_paths[i]);
         else
            snprintf(full, sizeof(full), "%s/%s", check_root, named_paths[i]);
         if (delegate_file_snapshot_changed(pre_run_files[i], delegate_file_snapshot(full)))
         {
            any_changed = 1;
            break;
         }
      }
      if (!any_changed)
      {
         /* Named paths were stable; check for any worktree or HEAD changes. */
         int wt_changed = delegate_worktree_has_changes(worktree_path[0] ? worktree_path
                                                                         : (cwd[0] ? cwd : NULL));
         /* A clean worktree can still be real progress if HEAD advanced. */
         int head_advanced = 0;
         if (!wt_changed && pre_run_head_sha[0])
         {
            const char *root = worktree_path[0] ? worktree_path : (cwd[0] ? cwd : NULL);
            head_advanced = delegate_head_changed(root, pre_run_head_sha);
         }
         if (wt_changed || head_advanced)
         {
            aimee_log(LOG_INFO, "delegate",
                      "delegate %s: named paths unchanged but %s — "
                      "named-path heuristic likely matched context refs (session %s)",
                      deleg_id,
                      head_advanced ? "HEAD advanced (delegate committed)"
                                    : "worktree has other changes",
                      sid ? sid : "unknown");
         }
         else
         {
            aimee_log(LOG_WARN, "delegate",
                      "delegate %s: no-op — role '%s' reported success but no owned files changed "
                      "(session %s)",
                      deleg_id, role, sid ? sid : "unknown");
            snprintf(err, errsz,
                     "delegate %s: no owned files changed; result treated as incomplete", role);
            return 1;
         }
      }
   }
   else if (is_write_role && allows_writes && named_path_count == 0 && !handoff_json && rc == 0)
   {
      const char *check_root = worktree_path[0] ? worktree_path : (cwd[0] ? cwd : NULL);
      int wt_changed = delegate_worktree_has_changes(check_root);
      int head_advanced = 0;
      if (!wt_changed && pre_run_head_sha[0] && check_root)
         head_advanced = delegate_head_changed(check_root, pre_run_head_sha);
      if (worktree_path[0] && !pre_run_head_sha[0])
      {
         aimee_log(LOG_INFO, "delegate",
                   "delegate %s: skipping global no-op check because delegate HEAD could not be "
                   "snapshotted (session %s)",
                   deleg_id, sid ? sid : "unknown");
      }
      else if (!wt_changed && !head_advanced)
      {
         aimee_log(LOG_WARN, "delegate",
                   "delegate %s: no-op — write role '%s' reported success but produced no diff "
                   "(session %s)",
                   deleg_id, role, sid ? sid : "unknown");
         snprintf(err, errsz, "delegate %s: no file changes detected; result treated as incomplete",
                  role);
         return 1;
      }
   }
   return 0;
}

/* --- Delegate worktree resolution (moved from server_compute.c) --- */
static int delegate_worktree_exclude_local_entry(const char *wt_path, const char *entry)
{
   if (!wt_path || !wt_path[0] || !entry || !entry[0])
      return -1;
   char cmd[MAX_PATH_LEN + 128];
   snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --git-path info/exclude 2>/dev/null", wt_path);
   int rc = 0;
   char *exclude_path = run_cmd(cmd, &rc);
   if (rc != 0 || !exclude_path || !exclude_path[0])
   {
      free(exclude_path);
      return -1;
   }
   size_t n = strlen(exclude_path);
   while (n > 0 && (exclude_path[n - 1] == '\n' || exclude_path[n - 1] == '\r' ||
                    exclude_path[n - 1] == ' ' || exclude_path[n - 1] == '\t'))
      exclude_path[--n] = '\0';
   if (!exclude_path[0])
   {
      free(exclude_path);
      return -1;
   }
   FILE *f = fopen(exclude_path, "a");
   int ok = -1;
   if (f)
   {
      fprintf(f, "\n%s\n", entry);
      fclose(f);
      ok = 0;
   }
   free(exclude_path);
   return ok;
}
static void delegate_worktree_link_parent_aimee(const char *anchor, const char *git_root,
                                                const char *wt_path)
{
   if (!wt_path || !wt_path[0])
      return;
   char src[MAX_PATH_LEN] = "";
   char candidate[MAX_PATH_LEN];
   if (anchor && anchor[0] == '/')
   {
      snprintf(candidate, sizeof(candidate), "%s/aimee", anchor);
      if (access(candidate, X_OK) == 0)
         snprintf(src, sizeof(src), "%s", candidate);
   }
   if (!src[0] && git_root && git_root[0] == '/')
   {
      snprintf(candidate, sizeof(candidate), "%s/aimee", git_root);
      if (access(candidate, X_OK) == 0)
         snprintf(src, sizeof(src), "%s", candidate);
   }
   if (!src[0])
      return;
   char dst[MAX_PATH_LEN];
   snprintf(dst, sizeof(dst), "%s/aimee", wt_path);
   struct stat st;
   if (lstat(dst, &st) == 0 || errno == EEXIST)
      return;
   if (delegate_worktree_exclude_local_entry(wt_path, "/aimee") != 0)
      return;

   if (symlink(src, dst) != 0 && errno != EEXIST)
      aimee_log(LOG_WARN, "delegate", "could not link %s into delegate worktree %s: %s", src,
                wt_path, strerror(errno));
}

void delegate_resolve_worktree(const char *cwd, const char *deleg_id, const char *branch,
                               int allows_writes, int needs_worktree, delegate_worktree_t *out)
{
   memset(out, 0, sizeof(*out));

   const char *anchor = (cwd[0] == '/') ? cwd : NULL;
   char proc_cwd[MAX_PATH_LEN];
   if (!anchor && getcwd(proc_cwd, sizeof(proc_cwd)))
      anchor = proc_cwd;

   if (!anchor || git_repo_root(anchor, out->git_root, sizeof(out->git_root)) != 0)
   {
      out->git_root[0] = '\0';
      return;
   }

   if (allows_writes && !needs_worktree)
   {
      /* Foreground sole-writer write delegate: share the parent worktree.
       * Point worktree_path at the parent worktree root (so the write guard's
       * write_root covers it) and clear git_root so the apply-back and
       * sibling-cleanup paths — which key off git_root — are skipped, exactly
       * like the shared fallback below. The delegate edits the parent's live
       * tree directly. */
      char top_cmd[MAX_PATH_LEN + 64];
      snprintf(top_cmd, sizeof(top_cmd), "git -C '%s' rev-parse --show-toplevel 2>/dev/null",
               anchor);
      int top_rc = 0;
      char *top_out = run_cmd(top_cmd, &top_rc);
      if (top_rc == 0 && top_out && top_out[0])
      {
         top_out[strcspn(top_out, "\r\n")] = '\0';
         snprintf(out->worktree_path, sizeof(out->worktree_path), "%s", top_out);
      }
      else
      {
         snprintf(out->worktree_path, sizeof(out->worktree_path), "%s", anchor);
      }
      free(top_out);
      out->git_root[0] = '\0';
      out->shared = 1;
      aimee_log(LOG_INFO, "delegate", "foreground write delegate sharing parent worktree %s",
                out->worktree_path);
   }
   if (needs_worktree && is_aimee_worktree_path(anchor))
   {
      char managed_root[MAX_PATH_LEN];
      if (worktree_managed_git_root(anchor, managed_root, sizeof(managed_root)) == 0)
      {
         snprintf(out->git_root, sizeof(out->git_root), "%s", managed_root);
      }
      else
      {
         /* An aimee worktree with no managed git-root marker is a WFE per-slice
          * tree ($AIMEE_HOME/wfe-worktrees/wi_<id>): the delegate OWNS it
          * exclusively (one implement delegate per slice tree, nothing else looks
          * at it). It is NOT a shared session tree, so it must be mounted
          * read-WRITE in a container and carry no host write guard — mark it
          * DEDICATED. git_root stays empty: the engine reads the delegate's tree
          * directly, so apply-back/sibling-cleanup are skipped exactly as for the
          * shared case. */
         char wt_cmd[MAX_PATH_LEN + 64];
         snprintf(wt_cmd, sizeof(wt_cmd), "git -C '%s' rev-parse --show-toplevel 2>/dev/null",
                  anchor);
         int wt_rc = 0;
         char *wt_out = run_cmd(wt_cmd, &wt_rc);
         if (wt_rc == 0 && wt_out && wt_out[0])
         {
            wt_out[strcspn(wt_out, "\r\n")] = '\0';
            snprintf(out->worktree_path, sizeof(out->worktree_path), "%s", wt_out);
            out->git_root[0] = '\0';
            out->dedicated = 1;
            aimee_log(LOG_INFO, "delegate", "delegate owns dedicated worktree %s (read-write)",
                      out->worktree_path);
         }
         free(wt_out);
      }
   }
   if (needs_worktree && !out->shared && out->git_root[0])
   {
      out->attempted = 1;
      /* Deterministic work-name from the delegate id so this compute path and
       * the dispatch path (cmd_agent_delegate.c) resolve to the SAME sibling
       * worktree — whichever runs second reuses the first's worktree instead of
       * spawning a second, divergent one. */
      worktree_delegate_work_name(deleg_id, out->work_name, sizeof(out->work_name));

      if (worktree_create_sibling_on_branch(out->git_root, deleg_id, out->work_name, branch,
                                            anchor) == 0)
      {
         worktree_sibling_path(out->git_root, deleg_id, out->work_name, out->worktree_path,
                               sizeof(out->worktree_path));

         if (out->worktree_path[0])
         {
            delegate_worktree_link_parent_aimee(anchor, out->git_root, out->worktree_path);
            run_cmd_set_cwd(out->worktree_path);
         }
      }
      else
      {
         aimee_log(LOG_WARN, "delegate", "could not create worktree under %s", out->git_root);
         out->git_root[0] = '\0';
      }
   }
}
