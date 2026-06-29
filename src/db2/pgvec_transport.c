#include "pgvec_transport.h"

#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"
#include "../db2/lifecycle.h" /* db2_embedding_dim */
#include "cJSON.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t latency_mu = PTHREAD_MUTEX_INITIALIZER;
static int64_t latency_total_us = 0;
static int64_t latency_count = 0;
static int64_t latency_max_us = 0;

static int64_t monotonic_us(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void record_latency(int64_t us)
{
   pthread_mutex_lock(&latency_mu);
   latency_total_us += us;
   latency_count++;
   if (us > latency_max_us)
      latency_max_us = us;
   pthread_mutex_unlock(&latency_mu);
}

void pgvec_search_latency_snapshot(int64_t *total_us, int64_t *count, int64_t *max_us, int reset)
{
   pthread_mutex_lock(&latency_mu);
   if (total_us)
      *total_us = latency_total_us;
   if (count)
      *count = latency_count;
   if (max_us)
      *max_us = latency_max_us;
   if (reset)
   {
      latency_total_us = 0;
      latency_count = 0;
      latency_max_us = 0;
   }
   pthread_mutex_unlock(&latency_mu);
}

/* Build pgvector text representation: "[f0,f1,...,fn-1]"
 * Caller must free the returned string. Returns NULL on alloc failure. */
static char *build_vec_text(const float *vec, int dim)
{
   /* Each float: up to 15 significant digits + sign + decimal + exponent + comma.
    * 24 chars per element is safe; add 4 for brackets and NUL. */
   size_t bufsz = (size_t)dim * 24 + 4;
   char *buf = malloc(bufsz);
   if (!buf)
      return NULL;
   size_t pos = 0;
   buf[pos++] = '[';
   for (int i = 0; i < dim; i++)
   {
      int written = snprintf(buf + pos, bufsz - pos, i ? ",%.9g" : "%.9g", (double)vec[i]);
      if (written <= 0 || (size_t)written >= bufsz - pos)
      {
         free(buf);
         return NULL;
      }
      pos += (size_t)written;
   }
   buf[pos++] = ']';
   buf[pos] = '\0';
   return buf;
}

/* Extract a string field from a JSON object; writes to buf (max len).
 * Writes "" if the field is absent or not a string. */
static void json_str(const cJSON *obj, const char *key, char *buf, size_t len)
{
   buf[0] = '\0';
   if (!obj || !key || !buf || len == 0)
      return;
   const cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
   if (cJSON_IsString(item) && item->valuestring)
      snprintf(buf, len, "%s", item->valuestring);
}

/* -------------------------------------------------------------------------
 * Table-level helpers
 * ---------------------------------------------------------------------- */

int pgvec_table_ready(const char *table)
{
   if (!table || !table[0])
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char sql[256];
   char errbuf[256];
   snprintf(sql, sizeof(sql),
            "SELECT 1 FROM pg_indexes WHERE tablename = :tbl AND indexname LIKE '%%_hnsw'");
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return -1;
   aimee_pg_bind_text(stmt, "tbl", table);
   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   int result = (rc == AIMEE_PG_ROW) ? 1 : (rc == AIMEE_PG_DONE ? 0 : -1);
   aimee_pg_finalize(stmt);
   return result;
}

int64_t pgvec_point_count(const char *table)
{
   if (!table || !table[0])
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char sql[128];
   char errbuf[256];
   snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return -1;
   int64_t count = 0;
   if (aimee_pg_step(stmt, errbuf, sizeof(errbuf)) == AIMEE_PG_ROW)
      count = aimee_pg_column_int64(stmt, 0);
   aimee_pg_finalize(stmt);
   return count;
}

int pgvec_ensure_index(const char *table, int dim, int recreate)
{
   if (!table || !table[0])
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char errbuf[256];
   (void)dim; /* pgvector infers dimension from data; not needed for index DDL */

   if (recreate)
   {
      char sql[256];
      snprintf(sql, sizeof(sql), "TRUNCATE %s", table);
      if (aimee_pg_exec(pg, sql, errbuf, sizeof(errbuf)) != 0)
      {
         LOG_WARN("pgvec", "truncate %s failed: %s", table, errbuf);
         return -1;
      }
      /* index name is idx_<table>_hnsw */
      snprintf(sql, sizeof(sql), "DROP INDEX IF EXISTS idx_%s_hnsw", table);
      if (aimee_pg_exec(pg, sql, errbuf, sizeof(errbuf)) != 0)
         LOG_WARN("pgvec", "drop index idx_%s_hnsw failed: %s", table, errbuf);
   }

   char sql[256];
   snprintf(sql, sizeof(sql),
            "CREATE INDEX IF NOT EXISTS idx_%s_hnsw "
            "ON %s USING hnsw (embedding halfvec_cosine_ops)",
            table, table);
   if (aimee_pg_exec(pg, sql, errbuf, sizeof(errbuf)) != 0)
   {
      LOG_WARN("pgvec", "create hnsw index on %s failed: %s", table, errbuf);
      return -1;
   }
   return 0;
}

/* -------------------------------------------------------------------------
 * Corpus index helpers
 * ---------------------------------------------------------------------- */

int pgvec_vectorscale_available(void)
{
   void *pg = db2_conn();
   if (!pg)
      return 0;
   char errbuf[256];
   const char *sql = "SELECT 1 FROM pg_extension WHERE extname = 'vectorscale' LIMIT 1";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return 0;
   int found = (aimee_pg_step(stmt, errbuf, sizeof(errbuf)) == AIMEE_PG_ROW);
   aimee_pg_finalize(stmt);
   return found;
}

const char *pgvec_corpus_index_type(const char *configured, int64_t corpus_rows,
                                    int vectorscale_available, int64_t diskann_threshold)
{
   if (!configured || !configured[0] || strcmp(configured, "hnsw") == 0)
      return "hnsw";
   if (strcmp(configured, "diskann") == 0)
      return vectorscale_available ? "diskann" : "hnsw";
   /* auto: pick diskann only when extension present and corpus exceeds threshold */
   if (vectorscale_available && corpus_rows >= diskann_threshold)
      return "diskann";
   return "hnsw";
}

int pgvec_ensure_corpus_index(const char *table, const char *index_type, int recreate)
{
   if (!table || !table[0])
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   const char *use_type = index_type;
   if (use_type && strcmp(use_type, "diskann") == 0 && !pgvec_vectorscale_available())
   {
      LOG_WARN("pgvec", "pgvectorscale unavailable; corpus table %s falls back to HNSW", table);
      use_type = "hnsw";
   }

   char errbuf[256];
   if (recreate)
   {
      char sql[512];
      snprintf(sql, sizeof(sql), "DROP INDEX IF EXISTS idx_%s_hnsw", table);
      aimee_pg_exec(pg, sql, errbuf, sizeof(errbuf));
      snprintf(sql, sizeof(sql), "DROP INDEX IF EXISTS idx_%s_diskann", table);
      aimee_pg_exec(pg, sql, errbuf, sizeof(errbuf));
   }

   char sql[512];
   if (use_type && strcmp(use_type, "diskann") == 0)
      snprintf(sql, sizeof(sql),
               "CREATE INDEX IF NOT EXISTS idx_%s_diskann "
               "ON %s USING diskann (embedding halfvec_cosine_ops)",
               table, table);
   else
      snprintf(sql, sizeof(sql),
               "CREATE INDEX IF NOT EXISTS idx_%s_hnsw "
               "ON %s USING hnsw (embedding halfvec_cosine_ops)",
               table, table);

   if (aimee_pg_exec(pg, sql, errbuf, sizeof(errbuf)) != 0)
   {
      LOG_WARN("pgvec", "create %s index on corpus table %s failed: %s",
               use_type ? use_type : "hnsw", table, errbuf);
      return -1;
   }
   LOG_INFO("pgvec", "corpus index type=%s table=%s", use_type ? use_type : "hnsw", table);
   return 0;
}

/* -------------------------------------------------------------------------
 * Memory upsert
 * ---------------------------------------------------------------------- */

int pgvec_memory_upsert(int64_t point_id, const float *vec, int dim, const char *payload_json)
{
   if (!vec || dim <= 0)
      return 0;
   /* Dimension guard: the embedding column is halfvec(N) where N is the active
    * configured dim (db2_embedding_dim). A vector of a different dim (e.g. the
    * 384-dim builtin fallback against a 1024/2560 column) is rejected by
    * Postgres with "expected N dimensions, not M" — but that error was being
    * counted as a successful embed, silently leaving memory_embeddings empty.
    * Fail loudly here instead of shipping a mismatched vector. */
   int expect = db2_embedding_dim();
   if (expect > 0 && dim != expect)
   {
      aimee_log(
          LOG_WARN, "pgvec",
          "memory embedding dim mismatch: got %d, expected %d (point_id=%lld); refusing upsert",
          dim, expect, (long long)point_id);
      return -1;
   }
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char record_type[64] = "";
   char primary_scope[64] = "";
   char workspace[128] = "";
   char project[256] = "";
   char kind[64] = "";

   if (payload_json && payload_json[0])
   {
      cJSON *obj = cJSON_Parse(payload_json);
      if (obj)
      {
         json_str(obj, "record_type", record_type, sizeof(record_type));
         json_str(obj, "primary_scope", primary_scope, sizeof(primary_scope));
         json_str(obj, "workspace", workspace, sizeof(workspace));
         json_str(obj, "project", project, sizeof(project));
         json_str(obj, "kind", kind, sizeof(kind));
         /* unit points use memory_kind for the kind filter */
         if (!kind[0])
            json_str(obj, "memory_kind", kind, sizeof(kind));
         cJSON_Delete(obj);
      }
   }

   char *vec_text = build_vec_text(vec, dim);
   if (!vec_text)
      return -1;

   static const char *sql = "INSERT INTO memory_embeddings "
                            "  (point_id, embedding, record_type, primary_scope, workspace, "
                            "project, kind, payload_json) "
                            "VALUES "
                            "  (:point_id, :embedding::halfvec, :record_type, :primary_scope, "
                            ":workspace, :project, :kind, :payload) "
                            "ON CONFLICT (point_id) DO UPDATE SET "
                            "  embedding = EXCLUDED.embedding, "
                            "  record_type = EXCLUDED.record_type, "
                            "  primary_scope = EXCLUDED.primary_scope, "
                            "  workspace = EXCLUDED.workspace, "
                            "  project = EXCLUDED.project, "
                            "  kind = EXCLUDED.kind, "
                            "  payload_json = EXCLUDED.payload_json";

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
   {
      free(vec_text);
      return -1;
   }
   aimee_pg_bind_int64(stmt, "point_id", point_id);
   aimee_pg_bind_text(stmt, "embedding", vec_text);
   aimee_pg_bind_text(stmt, "record_type", record_type);
   aimee_pg_bind_text(stmt, "primary_scope", primary_scope);
   aimee_pg_bind_text(stmt, "workspace", workspace);
   aimee_pg_bind_text(stmt, "project", project);
   aimee_pg_bind_text(stmt, "kind", kind);
   aimee_pg_bind_text(stmt, "payload", payload_json ? payload_json : "");

   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   free(vec_text);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int pgvec_memory_delete(int64_t point_id)
{
   void *pg = db2_conn();
   if (!pg)
      return -1;
   static const char *sql = "DELETE FROM memory_embeddings WHERE point_id = :point_id";
   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return -1;
   aimee_pg_bind_int64(stmt, "point_id", point_id);
   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * KB upsert
 * ---------------------------------------------------------------------- */

int pgvec_kb_upsert(int64_t point_id, const float *vec, int dim, const char *payload_json)
{
   if (!vec || dim <= 0)
      return 0;
   /* Same dim guard as pgvec_memory_upsert: never ship a vector whose dim does
    * not match the configured halfvec(N) column. */
   int expect = db2_embedding_dim();
   if (expect > 0 && dim != expect)
   {
      aimee_log(LOG_WARN, "pgvec",
                "kb embedding dim mismatch: got %d, expected %d (point_id=%lld); refusing upsert",
                dim, expect, (long long)point_id);
      return -1;
   }
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char project[256] = "";
   if (payload_json && payload_json[0])
   {
      cJSON *obj = cJSON_Parse(payload_json);
      if (obj)
      {
         json_str(obj, "project", project, sizeof(project));
         cJSON_Delete(obj);
      }
   }

   char *vec_text = build_vec_text(vec, dim);
   if (!vec_text)
      return -1;

   static const char *sql = "INSERT INTO kb_embeddings "
                            "  (point_id, embedding, project, payload_json) "
                            "VALUES "
                            "  (:point_id, :embedding::halfvec, :project, :payload) "
                            "ON CONFLICT (point_id) DO UPDATE SET "
                            "  embedding = EXCLUDED.embedding, "
                            "  project = EXCLUDED.project, "
                            "  payload_json = EXCLUDED.payload_json";

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
   {
      free(vec_text);
      return -1;
   }
   aimee_pg_bind_int64(stmt, "point_id", point_id);
   aimee_pg_bind_text(stmt, "embedding", vec_text);
   aimee_pg_bind_text(stmt, "project", project);
   aimee_pg_bind_text(stmt, "payload", payload_json ? payload_json : "");

   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   free(vec_text);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int pgvec_kb_upsert_batch(const int64_t *ids, const float *vecs, int dim,
                          const char *const *payloads, int count)
{
   if (!ids || !vecs || dim <= 0 || count <= 0)
      return -1;
   for (int i = 0; i < count; i++)
   {
      if (!payloads || !payloads[i])
         return -1;
      if (pgvec_kb_upsert(ids[i], vecs + (size_t)i * (size_t)dim, dim, payloads[i]) != 0)
         return -1;
   }
   return 0;
}

int pgvec_kb_delete(int64_t point_id)
{
   void *pg = db2_conn();
   if (!pg)
      return -1;
   static const char *sql = "DELETE FROM kb_embeddings WHERE point_id = :point_id";
   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return -1;
   aimee_pg_bind_int64(stmt, "point_id", point_id);
   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int pgvec_kb_delete_project(const char *project)
{
   if (!project || !project[0])
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;
   static const char *sql = "DELETE FROM kb_embeddings WHERE project = :project";
   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return -1;
   aimee_pg_bind_text(stmt, "project", project);
   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * Scroll
 * ---------------------------------------------------------------------- */

int pgvec_scroll(const char *table, int64_t offset, int64_t *ids_out, int max,
                 int64_t *next_offset_out, int *done_out)
{
   if (!table || !ids_out || max <= 0 || !next_offset_out || !done_out)
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char sql[256];
   char errbuf[256];
   if (offset < 0)
      snprintf(sql, sizeof(sql), "SELECT point_id FROM %s ORDER BY point_id LIMIT :lim", table);
   else
      snprintf(sql, sizeof(sql),
               "SELECT point_id FROM %s WHERE point_id > :off ORDER BY point_id LIMIT :lim", table);

   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return -1;
   if (offset >= 0)
      aimee_pg_bind_int64(stmt, "off", offset);
   aimee_pg_bind_int(stmt, "lim", max);

   int count = 0;
   int64_t last_id = offset;
   aimee_pg_step_t rc;
   while ((rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf))) == AIMEE_PG_ROW)
   {
      if (count >= max)
         break;
      last_id = aimee_pg_column_int64(stmt, 0);
      ids_out[count++] = last_id;
   }
   aimee_pg_finalize(stmt);

   if (rc == AIMEE_PG_ERR)
      return -1;

   *done_out = (count < max) ? 1 : 0;
   *next_offset_out = *done_out ? -1 : last_id;
   return count;
}

/* -------------------------------------------------------------------------
 * Search
 * ---------------------------------------------------------------------- */

int pgvec_memory_search(const float *vec, int dim, const char *record_type,
                        const char *const *kinds, int n_kinds, const char *workspace,
                        const char *project, int limit, int64_t *ids, double *scores, int max)
{
   if (!vec || dim <= 0 || !record_type || !ids || !scores || max <= 0)
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char *vec_text = build_vec_text(vec, dim);
   if (!vec_text)
      return -1;

   /* Build the WHERE clause.  record_type filter is always present.
    * Scope filter is added only when workspace or project is non-empty.
    * Kind IN filter is added only when n_kinds > 0. */
   char where[1024] = "WHERE record_type = :record_type";
   size_t wpos = strlen(where);

   int has_scope = (workspace && workspace[0]) || (project && project[0]);
   if (has_scope)
   {
      /* (primary_scope='global' OR workspace=:ws OR project=:pj) */
      snprintf(where + wpos, sizeof(where) - wpos,
               " AND (primary_scope = 'global' OR workspace = :ws OR project = :pj)");
      wpos = strlen(where);
   }

   /* Build kinds IN clause inline (values are not user input, they come from
    * internal callers; we still quote them safely). */
   char kinds_clause[512] = "";
   if (kinds && n_kinds > 0)
   {
      size_t kpos = 0;
      kpos += (size_t)snprintf(kinds_clause + kpos, sizeof(kinds_clause) - kpos, " AND kind IN (");
      for (int i = 0; i < n_kinds && kpos < sizeof(kinds_clause) - 4; i++)
      {
         if (!kinds[i] || !kinds[i][0])
            continue;
         /* Escape single quotes in kind values defensively. */
         if (i > 0)
            kpos += (size_t)snprintf(kinds_clause + kpos, sizeof(kinds_clause) - kpos, ",");
         kpos += (size_t)snprintf(kinds_clause + kpos, sizeof(kinds_clause) - kpos, "'");
         for (const char *p = kinds[i]; *p && kpos < sizeof(kinds_clause) - 4; p++)
         {
            if (*p == '\'')
               kinds_clause[kpos++] = '\'';
            kinds_clause[kpos++] = *p;
         }
         kpos += (size_t)snprintf(kinds_clause + kpos, sizeof(kinds_clause) - kpos, "'");
      }
      kpos += (size_t)snprintf(kinds_clause + kpos, sizeof(kinds_clause) - kpos, ")");
   }

   char sql[2048];
   snprintf(sql, sizeof(sql),
            "SELECT point_id, 1.0 - (embedding <=> :qvec::halfvec) AS score "
            "FROM memory_embeddings %s%s "
            "ORDER BY embedding <=> :qvec::halfvec "
            "LIMIT :lim",
            where, kinds_clause);

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
   {
      free(vec_text);
      return 0; /* pgvector not available — no vector results */
   }
   aimee_pg_bind_text(stmt, "qvec", vec_text);
   aimee_pg_bind_text(stmt, "record_type", record_type);
   if (has_scope)
   {
      aimee_pg_bind_text(stmt, "ws", workspace ? workspace : "");
      aimee_pg_bind_text(stmt, "pj", project ? project : "");
   }
   aimee_pg_bind_int(stmt, "lim", limit > 0 ? limit : max);

   int64_t t0 = monotonic_us();
   int n = 0;
   aimee_pg_step_t rc;
   while ((rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf))) == AIMEE_PG_ROW)
   {
      if (n >= max)
         break;
      ids[n] = aimee_pg_column_int64(stmt, 0);
      scores[n] = aimee_pg_column_double(stmt, 1);
      n++;
   }
   aimee_pg_finalize(stmt);
   free(vec_text);

   int64_t elapsed = monotonic_us() - t0;
   record_latency(elapsed);

   const char *slow_env = getenv("AIMEE_PGVEC_SLOW_QUERY_MS");
   int slow_ms = (slow_env && slow_env[0]) ? atoi(slow_env) : 100;
   if (slow_ms > 0 && elapsed > (int64_t)slow_ms * 1000)
      LOG_WARN("pgvec_slow", "slow search latency_ms=%.2f record_type=%.32s",
               (double)elapsed / 1000.0, record_type);

   return (rc == AIMEE_PG_ERR) ? -1 : n;
}

int pgvec_kb_search(const char *project, const float *vec, int dim, int limit, int64_t *ids,
                    double *scores, int max)
{
   if (!vec || dim <= 0 || !ids || !scores || max <= 0)
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char *vec_text = build_vec_text(vec, dim);
   if (!vec_text)
      return -1;

   /* A named project scopes the search to it; a NULL/empty project searches the
    * WHOLE corpus (all projects). The old code always bound `WHERE project =
    * :project` with "" for the NULL case, which matched no rows (every chunk has
    * a real project) — so a project-less /v1/kb/search silently returned zero
    * hits instead of searching everything. Mirrors pgvec_curator_entity_search,
    * which already exposes its scope filters as optional. */
   int has_project = (project && project[0]);
   const char *sql = has_project ? "SELECT point_id, 1.0 - (embedding <=> :qvec::halfvec) AS score "
                                   "FROM kb_embeddings "
                                   "WHERE project = :project "
                                   "ORDER BY embedding <=> :qvec::halfvec "
                                   "LIMIT :lim"
                                 : "SELECT point_id, 1.0 - (embedding <=> :qvec::halfvec) AS score "
                                   "FROM kb_embeddings "
                                   "ORDER BY embedding <=> :qvec::halfvec "
                                   "LIMIT :lim";

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
   {
      free(vec_text);
      return 0; /* pgvector not available — no vector results */
   }
   aimee_pg_bind_text(stmt, "qvec", vec_text);
   if (has_project)
      aimee_pg_bind_text(stmt, "project", project);
   aimee_pg_bind_int(stmt, "lim", limit > 0 ? limit : max);

   int64_t t0 = monotonic_us();
   int n = 0;
   aimee_pg_step_t rc;
   while ((rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf))) == AIMEE_PG_ROW)
   {
      if (n >= max)
         break;
      ids[n] = aimee_pg_column_int64(stmt, 0);
      scores[n] = aimee_pg_column_double(stmt, 1);
      n++;
   }
   aimee_pg_finalize(stmt);
   free(vec_text);

   int64_t elapsed = monotonic_us() - t0;
   record_latency(elapsed);
   return (rc == AIMEE_PG_ERR) ? -1 : n;
}

