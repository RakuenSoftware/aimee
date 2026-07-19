/* roadmap.c: spec-driven roadmap data model — deterministic Phase-1 core.
 *
 * Implements src/headers/roadmap.h: a goal decomposed into a dependency-aware
 * tree of milestone/slice/task `plan_unit` artifacts under one `roadmap`
 * artifact. Durable artifacts live in DB2 (kind='roadmap' / 'plan_unit' over the
 * shared `artifacts` table). Decomposition is produced by a reason/draft delegate
 * via an injectable hook; the validate -> write -> commit -> project path here is
 * fully deterministic so no LLM writes state directly.
 *
 * See docs/proposals/pending/spec-driven-roadmaps-and-autonomous-delegate-dispatch.md
 */

#include "roadmap.h"

#include "db2/artifacts.h"
#include "db2/db_postgres.h"
#include "db2/lifecycle.h"
#include "headers/dstr.h"
#include "headers/platform_path.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ROADMAP_MAX_UNITS 512

/* ── small helpers ───────────────────────────────────────────────────────── */

static int str_in_set(const char *v, const char *const *set)
{
   if (!v)
      return 0;
   for (int i = 0; set[i]; i++)
      if (strcmp(v, set[i]) == 0)
         return 1;
   return 0;
}

static const char *LEVELS[] = {"milestone", "slice", "task", NULL};
static const char *DEPTHS[] = {"standard", "deep", "progressive", NULL};
static const char *PROFILES[] = {"budget", "balanced", "quality", NULL};
static const char *POLICIES[] = {"planning", "docs", "execution", NULL};

/* Fetch a string member, or dflt when absent / not a string. */
static const char *obj_str(const cJSON *o, const char *k, const char *dflt)
{
   const cJSON *it = cJSON_GetObjectItem(o, k);
   if (it && cJSON_IsString(it) && it->valuestring)
      return it->valuestring;
   return dflt;
}

/* The parent local_id of a unit ("" when absent). */
static const char *unit_parent(const cJSON *u)
{
   return obj_str(u, "parent", "");
}

static int set_err(char *err, size_t len, const char *msg)
{
   if (err && len)
      snprintf(err, len, "%s", msg);
   return -1;
}

/* ── validation ──────────────────────────────────────────────────────────── */

/* Index of the unit whose local_id == lid, or -1. */
static int find_local(cJSON *const *units, int n, const char *lid)
{
   for (int i = 0; i < n; i++)
   {
      const char *u = obj_str(units[i], "local_id", "");
      if (lid && u && strcmp(u, lid) == 0)
         return i;
   }
   return -1;
}

/* DFS cycle detection over the depends_on edges (siblings only, already
 * validated). color: 0 white, 1 gray (on stack), 2 black. Returns 1 if a cycle
 * is reachable from i. */
static int dfs_cycle(cJSON *const *units, int n, int i, int *color)
{
   color[i] = 1;
   const cJSON *deps = cJSON_GetObjectItem(units[i], "depends_on");
   if (deps && cJSON_IsArray(deps))
   {
      int m = cJSON_GetArraySize(deps);
      for (int k = 0; k < m; k++)
      {
         const cJSON *d = cJSON_GetArrayItem(deps, k);
         if (!d || !cJSON_IsString(d))
            continue;
         int j = find_local(units, n, d->valuestring);
         if (j < 0)
            continue;
         if (color[j] == 1)
            return 1;
         if (color[j] == 0 && dfs_cycle(units, n, j, color))
            return 1;
      }
   }
   color[i] = 2;
   return 0;
}

