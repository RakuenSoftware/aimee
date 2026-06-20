/* server_sweep.c: server-side deepening-sweep handler (Part B PR-B3b).
 *
 * Reuses the roundtable's in-process machinery (config + agent fan-out) and the
 * shipped pure sweep logic (sweep.h). Read-only analysis: it proposes seams per
 * area, mechanically re-grounds each against the live code index (kb_client), and
 * returns a JSON report. It does NOT file work items (that is PR-B4) or edit source.
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
#include "kb_client.h"
#include "log.h"
#include "sweep.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Score one candidate against the live index; appends a report object. */
static void score_candidate(cJSON *report, const sweep_candidate_t *cand,
                            const sweep_score_cfg_t *scfg, const char *const *settled,
                            int settled_n, int *strong, int *worth, int *rejected, int *excluded)
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
   if (rank == SWEEP_STRONG)
      (*strong)++;
   else if (rank == SWEEP_WORTH)
      (*worth)++;
   else
      (*rejected)++;

   cJSON *o = cJSON_CreateObject();
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

   /* Resolve the walk root from the indexed projects (the corpus we verify against).
    * No index -> nothing can be verified -> refuse up front. */
   project_info_t projs[32];
   int np = kb_client_index_list(projs, 32);
   if (np <= 0)
      return server_send_error(conn, "no indexed project (run `aimee index scan` first)", NULL);
   const char *root = projs[0].root;
   for (int i = 0; i < np; i++)
      if (want_proj[0] && strcmp(projs[i].name, want_proj) == 0)
         root = projs[i].root;
   if (!root || !root[0])
      return server_send_error(conn, "indexed project has no root", NULL);

   config_t cfg;
   config_load(&cfg);
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   if (agent_load_config(&acfg) != 0)
      return server_send_error(conn, "could not load agents.json", NULL);
   ensemble_default_panel_from_agents(&cfg, &acfg);
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

   /* Exclusion set (proposals/work-queue/typed_facts) is wired in PR-B4; v1 has
    * an empty settled set so nothing is excluded yet. */
   const char *const *settled = NULL;
   int settled_n = 0;

   cJSON *resp = cJSON_CreateObject();
   cJSON *cands = cJSON_AddArrayToObject(resp, "candidates");
   int strong = 0, worth = 0, rejected = 0, excluded = 0, areas_run = 0;

   for (int a = 0; a < area_count; a++)
   {
      char *prompt = build_area_prompt(root, (const char(*)[MAX_PATH_LEN])cc.paths, area, cc.n, a);
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
                            &excluded);
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
   cJSON_AddBoolToObject(resp, "analysis_only", 1);
   /* v1: shared-state (blast radius) is not yet wired, so over-coupling is not
    * demoted — STRONG here means count+distribution+independence only. */
   cJSON_AddStringToObject(resp, "note",
                           "v1: shared-state/blast-radius conservative (0); "
                           "filing is a later phase — nothing was filed");

   free(area);
   free(cc.paths);
   return server_send_ok(conn, resp);
}
