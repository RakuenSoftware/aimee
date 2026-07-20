/* test_p3b_spend.c: P3b org spend reporting — pure C bits.
 *
 * Covers the two dependency-light halves of the reporting surface (kb_insights_util.c):
 *   1. boundary ISO-date validation (kb_insights_date_valid) — the 400 gate the HTTP
 *      route applies BEFORE the definer call;
 *   2. response JSON assembly (kb_insights_spend_json) — the total/by_model/by_project
 *      derivation, with the two acceptance invariants: cost_usd is ALWAYS a JSON STRING
 *      (never a float) and sum(by_model.cost_usd) == total.cost_usd EXACTLY (scale-10
 *      fixed-point, no floating-point drift). The authz gate lives in SQL (SECURITY
 *      DEFINER) and is proven by scripts/p3b_spend_rls_test.sql on real Postgres. */
#include "cJSON.h"
#include "kb_insights_util.h"
#include "org_spend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg)                                                                            \
   do                                                                                               \
   {                                                                                                \
      if (!(cond))                                                                                  \
      {                                                                                             \
         printf("FAIL: %s\n", msg);                                                                 \
         failures++;                                                                                \
      }                                                                                             \
   } while (0)

static void set_row(db2_org_spend_row_t *r, long long team, int has_project, long long project,
                    const char *model, long long pt, long long ct, long long crt, long long cwt,
                    const char *cost, long long calls)
{
   memset(r, 0, sizeof(*r));
   r->team_id = team;
   r->has_project = has_project;
   r->project_id = project;
   snprintf(r->billable_model, sizeof(r->billable_model), "%s", model);
   r->prompt_tokens = pt;
   r->completion_tokens = ct;
   r->cache_read_tokens = crt;
   r->cache_write_tokens = cwt;
   snprintf(r->cost_usd, sizeof(r->cost_usd), "%s", cost);
   r->calls = calls;
}

