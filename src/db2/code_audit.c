/* db2/code_audit.c: fetch + assemble the graph-derived code-health audit.
 *
 * Fetches `exports`/`imports` edges from entity_edges and code-unit content
 * hashes from code_embeddings, runs the pure algorithms in code_audit_graph.c
 * (dead-export set-diff, import-cycle DFS) and groups clones by content_hash,
 * and returns a JSON {dead_exports[], cycles[], clones[]} bundle.
 *
 * SQL is kept to simple SELECTs; all aggregation/graph logic is in C so it is
 * unit-testable without a DB (see tests/test_code_audit_graph.c).
 */
#include "aimee.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "code_audit_graph.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

#define CA_FETCH_MAX 4000
#define CA_DEP_MAX   8000

static int fetch_edges(const char *relation, char **src, char **tgt, int max)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "SELECT source, target FROM entity_edges WHERE relation = ?1 LIMIT ?2";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", relation);
   aimee_pg_bind_int(st, "?2", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *s = aimee_pg_column_text(st, 0);
      const char *t = aimee_pg_column_text(st, 1);
      src[n] = strdup(s ? s : "");
      tgt[n] = strdup(t ? t : "");
      if (!src[n] || !tgt[n])
      {
         free(src[n]);
         free(tgt[n]);
         break;
      }
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

static const char *key_tail(const char *key, const char *prefix)
{
   size_t pl = strlen(prefix);
   return strncmp(key, prefix, pl) == 0 ? key + pl : key;
}

/* Group clones: code_embeddings rows ordered by content_hash; consecutive runs
 * of the same non-empty hash with >= 2 members are clone groups. */
static void add_clones(cJSON *resp, const char *project, int limit)
{
   void *conn = db2_conn();
   cJSON *arr = cJSON_AddArrayToObject(resp, "clones");
   if (!conn || !arr)
      return;
   static const char *sql = "SELECT content_hash, symbol, file_path FROM code_embeddings"
                            " WHERE content_hash <> '' AND (?1 = '' OR project = ?1)"
                            " ORDER BY content_hash LIMIT 20000";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", project ? project : "");

   char cur_hash[128] = "";
   cJSON *group = NULL;
   int group_n = 0, emitted = 0;
   while (emitted < limit && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *h = aimee_pg_column_text(st, 0);
      const char *sym = aimee_pg_column_text(st, 1);
      const char *fp = aimee_pg_column_text(st, 2);
      if (!h)
         continue;
      if (strcmp(h, cur_hash) != 0)
      {
         /* flush previous group */
         if (group && group_n >= 2)
         {
            cJSON_AddItemToArray(arr, group);
            emitted++;
         }
         else
            cJSON_Delete(group);
         group = cJSON_CreateArray();
         group_n = 0;
         snprintf(cur_hash, sizeof(cur_hash), "%s", h);
      }
      char member[1024];
      snprintf(member, sizeof(member), "%s  %s", sym ? sym : "", fp ? fp : "");
      cJSON_AddItemToArray(group, cJSON_CreateString(member));
      group_n++;
   }
   if (group && group_n >= 2 && emitted < limit)
      cJSON_AddItemToArray(arr, group);
   else
      cJSON_Delete(group);
   aimee_pg_finalize(st);
}

/* Near-duplicate clones: for each code-unit embedding, find its nearest OTHER
 * embedding via the HNSW index and report pairs within a tight cosine distance
 * that are NOT identical (those are exact-hash clones in `clones`). Cosine
 * distance via pgvector `<=>` (0 = identical); kept in (0.0001, 0.06] so sim is
 * ~0.94..0.9999. `a.point_id < b.bpid` reports each pair once. pgvector-only;
 * runs on the live kb's Postgres (not exercised by the sqlite test shim). */
static void add_near_clones(cJSON *resp, const char *project, int limit)
{
   void *conn = db2_conn();
   cJSON *arr = cJSON_AddArrayToObject(resp, "near_clones");
   if (!conn || !arr)
      return;
   static const char *sql =
       "SELECT a.symbol, a.file_path, b.bsym, b.bfile, (1.0 - (a.embedding <=> b.bemb)) AS sim "
       "FROM code_embeddings a JOIN LATERAL ("
       "  SELECT c.point_id AS bpid, c.symbol AS bsym, c.file_path AS bfile, c.embedding AS bemb "
       "  FROM code_embeddings c "
       "  WHERE c.point_id <> a.point_id AND c.record_type = 'code_unit' "
       "    AND (?1 = '' OR c.project = ?1) "
       "  ORDER BY c.embedding <=> a.embedding LIMIT 1) b ON true "
       "WHERE a.record_type = 'code_unit' AND (?2 = '' OR a.project = ?2) "
       "  AND a.point_id < b.bpid AND (a.embedding <=> b.bemb) BETWEEN 0.0001 AND 0.06 "
       "ORDER BY sim DESC LIMIT ?3";
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", project ? project : "");
   aimee_pg_bind_text(st, "?2", project ? project : "");
   aimee_pg_bind_int(st, "?3", limit);
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *as = aimee_pg_column_text(st, 0);
      const char *af = aimee_pg_column_text(st, 1);
      const char *bs = aimee_pg_column_text(st, 2);
      const char *bf = aimee_pg_column_text(st, 3);
      double sim = aimee_pg_column_double(st, 4);
      char line[1100];
      snprintf(line, sizeof(line), "%.3f  %s (%s)  ~  %s (%s)", sim, as ? as : "", af ? af : "",
               bs ? bs : "", bf ? bf : "");
      cJSON_AddItemToArray(arr, cJSON_CreateString(line));
   }
   aimee_pg_finalize(st);
}