/* -------------------------------------------------------------------------
 * KB PDF vectors (structured-PDF Phase A1)
 *
 * A disjoint relation from kb_embeddings: the general /v1/search transport
 * (pgvec_kb_search) only ever names kb_embeddings, so PDF vectors are
 * structurally unreachable from general vector search. These mirror the
 * pgvec_kb_* helpers but target kb_pdf_embeddings, so the only caller that can
 * read PDF vectors is the access-controlled PDF search surface.
 * ---------------------------------------------------------------------- */

int pgvec_kbpdf_upsert(int64_t point_id, const float *vec, int dim, const char *payload_json)
{
   if (!vec || dim <= 0)
      return 0;
   int expect = db2_embedding_dim();
   if (expect > 0 && dim != expect)
   {
      aimee_log(LOG_WARN, "pgvec",
                "kb_pdf embedding dim mismatch: got %d, expected %d (point_id=%lld); refusing "
                "upsert",
                dim, expect, (long long)point_id);
      return -1;
   }
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char project[256] = "";
   if (payload_json && payload_json[0])
   {
      cJSON *obj = cJSON_Parse(payload_json);
      if (obj)
      {
         json_str(obj, "project", project, sizeof(project));
         cJSON_Delete(obj);
      }
   }

   char *vec_text = build_vec_text(vec, dim);
   if (!vec_text)
      return -1;

   static const char *sql = "INSERT INTO kb_pdf_embeddings "
                            "  (point_id, embedding, project, payload_json) "
                            "VALUES "
                            "  (:point_id, :embedding::halfvec, :project, :payload) "
                            "ON CONFLICT (point_id) DO UPDATE SET "
                            "  embedding = EXCLUDED.embedding, "
                            "  project = EXCLUDED.project, "
                            "  payload_json = EXCLUDED.payload_json";

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
   {
      free(vec_text);
      return -1;
   }
   aimee_pg_bind_int64(stmt, "point_id", point_id);
   aimee_pg_bind_text(stmt, "embedding", vec_text);
   aimee_pg_bind_text(stmt, "project", project);
   aimee_pg_bind_text(stmt, "payload", payload_json ? payload_json : "");

   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   free(vec_text);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int pgvec_kbpdf_delete(int64_t point_id)
{
   void *pg = db2_conn();
   if (!pg)
      return -1;
   static const char *sql = "DELETE FROM kb_pdf_embeddings WHERE point_id = :point_id";
   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return -1;
   aimee_pg_bind_int64(stmt, "point_id", point_id);
   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int pgvec_kbpdf_delete_project(const char *project)
{
   if (!project || !project[0])
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;
   static const char *sql = "DELETE FROM kb_pdf_embeddings WHERE project = :project";
   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return -1;
   aimee_pg_bind_text(stmt, "project", project);
   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

/* Vector search over PDF chunk embeddings ONLY. There is deliberately no
 * "search both kb_embeddings and kb_pdf_embeddings" helper: keeping the relations
 * disjoint at the transport layer is the structural isolation A1 requires. */
int pgvec_kbpdf_search(const char *project, const float *vec, int dim, int limit, int64_t *ids,
                       double *scores, int max)
{
   if (!vec || dim <= 0 || !ids || !scores || max <= 0)
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char *vec_text = build_vec_text(vec, dim);
   if (!vec_text)
      return -1;

   int has_project = (project && project[0]);
   const char *sql = has_project ? "SELECT point_id, 1.0 - (embedding <=> :qvec::halfvec) AS score "
                                   "FROM kb_pdf_embeddings "
                                   "WHERE project = :project "
                                   "ORDER BY embedding <=> :qvec::halfvec "
                                   "LIMIT :lim"
                                 : "SELECT point_id, 1.0 - (embedding <=> :qvec::halfvec) AS score "
                                   "FROM kb_pdf_embeddings "
                                   "ORDER BY embedding <=> :qvec::halfvec "
                                   "LIMIT :lim";

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
   {
      free(vec_text);
      return 0; /* pgvector not available — no vector results */
   }
   aimee_pg_bind_text(stmt, "qvec", vec_text);
   if (has_project)
      aimee_pg_bind_text(stmt, "project", project);
   aimee_pg_bind_int(stmt, "lim", limit > 0 ? limit : max);

   int n = 0;
   aimee_pg_step_t rc;
   while ((rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf))) == AIMEE_PG_ROW)
   {
      if (n >= max)
         break;
      ids[n] = aimee_pg_column_int64(stmt, 0);
      scores[n] = aimee_pg_column_double(stmt, 1);
      n++;
   }
   aimee_pg_finalize(stmt);
   free(vec_text);
   return (rc == AIMEE_PG_ERR) ? -1 : n;
}

int pgvec_curator_entity_upsert(int64_t point_id, const float *vec, int dim, const char *scope_kind,
                                const char *scope_id, const char *canonical_name,
                                const char *artifact_id, const char *payload_json)
{
   if (!vec || dim <= 0)
      return 0;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char *vec_text = build_vec_text(vec, dim);
   if (!vec_text)
      return -1;

   static const char *sql =
       "INSERT INTO curator_entity_vectors "
       "  (point_id, embedding, scope_kind, scope_id, canonical_name, artifact_id, payload_json) "
       "VALUES "
       "  (:point_id, :embedding::halfvec, :scope_kind, :scope_id, :canonical_name, :artifact_id, "
       "   :payload) "
       "ON CONFLICT (point_id) DO UPDATE SET "
       "  embedding = EXCLUDED.embedding, "
       "  scope_kind = EXCLUDED.scope_kind, "
       "  scope_id = EXCLUDED.scope_id, "
       "  canonical_name = EXCLUDED.canonical_name, "
       "  artifact_id = EXCLUDED.artifact_id, "
       "  payload_json = EXCLUDED.payload_json";

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
   {
      free(vec_text);
      return -1;
   }
   aimee_pg_bind_int64(stmt, "point_id", point_id);
   aimee_pg_bind_text(stmt, "embedding", vec_text);
   aimee_pg_bind_text(stmt, "scope_kind", scope_kind ? scope_kind : "");
   aimee_pg_bind_text(stmt, "scope_id", scope_id ? scope_id : "");
   aimee_pg_bind_text(stmt, "canonical_name", canonical_name ? canonical_name : "");
   aimee_pg_bind_text(stmt, "artifact_id", artifact_id ? artifact_id : "");
   aimee_pg_bind_text(stmt, "payload", payload_json ? payload_json : "");

   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   free(vec_text);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int pgvec_curator_entity_lookup(int64_t point_id, char *artifact_id_out, int aid_len,
                                char *name_out, int name_len)
{
   if (artifact_id_out && aid_len > 0)
      artifact_id_out[0] = '\0';
   if (name_out && name_len > 0)
      name_out[0] = '\0';
   void *pg = db2_conn();
   if (!pg)
      return -1;

   static const char *sql = "SELECT artifact_id, canonical_name FROM curator_entity_vectors "
                            "WHERE point_id = :point_id";
   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return -1;
   aimee_pg_bind_int64(stmt, "point_id", point_id);

   int found = 0;
   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   if (rc == AIMEE_PG_ROW)
   {
      found = 1;
      const char *aid = aimee_pg_column_text(stmt, 0);
      const char *name = aimee_pg_column_text(stmt, 1);
      if (artifact_id_out && aid_len > 0)
         snprintf(artifact_id_out, (size_t)aid_len, "%s", aid ? aid : "");
      if (name_out && name_len > 0)
         snprintf(name_out, (size_t)name_len, "%s", name ? name : "");
   }
   aimee_pg_finalize(stmt);
   return (rc == AIMEE_PG_ROW || rc == AIMEE_PG_DONE) ? found : -1;
}

int pgvec_curator_entity_delete(int64_t point_id)
{
   void *pg = db2_conn();
   if (!pg)
      return -1;
   static const char *sql = "DELETE FROM curator_entity_vectors WHERE point_id = :point_id";
   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return -1;
   aimee_pg_bind_int64(stmt, "point_id", point_id);
   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int pgvec_curator_entity_search(const char *scope_kind, const char *scope_id, const float *vec,
                                int dim, int limit, int64_t *ids, double *scores, int max)
{
   if (!vec || dim <= 0 || !ids || !scores || max <= 0)
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char *vec_text = build_vec_text(vec, dim);
   if (!vec_text)
      return -1;

   /* Build the WHERE clause dynamically so an empty scope_kind / scope_id
    * does not contribute a tautology. Mirrors pgvec_kb_search (which has a
    * hard-coded project filter) but exposes the filters as optional. */
   char where_sql[256];
   int n = 0;
   int have_kind = scope_kind && scope_kind[0];
   int have_id = scope_id && scope_id[0];
   if (have_kind && have_id)
      n = snprintf(where_sql, sizeof(where_sql),
                   "WHERE scope_kind = :scope_kind AND scope_id = :scope_id");
   else if (have_kind)
      n = snprintf(where_sql, sizeof(where_sql), "WHERE scope_kind = :scope_kind");
   else if (have_id)
      n = snprintf(where_sql, sizeof(where_sql), "WHERE scope_id = :scope_id");
   else
      n = snprintf(where_sql, sizeof(where_sql), "WHERE TRUE");
   if (n < 0 || n >= (int)sizeof(where_sql))
   {
      free(vec_text);
      return -1;
   }

   /* Compose SQL with the assembled WHERE clause. */
   size_t sql_len = strlen(where_sql) + 256;
   char *sql = malloc(sql_len);
   if (!sql)
   {
      free(vec_text);
      return -1;
   }
   snprintf(sql, sql_len,
            "SELECT point_id, 1.0 - (embedding <=> :qvec::halfvec) AS score "
            "FROM curator_entity_vectors "
            "%s "
            "ORDER BY embedding <=> :qvec::halfvec "
            "LIMIT :lim",
            where_sql);

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   free(sql);
   if (!stmt)
   {
      free(vec_text);
      return 0; /* pgvector not available — no vector results */
   }
   aimee_pg_bind_text(stmt, "qvec", vec_text);
   if (have_kind)
      aimee_pg_bind_text(stmt, "scope_kind", scope_kind);
   if (have_id)
      aimee_pg_bind_text(stmt, "scope_id", scope_id);
   aimee_pg_bind_int(stmt, "lim", limit > 0 ? limit : max);

   int64_t t0 = monotonic_us();
   int rows = 0;
   aimee_pg_step_t rc;
   while ((rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf))) == AIMEE_PG_ROW)
   {
      if (rows >= max)
         break;
      ids[rows] = aimee_pg_column_int64(stmt, 0);
      scores[rows] = aimee_pg_column_double(stmt, 1);
      rows++;
   }
   aimee_pg_finalize(stmt);
   free(vec_text);

   int64_t elapsed = monotonic_us() - t0;
   record_latency(elapsed);
   return (rc == AIMEE_PG_ERR) ? -1 : rows;
}

int pgvec_curator_narrative_upsert(int64_t point_id, const float *vec, int dim,
                                   const char *artifact_id, const char *kind, const char *doc_id,
                                   const char *status, const char *priority,
                                   const char *payload_json)
{
   if (!vec || dim <= 0)
      return 0;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char *vec_text = build_vec_text(vec, dim);
   if (!vec_text)
      return -1;

   static const char *sql =
       "INSERT INTO curator_narrative_vectors "
       "  (point_id, embedding, artifact_id, kind, doc_id, status, priority, payload_json) "
       "VALUES "
       "  (:point_id, :embedding::halfvec, :artifact_id, :kind, :doc_id, :status, :priority, "
       ":payload) "
       "ON CONFLICT (point_id) DO UPDATE SET "
       "  embedding = EXCLUDED.embedding, "
       "  artifact_id = EXCLUDED.artifact_id, "
       "  kind = EXCLUDED.kind, "
       "  doc_id = EXCLUDED.doc_id, "
       "  status = EXCLUDED.status, "
       "  priority = EXCLUDED.priority, "
       "  payload_json = EXCLUDED.payload_json";

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
   {
      free(vec_text);
      return -1;
   }
   aimee_pg_bind_int64(stmt, "point_id", point_id);
   aimee_pg_bind_text(stmt, "embedding", vec_text);
   aimee_pg_bind_text(stmt, "artifact_id", artifact_id ? artifact_id : "");
   aimee_pg_bind_text(stmt, "kind", kind ? kind : "");
   aimee_pg_bind_text(stmt, "doc_id", doc_id ? doc_id : "");
   aimee_pg_bind_text(stmt, "status", status ? status : "");
   aimee_pg_bind_text(stmt, "priority", priority ? priority : "");
   aimee_pg_bind_text(stmt, "payload", payload_json ? payload_json : "");

   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   free(vec_text);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int pgvec_curator_narrative_delete(int64_t point_id)
{
   void *pg = db2_conn();
   if (!pg)
      return -1;
   static const char *sql = "DELETE FROM curator_narrative_vectors WHERE point_id = :point_id";
   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return -1;
   aimee_pg_bind_int64(stmt, "point_id", point_id);
   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int pgvec_curator_narrative_search(const char *kind, const char *status, const char *priority,
                                   const float *vec, int dim, int limit, int64_t *ids,
                                   double *scores, int max)
{
   if (!vec || dim <= 0 || !ids || !scores || max <= 0)
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char *vec_text = build_vec_text(vec, dim);
   if (!vec_text)
      return -1;

   /* Build the WHERE clause dynamically so an empty filter does not contribute
    * a tautology.  Mirrors pgvec_curator_entity_search. */
   char where_sql[256];
   int n = 0;
   int have_kind = kind && kind[0];
   int have_status = status && status[0];
   int have_priority = priority && priority[0];
   if (have_kind && have_status && have_priority)
      n = snprintf(where_sql, sizeof(where_sql),
                   "WHERE kind = :kind AND status = :status AND priority = :priority");
   else if (have_kind && have_status)
      n = snprintf(where_sql, sizeof(where_sql), "WHERE kind = :kind AND status = :status");
   else if (have_kind && have_priority)
      n = snprintf(where_sql, sizeof(where_sql), "WHERE kind = :kind AND priority = :priority");
   else if (have_status && have_priority)
      n = snprintf(where_sql, sizeof(where_sql), "WHERE status = :status AND priority = :priority");
   else if (have_kind)
      n = snprintf(where_sql, sizeof(where_sql), "WHERE kind = :kind");
   else if (have_status)
      n = snprintf(where_sql, sizeof(where_sql), "WHERE status = :status");
   else if (have_priority)
      n = snprintf(where_sql, sizeof(where_sql), "WHERE priority = :priority");
   else
      n = snprintf(where_sql, sizeof(where_sql), "WHERE TRUE");
   if (n < 0 || n >= (int)sizeof(where_sql))
   {
      free(vec_text);
      return -1;
   }

   size_t sql_len = strlen(where_sql) + 256;
   char *sql = malloc(sql_len);
   if (!sql)
   {
      free(vec_text);
      return -1;
   }
   snprintf(sql, sql_len,
            "SELECT point_id, 1.0 - (embedding <=> :qvec::halfvec) AS score "
            "FROM curator_narrative_vectors "
            "%s "
            "ORDER BY embedding <=> :qvec::halfvec "
            "LIMIT :lim",
            where_sql);

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   free(sql);
   if (!stmt)
   {
      free(vec_text);
      return 0; /* pgvector not available — no vector results */
   }
   aimee_pg_bind_text(stmt, "qvec", vec_text);
   if (have_kind)
      aimee_pg_bind_text(stmt, "kind", kind);
   if (have_status)
      aimee_pg_bind_text(stmt, "status", status);
   if (have_priority)
      aimee_pg_bind_text(stmt, "priority", priority);
   aimee_pg_bind_int(stmt, "lim", limit > 0 ? limit : max);

   int64_t t0 = monotonic_us();
   int rows = 0;
   aimee_pg_step_t rc;
   while ((rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf))) == AIMEE_PG_ROW)
   {
      if (rows >= max)
         break;
      ids[rows] = aimee_pg_column_int64(stmt, 0);
      scores[rows] = aimee_pg_column_double(stmt, 1);
      rows++;
   }
   aimee_pg_finalize(stmt);
   free(vec_text);

   int64_t elapsed = monotonic_us() - t0;
   record_latency(elapsed);
   return (rc == AIMEE_PG_ERR) ? -1 : rows;
}

int pgvec_curator_claim_upsert(int64_t point_id, const float *subj_attr_vec, const float *value_vec,
                               int dim, const char *artifact_id, const char *subject,
                               const char *attribute, const char *value, const char *claim_kind,
                               const char *payload_json)
{
   if (!subj_attr_vec || !value_vec || dim <= 0)
      return 0;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char *subj_text = build_vec_text(subj_attr_vec, dim);
   char *val_text = build_vec_text(value_vec, dim);
   if (!subj_text || !val_text)
   {
      free(subj_text);
      free(val_text);
      return -1;
   }

   static const char *sql =
       "INSERT INTO curator_claim_vectors "
       "  (point_id, subj_attr_vec, value_vec, artifact_id, subject, attribute, value, "
       "   claim_kind, payload_json) "
       "VALUES "
       "  (:point_id, :subj_attr::halfvec, :value::halfvec, :artifact_id, :subject, :attribute, "
       "   :value_col, :claim_kind, :payload) "
       "ON CONFLICT (point_id) DO UPDATE SET "
       "  subj_attr_vec = EXCLUDED.subj_attr_vec, "
       "  value_vec = EXCLUDED.value_vec, "
       "  artifact_id = EXCLUDED.artifact_id, "
       "  subject = EXCLUDED.subject, "
       "  attribute = EXCLUDED.attribute, "
       "  value = EXCLUDED.value, "
       "  claim_kind = EXCLUDED.claim_kind, "
       "  payload_json = EXCLUDED.payload_json";

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
   {
      free(subj_text);
      free(val_text);
      return -1;
   }
   aimee_pg_bind_int64(stmt, "point_id", point_id);
   aimee_pg_bind_text(stmt, "subj_attr", subj_text);
   aimee_pg_bind_text(stmt, "value", val_text);
   aimee_pg_bind_text(stmt, "artifact_id", artifact_id ? artifact_id : "");
   aimee_pg_bind_text(stmt, "subject", subject ? subject : "");
   aimee_pg_bind_text(stmt, "attribute", attribute ? attribute : "");
   aimee_pg_bind_text(stmt, "value_col", value ? value : "");
   aimee_pg_bind_text(stmt, "claim_kind", claim_kind ? claim_kind : "");
   aimee_pg_bind_text(stmt, "payload", payload_json ? payload_json : "");

   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   free(subj_text);
   free(val_text);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int pgvec_curator_claim_delete(int64_t point_id)
{
   void *pg = db2_conn();
   if (!pg)
      return -1;
   static const char *sql = "DELETE FROM curator_claim_vectors WHERE point_id = :point_id";
   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return -1;
   aimee_pg_bind_int64(stmt, "point_id", point_id);
   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int pgvec_curator_claim_search(const char *which_vec, const char *claim_kind, const float *vec,
                               int dim, int limit, int64_t *ids, double *scores, int max)
{
   if (!vec || dim <= 0 || !ids || !scores || max <= 0)
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   /* Map which_vec to a fixed column name.  The column identifier comes
    * from this whitelist only — caller text is never interpolated into the
    * SQL, only the two literal column names. */
   const char *col_name;
   if (which_vec && strcmp(which_vec, "subj_attr") == 0)
      col_name = "subj_attr_vec";
   else if (which_vec && strcmp(which_vec, "value") == 0)
      col_name = "value_vec";
   else
      return -1;

   char *vec_text = build_vec_text(vec, dim);
   if (!vec_text)
      return -1;

   int have_kind = claim_kind && claim_kind[0];
   size_t sql_len = 512;
   char *sql = malloc(sql_len);
   if (!sql)
   {
      free(vec_text);
      return -1;
   }
   snprintf(sql, sql_len,
            "SELECT point_id, 1.0 - (%s <=> :qvec::halfvec) AS score "
            "FROM curator_claim_vectors "
            "%s "
            "ORDER BY %s <=> :qvec::halfvec "
            "LIMIT :lim",
            col_name, have_kind ? "WHERE claim_kind = :claim_kind" : "", col_name);

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   free(sql);
   if (!stmt)
   {
      free(vec_text);
      return 0; /* pgvector not available — no vector results */
   }
   aimee_pg_bind_text(stmt, "qvec", vec_text);
   if (have_kind)
      aimee_pg_bind_text(stmt, "claim_kind", claim_kind);
   aimee_pg_bind_int(stmt, "lim", limit > 0 ? limit : max);

   int64_t t0 = monotonic_us();
   int rows = 0;
   aimee_pg_step_t rc;
   while ((rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf))) == AIMEE_PG_ROW)
   {
      if (rows >= max)
         break;
      ids[rows] = aimee_pg_column_int64(stmt, 0);
      scores[rows] = aimee_pg_column_double(stmt, 1);
      rows++;
   }
   aimee_pg_finalize(stmt);
   free(vec_text);

   int64_t elapsed = monotonic_us() - t0;
   record_latency(elapsed);
   return (rc == AIMEE_PG_ERR) ? -1 : rows;
}

int pgvec_curator_code_unit_upsert(int64_t point_id, const float *intent_vec,
                                   const float *signature_vec, const float *body_vec, int dim,
                                   const char *artifact_id, const char *file_path,
                                   const char *def_kind, const char *signature,
                                   const char *body_hash, const char *payload_json)
{
   if (!intent_vec || !signature_vec || !body_vec || dim <= 0)
      return 0;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char *intent_text = build_vec_text(intent_vec, dim);
   char *sig_text = build_vec_text(signature_vec, dim);
   char *body_text = build_vec_text(body_vec, dim);
   if (!intent_text || !sig_text || !body_text)
   {
      free(intent_text);
      free(sig_text);
      free(body_text);
      return -1;
   }

   /* The `signature` column shares its name with the signature_vec column's
    * bind slot, so bind the column under a distinct name (`sig_col`) exactly
    * like the claim upsert binds the `value` column as `value_col`. */
   static const char *sql =
       "INSERT INTO curator_code_unit_vectors "
       "  (point_id, intent_vec, signature_vec, body_vec, artifact_id, file_path, "
       "   def_kind, signature, body_hash, payload_json) "
       "VALUES "
       "  (:point_id, :intent::halfvec, :signature_v::halfvec, :body::halfvec, :artifact_id, "
       "   :file_path, :def_kind, :sig_col, :body_hash, :payload) "
       "ON CONFLICT (point_id) DO UPDATE SET "
       "  intent_vec    = EXCLUDED.intent_vec, "
       "  signature_vec = EXCLUDED.signature_vec, "
       "  body_vec      = EXCLUDED.body_vec, "
       "  artifact_id   = EXCLUDED.artifact_id, "
       "  file_path     = EXCLUDED.file_path, "
       "  def_kind      = EXCLUDED.def_kind, "
       "  signature     = EXCLUDED.signature, "
       "  body_hash     = EXCLUDED.body_hash, "
       "  payload_json  = EXCLUDED.payload_json";

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
   {
      free(intent_text);
      free(sig_text);
      free(body_text);
      return -1;
   }
   aimee_pg_bind_int64(stmt, "point_id", point_id);
   aimee_pg_bind_text(stmt, "intent", intent_text);
   aimee_pg_bind_text(stmt, "signature_v", sig_text);
   aimee_pg_bind_text(stmt, "body", body_text);
   aimee_pg_bind_text(stmt, "artifact_id", artifact_id ? artifact_id : "");
   aimee_pg_bind_text(stmt, "file_path", file_path ? file_path : "");
   aimee_pg_bind_text(stmt, "def_kind", def_kind ? def_kind : "");
   aimee_pg_bind_text(stmt, "sig_col", signature ? signature : "");
   aimee_pg_bind_text(stmt, "body_hash", body_hash ? body_hash : "");
   aimee_pg_bind_text(stmt, "payload", payload_json ? payload_json : "");

   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   free(intent_text);
   free(sig_text);
   free(body_text);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int pgvec_curator_code_unit_delete(int64_t point_id)
{
   void *pg = db2_conn();
   if (!pg)
      return -1;
   static const char *sql = "DELETE FROM curator_code_unit_vectors WHERE point_id = :point_id";
   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return -1;
   aimee_pg_bind_int64(stmt, "point_id", point_id);
   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int pgvec_curator_code_unit_search(const char *which_vec, const char *def_kind, const float *vec,
                                   int dim, int limit, int64_t *ids, double *scores, int max)
{
   if (!vec || dim <= 0 || !ids || !scores || max <= 0)
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   /* Map which_vec to a fixed column name.  The column identifier comes
    * from this whitelist only — caller text is never interpolated into the
    * SQL, only the three literal column names. */
   const char *col_name;
   if (which_vec && strcmp(which_vec, "intent") == 0)
      col_name = "intent_vec";
   else if (which_vec && strcmp(which_vec, "signature") == 0)
      col_name = "signature_vec";
   else if (which_vec && strcmp(which_vec, "body") == 0)
      col_name = "body_vec";
   else
      return -1;

   char *vec_text = build_vec_text(vec, dim);
   if (!vec_text)
      return -1;

   int have_kind = def_kind && def_kind[0];
   size_t sql_len = 512;
   char *sql = malloc(sql_len);
   if (!sql)
   {
      free(vec_text);
      return -1;
   }
   snprintf(sql, sql_len,
            "SELECT point_id, 1.0 - (%s <=> :qvec::halfvec) AS score "
            "FROM curator_code_unit_vectors "
            "%s "
            "ORDER BY %s <=> :qvec::halfvec "
            "LIMIT :lim",
            col_name, have_kind ? "WHERE def_kind = :def_kind" : "", col_name);

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   free(sql);
   if (!stmt)
   {
      free(vec_text);
      return 0; /* pgvector not available — no vector results */
   }
   aimee_pg_bind_text(stmt, "qvec", vec_text);
   if (have_kind)
      aimee_pg_bind_text(stmt, "def_kind", def_kind);
   aimee_pg_bind_int(stmt, "lim", limit > 0 ? limit : max);

   int64_t t0 = monotonic_us();
   int rows = 0;
   aimee_pg_step_t rc;
   while ((rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf))) == AIMEE_PG_ROW)
   {
      if (rows >= max)
         break;
      ids[rows] = aimee_pg_column_int64(stmt, 0);
      scores[rows] = aimee_pg_column_double(stmt, 1);
      rows++;
   }
   aimee_pg_finalize(stmt);
   free(vec_text);

   int64_t elapsed = monotonic_us() - t0;
   record_latency(elapsed);
   return (rc == AIMEE_PG_ERR) ? -1 : rows;
}

/* --- Phase 5: code_embeddings operations --- */

int pgvec_code_upsert(int64_t point_id, const float *vec, int dim, const char *project,
                      const char *node_key, const char *file_path, const char *symbol,
                      const char *content_hash, const char *body_hash, const char *source_hash,
                      const char *payload_json)
{
   if (!vec || dim <= 0)
      return 0;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char *vec_text = build_vec_text(vec, dim);
   if (!vec_text)
      return -1;

   /* source_hash = the SOURCE FILE's hash (files.hash) at embed time, captured so
    * a later scan can detect *content* drift precisely (files.hash <> source_hash)
    * rather than by re-scan timestamp alone (auditable-correctness D7). */
   static const char *sql =
       "INSERT INTO code_embeddings"
       "  (point_id, embedding, project, node_key, file_path, symbol,"
       "   content_hash, body_hash, source_hash, payload_json,"
       "   updated_at)"
       " VALUES"
       "  (:point_id, :embedding::halfvec, :project, :node_key, :file_path, :symbol,"
       "   :content_hash, :body_hash, :source_hash, :payload,"
       "   to_char(CURRENT_TIMESTAMP,'YYYY-MM-DD HH24:MI:SS'))"
       " ON CONFLICT (point_id) DO UPDATE SET"
       "  embedding = EXCLUDED.embedding,"
       "  project = EXCLUDED.project,"
       "  node_key = EXCLUDED.node_key,"
       "  file_path = EXCLUDED.file_path,"
       "  symbol = EXCLUDED.symbol,"
       "  content_hash = EXCLUDED.content_hash,"
       "  body_hash = EXCLUDED.body_hash,"
       "  source_hash = EXCLUDED.source_hash,"
       "  payload_json = EXCLUDED.payload_json,"
       "  updated_at = EXCLUDED.updated_at";

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
   {
      free(vec_text);
      return -1;
   }
   aimee_pg_bind_int64(stmt, "point_id", point_id);
   aimee_pg_bind_text(stmt, "embedding", vec_text);
   aimee_pg_bind_text(stmt, "project", project ? project : "");
   aimee_pg_bind_text(stmt, "node_key", node_key ? node_key : "");
   aimee_pg_bind_text(stmt, "file_path", file_path ? file_path : "");
   aimee_pg_bind_text(stmt, "symbol", symbol ? symbol : "");
   aimee_pg_bind_text(stmt, "content_hash", content_hash ? content_hash : "");
   aimee_pg_bind_text(stmt, "body_hash", body_hash ? body_hash : "");
   aimee_pg_bind_text(stmt, "source_hash", source_hash ? source_hash : "");
   aimee_pg_bind_text(stmt, "payload", payload_json ? payload_json : "");

   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   free(vec_text);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int pgvec_code_delete(int64_t point_id)
{
   void *pg = db2_conn();
   if (!pg)
      return -1;
   static const char *sql = "DELETE FROM code_embeddings WHERE point_id = :pid";
   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return -1;
   aimee_pg_bind_int64(stmt, "pid", point_id);
   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   aimee_pg_finalize(stmt);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int pgvec_code_delete_project(const char *project)
{
   if (!project || !*project)
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;
   static const char *sql = "DELETE FROM code_embeddings WHERE project = :proj";
   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return -1;
   aimee_pg_bind_text(stmt, "proj", project);
   aimee_pg_step_t rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf));
   int changes = aimee_pg_stmt_changes(stmt);
   aimee_pg_finalize(stmt);
   return (rc == AIMEE_PG_DONE) ? changes : -1;
}

int pgvec_code_search(const char *project, const float *vec, int dim, int limit, int64_t *ids,
                      double *scores, int max)
{
   if (!vec || dim <= 0 || !ids || !scores || max <= 0)
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char *vec_text = build_vec_text(vec, dim);
   if (!vec_text)
      return -1;

   const char *sql;
   if (project && *project)
      sql = "SELECT point_id, 1.0 - (embedding <=> :qvec::halfvec) AS score"
            " FROM code_embeddings"
            " WHERE project = :project"
            " ORDER BY embedding <=> :qvec::halfvec LIMIT :lim";
   else
      sql = "SELECT point_id, 1.0 - (embedding <=> :qvec::halfvec) AS score"
            " FROM code_embeddings"
            " ORDER BY embedding <=> :qvec::halfvec LIMIT :lim";

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
   {
      free(vec_text);
      return 0;
   }
   aimee_pg_bind_text(stmt, "qvec", vec_text);
   if (project && *project)
      aimee_pg_bind_text(stmt, "project", project);
   aimee_pg_bind_int(stmt, "lim", limit > 0 ? limit : max);

   int64_t t0 = monotonic_us();
   int n = 0;
   aimee_pg_step_t rc;
   while ((rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf))) == AIMEE_PG_ROW)
   {
      if (n >= max)
         break;
      ids[n] = aimee_pg_column_int64(stmt, 0);
      scores[n] = aimee_pg_column_double(stmt, 1);
      n++;
   }
   aimee_pg_finalize(stmt);
   free(vec_text);
   int64_t elapsed = monotonic_us() - t0;
   record_latency(elapsed);
   return (rc == AIMEE_PG_ERR) ? -1 : n;
}