int roadmap_validate_decomposition(const char *decomposition_json, char *err, size_t err_len)
{
   if (!decomposition_json)
      return set_err(err, err_len, "null decomposition");

   cJSON *root = cJSON_Parse(decomposition_json);
   if (!root)
      return set_err(err, err_len, "invalid JSON");

   int rc = -1;
   cJSON *units = NULL;
   cJSON **arr = NULL;
   int *color = NULL;

   const char *goal = obj_str(root, "goal", "");
   if (!goal[0])
   {
      set_err(err, err_len, "goal is required");
      goto done;
   }
   const cJSON *depth = cJSON_GetObjectItem(root, "planning_depth");
   if (depth && cJSON_IsString(depth) && !str_in_set(depth->valuestring, DEPTHS))
   {
      set_err(err, err_len, "invalid planning_depth");
      goto done;
   }
   const cJSON *profile = cJSON_GetObjectItem(root, "token_profile");
   if (profile && cJSON_IsString(profile) && !str_in_set(profile->valuestring, PROFILES))
   {
      set_err(err, err_len, "invalid token_profile");
      goto done;
   }

   units = cJSON_GetObjectItem(root, "units");
   if (!units || !cJSON_IsArray(units) || cJSON_GetArraySize(units) == 0)
   {
      set_err(err, err_len, "units must be a non-empty array");
      goto done;
   }
   int n = cJSON_GetArraySize(units);
   if (n > ROADMAP_MAX_UNITS)
   {
      set_err(err, err_len, "too many units");
      goto done;
   }
   arr = calloc((size_t)n, sizeof(*arr));
   if (!arr)
      goto done;
   for (int i = 0; i < n; i++)
      arr[i] = cJSON_GetArrayItem(units, i);

   /* Pass 1: per-unit shape + unique local_id + valid level/policy. */
   for (int i = 0; i < n; i++)
   {
      cJSON *u = arr[i];
      if (!cJSON_IsObject(u))
      {
         set_err(err, err_len, "unit must be an object");
         goto done;
      }
      const char *lid = obj_str(u, "local_id", "");
      if (!lid[0])
      {
         set_err(err, err_len, "unit missing local_id");
         goto done;
      }
      for (int j = 0; j < i; j++)
         if (strcmp(obj_str(arr[j], "local_id", ""), lid) == 0)
         {
            set_err(err, err_len, "duplicate local_id");
            goto done;
         }
      const char *level = obj_str(u, "level", "");
      if (!str_in_set(level, LEVELS))
      {
         set_err(err, err_len, "invalid level");
         goto done;
      }
      const cJSON *tpm = cJSON_GetObjectItem(u, "tool_policy_mode");
      if (tpm && cJSON_IsString(tpm) && !str_in_set(tpm->valuestring, POLICIES))
      {
         set_err(err, err_len, "invalid tool_policy_mode");
         goto done;
      }
   }

   /* Pass 2: parent nesting, sibling depends_on, leaf-task completeness. */
   for (int i = 0; i < n; i++)
   {
      cJSON *u = arr[i];
      const char *level = obj_str(u, "level", "");
      const char *parent = unit_parent(u);

      if (strcmp(level, "milestone") == 0)
      {
         if (parent[0])
         {
            set_err(err, err_len, "milestone must have no parent");
            goto done;
         }
      }
      else
      {
         int pj = find_local(arr, n, parent);
         if (pj < 0)
         {
            set_err(err, err_len, "parent does not resolve");
            goto done;
         }
         const char *plevel = obj_str(arr[pj], "level", "");
         if (strcmp(level, "slice") == 0 && strcmp(plevel, "milestone") != 0)
         {
            set_err(err, err_len, "slice parent must be a milestone");
            goto done;
         }
         if (strcmp(level, "task") == 0 && strcmp(plevel, "slice") != 0)
         {
            set_err(err, err_len, "task parent must be a slice");
            goto done;
         }
      }

      const cJSON *deps = cJSON_GetObjectItem(u, "depends_on");
      if (deps)
      {
         if (!cJSON_IsArray(deps))
         {
            set_err(err, err_len, "depends_on must be an array");
            goto done;
         }
         int m = cJSON_GetArraySize(deps);
         for (int k = 0; k < m; k++)
         {
            const cJSON *d = cJSON_GetArrayItem(deps, k);
            if (!d || !cJSON_IsString(d))
            {
               set_err(err, err_len, "depends_on entry must be a string");
               goto done;
            }
            if (strcmp(d->valuestring, obj_str(u, "local_id", "")) == 0)
            {
               set_err(err, err_len, "unit depends on itself");
               goto done;
            }
            int dj = find_local(arr, n, d->valuestring);
            if (dj < 0)
            {
               set_err(err, err_len, "depends_on does not resolve");
               goto done;
            }
            if (strcmp(unit_parent(arr[dj]), parent) != 0)
            {
               set_err(err, err_len, "depends_on must reference a sibling");
               goto done;
            }
         }
      }

      if (strcmp(level, "task") == 0)
      {
         const cJSON *of = cJSON_GetObjectItem(u, "owned_files");
         const cJSON *ac = cJSON_GetObjectItem(u, "acceptance_criteria");
         if (!of || !cJSON_IsArray(of) || cJSON_GetArraySize(of) == 0)
         {
            set_err(err, err_len, "leaf task needs owned_files");
            goto done;
         }
         if (!ac || !cJSON_IsArray(ac) || cJSON_GetArraySize(ac) == 0)
         {
            set_err(err, err_len, "leaf task needs acceptance_criteria");
            goto done;
         }
      }
   }

   /* Pass 3: dependency DAG (no cycles). */
   color = calloc((size_t)n, sizeof(*color));
   if (!color)
      goto done;
   for (int i = 0; i < n; i++)
      if (color[i] == 0 && dfs_cycle(arr, n, i, color))
      {
         set_err(err, err_len, "dependency cycle");
         goto done;
      }

   rc = 0;
done:
   free(color);
   free(arr);
   cJSON_Delete(root);
   return rc;
}

