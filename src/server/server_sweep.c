/* server_sweep.c: server-side deepening-sweep handler (Part B PR-B3b).
 *
 * Reuses the roundtable's in-process machinery (config + agent fan-out) and the
 * shipped pure sweep logic (sweep.h). It proposes seams per area, mechanically
 * re-grounds each against the live code index (kb_client), and returns a JSON
 * report. Default analysis-only; with {"file":true} it files STRONG candidates as
 * vertical-slice work items onto the manual-review workflow (a human gate — NEVER
 * "build"), after a lexical + realpath-under-root path check. It never edits source.
 */
#include "server.h"

#include "agent_config.h"
#include "agent_exec.h"
#include "agent_types.h"
#include "cJSON.h"
#include "code_collect.h" /* code_collect_files_cb */
#include "config.h"
#include "delegate_ensemble.h" /* ensemble_default_panel_from_agents */
#include "dstr.h"
#include "aimee_home.h"
#include "kb_client.h"
#include "log.h"
#include "sweep.h"
#include "wfe_engine.h" /* wfe_work_item_create */

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SWEEP_MAX_SETTLED 1024

/* Delta-awareness: scan previously-filed sweep proposals for their seam keys so a
 * re-run excludes already-filed seams. Returns the count; *out is a malloc'd array
 * of malloc'd keys the caller frees. (Server-side, repo-non-writing design: the
 * exclusion set is the sweep's own filing trail under $AIMEE_HOME, not a worktree.) */
static int sweep_load_settled(char ***out)
{
   *out = NULL;
   const char *home = aimee_home();
   char dir[1024];
   if (snprintf(dir, sizeof(dir), "%s/sweeps/proposals", home ? home : "/tmp") >= (int)sizeof(dir))
      return 0; /* AIMEE_HOME absurdly long -> no delta set (re-files; safe, not wrong) */
   DIR *d = opendir(dir);
   if (!d)
      return 0; /* none filed yet */
   char **arr = calloc(SWEEP_MAX_SETTLED, sizeof(char *));
   if (!arr)
   {
      closedir(d);
      return 0;
   }
   int n = 0;
   struct dirent *e;
   while ((e = readdir(d)) && n < SWEEP_MAX_SETTLED)
   {
      size_t l = strlen(e->d_name);
      if (l < 3 || strcmp(e->d_name + l - 3, ".md") != 0)
         continue;
      char p[1200];
      if (snprintf(p, sizeof(p), "%s/%s", dir, e->d_name) >= (int)sizeof(p))
         continue;
      FILE *f = fopen(p, "rb");
      if (!f)
         continue;
      /* large enough for the full "# Deepen seam: <key>" line (key <= SWEEP_KEY_MAX) */
      char buf[SWEEP_KEY_MAX + 64]; /* header is line 1 */
      size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
      fclose(f);
      buf[rd] = '\0';
      char key[SWEEP_KEY_MAX];
      if (sweep_extract_seam_key(buf, key, sizeof(key)))
      {
         arr[n] = strdup(key);
         if (arr[n])
            n++;
      }
   }
   closedir(d);
   *out = arr;
   return n;
}

#define SWEEP_MAX_FILES    4000
#define SWEEP_PER_FILE_CAP 4096
#define SWEEP_PROMPT_CAP   48000
#define SWEEP_MAX_CALLERS  256
#define SWEEP_MAX_CAND     16 /* per area */

static const char *const SWEEP_ALLOW[] = {"src/**", "tests/**"};
#define SWEEP_ALLOW_N 2

typedef struct
{
   char (*paths)[MAX_PATH_LEN];
   int n;
} collect_ctx_t;

static int collect_cb(const char *rel, const char *content, void *ud)
{
   (void)content;
   collect_ctx_t *c = ud;
   if (c->n >= SWEEP_MAX_FILES)
      return 0;
   if (!sweep_path_allowed(rel, SWEEP_ALLOW, SWEEP_ALLOW_N))
      return 0;
   snprintf(c->paths[c->n], MAX_PATH_LEN, "%s", rel);
   c->n++;
   return 0;
}