/* Like pgvec_code_search, but returns each hit's file_path instead of its
 * point_id — the hybrid retrieval leg (§5) fuses signals in file-path space, so
 * it needs paths, not point ids. paths is a flat buffer of `max` slots each
 * `path_cap` bytes (paths + i*path_cap); rows with an empty file_path are
 * skipped. Returns the distinct-file count, or -1 on a bad arg / query error
 * (the caller treats <0 as "no vector signal" and degrades gracefully).
 *
 * code_embeddings holds MANY rows per file (one per code unit/symbol), so a
 * plain LIMIT N over rows would collapse to far fewer than N distinct files. We
 * over-fetch index-ordered rows (the ORDER BY uses the ANN index) and dedup by
 * file_path in C, keeping the first occurrence per file — its best (lowest-
 * distance) row, since rows arrive in ascending distance. */
int pgvec_code_search_paths(const char *project, const float *vec, int dim, int limit, char *paths,
                            int path_cap, double *scores, int max)
{
   if (!vec || dim <= 0 || !paths || path_cap <= 0 || !scores || max <= 0)
      return -1;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   char *vec_text = build_vec_text(vec, dim);
   if (!vec_text)
      return -1;

   const char *sql;
   if (project && *project)
      sql = "SELECT file_path, 1.0 - (embedding <=> :qvec::halfvec) AS score"
            " FROM code_embeddings"
            " WHERE project = :project AND file_path <> ''"
            " ORDER BY embedding <=> :qvec::halfvec LIMIT :lim";
   else
      sql = "SELECT file_path, 1.0 - (embedding <=> :qvec::halfvec) AS score"
            " FROM code_embeddings"
            " WHERE file_path <> ''"
            " ORDER BY embedding <=> :qvec::halfvec LIMIT :lim";

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
   {
      free(vec_text);
      return 0;
   }
   /* Over-fetch rows so the C-side dedup can fill `max` DISTINCT files (16x,
    * capped at 1024 — the ANN ORDER BY still drives the scan). */
   int want = limit > 0 ? limit : max;
   int row_lim = want <= 64 ? want * 16 : 1024;
   if (row_lim > 1024)
      row_lim = 1024;
   if (row_lim < want)
      row_lim = want;
   aimee_pg_bind_text(stmt, "qvec", vec_text);
   if (project && *project)
      aimee_pg_bind_text(stmt, "project", project);
   aimee_pg_bind_int(stmt, "lim", row_lim);

   int64_t t0 = monotonic_us();
   int n = 0;
   aimee_pg_step_t rc;
   while ((rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf))) == AIMEE_PG_ROW)
   {
      if (n >= max)
         break;
      const char *fp = aimee_pg_column_text(stmt, 0);
      if (!fp || !fp[0])
         continue; /* unfusable in file-path space */
      int dup = 0; /* keep only the first (best-distance) row per file */
      for (int k = 0; k < n; k++)
         if (strcmp(paths + (size_t)k * path_cap, fp) == 0)
         {
            dup = 1;
            break;
         }
      if (dup)
         continue;
      snprintf(paths + (size_t)n * path_cap, (size_t)path_cap, "%s", fp);
      scores[n] = aimee_pg_column_double(stmt, 1);
      n++;
   }
   aimee_pg_finalize(stmt);
   free(vec_text);
   record_latency(monotonic_us() - t0);
   return (rc == AIMEE_PG_ERR) ? -1 : n;
}