/* ── create + commit ─────────────────────────────────────────────────────── */

/* Deep-copy a string array member into a fresh array; empty array when absent. */
static cJSON *dup_str_array(const cJSON *u, const char *k)
{
   const cJSON *src = cJSON_GetObjectItem(u, k);
   if (src && cJSON_IsArray(src))
      return cJSON_Duplicate(src, 1);
   return cJSON_CreateArray();
}

int roadmap_create_from_decomposition(const char *decomposition_json, char *out_id, size_t out_len)
{
   char verr[256];
   if (roadmap_validate_decomposition(decomposition_json, verr, sizeof(verr)) != 0)
      return -1;

   cJSON *root = cJSON_Parse(decomposition_json);
   if (!root)
      return -1;
   cJSON *units = cJSON_GetObjectItem(root, "units");
   int n = cJSON_GetArraySize(units);

   char roadmap_id[37];
   db2_artifact_gen_id(roadmap_id, sizeof(roadmap_id));

   char(*art_ids)[37] = calloc((size_t)n, sizeof(*art_ids));
   cJSON **arr = calloc((size_t)n, sizeof(*arr));
   if (!art_ids || !arr)
   {
      free(art_ids);
      free(arr);
      cJSON_Delete(root);
      return -1;
   }
   for (int i = 0; i < n; i++)
   {
      arr[i] = cJSON_GetArrayItem(units, i);
      db2_artifact_gen_id(art_ids[i], sizeof(art_ids[i]));
   }

   int rc = 0;

   /* Write each plan_unit (proposed). */
   for (int i = 0; i < n && rc == 0; i++)
   {
      cJSON *u = arr[i];
      const char *level = obj_str(u, "level", "task");
      const char *parent = unit_parent(u);

      cJSON *p = cJSON_CreateObject();
      cJSON_AddStringToObject(p, "level", level);
      const char *parent_art = "";
      if (parent[0])
      {
         int pj = find_local(arr, n, parent);
         if (pj >= 0)
            parent_art = art_ids[pj];
      }
      cJSON_AddStringToObject(p, "parent_id", parent_art);
      cJSON_AddStringToObject(p, "title", obj_str(u, "title", ""));
      cJSON_AddStringToObject(p, "intent", obj_str(u, "intent", ""));
      cJSON_AddNumberToObject(p, "ord", i);

      /* depends_on: remap sibling local_ids -> artifact ids. */
      cJSON *deps_out = cJSON_CreateArray();
      const cJSON *deps = cJSON_GetObjectItem(u, "depends_on");
      if (deps && cJSON_IsArray(deps))
      {
         int m = cJSON_GetArraySize(deps);
         for (int k = 0; k < m; k++)
         {
            const cJSON *d = cJSON_GetArrayItem(deps, k);
            if (d && cJSON_IsString(d))
            {
               int dj = find_local(arr, n, d->valuestring);
               if (dj >= 0)
                  cJSON_AddItemToArray(deps_out, cJSON_CreateString(art_ids[dj]));
            }
         }
      }
      cJSON_AddItemToObject(p, "depends_on", deps_out);
      cJSON_AddItemToObject(p, "owned_files", dup_str_array(u, "owned_files"));
      cJSON_AddItemToObject(p, "read_context", dup_str_array(u, "read_context"));
      cJSON_AddItemToObject(p, "acceptance_criteria", dup_str_array(u, "acceptance_criteria"));
      cJSON_AddItemToObject(p, "verification_commands", dup_str_array(u, "verification_commands"));
      cJSON_AddStringToObject(p, "tool_policy_mode", obj_str(u, "tool_policy_mode", "execution"));
      cJSON_AddStringToObject(p, "plan_candidate_ref", "");
      cJSON_AddNullToObject(p, "summary_ref");
      cJSON_AddStringToObject(p, "state", "pending");

      char *payload = cJSON_PrintUnformatted(p);
      cJSON_Delete(p);
      if (!payload || db2_artifact_write(art_ids[i], "plan_unit", "proposed", "roadmap", roadmap_id,
                                         "", 1.0, payload) != 0)
         rc = -1;
      free(payload);
   }

   /* Write the roadmap artifact (proposed). */
   if (rc == 0)
   {
      cJSON *rp = cJSON_CreateObject();
      cJSON_AddStringToObject(rp, "goal", obj_str(root, "goal", ""));
      cJSON_AddStringToObject(rp, "requirements_ref", "");
      cJSON_AddStringToObject(rp, "planning_depth", obj_str(root, "planning_depth", "standard"));
      cJSON_AddStringToObject(rp, "token_profile", obj_str(root, "token_profile", "balanced"));
      cJSON *uid = cJSON_CreateArray();
      for (int i = 0; i < n; i++)
         if (strcmp(obj_str(arr[i], "level", ""), "milestone") == 0)
            cJSON_AddItemToArray(uid, cJSON_CreateString(art_ids[i]));
      cJSON_AddItemToObject(rp, "unit_ids", uid);
      cJSON *roll = cJSON_CreateObject();
      cJSON_AddNumberToObject(roll, "done", 0);
      cJSON_AddNumberToObject(roll, "active", 0);
      cJSON_AddNumberToObject(roll, "blocked", 0);
      cJSON_AddNumberToObject(roll, "total", n);
      cJSON_AddItemToObject(rp, "status_rollup", roll);

      char *payload = cJSON_PrintUnformatted(rp);
      cJSON_Delete(rp);
      if (!payload || db2_artifact_write(roadmap_id, "roadmap", "proposed", "roadmap", roadmap_id,
                                         "", 1.0, payload) != 0)
         rc = -1;
      free(payload);
   }

   /* Commit: proposed -> committed for the roadmap and every unit. */
   if (rc == 0 && db2_artifact_set_state(roadmap_id, "committed") != 0)
      rc = -1;
   for (int i = 0; i < n && rc == 0; i++)
      if (db2_artifact_set_state(art_ids[i], "committed") != 0)
         rc = -1;

   if (rc == 0 && out_id && out_len)
      snprintf(out_id, out_len, "%s", roadmap_id);

   free(art_ids);
   free(arr);
   cJSON_Delete(root);
   return rc;
}