cJSON *db2_kb_service_code_audit_json(const char *project, int limit)
{
   if (limit <= 0 || limit > 200)
      limit = 50;

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON_AddStringToObject(resp, "status", "ok");

   char **exp_src = calloc(CA_FETCH_MAX, sizeof(char *));
   char **exp_tgt = calloc(CA_FETCH_MAX, sizeof(char *));
   char **imp_src = calloc(CA_FETCH_MAX, sizeof(char *));
   char **imp_tgt = calloc(CA_FETCH_MAX, sizeof(char *));
   if (!exp_src || !exp_tgt || !imp_src || !imp_tgt)
   {
      free(exp_src);
      free(exp_tgt);
      free(imp_src);
      free(imp_tgt);
      return resp;
   }
   int ne = fetch_edges("exports", exp_src, exp_tgt, CA_FETCH_MAX);
   int ni = fetch_edges("imports", imp_src, imp_tgt, CA_FETCH_MAX);

   /* Dead exports: export target keys with no matching import target key. */
   const char **dead = calloc((size_t)(ne > 0 ? ne : 1), sizeof(char *));
   cJSON *darr = cJSON_AddArrayToObject(resp, "dead_exports");
   if (dead && darr)
   {
      int nd = code_audit_dead_exports((const char *const *)exp_tgt, ne,
                                       (const char *const *)imp_tgt, ni, dead, limit);
      for (int i = 0; i < nd; i++)
         cJSON_AddItemToArray(darr, cJSON_CreateString(dead[i]));
   }
   free(dead);

   /* Import cycles: importer file -> exporter file when import tail == export
    * tail (the tail embeds the project, so matching is project-scoped). */
   audit_edge_t *deps = calloc(CA_DEP_MAX, sizeof(audit_edge_t));
   cJSON *carr = cJSON_AddArrayToObject(resp, "cycles");
   if (deps && carr)
   {
      int ndep = 0;
      for (int j = 0; j < ni && ndep < CA_DEP_MAX; j++)
      {
         const char *itail = key_tail(imp_tgt[j], "import:");
         for (int k = 0; k < ne && ndep < CA_DEP_MAX; k++)
         {
            if (strcmp(itail, key_tail(exp_tgt[k], "export:")) != 0)
               continue;
            if (strcmp(imp_src[j], exp_src[k]) == 0)
               continue; /* a file importing its own export is not a cycle */
            deps[ndep].from = imp_src[j];
            deps[ndep].to = exp_src[k];
            ndep++;
         }
      }
      char **cyc = calloc((size_t)limit, sizeof(char *));
      if (cyc)
      {
         int nc = code_audit_find_cycles(deps, ndep, cyc, limit);
         for (int i = 0; i < nc; i++)
         {
            cJSON_AddItemToArray(carr, cJSON_CreateString(cyc[i]));
            free(cyc[i]);
         }
         free(cyc);
      }
   }
   free(deps);

   add_clones(resp, project, limit);
   add_near_clones(resp, project, limit);

   for (int i = 0; i < ne; i++)
   {
      free(exp_src[i]);
      free(exp_tgt[i]);
   }
   for (int i = 0; i < ni; i++)
   {
      free(imp_src[i]);
      free(imp_tgt[i]);
   }
   free(exp_src);
   free(exp_tgt);
   free(imp_src);
   free(imp_tgt);
   return resp;
}