int pgvec_code_similar_pairs(const char *project, int k, double min_cosine, int anchor_cap,
                             char *a_keys, char *b_keys, int key_cap, double *cosines, int max)
{
   if (!project || !*project || !a_keys || !b_keys || key_cap <= 0 || !cosines || max <= 0)
      return -1;
   if (k <= 0)
      k = 5;
   if (anchor_cap <= 0)
      anchor_cap = 5000;
   void *pg = db2_conn();
   if (!pg)
      return -1;

   /* For each file-node embedding, its top-k nearest OTHER embeddings (the lateral
    * rides the HNSW index per anchor row), cosine-floored, ordered closest first.
    * Pairs are deduped to a canonical (a<b) form in C rather than in SQL: kNN is
    * asymmetric (b can be in a's neighborhood without a being in b's), so filtering
    * `ak < bk` in SQL would silently drop one-directional pairs.
    * The anchor relation is bounded by :acap so worst-case work is O(acap*k) HNSW
    * probes regardless of project size — the final LIMIT can't push into the outer
    * scan (the ORDER BY cosine is global), so without the bound an N-file project
    * would fan out into N probes. */
   const char *sql = "SELECT ak, bk, cosine FROM ("
                     "  SELECT a.node_key AS ak, b.node_key AS bk,"
                     "         1.0 - (a.embedding <=> b.embedding) AS cosine"
                     "  FROM (SELECT node_key, embedding, point_id, project FROM code_embeddings"
                     "        WHERE project = :project AND node_key <> '' LIMIT :acap) a"
                     "  CROSS JOIN LATERAL ("
                     "     SELECT x.node_key, x.embedding FROM code_embeddings x"
                     "     WHERE x.project = a.project AND x.point_id <> a.point_id"
                     "       AND x.node_key <> ''"
                     "     ORDER BY x.embedding <=> a.embedding LIMIT :k"
                     "  ) b"
                     ") p WHERE cosine >= :minc ORDER BY cosine DESC LIMIT :lim";

   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return 0;
   /* Over-fetch: an unordered pair can surface from both endpoints, so the raw row
    * set is up to ~2x the distinct pairs; 8x (capped) lets the C dedup fill `max`. */
   int row_lim = max <= 1024 ? max * 8 : max * 2;
   if (row_lim > 8192)
      row_lim = 8192;
   if (row_lim < max)
      row_lim = max;
   aimee_pg_bind_int(stmt, "k", k);
   aimee_pg_bind_text(stmt, "project", project);
   aimee_pg_bind_double(stmt, "minc", min_cosine);
   aimee_pg_bind_int(stmt, "acap", anchor_cap);
   aimee_pg_bind_int(stmt, "lim", row_lim);

   int64_t t0 = monotonic_us();
   int n = 0;
   aimee_pg_step_t rc;
   while ((rc = aimee_pg_step(stmt, errbuf, sizeof(errbuf))) == AIMEE_PG_ROW)
   {
      if (n >= max)
         break;
      const char *ak = aimee_pg_column_text(stmt, 0);
      const char *bk = aimee_pg_column_text(stmt, 1);
      if (!ak || !ak[0] || !bk || !bk[0] || strcmp(ak, bk) == 0)
         continue; /* skip blanks + self-pairs */
      /* canonical order so {a,b} and {b,a} collapse to one slot */
      const char *lo = strcmp(ak, bk) < 0 ? ak : bk;
      const char *hi = strcmp(ak, bk) < 0 ? bk : ak;
      int dup = 0;
      for (int j = 0; j < n; j++)
         if (strcmp(a_keys + (size_t)j * key_cap, lo) == 0 &&
             strcmp(b_keys + (size_t)j * key_cap, hi) == 0)
         {
            dup = 1;
            break;
         }
      if (dup)
         continue; /* rows arrive cosine-desc, so the first hit already has the best cosine */
      snprintf(a_keys + (size_t)n * key_cap, (size_t)key_cap, "%s", lo);
      snprintf(b_keys + (size_t)n * key_cap, (size_t)key_cap, "%s", hi);
      cosines[n] = aimee_pg_column_double(stmt, 2);
      n++;
   }
   aimee_pg_finalize(stmt);
   record_latency(monotonic_us() - t0);
   return (rc == AIMEE_PG_ERR) ? -1 : n;
}

