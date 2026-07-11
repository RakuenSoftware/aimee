/* wfe_blocks.c: real non-gate block executors.
 *
 * freeze is fully implemented + unit-tested (real git). The delegate-driven
 * (author/implement) and forge (pr.open/merge) executors construct + run real
 * commands via safe_exec_capture; their success requires a configured delegate
 * / gh and a working repo, so they are exercised by integration, not the unit
 * suite (the engine test overrides them with mocks via the vtable). */
#include "wfe_blocks.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "aimee_home.h"
#include "config.h" /* config_autonomy_lookup: live autonomy.* (snapshot) instead of setenv */
#include "cJSON.h"
#include "util.h"
#include "wfe_deliver.h" /* gate.deliver verdict-graph re-verify (Q4) */
#include "wfe_def.h"
#include "wfe_engine.h"
#include "wfe_iface.h"
#include "wfe_manager_artifacts.h" /* typed intent/packet/verdict schema validators */
#include "wfe_store.h"             /* db1_work_item_set_worktree — persist the per-item worktree */

/* Resolve the local working repo for a work item: $AIMEE_WORKFLOW_REPO or cwd. */
static const char *repo_dir(void)
{
   const char *d = getenv("AIMEE_WORKFLOW_REPO");
   return (d && d[0]) ? d : ".";
}

/* The directory a producing block acts in: the per-work-item worktree (F2),
 * created on first use, or the shared repo dir if isolation is unavailable. Filled
 * into `buf`; never empty. */
static void resolve_workdir(wfe_ctx *ctx, char *buf, size_t n)
{
   if (wfe_worktree_ensure(wfe_ctx_work_item(ctx), wfe_ctx_worktree(ctx), repo_dir(),
                           wfe_autonomous_base(), buf, n) != 0)
      snprintf(buf, n, "%s", repo_dir()); /* fall back to the shared checkout */
   if (!buf[0])                           /* guarantee a non-empty workdir for every caller */
      snprintf(buf, n, "%s", repo_dir());
}

static int git_capture(const char *const argv[], char **out)
{
   *out = NULL;
   return safe_exec_capture(argv, out, 1 << 20);
}

static void chomp(char *s)
{
   if (!s)
      return;
   size_t n = strlen(s);
   while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
      s[--n] = '\0';
}

int wfe_git_freeze(const char *repo_dir_in, const char *base_branch, char out_base_sha[64],
                   char out_head_sha[64], char out_diff_hash[65], char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   const char *dir = (repo_dir_in && repo_dir_in[0]) ? repo_dir_in : ".";
   const char *base = (base_branch && base_branch[0]) ? base_branch : "HEAD";

   /* head sha */
   {
      const char *argv[] = {"git", "-C", dir, "rev-parse", "HEAD", NULL};
      char *o = NULL;
      if (git_capture(argv, &o) != 0 || !o)
      {
         snprintf(err, errlen, "git rev-parse HEAD failed");
         free(o);
         return -1;
      }
      chomp(o);
      snprintf(out_head_sha, 64, "%s", o);
      free(o);
   }
   /* base sha = merge-base(HEAD, base_branch); fall back to HEAD if base==HEAD */
   {
      const char *argv[] = {"git", "-C", dir, "merge-base", "HEAD", base, NULL};
      char *o = NULL;
      if (git_capture(argv, &o) == 0 && o && o[0])
      {
         chomp(o);
         snprintf(out_base_sha, 64, "%s", o);
      }
      else
      {
         snprintf(out_base_sha, 64, "%s", out_head_sha);
      }
      free(o);
   }
   /* cumulative diff base..head, hashed */
   {
      char range[140];
      snprintf(range, sizeof range, "%s..%s", out_base_sha, out_head_sha);
      const char *argv[] = {"git", "-C", dir, "diff", range, NULL};
      char *o = NULL;
      if (git_capture(argv, &o) != 0)
      {
         snprintf(err, errlen, "git diff failed");
         free(o);
         return -1;
      }
      wfe_sha256_hex(o ? o : "", o ? strlen(o) : 0, out_diff_hash);
      free(o);
   }
   return 0;
}

/* ---- per-work-item git worktree isolation (F2) ---- */

/* Ensure a per-work-item git worktree exists; fill out_path with it. If `existing`
 * is already set (created on an earlier step) it is returned as-is. Otherwise a
 * locked worktree aimee/wi/<id> is created off `base` in `repo_local` and the path
 * is persisted on the work item. Returns 0 on success (out_path filled), -1 on any
 * failure — the caller then falls back to the shared repo dir, so a worktree
 * problem degrades to today's shared-checkout behaviour rather than crashing.
 * `git worktree lock` keeps worktree-GC from pruning an active run (Q5). */