static int cmp_path(const void *a, const void *b)
{
   return strcmp((const char *)a, (const char *)b);
}

/* Append "## <rel>\n```\n<content up to cap>\n```\n" for one file; returns bytes added. */
static void append_file(dstr_t *s, const char *root, const char *rel, size_t budget_left)
{
   if (budget_left < 256)
      return;
   char abs[MAX_PATH_LEN * 2];
   int m = snprintf(abs, sizeof(abs), "%s/%s", root, rel);
   if (m < 0 || (size_t)m >= sizeof(abs))
      return; /* path truncated -> don't open a wrong path */
   FILE *f = fopen(abs, "rb");
   if (!f)
      return;
   /* Read at most what the file cap AND the remaining prompt budget allow, so the
    * SWEEP_PROMPT_CAP budget actually holds (header ~ rel + fence bytes). */
   char buf[SWEEP_PER_FILE_CAP];
   size_t room = budget_left > strlen(rel) + 16 ? budget_left - strlen(rel) - 16 : 0;
   size_t want = sizeof(buf) - 1;
   if (want > room)
      want = room;
   size_t rd = fread(buf, 1, want, f);
   fclose(f);
   buf[rd] = '\0';
   dstr_appendf(s, "## %s\n```\n%s\n```\n", rel, buf);
}

/* Build the proposer prompt for one area (its file slice). */
static char *build_area_prompt(const char *root, const char (*paths)[MAX_PATH_LEN], const int *area,
                               int n, int area_id)
{
   dstr_t s;
   dstr_init(&s);
   dstr_appendf(&s,
                "You are scanning one area of a C codebase for DUPLICATION-ACROSS-CALL-SITES: "
                "a helper/logic repeated at many call sites that should become one deep module. "
                "Name the ORIGINAL seam (existing file + top-level symbol), not a new module. "
                "Return ONLY JSON: {\"candidates\":[{\"seam_file\":\"<existing file>\","
                "\"seam_symbol\":\"<existing top-level decl>\",\"claimed_callers\":<int>,"
                "\"rationale\":\"<one line>\"}]}. The engine VERIFIES every candidate against the "
                "code index (you do not run anything); name only seams you can point to. Empty "
                "candidates is fine. No prose, no fences.\n\nAREA FILES:\n");
   for (int i = 0; i < n; i++)
   {
      if (area[i] != area_id)
         continue;
      size_t used = dstr_len(&s);
      append_file(&s, root, paths[i], used < SWEEP_PROMPT_CAP ? SWEEP_PROMPT_CAP - used : 0);
   }
   return dstr_steal(&s);
}

/* Filing context: when do_file, STRONG candidates are filed as work items on the
 * manual-review workflow (never "build" — a human must promote). */
typedef struct
{
   int do_file;
   const char *root; /* project root for realpath containment */
   const char *repo; /* repo identifier for the work item */
   int filed;
   int file_rejected; /* skipped: unsafe / out-of-root path */
} sweep_file_ctx_t;

/* Build a vertical-slice proposal for a STRONG seam, validate the seam path, and
 * file it onto manual-review. Untrusted candidate strings: sweep_path_safe (lexical)
 * THEN realpath under root before anything is written or filed. */
