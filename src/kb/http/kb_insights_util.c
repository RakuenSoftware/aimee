/* kb_insights_util.c: pure helpers for /v1/insights/spend (P3b). See kb_insights_util.h.
 * No db2/tenant/router deps — only cJSON — so it links into a small unit test. */

#include "kb_insights_util.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCALE10 10000000000LL /* 10^10 — the NUMERIC(20,10) fractional scale */

int kb_insights_date_valid(const char *s)
{
   if (!s)
      return 0;
   /* Shape: exactly YYYY-MM-DD, digits and dashes in the right places. */
   for (int i = 0; i < 10; ++i)
   {
      char c = s[i];
      if (i == 4 || i == 7)
      {
         if (c != '-')
            return 0;
      }
      else if (c < '0' || c > '9')
         return 0;
   }
   if (s[10] != '\0')
      return 0;
   int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
   int m = (s[5] - '0') * 10 + (s[6] - '0');
   int d = (s[8] - '0') * 10 + (s[9] - '0');
   if (m < 1 || m > 12 || d < 1)
      return 0;
   static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
   int dim = mdays[m - 1];
   if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)))
      dim = 29;
   return d <= dim;
}

/* Parse one non-negative scale-10 decimal string into whole + fractional (0..10^10-1)
 * integer parts. Tolerates a missing '.' (integer) and pads/truncates the fraction to
 * exactly 10 digits. A leading '-' (should never occur for a cost) is ignored so the
 * magnitude still accumulates rather than corrupting the sum. */
static void cost_parse(const char *s, long long *whole, long long *frac)
{
   *whole = 0;
   *frac = 0;
   if (!s)
      return;
   if (*s == '-' || *s == '+')
      ++s;
   long long w = 0;
   while (*s >= '0' && *s <= '9')
   {
      w = w * 10 + (*s - '0');
      ++s;
   }
   long long f = 0;
   if (*s == '.')
   {
      ++s;
      for (int i = 0; i < 10; ++i)
      {
         f *= 10;
         if (*s >= '0' && *s <= '9')
         {
            f += (*s - '0');
            ++s;
         }
      }
   }
   *whole = w;
   *frac = f;
}

void kb_insights_cost_sum(const char *const *costs, int n, char *out, size_t cap)
{
   long long whole_acc = 0, frac_acc = 0;
   for (int i = 0; i < n; ++i)
   {
      long long w, f;
      cost_parse(costs[i], &w, &f);
      whole_acc += w;
      frac_acc += f;
   }
   whole_acc += frac_acc / SCALE10;
   frac_acc %= SCALE10;
   /* Canonical 'W.FFFFFFFFFF' — 10 zero-padded fractional digits. */
   snprintf(out, cap, "%lld.%010lld", whole_acc, frac_acc);
}

/* Sum the tokens/calls (int64) + cost (exact decimal) over rows[idx[0..k)]. Emits an
 * object with the standard field set; cost_usd is a JSON STRING (numeric), never a
 * float. If label_key != NULL, the label (billable_model text) is added under it; if
 * proj_key != NULL, the project id (or null) is added under it. */
static cJSON *agg_object(const db2_org_spend_row_t *rows, const int *idx, int k,
                         const char *label_key, const char *label_val, const char *proj_key,
                         int proj_has, long long proj_id, const char *team_key, long long team_val)
{
   cJSON *o = cJSON_CreateObject();
   if (!o)
      return NULL;
   long long pt = 0, ct = 0, crt = 0, cwt = 0, calls = 0;
   const char *costs[512];
   int nc = 0;
   for (int i = 0; i < k; ++i)
   {
      const db2_org_spend_row_t *r = &rows[idx[i]];
      pt += r->prompt_tokens;
      ct += r->completion_tokens;
      crt += r->cache_read_tokens;
      cwt += r->cache_write_tokens;
      calls += r->calls;
      if (nc < (int)(sizeof(costs) / sizeof(costs[0])))
         costs[nc++] = r->cost_usd;
   }
   char cost[DB2_SPEND_COST_CAP];
   kb_insights_cost_sum(costs, nc, cost, sizeof(cost));
   if (team_key)
      cJSON_AddNumberToObject(o, team_key, (double)team_val);
   if (proj_key)
   {
      if (proj_has)
         cJSON_AddNumberToObject(o, proj_key, (double)proj_id);
      else
         cJSON_AddNullToObject(o, proj_key);
   }
   if (label_key)
      cJSON_AddStringToObject(o, label_key, label_val ? label_val : "");
   cJSON_AddNumberToObject(o, "prompt_tokens", (double)pt);
   cJSON_AddNumberToObject(o, "completion_tokens", (double)ct);
   cJSON_AddNumberToObject(o, "cache_read_tokens", (double)crt);
   cJSON_AddNumberToObject(o, "cache_write_tokens", (double)cwt);
   cJSON_AddStringToObject(o, "cost_usd", cost); /* NUMERIC string — never a float */
   cJSON_AddNumberToObject(o, "calls", (double)calls);
   return o;
}