/* True if a directory exists at `p`. */
static int is_dir(const char *p)
{
   struct stat st;
   return p && p[0] && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* snprintf that returns -1 on truncation (a corrupt path must never reach git). */
static int sn(char *buf, size_t cap, const char *fmt, const char *a)
{
   int r = snprintf(buf, cap, fmt, a);
   return (r >= 0 && (size_t)r < cap) ? 0 : -1;
}

/* Best-effort teardown of a partial/unrecorded worktree + its branch (ignore rc). */
static void wt_scrub(const char *rl, const char *path, const char *branch)
{
   char *o = NULL;
   const char *rm[] = {"git", "-C", rl, "worktree", "remove", "--force", path, NULL};
   safe_exec_capture(rm, &o, 1 << 14);
   free(o);
   o = NULL;
   const char *rmd[] = {"rm", "-rf", path, NULL};
   safe_exec_capture(rmd, &o, 1 << 14);
   free(o);
   o = NULL;
   const char *bd[] = {"git", "-C", rl, "branch", "-D", branch, NULL};
   safe_exec_capture(bd, &o, 1 << 14);
   free(o);
}

/* A positive long from `name`, else `def` (rejects junk/non-positive/overflow so a
 * malformed override can't silently disable the guardrail). */
static long wt_env_long(const char *name, long def)
{
   const char *v = getenv(name);
   if (!v || !v[0])
      return def;
   errno = 0;
   char *end = NULL;
   long n = strtol(v, &end, 10);
   if (errno == ERANGE || !end || *end != '\0' || n <= 0)
      return def;
   return n;
}

/* Count the worktree directories currently under `parent` (best-effort; a
 * missing/unreadable dir is 0). Lock files and non-dirs are ignored. */
static int wt_dir_count(const char *parent)
{
   DIR *dp = opendir(parent);
   if (!dp)
      return 0;
   int n = 0;
   struct dirent *e;
   while ((e = readdir(dp)) != NULL)
   {
      if (e->d_name[0] == '.')
         continue;
      char path[1024];
      struct stat st;
      if (snprintf(path, sizeof path, "%s/%s", parent, e->d_name) < (int)sizeof path &&
          stat(path, &st) == 0 && S_ISDIR(st.st_mode))
         n++;
   }
   closedir(dp);
   return n;
}

int wfe_worktree_ensure(const char *work_item_id, const char *existing, const char *repo_local,
                        const char *base, char *out_path, size_t n)
{
   if (!out_path || n == 0)
      return -1;
   out_path[0] = '\0';
   if (!work_item_id || !work_item_id[0])
      return -1;
   const char *rl = (repo_local && repo_local[0]) ? repo_local : ".";

   /* Reuse a recorded worktree ONLY if it still exists on disk; a pruned / failed-
    * cleanup path is dropped so we recreate rather than hand a delegate a vanished
    * CWD. */
   if (existing && existing[0])
   {
      if (is_dir(existing))
      {
         snprintf(out_path, n, "%s", existing);
         return 0;
      }
      db1_work_item_set_worktree(work_item_id, ""); /* stale -> clear, recreate below */
   }

   const char *home = aimee_home();
   if (!home || !home[0])
      return -1;
   char parent[768], path[1000], branch[200], lockf[1024];
   if (sn(parent, sizeof parent, "%s/wfe-worktrees", home) != 0 ||
       snprintf(path, sizeof path, "%s/%s", parent, work_item_id) >= (int)sizeof path ||
       sn(branch, sizeof branch, "aimee/wi/%s", work_item_id) != 0 ||
       snprintf(lockf, sizeof lockf, "%s/%s.lock", parent, work_item_id) >= (int)sizeof lockf)
      return -1;
   const char *b = (base && base[0]) ? base : "HEAD";

   char *o = NULL;
   const char *mk[] = {"mkdir", "-p", parent, NULL};
   if (safe_exec_capture(mk, &o, 1 << 14) != 0)
   {
      free(o);
      return -1;
   }
   free(o);

   /* Per-work-item serialization: an flock so two concurrent producers for the
    * same item can't both `git worktree add -b aimee/wi/<id>` (branch collision). */
   int lfd = open(lockf, O_CREAT | O_RDWR, 0600);
   if (lfd >= 0)
      (void)flock(lfd, LOCK_EX);
   int rc_final = -1;

   /* Re-check under the lock: another producer may have created it meanwhile. */
   db1_work_item_t wi;
   if (db1_work_item_get(work_item_id, &wi) == 1 && wi.worktree[0] && is_dir(wi.worktree))
   {
      snprintf(out_path, n, "%s", wi.worktree);
      rc_final = 0;
      goto unlock;
   }

   /* Inode guardrail: bound how many worktrees can exist at once so a runaway (or a
    * pile of orphaned checkouts from crashed runs) can't exhaust the filesystem's
    * inodes. Reclaim orphans first; only if still at the cap do we fail-closed here
    * — the caller then degrades to the shared checkout rather than adding an
    * unbounded worktree. AIMEE_WFE_WORKTREE_MAX overrides the default. */
   long cap = wt_env_long("AIMEE_WFE_WORKTREE_MAX", 32);
   if (wt_dir_count(parent) >= cap)
   {
      wfe_worktree_orphan_gc(rl, 0);
      if (wt_dir_count(parent) >= cap)
         goto unlock; /* rc_final stays -1 -> shared-checkout fallback */
   }

   o = NULL;
   const char *add[] = {"git", "-C", rl, "worktree", "add", "--lock", "-b", branch, path, b, NULL};
   int rc = safe_exec_capture(add, &o, 1 << 16);
   free(o);
   if (rc != 0)
   {
      wt_scrub(rl, path, branch); /* git may leave a partial worktree/branch */
      goto unlock;
   }
   if (db1_work_item_set_worktree(work_item_id, path) != 0)
   {
      wt_scrub(rl, path, branch); /* don't leave an UNRECORDED worktree+branch */
      goto unlock;
   }
   snprintf(out_path, n, "%s", path);
   rc_final = 0;
unlock:
   if (lfd >= 0)
   {
      flock(lfd, LOCK_UN);
      close(lfd);
   }
   return rc_final;
}

/* Tear down a per-work-item worktree (terminal cleanup): unlock then force-remove.
 * Best-effort; a missing/already-removed worktree is fine. */
int wfe_worktree_cleanup(const char *worktree, const char *repo_local)
{
   if (!worktree || !worktree[0])
      return 0;
   const char *rl = (repo_local && repo_local[0]) ? repo_local : ".";
   char *o = NULL;
   const char *unlock[] = {"git", "-C", rl, "worktree", "unlock", worktree, NULL};
   safe_exec_capture(unlock, &o, 1 << 14);
   free(o);
   o = NULL;
   const char *rm[] = {"git", "-C", rl, "worktree", "remove", "--force", worktree, NULL};
   int rc = safe_exec_capture(rm, &o, 1 << 14);
   free(o);
   return rc == 0 ? 0 : -1;
}

/* True for an immutable-terminal work-item state (mirrors the scheduler sweep's
 * set): such an item's worktree should already be gone, so a lingering one is
 * reapable rather than in-use. */
static int wt_state_terminal(const char *st)
{
   return st && (!strcmp(st, "accepted") || !strcmp(st, "rejected") || !strcmp(st, "abandoned"));
}

int wfe_worktree_orphan_gc(const char *repo_local, long grace_secs)
{
   const char *home = aimee_home();
   if (!home || !home[0])
      return 0;
   char parent[768];
   if (sn(parent, sizeof parent, "%s/wfe-worktrees", home) != 0)
      return 0;
   DIR *dp = opendir(parent);
   if (!dp)
      return 0; /* no worktrees dir yet -> nothing to reap */
   const char *rl = (repo_local && repo_local[0]) ? repo_local : ".";
   time_t now = time(NULL);
   int reaped = 0;
   struct dirent *e;
   while ((e = readdir(dp)) != NULL)
   {
      if (e->d_name[0] == '.')
         continue;
      size_t nl = strlen(e->d_name);
      if (nl >= 5 && strcmp(e->d_name + nl - 5, ".lock") == 0)
         continue; /* a lock is reaped alongside its worktree, below */
      char path[1024];
      if (snprintf(path, sizeof path, "%s/%s", parent, e->d_name) >= (int)sizeof path)
         continue;
      struct stat stt;
      if (stat(path, &stt) != 0 || !S_ISDIR(stt.st_mode))
         continue; /* only directories are worktrees */

      /* Keep a worktree that a LIVE (non-terminal) work item still owns — an
       * active/parked item needs it, and the terminal-sweep handles terminal ones.
       * This GC only fills the gap those miss: a vanished row, or a terminal row
       * whose worktree was left behind. */
      db1_work_item_t wi;
      int have = db1_work_item_get(e->d_name, &wi);
      if (have == 1 && !wt_state_terminal(wi.state))
         continue;

      /* Age gate: a worktree dir exists before its row/column is written, so never
       * reap one younger than the grace window (grace_secs <= 0 = reap now). */
      if (grace_secs > 0 && now - stt.st_mtime < grace_secs)
         continue;

      char branch[220];
      if (sn(branch, sizeof branch, "aimee/wi/%s", e->d_name) != 0)
         continue;
      wt_scrub(rl, path, branch); /* force-remove worktree + rm -rf + delete branch */
      char lockf[1100];
      if (snprintf(lockf, sizeof lockf, "%s.lock", path) < (int)sizeof lockf)
         unlink(lockf);
      if (have == 1)
         db1_work_item_set_worktree(e->d_name, ""); /* clear a stale terminal column */
      reaped++;
   }
   closedir(dp);
   if (reaped > 0)
   {
      /* Drop git's admin refs (.git/worktrees/<id>) for the dirs we removed. */
      char *o = NULL;
      const char *prune[] = {"git", "-C", rl, "worktree", "prune", NULL};
      safe_exec_capture(prune, &o, 1 << 14);
      free(o);
   }
   return reaped;
}

/* ---- forge seam (gate.ci / check.mergeable / merge) ---- */

/* Default live provider would call `gh`, but PR-identity threading is
 * integration-gated, so until that lands it fails closed (unknown -> park,
 * merge unavailable). Tests inject a mock to exercise the state mapping. */
static wfe_ci_status_t live_ci_status(const char *repo, const char *pr)
{
   (void)repo;
   (void)pr;
   return WFE_CI_NONE;
}
static int live_mergeable(const char *repo, const char *pr)
{
   (void)repo;
   (void)pr;
   return -1;
}
static int live_is_merged(const char *repo, const char *pr)
{
   (void)repo;
   (void)pr;
   return -1;
}
static wfe_merge_result_t live_merge(const char *repo, const char *pr)
{
   (void)repo;
   (void)pr;
   return WFE_MERGE_ERROR;
}
/* Default forge open: integration-gated (the live provider — git push + gh pr
 * create — is registered by the server). Fail closed here so pr.open re-loops. */
static int live_open(const char *repo, const char *branch, const char *base, const char *title,
                     const char *body, char out_pr_ref[128])
{
   (void)repo;
   (void)branch;
   (void)base;
   (void)title;
   (void)body;
   if (out_pr_ref)
      out_pr_ref[0] = '\0';
   return -1;
}
static const wfe_forge_t LIVE_FORGE = {
    live_ci_status, live_mergeable, live_is_merged,
    live_merge,     live_open,      NULL /* publish_base: live provider registers it */};
static const wfe_forge_t *g_forge = &LIVE_FORGE;

void wfe_set_forge_provider(const wfe_forge_t *p)
{
   g_forge = p ? p : &LIVE_FORGE;
}

/* Delegate seam (see wfe_blocks.h). Default NULL: producing blocks fail closed
 * until the server registers a live provider, preserving the integration-gated
 * behavior the stubs had before. */
static const wfe_delegate_provider_t *g_delegate = NULL;

void wfe_set_delegate_provider(const wfe_delegate_provider_t *p)
{
   g_delegate = p;
}

/* Verify seam (see wfe_blocks.h). Default NULL: implement does not gate on
 * verification (pre-WP-1b behavior). */
static const wfe_verify_provider_t *g_verify = NULL;

void wfe_set_verify_provider(const wfe_verify_provider_t *p)
{
   g_verify = p;
}

static const wfe_judge_provider_t *g_judge = NULL;

void wfe_set_judge_provider(const wfe_judge_provider_t *p)
{
   g_judge = p;
}

/* Run one judgment under `lens`; returns 1 if it REFUTED the change (incl. an
 * unrunnable judge / unparseable verdict -> fail closed = refuted). */
static int wfe_judge_refuted(const char *workdir, const char *lens)
{
   if (!g_judge || !g_judge->judge)
      return 1; /* no provider -> fail closed */
   char verdict[4096] = "";
   if (g_judge->judge(workdir, lens, verdict, sizeof verdict) != 0)
      return 1; /* could not run -> refuted */
   cJSON *doc = cJSON_Parse(verdict);
   if (!doc)
      return 1; /* unparseable -> refuted */
   const cJSON *rf = cJSON_GetObjectItemCaseSensitive(doc, "refuted");
   /* Default to REFUTED unless the verdict carries an explicit JSON BOOLEAN false.
    * A missing field, a numeric 0, a string, or any non-boolean is a schema-invalid
    * verdict -> fail closed (refuted). */
   int refuted = !(rf && cJSON_IsFalse(rf));
   cJSON_Delete(doc);
   return refuted;
}

int wfe_implement_adversarial_ok(const char *workdir)
{
   /* LIVE value (operator env override > config snapshot); config-backed so a config.set on
    * autonomy.skeptics applies without a restart. */
   long k = 0;
   long lv;
   if (config_autonomy_lookup("AIMEE_AUTONOMY_SKEPTICS", &lv) && lv >= 0)
      k = lv;
   if (k <= 0)
      return 1; /* tier OFF (default) -> unchanged behavior */
   if (!g_judge || !g_judge->judge)
      return 0; /* tier ON but no judge -> fail closed */
   /* Review lens: a refute blocks. */
   if (wfe_judge_refuted(workdir, "reviewer"))
      return 0;
   /* N skeptics (each prompted to refute); accept only when FEWER THAN HALF refute —
    * an exact tie (even K) REJECTS (bias toward safety). refutes*2 >= k <=> refutes >=
    * ceil(K/2) with the tie counted as a reject. */
   int refutes = 0;
   for (long i = 0; i < k; i++)
      if (wfe_judge_refuted(workdir, "skeptic"))
         refutes++;
   return ((long)refutes * 2 >= k) ? 0 : 1;
}

/* Run the mechanical verify gate on the implemented worktree. Returns 1 to ADVANCE
 * (only when the top-level verdict is an explicit "passed"); 0 to BLOCK in every
 * other case — FAIL CLOSED. Blocking cases: no provider installed (a missing safety
 * gate must never let unverified work advance), the gate could not run, an
 * unparseable verdict, or a verdict other than "passed". We parse the git_verify
 * format=json document and read its TOP-LEVEL "verdict" field, so a nested or echoed
 * verdict token cannot spoof the gate. Exposed (non-static) for the unit test. */
int wfe_implement_verify_ok(const char *workdir)
{
   if (!g_verify || !g_verify->verify)
      return 0; /* no gate -> fail closed (unverified work never advances) */
   char verdict[4096] = "";
   if (g_verify->verify(workdir, verdict, sizeof verdict) != 0)
      return 0; /* gate could not run -> fail closed */
   cJSON *doc = cJSON_Parse(verdict);
   if (!doc)
      return 0; /* unparseable -> fail closed */
   const cJSON *vd = cJSON_GetObjectItemCaseSensitive(doc, "verdict");
   int passed = cJSON_IsString(vd) && vd->valuestring && strcmp(vd->valuestring, "passed") == 0;
   cJSON_Delete(doc);
   return passed ? 1 : 0;
}

/* The step's assigned delegate from node params ("delegate"), or "" for none.
 * May be the sentinel "$random" — the provider resolves it to a random agent. */
static const char *node_delegate(const wfe_node_t *node)
{
   if (!node || !node->params)
      return "";
   const cJSON *d = cJSON_GetObjectItemCaseSensitive(node->params, "delegate");
   return (d && cJSON_IsString(d) && d->valuestring) ? d->valuestring : "";
}

/* Dispatch one block's delegate work, if a provider is installed. `delegate` is
 * the step's assigned agent (or "$random", or "" to route by role). out_cost (may
 * be NULL) receives the server-side wall-clock USD estimate for the turn (WP-5
 * budget). Returns:
 *   1  provider ran and succeeded,
 *   0  no provider installed (caller falls back to its fail-closed path),
 *  -1  provider ran and failed (caller should loop/retry). */
static int wfe_delegate_dispatch(const char *workdir, const char *role, const char *delegate,
                                 const char *prompt, const char *artifact_path,
                                 char out_commit_sha[64], double *out_cost)
{
   if (out_commit_sha)
      out_commit_sha[0] = '\0';
   if (out_cost)
      *out_cost = 0.0;
   if (!g_delegate || !g_delegate->run)
      return 0;
   const char *wd = (workdir && workdir[0]) ? workdir : repo_dir();
   char err[256] = "";
   struct timespec t0 = {0, 0}, t1 = {0, 0};
   int ok0 = clock_gettime(CLOCK_MONOTONIC, &t0) == 0;
   int rc = g_delegate->run(wd, role, delegate ? delegate : "", prompt, artifact_path,
                            out_commit_sha, err, sizeof err);
   int ok1 = clock_gettime(CLOCK_MONOTONIC, &t1) == 0;
   if (out_cost) /* a failed turn still consumed wall-clock -> still costs */
      *out_cost = (ok0 && ok1) ? wfe_autonomy_cost_estimate((double)(t1.tv_sec - t0.tv_sec) +
                                                            (double)(t1.tv_nsec - t0.tv_nsec) / 1e9)
                               : 0.0;
   return rc == 0 ? 1 : -1;
}

/* Attach the measured delegate cost to a result, so a turn's wall-clock cost is
 * charged against the per-run budget on EVERY return path (loop/fail/advance),
 * not only on advance — else a retry-loop or broken-artifact runaway pays nothing
 * and the USD cap never bites. */
static wfe_step_result_t with_cost(wfe_step_result_t r, double cost)
{
   r.cost_usd = cost;
   return r;
}

/* ---- executors ---- */

/* author.proposal / author.plan: drive a delegate to produce/edit the artifact,
 * then hash the artifact file. A failed dispatch loops to retry — except
 * author.proposal accepts an already-present, non-empty proposal (see below), so
 * a pre-supplied complete proposal (the proposals-trigger case) is not looped. */
static wfe_step_result_t exec_author(wfe_ctx *ctx, const wfe_node_t *node)
{
   const char *path = wfe_ctx_proposal_path(ctx);
   /* Dispatch a delegate to author/edit `path` (no-op if no provider installed). */
   char wd[1024];
   resolve_workdir(ctx, wd, sizeof wd);
   char commit[64] = "";
   double cost = 0.0;
   int drc = wfe_delegate_dispatch(wd, "architect", node_delegate(node),
                                   "Author or revise the workflow artifact at the given path "
                                   "per the work item, then commit it.",
                                   path, commit, &cost);
   /* Hash the artifact file as the produced content, and note whether it holds
    * any content at all. */
   char hash[65] = "";
   long sz = 0;
   if (path && path[0])
   {
      FILE *f = fopen(path, "rb");
      if (f)
      {
         fseek(f, 0, SEEK_END);
         sz = ftell(f);
         if (sz < 0)
            sz = 0;
         fseek(f, 0, SEEK_SET);
         char *buf = malloc((size_t)sz + 1);
         if (buf)
         {
            size_t rd = fread(buf, 1, (size_t)sz, f);
            buf[rd] = '\0';
            wfe_sha256_hex(buf, rd, hash);
            free(buf);
         }
         fclose(f);
      }
   }
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   if (drc < 0)
   {
      /* The delegate did not advance the artifact (no provider, an error, or a
       * no-op that changed no files). For author.proposal that is acceptable when
       * the proposal artifact is ALREADY present and non-empty: the proposals
       * trigger supplies a complete, already-approved proposal (the merge IS the
       * approval), so a "revise" delegate correctly changes nothing — accept the
       * existing proposal instead of looping to max_iters. With no artifact yet
       * (empty/missing file) it is a genuine non-advance, so loop and retry.
       * author.plan and every other author node have no pre-supplied artifact, so
       * they always loop on a failed dispatch. */
      if (node->block == WFE_BLK_AUTHOR_PROPOSAL && sz > 0 && hash[0])
         return wfe_step_advanced(handle, hash, cost);
      return with_cost(wfe_step_looped(), cost);
   }
   return wfe_step_advanced(handle, hash, cost);
}

/* implement: the manager loop. The live delegate provider owns decompose -> fan
 * out -> verify -> re-delegate (see the full-autonomous-development plan); this
 * executor dispatches it, then captures the branch head SHA as the produced
 * artifact. A failed dispatch loops (retry); fails closed if the repo is
 * unavailable. With no provider installed it preserves the prior freeze-only
 * behavior so the engine remains drivable without a live delegate. */
/* A positive-long env with a default (0/garbage/negative -> default). */
static long wfe_env_pos(const char *name, long def)
{
   /* live-config-reload: for a config-backed AIMEE_AUTONOMY_* var, take the LIVE value
    * (operator env override > snapshot) so a config.set applies without a restart. */
   long lv;
   if (config_autonomy_lookup(name, &lv))
      return lv > 0 ? lv : def;
   const char *v = getenv(name);
   if (!v || !v[0])
      return def;
   char *e = NULL;
   long n = strtol(v, &e, 10);
   return (e && *e == '\0' && n > 0) ? n : def;
}

/* Read `.aimee/units.json` (a JSON array of unit-task strings the coordinator wrote)
 * from `wd`. Returns a NEW cJSON array (caller frees) or NULL if absent/invalid. */
static cJSON *read_units_json(const char *wd)
{
   char path[1200];
   if (snprintf(path, sizeof path, "%s/.aimee/units.json", wd) >= (int)sizeof path)
      return NULL;
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return NULL;
   }
   long len = ftell(f);
   if (len <= 0 || len > (1 << 20) || fseek(f, 0, SEEK_SET) != 0)
   {
      fclose(f);
      return NULL;
   }
   char *buf = malloc((size_t)len + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t rd = fread(buf, 1, (size_t)len, f);
   fclose(f);
   buf[rd] = '\0';
   cJSON *doc = cJSON_Parse(buf);
   free(buf);
   if (!doc)
      return NULL;
   /* accept either a bare array or {"units":[...]} */
   cJSON *arr = cJSON_IsArray(doc) ? doc : cJSON_GetObjectItemCaseSensitive(doc, "units");
   if (!cJSON_IsArray(arr))
   {
      cJSON_Delete(doc);
      return NULL;
   }
   if (arr == doc)
      return doc;
   cJSON *detached = cJSON_Duplicate(arr, 1);
   cJSON_Delete(doc);
   return detached;
}

/* Engine-level fan-out (PC3b/Q2), gated by AIMEE_AUTONOMY_FANOUT. Decompose the plan
 * into units via a coordinator delegate, then implement each unit SEQUENTIALLY (the
 * scheduler is concurrency=1) with a per-unit mechanical verify and retry on a
 * DIFFERENT delegate (bounded by AIMEE_AUTONOMY_UNIT_RETRY). Units land as sequential
 * commits on the one work-item branch (the patch-coordinator merge is implicit). A
 * decompose that yields nothing falls back to a SINGLE unit (the whole plan) — which
 * still runs the mandatory per-unit + aggregate tiers (no verification bypass).
 * Returns 0 if every unit passed its per-unit verify (caller then runs the mandatory
 * aggregate gate), -1 if any unit permanently failed (caller parks — NO silent partial
 * advance). *cost accumulates every dispatch. */
/* Copy `src` into `dst` stripping control bytes (< 0x20) to a printable space and
 * hard-capping the length, so untrusted coordinator-generated unit text cannot smuggle
 * newline-delimited prompt-injection directives into a privileged engineer prompt. */
static void sanitize_unit(const char *src, char *dst, size_t cap)
{
   size_t o = 0;
   for (const char *p = src; *p && o + 1 < cap; p++)
   {
      unsigned char ch = (unsigned char)*p;
      dst[o++] = (ch < 0x20 || ch == 0x7f) ? ' ' : (char)ch;
   }
   dst[o] = '\0';
}

static int run_fanout_units(const char *wd, const wfe_node_t *node, double *cost)
{
   /* 1. Decompose — but ONLY if a prior loop hasn't already produced units (the file
    * is committed, so a re-driven implement reuses the established unit list instead of
    * re-decomposing every loop). Coordinator writes .aimee/units.json; does NOT implement. */
   cJSON *units = read_units_json(wd);
   if (!units)
   {
      char c[64] = "";
      (void)wfe_delegate_dispatch(
          wd, "architect", "$random",
          "Decompose the approved plan into a SMALL number of INDEPENDENT implementation units. "
          "Write ONLY a JSON array of concise imperative unit-task strings to .aimee/units.json in "
          "the repo root, then commit that file. Do NOT implement anything else.",
          NULL, c, cost);
      units = read_units_json(wd);
   }
   cJSON *fallback = NULL;
   int n_units = units ? cJSON_GetArraySize(units) : 0;
   if (n_units <= 0)
   {
      /* single-unit fallback = the whole plan (still fully verified below). */
      fallback = cJSON_CreateArray();
      cJSON_AddItemToArray(fallback, cJSON_CreateString("Implement the entire approved plan."));
      units = fallback;
      n_units = 1;
   }
   /* Bound the fan-out: a pathological/hostile decomposition (thousands of units) would
    * fan out thousands of dispatches. Cap it; an over-cap decomposition is itself a
    * coordinator failure -> DEGRADED park. */
   long unit_max = wfe_env_pos("AIMEE_AUTONOMY_UNIT_MAX", 16);
   if (n_units > unit_max)
   {
      cJSON_Delete(units);
      return -1;
   }

   long retry_max = wfe_env_pos("AIMEE_AUTONOMY_UNIT_RETRY", 2);
   int failed = 0;
   for (int i = 0; i < n_units && !failed; i++)
   {
      const cJSON *u = cJSON_GetArrayItem(units, i);
      if (!cJSON_IsString(u) || !u->valuestring || !u->valuestring[0])
         continue; /* skip an empty/non-string entry (no work to dispatch) */
      char utext[512];
      sanitize_unit(u->valuestring, utext, sizeof utext);
      char prompt[1024];
      snprintf(prompt, sizeof prompt,
               "Implement ONLY this unit of the approved plan on the work-item branch, run "
               "`aimee git verify`, fix any failures, then commit. UNIT: %s",
               utext);
      int ok = 0;
      for (long attempt = 0; attempt <= retry_max && !ok; attempt++)
      {
         /* retry-DIFFERENT-delegate: the first attempt uses the pinned delegate;
          * retries use $random so a fresh perspective takes over (Q2). */
         const char *deleg = (attempt == 0) ? node_delegate(node) : "$random";
         char uc[64] = "";
         if (wfe_delegate_dispatch(wd, "engineer", deleg, prompt, NULL, uc, cost) < 0)
            continue; /* dispatch failed -> next attempt */
         if (wfe_implement_verify_ok(wd))
            ok = 1;
      }
      if (!ok)
         failed = 1; /* this unit could not be made to pass -> park (no partial advance) */
   }
   cJSON_Delete(units); /* whether the real list or the fallback; fallback aliases units */
   return failed ? -1 : 0;
}

static wfe_step_result_t exec_implement(wfe_ctx *ctx, const wfe_node_t *node)
{
   char wd[1024];
   resolve_workdir(ctx, wd, sizeof wd);
   char commit[64] = "";
   double cost = 0.0;

   long fanout_on = 0;
   (void)config_autonomy_lookup("AIMEE_AUTONOMY_FANOUT", &fanout_on); /* live: env > snapshot */
   if (fanout_on)
   {
      /* Manager loop (PC3b): decompose -> fan out engineer per unit -> per-unit verify
       * + retry-different. A unit that never passes parks pending_human via DEGRADED
       * (NO silent partial advance); the mandatory aggregate gate still runs below. */
      if (run_fanout_units(wd, node, &cost) < 0)
         return with_cost(wfe_step_failed_class(WFE_FAIL_DEGRADED, 0), cost);
   }
   else if (wfe_delegate_dispatch(
                wd, "engineer", node_delegate(node),
                "Implement the approved plan on the work-item branch: split into units, delegate "
                "each, VERIFY each with `aimee git verify` and fix any failures, then commit the "
                "accepted work.",
                NULL, commit, &cost) < 0)
      return with_cost(wfe_step_looped(), cost);

   char base[64] = "", head[64] = "", dhash[65] = "", err[128] = "";
   if (wfe_git_freeze(wd, "HEAD", base, head, dhash, err, sizeof err) != 0 || !head[0])
      return with_cost(wfe_step_failed_class(WFE_FAIL_CORRUPTION, 0), cost); /* worktree/git */
   /* MANDATORY aggregate mechanical verify (WP-1b): the whole merged change must PASS
    * (integration breakage from independently-verified units cannot advance). A failed
    * verdict loops back (the engine bounds retries via stage_attempt, parks
    * max_attempts on exhaustion); a re-dispatched fresh engineer sees + fixes findings. */
   if (!wfe_implement_verify_ok(wd))
      return with_cost(wfe_step_looped(), cost);
   /* Aggregate adversarial gate (PC3/Q2), OPT-IN via AIMEE_AUTONOMY_SKEPTICS>0 (default
    * off -> a pass-through, unchanged behavior). When enabled, a reviewer + N skeptic
    * judgments of the merged change must not refute; a refute loops back (bounded). */
   if (!wfe_implement_adversarial_ok(wd))
      return with_cost(wfe_step_looped(), cost);
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   return wfe_step_advanced(handle, head, cost);
}

/* document: produces the (documented) branch. In production a delegate writes
 * docs onto the branch (README/CHANGELOG/docs/ + inline comments); that live
 * dispatch is integration-gated (see file header). What this does NOW: captures
 * the current branch head SHA as the produced artifact, and fails closed if the
 * repo is unavailable. */
static wfe_step_result_t exec_document(wfe_ctx *ctx, const wfe_node_t *node)
{
   char wd[1024];
   resolve_workdir(ctx, wd, sizeof wd);
   char commit[64] = "";
   double cost = 0.0;
   if (wfe_delegate_dispatch(wd, "engineer", node_delegate(node),
                             "Document the change on the work-item branch (README/CHANGELOG/docs "
                             "+ inline comments), then commit.",
                             NULL, commit, &cost) < 0)
      return with_cost(wfe_step_looped(), cost);
   char base[64] = "", head[64] = "", dhash[65] = "", err[128] = "";
   if (wfe_git_freeze(wd, "HEAD", base, head, dhash, err, sizeof err) != 0 || !head[0])
      return with_cost(wfe_step_failed_class(WFE_FAIL_CORRUPTION, 0), cost); /* worktree/git */
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   return wfe_step_advanced(handle, head, cost);
}

/* freeze: capture the cumulative diff at a stable freeze commit. */
static wfe_step_result_t exec_freeze(wfe_ctx *ctx, const wfe_node_t *node)
{
   char wd[1024];
   resolve_workdir(ctx, wd, sizeof wd);
   const char *base_branch = getenv("AIMEE_WORKFLOW_BASE");
   char base[64] = "", head[64] = "", dhash[65] = "", err[128];
   if (wfe_git_freeze(wd, base_branch ? base_branch : "HEAD", base, head, dhash, err, sizeof err) !=
       0)
      return wfe_step_failed();
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   return wfe_step_advanced(handle, dhash, 0.0);
}

/* pr.open: push the branch + open a forge PR via the forge seam. The PR ref the
 * forge returns becomes the produced content. A failed open loops (retry). With
 * no live `open` installed (default/mocks) it preserves the prior advance so the
 * engine stays drivable without a forge. */
/* A forge PR ref must be non-empty, printable, and fit the wfe_step_result_t
 * content_hash transport (it rides that field to the engine, which persists it as
 * the work item's pr_ref). A live provider that signals success with a junk or
 * over-long ref is rejected so a bogus ref never reaches the merge gates. */
static int pr_ref_is_sane(const char *ref)
{
   size_t n = ref ? strlen(ref) : 0;
   if (n == 0 || n >= 64) /* < sizeof(((wfe_step_result_t*)0)->content_hash) (65) */
      return 0;
   for (size_t i = 0; i < n; i++)
      if ((unsigned char)ref[i] < 0x20 || (unsigned char)ref[i] > 0x7e)
         return 0;
   return 1;
}

/* The durable feature branch for a run: aimee/feat/<work_item_id>. A slice child's
 * sub-PR targets its PARENT's feature branch, so the caller passes the id whose
 * feature branch it wants (the parent id for a child; the run's own id for the
 * parent's branch.open). */
static void feature_branch_name(const char *wi_id, char *buf, size_t n)
{
   snprintf(buf, n, "aimee/feat/%s", (wi_id && wi_id[0]) ? wi_id : "orphan");
}

/* Resolve the target repo's REAL default branch (its trunk: the branch a final,
 * human-reviewed PR targets). Resolution order:
 *   1. AIMEE_DEFAULT_BRANCH override -- deterministic operator knob + test seam;
 *   2. else `git symbolic-ref --short refs/remotes/origin/HEAD` in `repo_dir`
 *      (the repo's advertised default, e.g. "origin/main" -> "main").
 * Returns 0 with `buf` filled on success; returns -1 (and clears `buf`) if the trunk
 * cannot be determined -- there is NO "main" guess: a base:trunk PR is safety-relevant
 * (it drives allow_protected), so an unresolved trunk FAILS CLOSED at the caller rather
 * than opening a PR against a guessed branch that may not be the real trunk. This is the
 * repo's own trunk, NOT wfe_autonomous_base() (the aimee integration branch "testing"). */
static int wfe_repo_default_branch(const char *repo_dir, char *buf, size_t n)
{
   if (!buf || n < 2)
      return -1;
   buf[0] = '\0';
   const char *env = getenv("AIMEE_DEFAULT_BRANCH");
   if (env && env[0])
   {
      snprintf(buf, n, "%s", env);
      return 0;
   }
   if (repo_dir && repo_dir[0])
   {
      const char *argv[] = {
          "git", "-C", repo_dir, "symbolic-ref", "--short", "refs/remotes/origin/HEAD", NULL};
      char *o = NULL;
      if (safe_exec_capture(argv, &o, 1 << 14) == 0 && o)
      {
         size_t l = strlen(o);
         while (l && (o[l - 1] == '\n' || o[l - 1] == '\r' || o[l - 1] == ' ' || o[l - 1] == '\t'))
            o[--l] = '\0';
         const char *name = (strncmp(o, "origin/", 7) == 0) ? o + 7 : o;
         if (name[0])
         {
            snprintf(buf, n, "%s", name);
            free(o);
            return 0;
         }
      }
      free(o);
   }
   return -1; /* unresolved -> caller fails closed (no "main" guess) */
}

/* Resolve a pr.open node's target base branch into `buf`. params.base:
 *   "feature" -> the parent feature branch aimee/feat/<parent_id> (a slice sub-PR
 *                merges INTO the feature branch); requires the run to have a parent.
 *   "trunk"   -> the repo's REAL default branch (wfe_repo_default_branch). A final
 *                feature PR targets the trunk; sets *allow_protected so exec_pr_open
 *                may OPEN a PR against main/master (it never merges -- merge keeps its
 *                own wfe_autonomous_target_ok() rail).
 *   "default"/absent -> the autonomous base (the aimee integration branch).
 * *allow_protected defaults to 0; only "trunk" sets it. Returns 0 on success, -1 if a
 * feature base was requested but the run has no parent (a misconfiguration -> the
 * caller fails closed). */
static int resolve_pr_base(wfe_ctx *ctx, const wfe_node_t *node, char *buf, size_t n,
                           int *allow_protected)
{
   if (allow_protected)
      *allow_protected = 0;
   const cJSON *b = node->params ? cJSON_GetObjectItemCaseSensitive(node->params, "base") : NULL;
   const char *base_kind = (b && cJSON_IsString(b) && b->valuestring) ? b->valuestring : "default";
   if (strcmp(base_kind, "feature") == 0)
   {
      const char *wi = wfe_ctx_work_item(ctx);
      db1_work_item_t row;
      if (!wi || !wi[0] || db1_work_item_get(wi, &row) != 1 || !row.parent_id[0])
         return -1; /* base:feature outside a slice child -> misconfig */
      feature_branch_name(row.parent_id, buf, n);
      return 0;
   }
   if (strcmp(base_kind, "trunk") == 0)
   {
      char wd[1024];
      resolve_workdir(ctx, wd, sizeof wd);
      if (allow_protected)
         *allow_protected = 1; /* open-only against the repo trunk (never merged here) */
      return wfe_repo_default_branch(wd, buf, n);
   }
   const char *ab = wfe_autonomous_base();
   snprintf(buf, n, "%s", (ab && ab[0]) ? ab : "");
   return buf[0] ? 0 : -1;
}

static wfe_step_result_t exec_pr_open(wfe_ctx *ctx, const wfe_node_t *node)
{
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   /* Resolve the target base (autonomous base, the parent feature branch for a slice
    * sub-PR, or the repo trunk for a final feature PR). Safety rail: refuse a base that
    * resolves to a protected branch (main/master/release*) UNLESS this is an explicit
    * base:trunk final PR -- pr.open only OPENS a PR (never merges), so a human-reviewed
    * PR against the trunk is the intended, safe terminal. merge keeps its own
    * wfe_autonomous_target_ok() rail, so nothing can auto-merge into a protected base. */
   char base[256] = "";
   int allow_protected = 0;
   if (resolve_pr_base(ctx, node, base, sizeof base, &allow_protected) != 0)
      return wfe_step_failed();
   if (!allow_protected && wfe_base_is_protected(base))
      return wfe_step_failed();
   if (g_forge->open)
   {
      /* The branch the run committed to: the per-work-item branch aimee/wi/<id>
       * (F2's worktree branch), unless an explicit AIMEE_WORKFLOW_BRANCH overrides.
       * worktrees share the branch namespace, so the live forge's push resolves it
       * from any checkout. */
      const char *env_branch = getenv("AIMEE_WORKFLOW_BRANCH");
      char branchbuf[200];
      const char *branch = env_branch;
      if (!branch || !branch[0])
      {
         const char *wi = wfe_ctx_work_item(ctx);
         if (wi && wi[0])
         {
            snprintf(branchbuf, sizeof branchbuf, "aimee/wi/%s", wi);
            branch = branchbuf;
         }
         else
            branch = "HEAD";
      }
      char pr_ref[128] = ""; /* the forge open() contract writes up to 128 bytes */
      if (g_forge->open(wfe_ctx_repo(ctx), branch, base, wfe_ctx_work_item(ctx), "", pr_ref) != 0)
         return wfe_step_looped();
      /* Fail closed on a success-with-bad-ref: advancing with no resolvable PR would
       * silently push the merge gates onto the wrong target. */
      if (!pr_ref_is_sane(pr_ref))
         return wfe_step_looped();
      /* pr_ref rides content_hash; the engine persists it as the work item's pr_ref
       * when this pr.open block advances (see wfe_engine.c ADVANCED branch). */
      return wfe_step_advanced(handle, pr_ref, 0.0);
   }
   return wfe_step_advanced(handle, "", 0.0);
}

/* The PR ref the forge ops (gate.ci / check.mergeable / merge) act on. Resolution:
 *   1. the real forge ref persisted when pr.open ran -> use it;
 *   2. else, if a PR-opening provider IS registered, a missing ref is an anomaly
 *      (a gate ran without a recorded PR) -> return "" so the forge ops fail closed
 *      (NONE/unknown -> park), rather than masking it with a wrong ref;
 *   3. else (no `open` provider: the live default + the safety-test mock) fall back
 *      to the work-item id, preserving today's behavior for the gated/legacy path. */
static const char *pr_ref(wfe_ctx *ctx)
{
   const char *ref = wfe_ctx_pr_ref(ctx);
   if (ref && ref[0])
      return ref;
   if (g_forge->open)
      return ""; /* open provider present but no ref recorded -> fail closed */
   const char *wi = wfe_ctx_work_item(ctx);
   return wi ? wi : "";
}

/* merge: idempotent + race-safe. The never-re-merge invariant is enforced by
 * requiring a CONFIRMED not-merged state before we ever call merge():
 *   is_merged == 1  -> idempotent no-op success (already merged)
 *   is_merged <  0  -> state could not be determined (transient forge error) ->
 *                      park; we never call merge() when we can't confirm the PR
 *                      is unmerged, so a transient error can never double-merge.
 *   is_merged == 0  -> confirmed unmerged: the forge merge act is the single
 *                      decision (already-race -> success, not-mergeable -> loop,
 *                      transient error -> park for a human re-drive). */
static wfe_step_result_t exec_merge(wfe_ctx *ctx, const wfe_node_t *node)
{
   /* Safety rail (mirror pr.open): never merge an autonomous run into a protected
    * branch. Fail closed on a misconfigured base. */
   if (!wfe_autonomous_target_ok())
      return wfe_step_failed();
   const char *repo = wfe_ctx_repo(ctx), *pr = pr_ref(ctx);
   int im = g_forge->is_merged(repo, pr);
   if (im == 1)
      return wfe_step_advanced("merged", "", 0.0); /* idempotent no-op */
   if (im < 0)
      return wfe_step_pending(WFE_PAUSE_MERGE_PENDING); /* unconfirmed -> never merge */
   switch (g_forge->merge(repo, pr))
   {
   case WFE_MERGE_OK:
   case WFE_MERGE_ALREADY:
      return wfe_step_advanced("merged", "", 0.0);
   case WFE_MERGE_NOT_MERGEABLE:
      return wfe_step_looped();
   default:
      /* transient/unknown forge error on the merge act itself: park for a human
       * re-drive rather than hard-failing the run. We only reach here with a
       * confirmed-unmerged PR, so this can never double-merge. */
      (void)node;
      return wfe_step_pending(WFE_PAUSE_MERGE_PENDING);
   }
}

/* gate.ci: only an explicit all-green advances; everything else fails closed. */
/* PC2: the latest CI outcome RECORDED by the /v1/dev/ci-event webhook for this work
 * item (preferred over a live forge poll — it is authoritative + free), plus the
 * count of recorded CI FAILURES (for the per-work-item red-CI retry cap). The webhook
 * writes a "ci_event" lifecycle event with detail "<status>|<head_sha>|<log_url>";
 * status ∈ passed|failed|error|pending. Returns WFE_CI_NONE when nothing is recorded
 * (caller falls back to the forge poll). Events are chronological, so the last
 * ci_event is the latest. */
static wfe_ci_status_t wfe_last_ci_outcome(const char *work_item_id, int *out_fail_count)
{
   if (out_fail_count)
      *out_fail_count = 0;
   db1_lifecycle_event_t *evs = NULL;
   int n = db1_lifecycle_event_list(work_item_id, &evs);
   if (n <= 0)
   {
      free(evs);
      return WFE_CI_NONE;
   }
   wfe_ci_status_t latest = WFE_CI_NONE;
   for (int i = 0; i < n; i++)
   {
      if (strcmp(evs[i].kind, "ci_event") != 0)
         continue;
      /* status is the prefix up to the first '|' */
      const char *d = evs[i].detail;
      if (strncmp(d, "passed", 6) == 0 && (d[6] == '\0' || d[6] == '|'))
         latest = WFE_CI_SUCCESS;
      else if (strncmp(d, "pending", 7) == 0 && (d[7] == '\0' || d[7] == '|'))
         latest = WFE_CI_PENDING;
      else /* failed | error -> CI failure */
      {
         latest = WFE_CI_FAILURE;
         if (out_fail_count)
            (*out_fail_count)++;
      }
   }
   free(evs);
   return latest;
}

static wfe_step_result_t exec_gate_ci(wfe_ctx *ctx, const wfe_node_t *node)
{
   /* PC2: prefer the webhook-recorded outcome; fall back to a live forge poll only
    * when nothing has been recorded (e.g. a poll-only deployment). */
   int fail_count = 0;
   int from_recorded = 1;
   wfe_ci_status_t st = wfe_last_ci_outcome(wfe_ctx_work_item(ctx), &fail_count);
   if (st == WFE_CI_NONE)
   {
      from_recorded = 0;
      st = g_forge->ci_status(wfe_ctx_repo(ctx), pr_ref(ctx));
   }

   switch (st)
   {
   case WFE_CI_SUCCESS:
   {
      char h[80];
      snprintf(h, sizeof h, "%s.out", node->id);
      return wfe_step_advanced(h, "", 0.0);
   }
   case WFE_CI_FAILURE:
   {
      /* Red-CI retry (PC2 / Q3): loop back to `implement` with the CI failure as new
       * input, bounded per-work-item by AIMEE_AUTONOMY_CI_RETRY_MAX (default 2,
       * counted from the recorded ci_event failures). On exhaustion, park for a human
       * via the PC1 DEGRADED class rather than spinning the loop. */
      long cap = 2;
      long lv; /* live: operator env override > config snapshot */
      if (config_autonomy_lookup("AIMEE_AUTONOMY_CI_RETRY_MAX", &lv) && lv > 0)
         cap = lv;
      if (fail_count >= cap)
         return wfe_step_failed_class(WFE_FAIL_DEGRADED, 0); /* park pending_human */
      /* Poll-only path (no webhook events): record this failure so the per-work-item
       * cap bounds it too. The webhook path already recorded its ci_event, so skip
       * (avoids double-counting). */
      if (!from_recorded)
         db1_lifecycle_event_add(wfe_ctx_work_item(ctx), node->id, "ci_event", "ci-poll",
                                 "failed|poll", "", 0);
      return wfe_step_looped();
   }
   default: /* PENDING or NONE/unknown -> park, never advance */
      return wfe_step_pending(WFE_PAUSE_CI_PENDING);
   }
}

/* check.mergeable: mergeable (1) advances; conflict (0) loops back to fix it;
 * unknown (-1, transient forge error) parks -- it never advances on an
 * undetermined state (fail closed), and uses the merge-state pause reason rather
 * than the CI one so the park is labelled correctly. */
static wfe_step_result_t exec_check_mergeable(wfe_ctx *ctx, const wfe_node_t *node)
{
   int m = g_forge->mergeable(wfe_ctx_repo(ctx), pr_ref(ctx));
   if (m == 1)
   {
      char h[80];
      snprintf(h, sizeof h, "%s.out", node->id);
      return wfe_step_advanced(h, "", 0.0);
   }
   if (m == 0)
      return wfe_step_looped();
   return wfe_step_pending(WFE_PAUSE_MERGE_PENDING); /* unknown -> park, never advance */
}

/* custom: the one generic executor for config-defined blocks. */
static wfe_step_result_t exec_custom(wfe_ctx *ctx, const wfe_node_t *node)
{
   const wfe_custom_block_t *c = wfe_custom_lookup(node->custom_name);
   if (!c)
      return wfe_step_failed();
   char head[64] = "", base[64] = "", dhash[65] = "", err[128] = "";
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   char wd[1024]; /* F2: a custom block acts in the work-item worktree too */
   resolve_workdir(ctx, wd, sizeof wd);

   if (c->executor == WFE_EXEC_COMMAND)
   {
      if (!wfe_custom_commands_allowed())
         return wfe_step_failed(); /* opt-in: lifecycle allow_command not set */
      if (c->argc == 0)
         return wfe_step_failed();
      /* A command block must NOT inherit the engine's environment (it carries
       * AIMEE_APPROVAL_KEY, vault tokens, etc.): pass only a minimal allowlist
       * (PATH/HOME/LANG — enough to run an ordinary build/lint tool; engine paths
       * and secrets are deliberately withheld). Stack buffers outlive the
       * synchronous exec call below. */
      char e_path[2048], e_home[2048], e_lang[256];
      const char *v;
      char *envp[4];
      int ei = 0;
      v = getenv("PATH");
      snprintf(e_path, sizeof e_path, "PATH=%s", (v && v[0]) ? v : "/usr/bin:/bin");
      envp[ei++] = e_path;
      if ((v = getenv("HOME")) && v[0])
      {
         snprintf(e_home, sizeof e_home, "HOME=%s", v);
         envp[ei++] = e_home;
      }
      if ((v = getenv("LANG")) && v[0])
      {
         snprintf(e_lang, sizeof e_lang, "LANG=%s", v);
         envp[ei++] = e_lang;
      }
      envp[ei] = NULL;
      /* argv is the registry's OWNED, length-bounded, all-string, NULL-terminated
       * array (validated at load); run it directly (no shell), pinned to the
       * work-item repo, under a wall-clock timeout (kill -> step failed). */
      char *out = NULL;
      int rc = safe_exec_capture_cwd_env_timeout((const char *const *)c->argv, wd, envp, &out,
                                                 1 << 20, wfe_custom_command_timeout_ms());
      free(out);
      if (rc != 0)
         return wfe_step_failed(); /* non-zero exit or SAFE_EXEC_TIMEOUT */
   }
   /* delegate executor is integration-gated (like implement/document). */

   if (c->produces == WFE_ART_BRANCH)
   {
      if (wfe_git_freeze(wd, "HEAD", base, head, dhash, err, sizeof err) != 0 || !head[0])
         return wfe_step_failed_class(WFE_FAIL_CORRUPTION, 0);
      return wfe_step_advanced(handle, head, 0.0);
   }
   return wfe_step_advanced(handle, "", 0.0); /* produces: none (sink) */
}

/* ---- primary-as-manager executors (understand / split / review / gate.deliver).
 * understand/split drive a delegate to write a typed JSON artifact to a per-node
 * worktree file, then the executor SCHEMA-VALIDATES it and hashes it as the
 * produced content (a schema-invalid or missing artifact re-loops, with the
 * validation-failure reason recorded to the append-only lifecycle log so a
 * bounded loop's terminal failure has an audit trail). The engine captures the
 * returned content_hash into engine-owned append-only state on advance, so
 * downstream blocks trust the recorded hash, not the mutable file. ---- */

/* per-node JSON artifact path at the worktree root (no mkdir needed). */
static void manager_artifact_path(wfe_ctx *ctx, const wfe_node_t *node, char *buf, size_t n)
{
   char wd[1024];
   resolve_workdir(ctx, wd, sizeof wd);
   snprintf(buf, n, "%s/.wfe-%s.json", wd, node->id);
}

/* read + sha256 `path` into hash[65] (empty if absent), returning parsed cJSON
 * (caller frees) or NULL. */
static cJSON *manager_read_hash_json(const char *path, char *hash)
{
   hash[0] = '\0';
   if (!path || !path[0])
      return NULL;
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   if (sz < 0)
      sz = 0;
   fseek(f, 0, SEEK_SET);
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t rd = fread(buf, 1, (size_t)sz, f);
   buf[rd] = '\0';
   fclose(f);
   wfe_sha256_hex(buf, rd, hash);
   cJSON *j = cJSON_Parse(buf);
   free(buf);
   return j;
}

/* dispatch a delegate to author a typed artifact, then validate+hash it.
 * `validate` is the schema validator; on missing/invalid the reason is logged
 * and the step loops (bounded by the engine's per-stage attempt cap). */
static wfe_step_result_t manager_produce(wfe_ctx *ctx, const wfe_node_t *node, const char *role,
                                         const char *prompt,
                                         int (*validate)(const cJSON *, char *, size_t))
{
   char wd[1024];
   resolve_workdir(ctx, wd, sizeof wd);
   char path[1200];
   manager_artifact_path(ctx, node, path, sizeof path);
   char commit[64] = "";
   double cost = 0.0;
   if (wfe_delegate_dispatch(wd, role, node_delegate(node), prompt, path, commit, &cost) < 0)
      return with_cost(wfe_step_looped(), cost);
   char hash[65] = "";
   cJSON *j = manager_read_hash_json(path, hash);
   char verr[200] = "";
   int ok = j && validate(j, verr, sizeof verr) == 0;
   if (j)
      cJSON_Delete(j);
   if (!ok)
   {
      db1_lifecycle_event_add(wfe_ctx_work_item(ctx), node->id, "loop", "engine",
                              verr[0] ? verr : "artifact missing/invalid", "", cost);
      return with_cost(wfe_step_looped(), cost);
   }
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   return wfe_step_advanced(handle, hash, cost);
}

static wfe_step_result_t exec_understand(wfe_ctx *ctx, const wfe_node_t *node)
{
   return manager_produce(
       ctx, node, "architect",
       "Scope this work item into a structured INTENT RECORD and write it as JSON to the given "
       "path (nothing else): {\"schema_version\":1,\"status\":\"unconfirmed\",\"summary\":\"<one "
       "line>\",\"rationale\":\"<why>\",\"acceptance_criteria\":[\"<testable>\"]}. Then commit it.",
       wfe_intent_validate);
}

static wfe_step_result_t exec_split(wfe_ctx *ctx, const wfe_node_t *node)
{
   return manager_produce(
       ctx, node, "architect",
       "Decompose the approved intent into a structured PACKET PLAN and write it as JSON to the "
       "given path (nothing else): {\"schema_version\":1,\"packets\":[{\"packet_id\":\"p1\","
       "\"summary\":\"...\",\"target_blocks\":[\"implement\"],\"dependencies\":[],"
       "\"acceptance_criteria\":[\"...\"]}]}. Then commit it.",
       wfe_packets_validate);
}

/* review: a READ-ONLY reviewer delegate (persona from node params `reviewer`,
 * validator-checked disjoint from the roundtable panel + producer) emits a typed
 * verdict. pass -> ADVANCED (on_pass); changes -> LOOPED (on_fail, re-delegate).
 * The re-delegate loop is bounded by the engine's GENERIC per-node loop cap
 * (params.max_iters / on_max, keyed on the gate node): the engine owns the cap
 * and its resolution, so this block no longer self-terminates. A review node
 * that wants the historical "3 tries then terminal fail" sets max_iters:3 and
 * on_max:fail. Tool-level read-only enforcement is the S2 tool-policy slice. */
static wfe_step_result_t exec_review(wfe_ctx *ctx, const wfe_node_t *node)
{
   const char *wi = wfe_ctx_work_item(ctx);
   const cJSON *jrev =
       node->params ? cJSON_GetObjectItemCaseSensitive(node->params, "reviewer") : NULL;
   const char *reviewer =
       (jrev && cJSON_IsString(jrev) && jrev->valuestring) ? jrev->valuestring : "";
   char wd[1024];
   resolve_workdir(ctx, wd, sizeof wd);
   char path[1200];
   manager_artifact_path(ctx, node, path, sizeof path);
   char commit[64] = "";
   double cost = 0.0;
   if (wfe_delegate_dispatch(
           wd, "reviewer", reviewer[0] ? reviewer : node_delegate(node),
           "Review the delegate's frozen diff READ-ONLY (do NOT edit files). Emit a REVIEW VERDICT "
           "as JSON to the given path (nothing else): {\"schema_version\":1,\"verdict\":\"pass\" "
           "or "
           "\"changes\",\"blocking_findings\":[{\"block_id\":\"...\",\"rule_id\":\"...\","
           "\"expected\":\"...\",\"observed\":\"...\",\"suggested_fix\":\"...\"}],"
           "\"non_blocking\":[]}. Use \"changes\" with >=1 blocking finding only if re-work is "
           "required; otherwise \"pass\".",
           path, commit, &cost) < 0)
      return with_cost(wfe_step_looped(), cost);
   char hash[65] = "";
   cJSON *j = manager_read_hash_json(path, hash);
   char verr[200] = "";
   wfe_review_verdict_t v = WFE_REVIEW_CHANGES;
   int ok = j && wfe_review_validate(j, verr, sizeof verr) == 0 && wfe_review_verdict(j, &v) == 0;
   if (j)
      cJSON_Delete(j);
   if (!ok)
   {
      db1_lifecycle_event_add(wi, node->id, "loop", "engine",
                              verr[0] ? verr : "review verdict missing/invalid", "", cost);
      return with_cost(wfe_step_looped(), cost);
   }
   if (v == WFE_REVIEW_PASS)
   {
      char handle[80];
      snprintf(handle, sizeof handle, "%s.out", node->id);
      return wfe_step_advanced(handle, hash, cost);
   }
   return with_cost(wfe_step_looped(), cost); /* changes requested -> re-delegate */
}

/* gate.deliver: the terminal enforcement gate. Re-verify (Q4) that every
 * delivery-gating gate has an engine-owned approving "advance" record before
 * crossing; on any missing verdict FAIL (halt) -- never loop. Structural lookup
 * over the append-only lifecycle log, no LLM judgement. */
static int deliver_gate_advanced(const char *node_id, void *ctx)
{
   const char *wi = (const char *)ctx;
   if (!wi)
      return 0;
   db1_lifecycle_event_t *ev = NULL;
   int n = db1_lifecycle_event_list(wi, &ev);
   int found = 0;
   for (int i = 0; i < n && !found; i++)
      if (strcmp(ev[i].stage, node_id) == 0 && strcmp(ev[i].kind, "advance") == 0)
         found = 1;
   free(ev);
   return found;
}

static wfe_step_result_t exec_gate_deliver(wfe_ctx *ctx, const wfe_node_t *node)
{
   const char *wi = wfe_ctx_work_item(ctx);
   const wfe_def_t *def = wfe_ctx_def(ctx);
   char err[200] = "";
   if (!def ||
       wfe_deliver_reverify(def, node->id, deliver_gate_advanced, (void *)wi, err, sizeof err) != 0)
   {
      db1_lifecycle_event_add(wi, node->id, "failed", "engine",
                              err[0] ? err : "delivery re-verification failed", "", 0.0);
      return wfe_step_failed();
   }
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   return wfe_step_advanced(handle, "", 0.0); /* terminal: engine logs "terminal" */
}

/* 1 if `ref` resolves to a commit in `wd` (used to pick a startpoint that exists). */
static int git_ref_ok(const char *wd, const char *ref)
{
   char spec[192];
   snprintf(spec, sizeof spec, "%s^{commit}", ref);
   const char *argv[] = {"git", "-C", wd, "rev-parse", "--verify", "--quiet", spec, NULL};
   char *o = NULL;
   int rc = safe_exec_capture(argv, &o, 1 << 12);
   free(o);
   return rc == 0;
}

/* branch.open: open/return the durable feature branch that the per-slice sub-PRs
 * target and merge into. Produces the branch head SHA as its artifact (mirrors
 * freeze/document). Creating + pushing a named durable branch is the live-forge
 * concern (integration-gated); with no repo available it fails closed.
 * params.base:trunk bases the feature branch on the repo's REAL default branch
 * (origin/<trunk>) so the eventual feature->trunk PR is a clean diff; absent, it
 * bases at HEAD (the run's current base) as before. */
static wfe_step_result_t exec_branch_open(wfe_ctx *ctx, const wfe_node_t *node)
{
   char wd[1024];
   resolve_workdir(ctx, wd, sizeof wd);
   char base[64] = "", head[64] = "", dhash[65] = "", err[128] = "";
   if (wfe_git_freeze(wd, "HEAD", base, head, dhash, err, sizeof err) != 0 || !head[0])
      return wfe_step_failed_class(WFE_FAIL_CORRUPTION, 0);
   /* The durable feature branch slice sub-PRs merge into: aimee/feat/<work-item>. */
   const char *wi = wfe_ctx_work_item(ctx);
   char feat[200];
   feature_branch_name(wi, feat, sizeof feat);
   /* Startpoint: base:trunk -> the repo trunk (prefer origin/<trunk>, else a local
    * <trunk>); absent -> HEAD (prior behavior). base:trunk FAILS CLOSED (parks) if the
    * trunk can't be resolved to a real ref -- a feature branch silently based off the
    * wrong point would produce a noisy/incorrect feature->trunk diff, so we refuse
    * rather than guess. */
   const char *startpoint = "HEAD";
   char sp[192];
   const cJSON *bp = node->params ? cJSON_GetObjectItemCaseSensitive(node->params, "base") : NULL;
   if (bp && cJSON_IsString(bp) && bp->valuestring && strcmp(bp->valuestring, "trunk") == 0)
   {
      char db[64];
      if (wfe_repo_default_branch(wd, db, sizeof db) != 0)
         return wfe_step_failed(); /* trunk name unresolved (no env + no origin/HEAD) */
      snprintf(sp, sizeof sp, "origin/%s", db);
      if (!git_ref_ok(wd, sp))
         snprintf(sp, sizeof sp, "%s", db);
      if (!git_ref_ok(wd, sp))
         return wfe_step_failed(); /* trunk resolved but no local ref -> fail closed */
      startpoint = sp;
   }
   /* Create the local branch at the startpoint if absent; NO `-f`, so a re-entry never
    * resets an existing feature branch back to base (which would discard already-merged
    * slices). "already exists" is a harmless non-zero rc here (best-effort). */
   const char *br[] = {"git", "-C", wd, "branch", feat, startpoint, NULL};
   char *o = NULL;
   (void)safe_exec_capture(br, &o, 1 << 14);
   free(o);
   if (g_forge->publish_base && g_forge->publish_base(wfe_ctx_repo(ctx), feat) != 0)
      return wfe_step_looped(); /* couldn't publish the base -> retry */
   char handle[80];
   snprintf(handle, sizeof handle, "%s.out", node->id);
   /* Produce the feature branch NAME as the artifact (downstream slices derive their
    * base from the parent linkage, not this handle, but the name records intent). */
   return wfe_step_advanced(handle, feat, 0.0);
}

/* Child-workflow fan-out seam (see wfe_blocks.h). Default NULL -> foreach.workflow
 * parks (no child spawned; nothing silently advances). */
static const wfe_foreach_provider_t *g_foreach = NULL;

void wfe_set_foreach_provider(const wfe_foreach_provider_t *p)
{
   g_foreach = p;
}

/* foreach.workflow: fan the split packets out to one child "slice" workflow each,
 * parking until every child has merged into the feature branch. Aggregation is keyed
 * off the DB parent<->child linkage; only SPAWNING is delegated to the seam. With no
 * spawn provider installed (and no children yet) it parks pending_human (fail closed).
 *   - no children yet    -> spawn (park while they run); no provider -> park
 *   - any child FAILED    -> a slice will not merge (rejected/abandoned) -> park for a human
 *   - all children accepted -> advance (feature branch carries every merged slice)
 *   - else                -> children still running -> park, re-drive later
 * Trouble always PARKS (pending_human), never a silent advance and never a hard
 * run-fail: a human resolves the failed slice, then the run resumes. */
static wfe_step_result_t exec_foreach_workflow(wfe_ctx *ctx, const wfe_node_t *node)
{
   const char *wi = wfe_ctx_work_item(ctx);
   int total = 0, accepted = 0, failed = 0;
   if (wi && wi[0])
      (void)db1_work_item_child_counts(wi, &total, &accepted, &failed);

   if (total == 0)
   {
      /* No children spawned yet: ask the seam to fan out one per packet. */
      if (!g_foreach || !g_foreach->spawn)
         return wfe_step_pending(WFE_PAUSE_PENDING_HUMAN); /* no spawner -> fail closed */
      const cJSON *wf =
          node->params ? cJSON_GetObjectItemCaseSensitive(node->params, "workflow") : NULL;
      const char *child = (wf && cJSON_IsString(wf) && wf->valuestring) ? wf->valuestring : "slice";
      long max_children = wfe_env_pos("AIMEE_AUTONOMY_UNIT_MAX", 16);
      /* The split packet-plan the spawner fans out: <worktree>/.wfe-<producer>.json,
       * where <producer> is the node bound to this foreach's `packets` input. */
      char packets_path[1200] = "";
      const char *producer = NULL;
      for (int i = 0; i < node->n_ins; i++)
         if (strcmp(node->ins[i].input_name, "packets") == 0)
            producer = node->ins[i].producer_id;
      if (producer && producer[0])
      {
         char pwd[1024];
         resolve_workdir(ctx, pwd, sizeof pwd);
         snprintf(packets_path, sizeof packets_path, "%s/.wfe-%s.json", pwd, producer);
      }
      char err[200] = "";
      int n = g_foreach->spawn(wi, child, packets_path[0] ? packets_path : NULL, (int)max_children,
                               err, sizeof err);
      if (n < 0)
         return wfe_step_pending(WFE_PAUSE_PENDING_HUMAN); /* could not fan out -> park */
      if (n == 0)
      {
         /* no packets to slice -> nothing to do, advance past the fan-out. */
         char handle[80];
         snprintf(handle, sizeof handle, "%s.out", node->id);
         return wfe_step_advanced(handle, "", 0.0);
      }
      return wfe_step_pending(WFE_PAUSE_PENDING_HUMAN); /* spawned; run + re-drive */
   }

   if (failed > 0)
      return wfe_step_pending(WFE_PAUSE_PENDING_HUMAN); /* a slice will not merge -> park human */
   if (accepted >= total)
   {
      /* every slice merged into the feature branch; its content is re-derived
       * downstream by the acceptance freeze. */
      char handle[80];
      snprintf(handle, sizeof handle, "%s.out", node->id);
      return wfe_step_advanced(handle, "", 0.0);
   }
   return wfe_step_pending(WFE_PAUSE_PENDING_HUMAN); /* slices still running -> re-drive */
}

/* Register just the foreach.workflow executor over any prior (stub) registration --
 * lets a test drive the real fan-in aggregation while stubbing the git/delegate
 * producing blocks. */
void wfe_register_foreach_block(void)
{
   wfe_register_block_executor(WFE_BLK_FOREACH_WORKFLOW, exec_foreach_workflow);
}

void wfe_register_default_executors(void)
{
   wfe_register_block_executor(WFE_BLK_BRANCH_OPEN, exec_branch_open);
   wfe_register_block_executor(WFE_BLK_FOREACH_WORKFLOW, exec_foreach_workflow);
   wfe_register_block_executor(WFE_BLK_UNDERSTAND, exec_understand);
   wfe_register_block_executor(WFE_BLK_SPLIT, exec_split);
   wfe_register_block_executor(WFE_BLK_REVIEW, exec_review);
   wfe_register_block_executor(WFE_BLK_GATE_DELIVER, exec_gate_deliver);
   wfe_register_block_executor(WFE_BLK_AUTHOR_PROPOSAL, exec_author);
   wfe_register_block_executor(WFE_BLK_AUTHOR_PLAN, exec_author);
   wfe_register_block_executor(WFE_BLK_IMPLEMENT, exec_implement);
   wfe_register_block_executor(WFE_BLK_DOCUMENT, exec_document);
   wfe_register_block_executor(WFE_BLK_FREEZE, exec_freeze);
   wfe_register_block_executor(WFE_BLK_PR_OPEN, exec_pr_open);
   wfe_register_block_executor(WFE_BLK_MERGE, exec_merge);
   wfe_register_block_executor(WFE_BLK_GATE_CI, exec_gate_ci);
   wfe_register_block_executor(WFE_BLK_CHECK_MERGEABLE, exec_check_mergeable);
   wfe_register_block_executor(WFE_BLK_CUSTOM, exec_custom);
}
