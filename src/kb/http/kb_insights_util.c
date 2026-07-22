/* kb_insights_util.c: pure helpers for /v1/insights/spend (P3b). See kb_insights_util.h.
 * No db2/tenant/router deps — only cJSON — so it links into a small unit test.
 *
 * The row count is bounded upstream: db2_org_spend_query returns DB2_SPEND_ERR_TOOBIG
 * (never a truncated set) once the grouped result would exceed DB2_SPEND_MAX_ROWS, so by
 * the time rows reach here n is a complete set (n <= DB2_SPEND_MAX_ROWS). The grouping
 * scratch buffers are therefore sized to n (heap, freed here) rather than a fixed cap
 * that could silently drop groups — total/by_team/by_model/by_project ALWAYS reconcile. */

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

/* Normalize accumulated (whole, frac) into canonical 'W.FFFFFFFFFF' text (10 fractional
 * digits, no floating point). */
static void cost_format(long long whole_acc, long long frac_acc, char *out, size_t cap)
{
   whole_acc += frac_acc / SCALE10;
   frac_acc %= SCALE10;
   snprintf(out, cap, "%lld.%010lld", whole_acc, frac_acc);
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
   cost_format(whole_acc, frac_acc, out, cap);
}

/* Sum tokens/calls (int64) + cost (exact decimal) over rows[idx[0..k)], or over rows
 * [0..k) when idx==NULL. Emits an object with the standard field set; cost_usd is a JSON
 * STRING (numeric), never a float. Cost is summed inline (incremental accumulator) so no
 * per-group array is needed. team_key/proj_key/label_key add the group's key field. */
static cJSON *agg_object(const db2_org_spend_row_t *rows, const int *idx, int k,
                         const char *label_key, const char *label_val, const char *proj_key,
                         int proj_has, long long proj_id, const char *team_key, long long team_val)
{
   cJSON *o = cJSON_CreateObject();
   if (!o)
      return NULL;
   long long pt = 0, ct = 0, crt = 0, cwt = 0, calls = 0;
   long long whole_acc = 0, frac_acc = 0;
   for (int i = 0; i < k; ++i)
   {
      const db2_org_spend_row_t *r = &rows[idx ? idx[i] : i];
      pt += r->prompt_tokens;
      ct += r->completion_tokens;
      crt += r->cache_read_tokens;
      cwt += r->cache_write_tokens;
      calls += r->calls;
      long long w, f;
      cost_parse(r->cost_usd, &w, &f);
      whole_acc += w;
      frac_acc += f;
   }
   char cost[DB2_SPEND_COST_CAP];
   cost_format(whole_acc, frac_acc, cost, sizeof(cost));
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

/* Group kind for the single grouping helper below. */
typedef enum
{
   GROUP_TEAM = 0,
   GROUP_MODEL = 1,
   GROUP_PROJECT = 2,
} group_kind_t;

/* Do rows a and b belong to the same group for this kind? */
static int same_group(const db2_org_spend_row_t *a, const db2_org_spend_row_t *b, group_kind_t kind)
{
   switch (kind)
   {
   case GROUP_TEAM:
      return a->team_id == b->team_id;
   case GROUP_MODEL:
      return strcmp(a->billable_model, b->billable_model) == 0;
   case GROUP_PROJECT:
   default:
      return (a->has_project == b->has_project) &&
             (!a->has_project || a->project_id == b->project_id);
   }
}

/* Build a by_<kind> array over rows[0..n), reusing the caller-provided scratch buffers
 * (grp: n ints, done: n bytes — both sized to n upstream, so no group is ever dropped).
 * Returns a new cJSON array (caller owns), or NULL on OOM. */
static cJSON *group_array(const db2_org_spend_row_t *rows, int n, group_kind_t kind, int *grp,
                          char *done)
{
   cJSON *arr = cJSON_CreateArray();
   if (!arr)
      return NULL;
   for (int i = 0; i < n; ++i)
      done[i] = 0;
   for (int i = 0; i < n; ++i)
   {
      if (done[i])
         continue;
      int gk = 0;
      for (int j = i; j < n; ++j)
         if (!done[j] && same_group(&rows[j], &rows[i], kind))
         {
            done[j] = 1;
            grp[gk++] = j;
         }
      cJSON *o = NULL;
      if (kind == GROUP_TEAM)
         o = agg_object(rows, grp, gk, NULL, NULL, NULL, 0, 0, "team_id",
                        (long long)rows[i].team_id);
      else if (kind == GROUP_MODEL)
         o = agg_object(rows, grp, gk, "billable_model", rows[i].billable_model, NULL, 0, 0, NULL,
                        0);
      else
         o = agg_object(rows, grp, gk, NULL, NULL, "project_id", rows[i].has_project,
                        rows[i].project_id, NULL, 0);
      cJSON_AddItemToArray(arr, o);
   }
   return arr;
}

char *kb_insights_spend_json(int has_team, long long team, int has_project, long long project,
                             const char *since, const char *until, const db2_org_spend_row_t *rows,
                             int n)
{
   if (n < 0)
      n = 0;
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

   /* total: every row (idx==NULL means 0..n). */
   cJSON_AddItemToObject(root, "total", agg_object(rows, NULL, n, NULL, NULL, NULL, 0, 0, NULL, 0));

   /* Scratch for the three groupings, sized to n (never a fixed cap → no dropped group).
    * n is bounded to DB2_SPEND_MAX_ROWS upstream (a larger report is a TOOBIG error). */
   int *grp = NULL;
   char *done = NULL;
   if (n > 0)
   {
      grp = (int *)malloc((size_t)n * sizeof(int));
      done = (char *)malloc((size_t)n);
      if (!grp || !done)
      {
         free(grp);
         free(done);
         cJSON_Delete(root);
         return NULL;
      }
   }

   cJSON_AddItemToObject(root, "by_team", group_array(rows, n, GROUP_TEAM, grp, done));
   cJSON_AddItemToObject(root, "by_model", group_array(rows, n, GROUP_MODEL, grp, done));
   cJSON_AddItemToObject(root, "by_project", group_array(rows, n, GROUP_PROJECT, grp, done));

   free(grp);
   free(done);
   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   return s;
}