char *kb_insights_spend_json(int has_team, long long team, int has_project, long long project,
                             const char *since, const char *until,
                             const db2_org_spend_row_t *rows, int n)
{
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return NULL;
   if (has_team)
      cJSON_AddNumberToObject(root, "team", (double)team);
   else
      cJSON_AddNullToObject(root, "team");
   if (has_project)
      cJSON_AddNumberToObject(root, "project", (double)project);
   cJSON_AddStringToObject(root, "since", since ? since : "");
   cJSON_AddStringToObject(root, "until", until ? until : "");

   int all[512];
   int na = 0;
   for (int i = 0; i < n && na < (int)(sizeof(all) / sizeof(all[0])); ++i)
      all[na++] = i;

   /* total: every row. */
   cJSON *total = agg_object(rows, all, na, NULL, NULL, NULL, 0, 0, NULL, 0);
   cJSON_AddItemToObject(root, "total", total);

   /* by_team: group by team_id (org-wide report's key breakdown; one entry when
    * team-scoped). Preserves the team dimension the definer now keeps. */
   cJSON *by_team = cJSON_CreateArray();
   {
      int done[512] = {0};
      for (int i = 0; i < n; ++i)
      {
         if (done[i])
            continue;
         int grp[512];
         int gk = 0;
         for (int j = i; j < n; ++j)
            if (!done[j] && rows[j].team_id == rows[i].team_id)
            {
               done[j] = 1;
               if (gk < (int)(sizeof(grp) / sizeof(grp[0])))
                  grp[gk++] = j;
            }
         cJSON *o = agg_object(rows, grp, gk, NULL, NULL, NULL, 0, 0, "team_id",
                               (long long)rows[i].team_id);
         cJSON_AddItemToArray(by_team, o);
      }
   }
   cJSON_AddItemToObject(root, "by_team", by_team);

   /* by_model: group by billable_model (sum across projects). */
   cJSON *by_model = cJSON_CreateArray();
   {
      int done[512] = {0};
      for (int i = 0; i < n; ++i)
      {
         if (done[i])
            continue;
         int grp[512];
         int gk = 0;
         for (int j = i; j < n; ++j)
            if (!done[j] && strcmp(rows[j].billable_model, rows[i].billable_model) == 0)
            {
               done[j] = 1;
               if (gk < (int)(sizeof(grp) / sizeof(grp[0])))
                  grp[gk++] = j;
            }
         cJSON *o = agg_object(rows, grp, gk, "billable_model", rows[i].billable_model, NULL, 0, 0,
                               NULL, 0);
         cJSON_AddItemToArray(by_model, o);
      }
   }
   cJSON_AddItemToObject(root, "by_model", by_model);

   /* by_project: group by project_id (NULL project is its own group). */
   cJSON *by_project = cJSON_CreateArray();
   {
      int done[512] = {0};
      for (int i = 0; i < n; ++i)
      {
         if (done[i])
            continue;
         int grp[512];
         int gk = 0;
         for (int j = i; j < n; ++j)
         {
            if (done[j])
               continue;
            int same = (rows[j].has_project == rows[i].has_project) &&
                       (!rows[i].has_project || rows[j].project_id == rows[i].project_id);
            if (same)
            {
               done[j] = 1;
               if (gk < (int)(sizeof(grp) / sizeof(grp[0])))
                  grp[gk++] = j;
            }
         }
         cJSON *o = agg_object(rows, grp, gk, NULL, NULL, "project_id", rows[i].has_project,
                               rows[i].project_id, NULL, 0);
         cJSON_AddItemToArray(by_project, o);
      }
   }
   cJSON_AddItemToObject(root, "by_project", by_project);

   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   return s;
}