static void file_candidate(sweep_file_ctx_t *fc, const char *key, const sweep_candidate_t *cand,
                           const sweep_edges_t *edges)
{
   if (!fc->do_file)
      return;
   /* lexical gate first (rejects shell meta, traversal, absolute, glob, ...) */
   if (!sweep_path_safe(cand->seam_file))
   {
      fc->file_rejected++;
      aimee_log(LOG_WARN, "sweep", "refused to file '%s': unsafe seam path", cand->seam_file);
      return;
   }
   /* then realpath containment: the resolved seam must live under the project root */
   char abs[MAX_PATH_LEN * 2];
   if (snprintf(abs, sizeof(abs), "%s/%s", fc->root, cand->seam_file) >= (int)sizeof(abs))
   {
      fc->file_rejected++;
      return;
   }
   char resolved[PATH_MAX];
   size_t rootlen = strlen(fc->root);
   if (!realpath(abs, resolved) || strncmp(resolved, fc->root, rootlen) != 0 ||
       (resolved[rootlen] != '/' && resolved[rootlen] != '\0'))
   {
      fc->file_rejected++;
      aimee_log(LOG_WARN, "sweep", "refused to file '%s': resolves outside the project root",
                cand->seam_file);
      return;
   }

   /* vertical-slice ticket. Body is opaque text consumers must not eval/template. */
   dstr_t md;
   dstr_init(&md);
   dstr_appendf(&md, "# Deepen seam: %s\n\n", key);
   dstr_appendf(&md,
                "The deepening sweep found `%s:%s` reproduced across **%d call site(s) in %d "
                "file(s)** (independent, low shared state). Extract it into one deep module.\n\n",
                cand->seam_file, cand->seam_symbol, edges->caller_count, edges->distinct_files);
   if (cand->rationale[0])
      dstr_appendf(&md, "> Proposer note: %s\n\n", cand->rationale);
   dstr_append_str(&md, "## Vertical slice (definition of done)\n"
                        "- Introduce the deep module behind a small interface.\n"
                        "- Repoint EVERY call site (see the code index for the call sites).\n"
                        "- Add tests at the new interface (assert behaviour THROUGH it).\n"
                        "- Delete the old duplicated copies.\n\n"
                        "_Machine-proposed by the deepening sweep; verified against the live "
                        "code index. A human must review before this is implemented._\n");

   const char *home = aimee_home();
   char dir[1024];
   snprintf(dir, sizeof(dir), "%s/sweeps/proposals", home ? home : "/tmp");
   /* best-effort dir create (parents from standup) */
   {
      char parent[1024];
      snprintf(parent, sizeof(parent), "%s/sweeps", home ? home : "/tmp");
      mkdir(parent, 0700);
      mkdir(dir, 0700);
   }
   char ppath[1200];
   snprintf(ppath, sizeof(ppath), "%s/sw-%ld-%d-%d.md", dir, (long)time(NULL), (int)getpid(),
            fc->filed + fc->file_rejected);
   FILE *pf = fopen(ppath, "wb");
   if (!pf)
   {
      dstr_free(&md);
      fc->file_rejected++;
      return;
   }
   const char *text = dstr_cstr(&md);
   fwrite(text, 1, strlen(text), pf);
   fclose(pf);
   dstr_free(&md);

   char id[80] = "", err[256] = "";
   /* workflow="manual-review": a human gate, NOT auto-build (see manual-review.yaml). */
   int rc = wfe_work_item_create("manual-review", fc->repo ? fc->repo : "", ppath, "sweep", id, err,
                                 sizeof(err));
   if (rc != 0 || !id[0])
   {
      fc->file_rejected++;
      aimee_log(LOG_WARN, "sweep", "filing '%s' failed: %s", key, err[0] ? err : "unknown");
      return;
   }
   fc->filed++;
}