int main(void)
{
   /* ---- date validation ---- */
   CHECK(kb_insights_date_valid("2026-07-19"), "ordinary date is valid");
   CHECK(kb_insights_date_valid("2024-02-29"), "leap-year Feb 29 is valid");
   CHECK(!kb_insights_date_valid("2026-02-29"), "non-leap Feb 29 is rejected");
   CHECK(!kb_insights_date_valid("2026-13-40"), "out-of-range month/day rejected");
   CHECK(!kb_insights_date_valid("2026-00-10"), "month 00 rejected");
   CHECK(!kb_insights_date_valid("2026-01-00"), "day 00 rejected");
   CHECK(!kb_insights_date_valid("2026-7-19"), "non-zero-padded is rejected");
   CHECK(!kb_insights_date_valid("2026/07/19"), "wrong separator rejected");
   CHECK(!kb_insights_date_valid("2026-07-19x"), "trailing junk rejected");
   CHECK(!kb_insights_date_valid("2026-07-1"), "too short rejected");
   CHECK(!kb_insights_date_valid(""), "empty rejected");
   CHECK(!kb_insights_date_valid(NULL), "NULL rejected");

   /* ---- exact scale-10 cost summation (no float drift) ---- */
   {
      const char *costs[] = {"0.0000000001", "0.0000000002", "0.0000000003"};
      char out[64];
      kb_insights_cost_sum(costs, 3, out, sizeof(out));
      CHECK(strcmp(out, "0.0000000006") == 0, "sub-cent decimals sum exactly");
      const char *big[] = {"1.5000000000", "2.7500000000"};
      kb_insights_cost_sum(big, 2, out, sizeof(out));
      CHECK(strcmp(out, "4.2500000000") == 0, "dollar-scale decimals sum exactly");
      const char *carry[] = {"0.9999999999", "0.0000000001"};
      kb_insights_cost_sum(carry, 2, out, sizeof(out));
      CHECK(strcmp(out, "1.0000000000") == 0, "fractional carry propagates to whole");
   }

   /* ---- JSON shape + reconciliation ---- */
   db2_org_spend_row_t rows[4];
   /* two teams x two projects x two models, distinct costs. */
   set_row(&rows[0], 940001, 1, 100, "modelA", 10, 5, 0, 0, "0.0020000000", 1);
   set_row(&rows[1], 940001, 1, 100, "modelB", 20, 10, 0, 0, "0.0040000000", 2);
   set_row(&rows[2], 940002, 1, 200, "modelA", 30, 15, 0, 0, "0.0060000000", 3);
   set_row(&rows[3], 940002, 0, 0, "modelB", 40, 20, 0, 0, "0.0080000000", 4);

   char *json = kb_insights_spend_json(1, 940001, 0, 0, "2026-07-01", "2026-07-31", rows, 4);
   CHECK(json != NULL, "spend json built");
   cJSON *root = json ? cJSON_Parse(json) : NULL;
   CHECK(root != NULL, "spend json parses");
   if (root)
   {
      cJSON *team = cJSON_GetObjectItemCaseSensitive(root, "team");
      CHECK(cJSON_IsNumber(team) && team->valuedouble == 940001, "team echoed");
      CHECK(cJSON_GetObjectItemCaseSensitive(root, "project") == NULL, "no project field when absent");

      cJSON *total = cJSON_GetObjectItemCaseSensitive(root, "total");
      cJSON *tcost = total ? cJSON_GetObjectItemCaseSensitive(total, "cost_usd") : NULL;
      CHECK(cJSON_IsString(tcost), "total.cost_usd is a JSON string, not a float");
      CHECK(tcost && strcmp(tcost->valuestring, "0.0200000000") == 0, "total cost reconciles");
      cJSON *tpt = total ? cJSON_GetObjectItemCaseSensitive(total, "prompt_tokens") : NULL;
      CHECK(cJSON_IsNumber(tpt) && tpt->valuedouble == 100, "total prompt_tokens = 100");
      cJSON *tcalls = total ? cJSON_GetObjectItemCaseSensitive(total, "calls") : NULL;
      CHECK(cJSON_IsNumber(tcalls) && tcalls->valuedouble == 10, "total calls = 10");

      /* by_model cost strings must sum to the total cost exactly. */
      cJSON *by_model = cJSON_GetObjectItemCaseSensitive(root, "by_model");
      CHECK(cJSON_IsArray(by_model) && cJSON_GetArraySize(by_model) == 2, "two models");
      const char *mcosts[8];
      int mc = 0;
      cJSON *it = NULL;
      cJSON_ArrayForEach(it, by_model)
      {
         cJSON *c = cJSON_GetObjectItemCaseSensitive(it, "cost_usd");
         CHECK(cJSON_IsString(c), "by_model cost_usd is a string");
         CHECK(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(it, "billable_model")),
               "by_model has billable_model");
         if (cJSON_IsString(c))
            mcosts[mc++] = c->valuestring;
      }
      char msum[64];
      kb_insights_cost_sum(mcosts, mc, msum, sizeof(msum));
      CHECK(tcost && strcmp(msum, tcost->valuestring) == 0,
            "sum(by_model.cost_usd) == total.cost_usd");

      /* by_project: three groups (100, 200, and the NULL-project row). */
      cJSON *by_project = cJSON_GetObjectItemCaseSensitive(root, "by_project");
      CHECK(cJSON_IsArray(by_project) && cJSON_GetArraySize(by_project) == 3, "three project groups");
      int saw_null = 0;
      cJSON *pit = NULL;
      cJSON_ArrayForEach(pit, by_project)
      {
         cJSON *pid = cJSON_GetObjectItemCaseSensitive(pit, "project_id");
         if (cJSON_IsNull(pid))
            saw_null = 1;
         CHECK(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(pit, "cost_usd")),
               "by_project cost_usd is a string");
      }
      CHECK(saw_null, "NULL-project row surfaces as project_id:null");

      /* by_team: two teams; team_id preserved (org-wide report stays team-aware), and
       * the per-team costs reconcile to the total exactly. */
      cJSON *by_team = cJSON_GetObjectItemCaseSensitive(root, "by_team");
      CHECK(cJSON_IsArray(by_team) && cJSON_GetArraySize(by_team) == 2, "two team groups");
      const char *tcosts[8];
      int tc = 0;
      int saw_t1 = 0, saw_t2 = 0;
      cJSON *tit = NULL;
      cJSON_ArrayForEach(tit, by_team)
      {
         cJSON *tid = cJSON_GetObjectItemCaseSensitive(tit, "team_id");
         CHECK(cJSON_IsNumber(tid), "by_team has a numeric team_id");
         if (cJSON_IsNumber(tid) && tid->valuedouble == 940001)
            saw_t1 = 1;
         if (cJSON_IsNumber(tid) && tid->valuedouble == 940002)
            saw_t2 = 1;
         cJSON *c = cJSON_GetObjectItemCaseSensitive(tit, "cost_usd");
         CHECK(cJSON_IsString(c), "by_team cost_usd is a string");
         if (cJSON_IsString(c))
            tcosts[tc++] = c->valuestring;
      }
      CHECK(saw_t1 && saw_t2, "both teams preserved (no cross-team merge)");
      char tsum[64];
      kb_insights_cost_sum(tcosts, tc, tsum, sizeof(tsum));
      CHECK(tcost && strcmp(tsum, tcost->valuestring) == 0,
            "sum(by_team.cost_usd) == total.cost_usd");

      cJSON_Delete(root);
   }
   if (json)
      free(json);

   /* org-wide (team absent) emits team:null. */
   char *j2 = kb_insights_spend_json(0, 0, 0, 0, "2026-07-01", "2026-07-31", rows, 1);
   cJSON *r2 = j2 ? cJSON_Parse(j2) : NULL;
   if (r2)
   {
      CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(r2, "team")),
            "team is null for the org-wide report");
      cJSON_Delete(r2);
   }
   if (j2)
      free(j2);

   /* Large-set regression: well past the former fixed 512-element scratch cap. Prove the
    * aggregation neither drops groups nor mis-reconciles at scale (fix #1: no silent
    * truncation in the util; overflow beyond DB2_SPEND_MAX_ROWS is the db2 layer's job). */
   {
      const int N = 1000;
      db2_org_spend_row_t *big = calloc((size_t)N, sizeof(*big));
      CHECK(big != NULL, "large-set alloc");
      if (big)
      {
         const char **bcosts = calloc((size_t)N, sizeof(char *));
         for (int i = 0; i < N; ++i)
            /* distinct team per row -> N by_team groups; 1 cent each. */
            set_row(&big[i], 950000 + i, 0, 0, "modelZ", 1, 1, 0, 0, "0.0100000000", 1);
         char *jb = kb_insights_spend_json(0, 0, 0, 0, "2026-01-01", "2026-12-31", big, N);
         cJSON *rb = jb ? cJSON_Parse(jb) : NULL;
         CHECK(rb != NULL, "large-set json parses");
         if (rb)
         {
            cJSON *bt = cJSON_GetObjectItemCaseSensitive(rb, "by_team");
            CHECK(cJSON_IsArray(bt) && cJSON_GetArraySize(bt) == N,
                  "all 1000 teams present (no 512 truncation)");
            cJSON *tot = cJSON_GetObjectItemCaseSensitive(rb, "total");
            cJSON *tcst = tot ? cJSON_GetObjectItemCaseSensitive(tot, "cost_usd") : NULL;
            /* 1000 * 0.01 = 10.00 exactly. */
            CHECK(tcst && strcmp(tcst->valuestring, "10.0000000000") == 0,
                  "large-set total reconciles exactly (10.0)");
            int bt_n = 0;
            cJSON *bit = NULL;
            cJSON_ArrayForEach(bit, bt)
            {
               cJSON *c = cJSON_GetObjectItemCaseSensitive(bit, "cost_usd");
               if (cJSON_IsString(c) && bt_n < N)
                  bcosts[bt_n++] = c->valuestring;
            }
            char bsum[64];
            kb_insights_cost_sum(bcosts, bt_n, bsum, sizeof(bsum));
            CHECK(tcst && strcmp(bsum, tcst->valuestring) == 0,
                  "sum(by_team) == total across 1000 groups");
            cJSON_Delete(rb);
         }
         free(jb);
         free(bcosts);
         free(big);
      }
   }

   if (failures == 0)
      printf("test_p3b_spend: ALL PASS\n");
   else
      printf("test_p3b_spend: %d FAILURE(S)\n", failures);
   return failures == 0 ? 0 : 1;
}