/* ── decomposer hook + roadmap_new ───────────────────────────────────────── */

static roadmap_decompose_fn g_decompose = NULL;
static void *g_decompose_ud = NULL;

void roadmap_set_decomposer(roadmap_decompose_fn fn, void *ud)
{
   g_decompose = fn;
   g_decompose_ud = ud;
}

int roadmap_new(const char *goal, const char *planning_depth, const char *token_profile,
                char *out_id, size_t out_len)
{
   if (!goal || !goal[0])
      return -1;
   if (!g_decompose)
      return -1;

   char *json = NULL;
   if (g_decompose(goal, planning_depth, token_profile, &json, g_decompose_ud) != 0 || !json)
   {
      free(json);
      return -1;
   }
   int rc = roadmap_create_from_decomposition(json, out_id, out_len);
   free(json);
   return rc;
}

/* ── load + render ───────────────────────────────────────────────────────── */

typedef struct
{
   char id[37];
   cJSON *payload;
} loaded_unit_t;

/* Load all plan_unit artifacts scoped to roadmap_id. Caller frees each payload. */
static int load_units(const char *roadmap_id, loaded_unit_t *out, int max, int *count)
{
   *count = 0;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT id, payload FROM artifacts WHERE kind = ?1 AND scope_id = ?2",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", "plan_unit");
   aimee_pg_bind_text(st, "?2", roadmap_id);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *id = aimee_pg_column_text(st, 0);
      const char *pl = aimee_pg_column_text(st, 1);
      cJSON *p = pl ? cJSON_Parse(pl) : NULL;
      if (!p)
         continue;
      snprintf(out[n].id, sizeof(out[n].id), "%s", id ? id : "");
      out[n].payload = p;
      n++;
   }
   aimee_pg_finalize(st);
   *count = n;
   return 0;
}

static void free_units(loaded_unit_t *u, int n)
{
   for (int i = 0; i < n; i++)
      cJSON_Delete(u[i].payload);
}

static int unit_ord(const cJSON *p)
{
   const cJSON *o = cJSON_GetObjectItem(p, "ord");
   return (o && cJSON_IsNumber(o)) ? o->valueint : 0;
}

static int find_by_id(const loaded_unit_t *u, int n, const char *id)
{
   for (int i = 0; i < n; i++)
      if (strcmp(u[i].id, id) == 0)
         return i;
   return -1;
}

/* Collect child indices (units whose parent_id == parent_id), sorted by ord. */
static int children_of(const loaded_unit_t *u, int n, const char *parent_id, int *out)
{
   int c = 0;
   for (int i = 0; i < n; i++)
      if (strcmp(obj_str(u[i].payload, "parent_id", ""), parent_id) == 0)
         out[c++] = i;
   /* insertion sort by ord (stable, deterministic) */
   for (int i = 1; i < c; i++)
   {
      int key = out[i], j = i - 1;
      while (j >= 0 && unit_ord(u[out[j]].payload) > unit_ord(u[key].payload))
      {
         out[j + 1] = out[j];
         j--;
      }
      out[j + 1] = key;
   }
   return c;
}