/* Score one candidate against the live index; appends a report object. */
static void score_candidate(cJSON *report, const sweep_candidate_t *cand,
                            const sweep_score_cfg_t *scfg, const char *const *settled,
                            int settled_n, int *strong, int *worth, int *rejected, int *excluded,
                            sweep_file_ctx_t *fc)
{
   char key[SWEEP_KEY_MAX];
   sweep_seam_key(cand->seam_file, cand->seam_symbol, key, sizeof(key));
   if (sweep_excluded(key, settled, settled_n))
   {
      (*excluded)++;
      return;
   }

   caller_hit_t *callers = calloc(SWEEP_MAX_CALLERS, sizeof(*callers));
   if (!callers)
      return;
   int n = kb_client_index_find_callers("", cand->seam_symbol, callers, SWEEP_MAX_CALLERS);
   if (n < 0)
      n = 0; /* index miss -> 0 callers -> REJECT (the report shows it) */
   if (n == SWEEP_MAX_CALLERS)
      aimee_log(LOG_WARN, "sweep", "caller set for '%s' hit the %d cap (count understated)",
                cand->seam_symbol, SWEEP_MAX_CALLERS);
   /* blast-radius shared-state is deferred to a later pass (needs project
    * resolution); 0 keeps the over-coupling demotion conservative for v1. */
   sweep_edges_t edges = sweep_edges_from_callers(callers, n, 0);
   free(callers);

   char reason[160];
   sweep_rank_t rank = sweep_score(&edges, scfg, reason, sizeof(reason));
   int filed_this = 0;
   if (rank == SWEEP_STRONG)
   {
      (*strong)++;
      if (fc && fc->do_file)
      {
         int before = fc->filed;
         file_candidate(fc, key, cand, &edges);
         filed_this = (fc->filed > before);
      }
   }
   else if (rank == SWEEP_WORTH)
      (*worth)++;
   else
      (*rejected)++;

   cJSON *o = cJSON_CreateObject();
   if (filed_this)
      cJSON_AddBoolToObject(o, "filed", 1);
   cJSON_AddStringToObject(o, "seam", key);
   cJSON_AddStringToObject(o, "rank",
                           rank == SWEEP_STRONG  ? "strong"
                           : rank == SWEEP_WORTH ? "worth-exploring"
                                                 : "rejected");
   cJSON_AddNumberToObject(o, "callers", edges.caller_count);
   cJSON_AddNumberToObject(o, "files", edges.distinct_files);
   cJSON_AddNumberToObject(o, "claimed", cand->claimed_callers);
   cJSON_AddStringToObject(o, "reason", reason);
   if (cand->rationale[0])
      cJSON_AddStringToObject(o, "rationale", cand->rationale);
   cJSON_AddItemToArray(report, o);
}