int pgvec_code_node_path(const char *project, const char *node_key, char *out, int out_cap)
{
   if (!project || !*project || !node_key || !*node_key || !out || out_cap <= 0)
      return -1;
   out[0] = '\0';
   void *pg = db2_conn();
   if (!pg)
      return -1;
   const char *sql = "SELECT file_path FROM code_embeddings"
                     " WHERE project = :project AND node_key = :node_key AND file_path <> ''"
                     " LIMIT 1";
   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return -1;
   aimee_pg_bind_text(stmt, "project", project);
   aimee_pg_bind_text(stmt, "node_key", node_key);
   int found = 0;
   if (aimee_pg_step(stmt, errbuf, sizeof(errbuf)) == AIMEE_PG_ROW)
   {
      const char *fp = aimee_pg_column_text(stmt, 0);
      if (fp && fp[0])
      {
         snprintf(out, (size_t)out_cap, "%s", fp);
         found = 1;
      }
   }
   aimee_pg_finalize(stmt);
   return found ? 0 : -1;
}

int pgvec_code_exists_by_hash(const char *project, const char *node_key, const char *content_hash,
                              const char *body_hash)
{
   if (!project || !node_key || !content_hash || !*content_hash)
      return 0;
   void *pg = db2_conn();
   if (!pg)
      return 0;
   static const char *sql = "SELECT 1 FROM code_embeddings"
                            " WHERE project = :proj AND node_key = :nk AND content_hash = :ch"
                            "   AND (:bh = '' OR body_hash = :bh) LIMIT 1";
   char errbuf[256];
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(pg, sql, errbuf, sizeof(errbuf));
   if (!stmt)
      return 0;
   aimee_pg_bind_text(stmt, "proj", project);
   aimee_pg_bind_text(stmt, "nk", node_key);
   aimee_pg_bind_text(stmt, "ch", content_hash);
   aimee_pg_bind_text(stmt, "bh", body_hash ? body_hash : "");
   int found = (aimee_pg_step(stmt, errbuf, sizeof(errbuf)) == AIMEE_PG_ROW) ? 1 : 0;
   aimee_pg_finalize(stmt);
   return found;
}