static int cmp_str(const void *a, const void *b)
{
   return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Append a unit line + its acceptance criteria + sorted deps to s. */
static void render_unit(dstr_t *s, const loaded_unit_t *u, int idx, int depth)
{
   const cJSON *p = u[idx].payload;
   for (int d = 0; d < depth; d++)
      dstr_append_str(s, "  ");
   dstr_appendf(s, "- [%s] %s: %s\n", obj_str(p, "state", "pending"), obj_str(p, "level", "task"),
                obj_str(p, "title", ""));

   const cJSON *deps = cJSON_GetObjectItem(p, "depends_on");
   int m = (deps && cJSON_IsArray(deps)) ? cJSON_GetArraySize(deps) : 0;
   if (m > 0)
   {
      const char **ids = calloc((size_t)m, sizeof(*ids));
      int c = 0;
      for (int k = 0; k < m; k++)
      {
         const cJSON *d = cJSON_GetArrayItem(deps, k);
         if (d && cJSON_IsString(d))
            ids[c++] = d->valuestring;
      }
      qsort(ids, (size_t)c, sizeof(*ids), cmp_str);
      for (int d = 0; d <= depth; d++)
         dstr_append_str(s, "  ");
      dstr_append_str(s, "deps=[");
      for (int k = 0; k < c; k++)
         dstr_appendf(s, "%s%s", k ? "," : "", ids[k]);
      dstr_append_str(s, "]\n");
      free(ids);
   }

   const cJSON *ac = cJSON_GetObjectItem(p, "acceptance_criteria");
   int an = (ac && cJSON_IsArray(ac)) ? cJSON_GetArraySize(ac) : 0;
   for (int k = 0; k < an; k++)
   {
      const cJSON *a = cJSON_GetArrayItem(ac, k);
      if (a && cJSON_IsString(a))
      {
         for (int d = 0; d <= depth; d++)
            dstr_append_str(s, "  ");
         dstr_appendf(s, "* %s\n", a->valuestring);
      }
   }
}

/* Render the full tree (milestones in roadmap unit_ids order, then slices, then
 * tasks, each sorted by ord) into s. */
static void render_tree(dstr_t *s, const cJSON *rp, const loaded_unit_t *u, int n)
{
   int kids[ROADMAP_MAX_UNITS];
   const cJSON *uid = cJSON_GetObjectItem(rp, "unit_ids");
   int mn = (uid && cJSON_IsArray(uid)) ? cJSON_GetArraySize(uid) : 0;
   for (int mi = 0; mi < mn; mi++)
   {
      const cJSON *mid = cJSON_GetArrayItem(uid, mi);
      if (!mid || !cJSON_IsString(mid))
         continue;
      int m = find_by_id(u, n, mid->valuestring);
      if (m < 0)
         continue;
      render_unit(s, u, m, 0);
      int sc = children_of(u, n, u[m].id, kids);
      int slices[ROADMAP_MAX_UNITS];
      memcpy(slices, kids, (size_t)sc * sizeof(int));
      for (int si = 0; si < sc; si++)
      {
         render_unit(s, u, slices[si], 1);
         int tasks[ROADMAP_MAX_UNITS];
         int tc = children_of(u, n, u[slices[si]].id, tasks);
         for (int ti = 0; ti < tc; ti++)
            render_unit(s, u, tasks[ti], 2);
      }
   }
}

int roadmap_show_json(const char *roadmap_id, char **out)
{
   if (out)
      *out = NULL;
   if (!roadmap_id || !roadmap_id[0] || !out)
      return -1;
   db2_artifact_row_t row;
   int cc = 0;
   if (db2_artifact_read(roadmap_id, &row, NULL, 0, &cc) != 0)
      return -1;
   cJSON *rp = cJSON_Parse(row.payload_json);
   if (!rp)
      return -1;

   loaded_unit_t units[ROADMAP_MAX_UNITS];
   int n = 0;
   if (load_units(roadmap_id, units, ROADMAP_MAX_UNITS, &n) != 0)
   {
      cJSON_Delete(rp);
      return -1;
   }

   cJSON *o = cJSON_CreateObject();
   cJSON_AddItemToObject(o, "roadmap", rp); /* ownership transferred to o */
   cJSON *ua = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
      cJSON_AddItemToArray(ua, cJSON_Duplicate(units[i].payload, 1));
   cJSON_AddItemToObject(o, "units", ua);
   char *s = cJSON_PrintUnformatted(o);
   cJSON_Delete(o); /* also frees rp (transferred above) */
   free_units(units, n);
   if (!s)
      return -1;
   *out = s;
   return 0;
}

int roadmap_list_json(char **out)
{
   if (!out)
      return -1;
   *out = NULL;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT id, payload FROM artifacts WHERE kind = ?1 AND state = ?2 ORDER BY id", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", "roadmap");
   aimee_pg_bind_text(st, "?2", "committed");
   cJSON *arr = cJSON_CreateArray();
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *id = aimee_pg_column_text(st, 0);
      const char *pl = aimee_pg_column_text(st, 1);
      cJSON *p = pl ? cJSON_Parse(pl) : NULL;
      cJSON *e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "id", id ? id : "");
      cJSON_AddStringToObject(e, "goal", p ? obj_str(p, "goal", "") : "");
      const cJSON *roll = p ? cJSON_GetObjectItem(p, "status_rollup") : NULL;
      if (roll)
         cJSON_AddItemToObject(e, "status_rollup", cJSON_Duplicate(roll, 1));
      cJSON_AddItemToArray(arr, e);
      if (p)
         cJSON_Delete(p);
   }
   aimee_pg_finalize(st);

   cJSON *o = cJSON_CreateObject();
   cJSON_AddItemToObject(o, "roadmaps", arr);
   char *s = cJSON_PrintUnformatted(o);
   cJSON_Delete(o);
   if (!s)
      return -1;
   *out = s;
   return 0;
}