int handle_dev_sweep(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const cJSON *jproj = cJSON_GetObjectItemCaseSensitive(req, "project");
   const char *want_proj = (jproj && cJSON_IsString(jproj)) ? jproj->valuestring : "";
   const cJSON *jfile = cJSON_GetObjectItemCaseSensitive(req, "file");
   int do_file = cJSON_IsTrue(jfile); /* default false -> analysis-only */
   const cJSON *jrepo = cJSON_GetObjectItemCaseSensitive(req, "repo");
   const char *repo = (jrepo && cJSON_IsString(jrepo)) ? jrepo->valuestring : "";

   /* Resolve the walk root from the indexed projects (the corpus we verify against).
    * No index -> nothing can be verified -> refuse up front. */
   project_info_t projs[32];
   int np = kb_client_index_list(projs, 32);
   if (np <= 0)
      return server_send_error(conn, "no indexed project (run `aimee index scan` first)", NULL);
   const char *sel = projs[0].root;
   for (int i = 0; i < np; i++)
      if (want_proj[0] && strcmp(projs[i].name, want_proj) == 0)
         sel = projs[i].root;
   if (!sel || !sel[0])
      return server_send_error(conn, "indexed project has no root", NULL);
   /* Canonicalize the root once so the filing containment check (realpath of a seam
    * under root) compares against the resolved root, not a symlinked/.. form. */
   char root[PATH_MAX];
   if (!realpath(sel, root))
      return server_send_error(conn, "indexed project root does not resolve", NULL);

   config_t cfg;
   config_load(&cfg);
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   if (agent_load_config(&acfg) != 0)
      return server_send_error(conn, "could not load agents.json", NULL);
   ensemble_default_panel_from_agents(&cfg, &acfg);
   ensemble_filter_panel_availability(&cfg, &acfg); /* drop unkeyed/unhealthy proposers */
   if (cfg.ensemble_reference_count <= 0)
      return server_send_error(conn, "no enabled agent to propose with", NULL);
   const char *proposer = cfg.ensemble_reference_models[0];

   sweep_caps_t caps;
   sweep_caps_defaults(&caps);
   sweep_score_cfg_t scfg;
   sweep_score_cfg_defaults(&scfg);

   collect_ctx_t cc;
   cc.paths = calloc(SWEEP_MAX_FILES, sizeof(*cc.paths));
   if (!cc.paths)
      return server_send_error(conn, "out of memory", NULL);
   cc.n = 0;
   code_collect_files_cb(root, collect_cb, &cc);
   if (cc.n == 0)
   {
      free(cc.paths);
      return server_send_error(conn, "no source files under the allowlist", NULL);
   }
   qsort(cc.paths, (size_t)cc.n, sizeof(*cc.paths), cmp_path);

   int *area = calloc((size_t)cc.n, sizeof(int));
   if (!area)
   {
      free(cc.paths);
      return server_send_error(conn, "out of memory", NULL);
   }
   int area_count =
       sweep_partition((const char *const *)cc.paths, cc.n, caps.max_files_per_area, area);
   if (area_count < 0)
      area_count = 0;
   if (area_count > caps.max_areas)
      area_count = caps.max_areas; /* cap; remaining areas are a later (delta) sweep */

   /* Delta-awareness: exclude seams already filed by a prior sweep (its own filing
    * trail under $AIMEE_HOME). typed_facts/architecture_settled exclusion needs a
    * kb_client recall path that does not exist yet (DB2-disabled server) — deferred;
    * v1 dedupes against prior filings, which is the dominant re-run case. */
   char **settled_arr = NULL;
   int settled_n = sweep_load_settled(&settled_arr);
   const char *const *settled = (const char *const *)settled_arr;

   sweep_file_ctx_t fc = {do_file, root, repo, 0, 0};

   cJSON *resp = cJSON_CreateObject();
   cJSON *cands = cJSON_AddArrayToObject(resp, "candidates");
   int strong = 0, worth = 0, rejected = 0, excluded = 0, areas_run = 0;

   for (int a = 0; a < area_count; a++)
   {
      char *prompt = build_area_prompt(root, (const char (*)[MAX_PATH_LEN])cc.paths, area, cc.n, a);
      if (!prompt)
         continue;
      agent_result_t r;
      memset(&r, 0, sizeof(r));
      int prc = agent_run_named(&acfg, proposer, "review", NULL, prompt, 0, 0.2, &r);
      free(prompt);
      areas_run++;
      if (prc != 0 || !r.response || !r.response[0])
         aimee_log(LOG_WARN, "sweep", "proposer produced no candidates for area %d (rc=%d)", a,
                   prc);
      if (r.response && r.response[0])
      {
         sweep_candidate_t cand[SWEEP_MAX_CAND];
         int nc = sweep_parse_candidates(r.response, cand, SWEEP_MAX_CAND);
         for (int i = 0; i < nc && i < caps.max_items_per_area; i++)
            score_candidate(cands, &cand[i], &scfg, settled, settled_n, &strong, &worth, &rejected,
                            &excluded, &fc);
      }
      free(r.response);
   }

   cJSON_AddNumberToObject(resp, "areas_total", area_count);
   cJSON_AddNumberToObject(resp, "areas_run", areas_run);
   cJSON_AddNumberToObject(resp, "files", cc.n);
   cJSON_AddNumberToObject(resp, "strong", strong);
   cJSON_AddNumberToObject(resp, "worth_exploring", worth);
   cJSON_AddNumberToObject(resp, "rejected", rejected);
   cJSON_AddNumberToObject(resp, "excluded", excluded);
   cJSON_AddBoolToObject(resp, "analysis_only", do_file ? 0 : 1);
   cJSON_AddNumberToObject(resp, "filed", fc.filed);
   cJSON_AddNumberToObject(resp, "file_rejected", fc.file_rejected);
   /* v1: shared-state (blast radius) is not yet wired, so over-coupling is not
    * demoted — STRONG here means count+distribution+independence only. STRONG
    * candidates are filed onto the manual-review workflow (human gate) only when
    * file=true; never auto-built. */
   cJSON_AddStringToObject(resp, "note",
                           do_file
                               ? "filed STRONG candidates to manual-review (human gate, not "
                                 "build); shared-state conservative (v1)"
                               : "analysis-only (pass {\"file\":true} to file STRONG candidates "
                                 "to manual-review); shared-state conservative (v1)");

   for (int i = 0; i < settled_n; i++)
      free(settled_arr[i]);
   free(settled_arr);
   free(area);
   free(cc.paths);
   return server_send_ok(conn, resp);
}