int roadmap_show(const char *roadmap_id, int json_output)
{
   if (!roadmap_id || !roadmap_id[0])
      return -1;

   if (json_output)
   {
      char *s = NULL;
      if (roadmap_show_json(roadmap_id, &s) != 0)
         return -1;
      printf("%s\n", s);
      free(s);
      return 0;
   }

   db2_artifact_row_t row;
   int cc = 0;
   if (db2_artifact_read(roadmap_id, &row, NULL, 0, &cc) != 0)
      return -1;
   cJSON *rp = cJSON_Parse(row.payload_json);
   if (!rp)
      return -1;

   loaded_unit_t units[ROADMAP_MAX_UNITS];
   int n = 0;
   if (load_units(roadmap_id, units, ROADMAP_MAX_UNITS, &n) != 0)
   {
      cJSON_Delete(rp);
      return -1;
   }

   dstr_t s;
   dstr_init(&s);
   dstr_appendf(&s, "roadmap %s\n", roadmap_id);
   dstr_appendf(&s, "goal: %s\n", obj_str(rp, "goal", ""));
   render_tree(&s, rp, units, n);
   printf("%s", dstr_cstr(&s));
   dstr_free(&s);

   free_units(units, n);
   cJSON_Delete(rp);
   return 0;
}

/* ── projections ─────────────────────────────────────────────────────────── */

static int write_file(const char *path, const char *data)
{
   FILE *f = fopen(path, "wb");
   if (!f)
      return -1;
   size_t len = strlen(data);
   size_t wr = fwrite(data, 1, len, f);
   fclose(f);
   return wr == len ? 0 : -1;
}

int roadmap_projections_write(const char *roadmap_id)
{
   if (!roadmap_id || !roadmap_id[0])
      return -1;
   db2_artifact_row_t row;
   int cc = 0;
   if (db2_artifact_read(roadmap_id, &row, NULL, 0, &cc) != 0)
      return -1;
   cJSON *rp = cJSON_Parse(row.payload_json);
   if (!rp)
      return -1;

   loaded_unit_t units[ROADMAP_MAX_UNITS];
   int n = 0;
   if (load_units(roadmap_id, units, ROADMAP_MAX_UNITS, &n) != 0)
   {
      cJSON_Delete(rp);
      return -1;
   }

   if (platform_mkdir_p(".aimee/roadmap", 0755) != 0)
   {
      free_units(units, n);
      cJSON_Delete(rp);
      return -1;
   }

   int rc = 0;

   /* ROADMAP.md — deterministic full tree. */
   {
      dstr_t s;
      dstr_init(&s);
      dstr_appendf(&s, "# Roadmap\n\ngoal: %s\n\n", obj_str(rp, "goal", ""));
      render_tree(&s, rp, units, n);
      if (write_file(".aimee/roadmap/ROADMAP.md", dstr_cstr(&s)) != 0)
         rc = -1;
      dstr_free(&s);
   }

   /* STATE.md — counts + the first pending unit (lowest ord). */
   if (rc == 0)
   {
      const cJSON *roll = cJSON_GetObjectItem(rp, "status_rollup");
      int total = 0, done = 0, active = 0, blocked = 0;
      if (roll)
      {
         const cJSON *t = cJSON_GetObjectItem(roll, "total");
         const cJSON *d = cJSON_GetObjectItem(roll, "done");
         const cJSON *a = cJSON_GetObjectItem(roll, "active");
         const cJSON *b = cJSON_GetObjectItem(roll, "blocked");
         total = t && cJSON_IsNumber(t) ? t->valueint : 0;
         done = d && cJSON_IsNumber(d) ? d->valueint : 0;
         active = a && cJSON_IsNumber(a) ? a->valueint : 0;
         blocked = b && cJSON_IsNumber(b) ? b->valueint : 0;
      }
      const char *next = "(none)";
      int best = -1;
      for (int i = 0; i < n; i++)
         if (strcmp(obj_str(units[i].payload, "state", "pending"), "pending") == 0)
            if (best < 0 || unit_ord(units[i].payload) < unit_ord(units[best].payload))
               best = i;
      if (best >= 0)
         next = obj_str(units[best].payload, "title", "(none)");

      dstr_t s;
      dstr_init(&s);
      dstr_appendf(&s, "# State\n\ngoal: %s\n", obj_str(rp, "goal", ""));
      dstr_appendf(&s, "total: %d  done: %d  active: %d  blocked: %d\n", total, done, active,
                   blocked);
      dstr_appendf(&s, "Next: %s\n", next);
      if (write_file(".aimee/roadmap/STATE.md", dstr_cstr(&s)) != 0)
         rc = -1;
      dstr_free(&s);
   }

   free_units(units, n);
   cJSON_Delete(rp);
   return rc;
}

int roadmap_projections_rebuild(const char *roadmap_id)
{
   unlink(".aimee/roadmap/ROADMAP.md");
   unlink(".aimee/roadmap/STATE.md");
   return roadmap_projections_write(roadmap_id);
}

/* ── HTML report ─────────────────────────────────────────────────────────── */

/* Escape HTML special characters; returns a static 1KB buffer (safe for short
 * strings — titles, states). Not reentrant but fine for sequential rendering. */
static const char *html_escape(const char *s)
{
   static char buf[1024];
   if (!s)
      return "";
   char *d = buf;
   char *end = buf + sizeof(buf) - 6;
   while (*s && d < end)
   {
      switch (*s)
      {
      case '&':
         memcpy(d, "&amp;", 5);
         d += 5;
         break;
      case '<':
         memcpy(d, "&lt;", 4);
         d += 4;
         break;
      case '>':
         memcpy(d, "&gt;", 4);
         d += 4;
         break;
      case '"':
         memcpy(d, "&quot;", 6);
         d += 6;
         break;
      default:
         *d++ = *s;
         break;
      }
      s++;
   }
   *d = '\0';
   return buf;
}

/* CSS badge colour for a unit state. */
static const char *state_colour(const char *state)
{
   if (!state)
      return "#888";
   if (strcmp(state, "done") == 0)
      return "#2a9d5c";
   if (strcmp(state, "active") == 0)
      return "#2196f3";
   if (strcmp(state, "needs_review") == 0)
      return "#e67e22";
   if (strcmp(state, "blocked") == 0)
      return "#c0392b";
   return "#888"; /* pending */
}

int roadmap_report_html(const char *roadmap_id, const char *output_path)
{
   if (!roadmap_id || !roadmap_id[0])
      return -1;

   /* Load the roadmap artifact. */
   db2_artifact_row_t row;
   int cc = 0;
   if (db2_artifact_read(roadmap_id, &row, NULL, 0, &cc) != 0)
      return -1;
   cJSON *rp = cJSON_Parse(row.payload_json);
   if (!rp)
      return -1;

   loaded_unit_t units[ROADMAP_MAX_UNITS];
   int n = 0;
   if (load_units(roadmap_id, units, ROADMAP_MAX_UNITS, &n) != 0)
   {
      cJSON_Delete(rp);
      return -1;
   }

   /* Count rollup. */
   int total = n, done = 0, active = 0, blocked = 0, needs_review = 0;
   for (int i = 0; i < n; i++)
   {
      const char *st = obj_str(units[i].payload, "state", "pending");
      if (strcmp(st, "done") == 0)
         done++;
      else if (strcmp(st, "active") == 0)
         active++;
      else if (strcmp(st, "blocked") == 0)
         blocked++;
      else if (strcmp(st, "needs_review") == 0)
         needs_review++;
   }
   int pct = total > 0 ? (done * 100 / total) : 0;

   const char *goal = obj_str(rp, "goal", "");

   /* Build HTML. */
   dstr_t s;
   dstr_init(&s);

   dstr_append_str(&s, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
                       "<meta charset=\"UTF-8\">\n"
                       "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n");
   dstr_appendf(&s, "<title>Roadmap Report: %s</title>\n", html_escape(goal));
   dstr_append_str(
       &s,
       "<style>\n"
       "body{font-family:system-ui,sans-serif;max-width:860px;margin:2rem auto;padding:0 1rem;"
       "color:#222}\n"
       "h1{font-size:1.4rem;margin-bottom:.25rem}h2{font-size:1.1rem;border-bottom:1px solid #ddd}"
       "\n.meta{color:#666;font-size:.85rem;margin-bottom:1.5rem}\n"
       ".progress-bar{background:#eee;border-radius:6px;height:18px;margin:.5rem 0 1.5rem}\n"
       ".progress-fill{height:18px;border-radius:6px;background:#2a9d5c;transition:width .3s}\n"
       ".unit{display:flex;align-items:flex-start;padding:.4rem 0;border-bottom:1px solid #f0f0f0}"
       "\n.badge{border-radius:4px;padding:2px 8px;font-size:.75rem;font-weight:600;color:#fff;"
       "white-space:nowrap;flex-shrink:0;margin-right:.75rem;margin-top:.1rem}\n"
       ".level-milestone{font-weight:700;font-size:1rem;padding:.6rem 0 .2rem}\n"
       ".level-slice{padding-left:1.2rem;font-weight:600}\n"
       ".level-task{padding-left:2.4rem;font-size:.9rem}\n"
       ".ac{font-size:.8rem;color:#555;margin:.15rem 0 0 3rem;list-style:disc inside}\n"
       "</style>\n</head>\n<body>\n");

   dstr_appendf(&s, "<h1>%s</h1>\n", html_escape(goal));
   dstr_appendf(&s,
                "<div class=\"meta\">roadmap id: %s &nbsp;|&nbsp; "
                "%d units &nbsp;|&nbsp; %d done &nbsp;|&nbsp; %d active"
                " &nbsp;|&nbsp; %d blocked &nbsp;|&nbsp; %d needs&nbsp;review</div>\n",
                html_escape(roadmap_id), total, done, active, blocked, needs_review);

   dstr_appendf(&s,
                "<div class=\"progress-bar\"><div class=\"progress-fill\" "
                "style=\"width:%d%%\"></div></div>\n",
                pct);
   dstr_appendf(&s, "<p><strong>%d%%</strong> complete (%d / %d units done)</p>\n", pct, done,
                total);

   dstr_append_str(&s, "<h2>Units</h2>\n");

   for (int i = 0; i < n; i++)
   {
      const char *level = obj_str(units[i].payload, "level", "task");
      const char *title = obj_str(units[i].payload, "title", units[i].id);
      const char *state = obj_str(units[i].payload, "state", "pending");

      dstr_appendf(&s,
                   "<div class=\"unit level-%s\">"
                   "<span class=\"badge\" style=\"background:%s\">%s</span>"
                   "<div><strong>%s</strong>",
                   html_escape(level), state_colour(state), html_escape(state), html_escape(title));

      /* Acceptance criteria (tasks only). */
      cJSON *ac = cJSON_GetObjectItemCaseSensitive(units[i].payload, "acceptance_criteria");
      if (strcmp(level, "task") == 0 && cJSON_IsArray(ac) && cJSON_GetArraySize(ac) > 0)
      {
         dstr_append_str(&s, "<ul class=\"ac\">");
         cJSON *item;
         cJSON_ArrayForEach(item, ac)
         {
            if (cJSON_IsString(item))
               dstr_appendf(&s, "<li>%s</li>", html_escape(item->valuestring));
         }
         dstr_append_str(&s, "</ul>");
      }

      dstr_append_str(&s, "</div></div>\n");
   }

   dstr_append_str(&s, "</body>\n</html>\n");

   /* Determine output path. */
   char default_path[512];
   if (!output_path || !output_path[0])
   {
      if (platform_mkdir_p(".aimee/roadmap/reports", 0755) != 0)
      {
         free_units(units, n);
         cJSON_Delete(rp);
         dstr_free(&s);
         return -1;
      }
      snprintf(default_path, sizeof(default_path), ".aimee/roadmap/reports/%.60s.html", roadmap_id);
      output_path = default_path;
   }

   int rc = write_file(output_path, dstr_cstr(&s));
   dstr_free(&s);
   free_units(units, n);
   cJSON_Delete(rp);
   return rc;
}
