/* db2/sketch.c: DB2 persistence for approximate sketch state (Bloom, Count-Min, HLL). */
#include "sketch.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

#define SK_ERR 256

/* Generic upsert helper: writes raw bytes under (sketch_kind, scope_kind, scope_id,
 * feature_family). Returns 0 on success, -1 on error. */
static int sketch_save_bytes(const char *sketch_kind, const char *scope_kind, const char *scope_id,
                             const char *feature_family, const void *bytes, int byte_len,
                             uint64_t item_count, const char *params_json)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "INSERT INTO sketch_store"
                            "  (sketch_kind, scope_kind, scope_id, feature_family,"
                            "   state_bytes, item_count, params_json, created_at, updated_at)"
                            " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, pg_now_text(), pg_now_text())"
                            " ON CONFLICT (sketch_kind, scope_kind, scope_id, feature_family)"
                            " DO UPDATE SET state_bytes = EXCLUDED.state_bytes,"
                            "               item_count  = EXCLUDED.item_count,"
                            "               params_json = EXCLUDED.params_json,"
                            "               updated_at  = pg_now_text()";

   char err[SK_ERR];
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", sketch_kind);
   aimee_pg_bind_text(st, "?2", scope_kind);
   aimee_pg_bind_text(st, "?3", scope_id ? scope_id : "");
   aimee_pg_bind_text(st, "?4", feature_family);
   aimee_pg_bind_blob(st, "?5", bytes, byte_len);
   aimee_pg_bind_int64(st, "?6", (int64_t)item_count);
   aimee_pg_bind_text(st, "?7", params_json ? params_json : "{}");

   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_ERR) ? -1 : 0;
}

/* Generic load helper: reads raw bytes from sketch_store.
 * Returns 0 on hit, 1 on no-row, -1 on error.
 * On hit, copies min(byte_len, stored_len) bytes into out_bytes and
 * sets *out_count to the stored item_count. */
static int sketch_load_bytes(const char *sketch_kind, const char *scope_kind, const char *scope_id,
                             const char *feature_family, void *out_bytes, int byte_len,
                             uint64_t *out_count)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT state_bytes, item_count FROM sketch_store"
                            " WHERE sketch_kind = ?1 AND scope_kind = ?2"
                            "   AND scope_id = ?3 AND feature_family = ?4"
                            " LIMIT 1";

   char err[SK_ERR];
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", sketch_kind);
   aimee_pg_bind_text(st, "?2", scope_kind);
   aimee_pg_bind_text(st, "?3", scope_id ? scope_id : "");
   aimee_pg_bind_text(st, "?4", feature_family);

   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return (step == AIMEE_PG_ERR) ? -1 : 1;
   }

   const void *blob = aimee_pg_column_blob(st, 0);
   int blob_len = aimee_pg_column_bytes(st, 0);
   if (blob && blob_len > 0)
   {
      int copy_len = blob_len < byte_len ? blob_len : byte_len;
      memcpy(out_bytes, blob, (size_t)copy_len);
   }
   if (out_count)
      *out_count = (uint64_t)aimee_pg_column_int64(st, 1);

   aimee_pg_finalize(st);
   return 0;
}

/* ── Bloom ──────────────────────────────────────────────────────────────── */

int db2_sketch_bloom_load(sketch_bloom_t *out, const char *scope_kind, const char *scope_id,
                          const char *feature_family)
{
   sketch_bloom_init(out);
   return sketch_load_bytes("bloom", scope_kind, scope_id, feature_family, out->bits,
                            SKETCH_BLOOM_BYTES, &out->item_count);
}

int db2_sketch_bloom_save(const sketch_bloom_t *b, const char *scope_kind, const char *scope_id,
                          const char *feature_family)
{
   char params[128];
   snprintf(params, sizeof(params), "{\"m\":%u,\"k\":%d,\"rotation\":{\"bloom_rotate\":\"30d\"}}",
            SKETCH_BLOOM_M_BITS, SKETCH_BLOOM_K);
   return sketch_save_bytes("bloom", scope_kind, scope_id, feature_family, b->bits,
                            SKETCH_BLOOM_BYTES, b->item_count, params);
}

/* ── MinHash ────────────────────────────────────────────────────────────── */

int db2_sketch_minhash_save(const sketch_minhash_t *sig, const char *scope_kind,
                            const char *scope_id, const char *feature_family)
{
   char params[128];
   snprintf(params, sizeof(params),
            "{\"permutations\":%d,\"lsh_bands\":%d,\"lsh_rows_per_band\":%d,"
            "\"rotation\":{\"lsh_refresh\":\"7d\"}}",
            SKETCH_MINHASH_PERMUTATIONS, SKETCH_LSH_BANDS, SKETCH_LSH_ROWS_PER_BAND);
   return sketch_save_bytes("minhash", scope_kind, scope_id, feature_family, sig->values,
                            (int)sizeof(sig->values), 1, params);
}

static void lsh_band_hash_text(const sketch_minhash_t *sig, int band, char *out, size_t cap)
{
   if (!out || cap == 0)
      return;
   uint64_t h = sketch_lsh_band_hash(sig, band);
   snprintf(out, cap, "%016llx", (unsigned long long)h);
}

static int lsh_bucket_delete_file(void *conn, const char *project, const char *file_path)
{
   static const char *sql = "DELETE FROM kb_lsh_buckets WHERE project=?1 AND file_path=?2"
                            " AND generation=(SELECT current_generation FROM projects"
                            " WHERE name=?1 AND lifecycle_state='current')";
   char err[SK_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", file_path);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_ERR) ? -1 : 0;
}

static int lsh_bucket_refresh(void *conn, const char *project, const char *file_path,
                              const sketch_minhash_t *sig)
{
   if (lsh_bucket_delete_file(conn, project, file_path) != 0)
      return -1;
   static const char *sql =
       "INSERT INTO kb_lsh_buckets (project,generation,band,band_hash,file_path,updated_at)"
       " VALUES (?1,(SELECT current_generation FROM projects"
       " WHERE name=?1 AND lifecycle_state='current'),?2,?3,?4,pg_now_text())"
       " ON CONFLICT (project, generation, band, band_hash, file_path) DO UPDATE"
       " SET updated_at=pg_now_text()";
   for (int band = 0; band < SKETCH_LSH_BANDS; band++)
   {
      char hash_text[32];
      lsh_band_hash_text(sig, band, hash_text, sizeof(hash_text));
      char err[SK_ERR] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", project);
      aimee_pg_bind_int(st, "?2", band);
      aimee_pg_bind_text(st, "?3", hash_text);
      aimee_pg_bind_text(st, "?4", file_path);
      aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
      aimee_pg_finalize(st);
      if (rc == AIMEE_PG_ERR)
         return -1;
   }
   return 0;
}

int db2_sketch_minhash_signature_upsert(const char *project, const char *file_path,
                                        const char *file_hash, const sketch_minhash_t *sig)
{
   if (!project || !*project || !file_path || !*file_path || !sig)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "INSERT INTO kb_minhash_signatures"
                            " (project,generation,file_path,file_hash,signature_bytes,updated_at)"
                            " VALUES (?1,(SELECT current_generation FROM projects"
                            " WHERE name=?1 AND lifecycle_state='current'),?2,?3,?4,pg_now_text())"
                            " ON CONFLICT (project, generation, file_path) DO UPDATE"
                            " SET file_hash=EXCLUDED.file_hash,"
                            "     signature_bytes=EXCLUDED.signature_bytes,"
                            "     updated_at = pg_now_text()";
   char err[SK_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", file_path);
   aimee_pg_bind_text(st, "?3", file_hash ? file_hash : "");
   aimee_pg_bind_blob(st, "?4", sig->values, (int)sizeof(sig->values));
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (rc == AIMEE_PG_ERR)
      return -1;
   return lsh_bucket_refresh(conn, project, file_path, sig);
}

static void minhash_load_row(aimee_pg_stmt_t *st, db2_sketch_minhash_row_t *out)
{
   memset(out, 0, sizeof(*out));
   const char *fp = aimee_pg_column_text(st, 0), *fh = aimee_pg_column_text(st, 1);
   const void *blob = aimee_pg_column_blob(st, 2);
   int blob_len = aimee_pg_column_bytes(st, 2);
   snprintf(out->file_path, sizeof(out->file_path), "%s", fp ? fp : "");
   snprintf(out->file_hash, sizeof(out->file_hash), "%s", fh ? fh : "");
   sketch_minhash_init(&out->signature);
   if (blob && blob_len > 0)
   {
      int copy_len = blob_len < (int)sizeof(out->signature.values)
                         ? blob_len
                         : (int)sizeof(out->signature.values);
      memcpy(out->signature.values, blob, (size_t)copy_len);
   }
}

int db2_sketch_minhash_signature_get(const char *project, const char *file_path,
                                     db2_sketch_minhash_row_t *out)
{
   if (!project || !*project || !file_path || !*file_path || !out)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql =
       "SELECT s.file_path,s.file_hash,s.signature_bytes FROM kb_minhash_signatures s"
       " JOIN projects p ON p.name=s.project WHERE s.project=?1 AND s.file_path=?2"
       " AND p.lifecycle_state='current' AND s.generation=p.current_generation";
   char err[SK_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", file_path);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   if (rc == AIMEE_PG_ROW)
      minhash_load_row(st, out);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_ROW) ? 1 : 0;
}

int db2_sketch_minhash_signature_delete(const char *project, const char *file_path)
{
   if (!project || !*project || !file_path || !*file_path)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "DELETE FROM kb_minhash_signatures WHERE project=?1 AND file_path=?2"
                            " AND generation=(SELECT current_generation FROM projects"
                            " WHERE name=?1 AND lifecycle_state='current')";
   char err[SK_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", file_path);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (rc == AIMEE_PG_ERR)
      return -1;
   return lsh_bucket_delete_file(conn, project, file_path);
}

int db2_sketch_minhash_signature_delete_project(const char *project)
{
   if (!project || !*project)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "DELETE FROM kb_minhash_signatures WHERE project=?1"
                            " AND generation=(SELECT current_generation FROM projects"
                            " WHERE name=?1 AND lifecycle_state='current')";
   char err[SK_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (rc == AIMEE_PG_ERR)
      return -1;
   st = aimee_pg_prepare(conn,
                         "DELETE FROM kb_lsh_buckets WHERE project=?1"
                         " AND generation=(SELECT current_generation FROM projects"
                         " WHERE name=?1 AND lifecycle_state='current')",
                         err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_ERR) ? -1 : 0;
}

int db2_sketch_minhash_signature_list(const char *project, db2_sketch_minhash_row_t *out,
                                      int max_rows)
{
   if (!project || !*project || !out || max_rows <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT s.file_path,s.file_hash,s.signature_bytes FROM kb_minhash_signatures s"
       " JOIN projects p ON p.name=s.project WHERE s.project=?1"
       " AND p.lifecycle_state='current' AND s.generation=p.current_generation"
       " ORDER BY s.updated_at DESC LIMIT ?2";
   char err[SK_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_int(st, "?2", max_rows);
   int n = 0;
   while (n < max_rows && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      minhash_load_row(st, &out[n]);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

static int minhash_row_seen(const db2_sketch_minhash_row_t *rows, int n, const char *file_path)
{
   for (int i = 0; i < n; i++)
      if (strcmp(rows[i].file_path, file_path ? file_path : "") == 0)
         return 1;
   return 0;
}

int db2_sketch_minhash_candidate_list(const char *project, const sketch_minhash_t *sig,
                                      db2_sketch_minhash_row_t *out, int max_rows)
{
   if (!project || !*project || !sig || !out || max_rows <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT s.file_path, s.file_hash, s.signature_bytes"
       " FROM kb_lsh_buckets b"
       " JOIN kb_minhash_signatures s ON s.project=b.project AND s.generation=b.generation"
       " AND s.file_path=b.file_path JOIN projects p ON p.name=b.project"
       " WHERE b.project=?1 AND b.band=?2 AND b.band_hash=?3"
       " AND p.lifecycle_state='current' AND b.generation=p.current_generation"
       " ORDER BY s.updated_at DESC LIMIT ?4";
   int n = 0;
   for (int band = 0; band < SKETCH_LSH_BANDS && n < max_rows; band++)
   {
      char hash_text[32];
      lsh_band_hash_text(sig, band, hash_text, sizeof(hash_text));
      char err[SK_ERR] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (!st)
         continue;
      aimee_pg_bind_text(st, "?1", project);
      aimee_pg_bind_int(st, "?2", band);
      aimee_pg_bind_text(st, "?3", hash_text);
      aimee_pg_bind_int(st, "?4", max_rows);
      while (n < max_rows && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *fp = aimee_pg_column_text(st, 0);
         if (minhash_row_seen(out, n, fp))
            continue;
         minhash_load_row(st, &out[n]);
         n++;
      }
      aimee_pg_finalize(st);
   }
   return n;
}

/* ── Count-Min ──────────────────────────────────────────────────────────── */

int db2_sketch_count_min_load(sketch_count_min_t *out, const char *scope_kind, const char *scope_id,
                              const char *feature_family)
{
   sketch_count_min_init(out);
   return sketch_load_bytes("count_min", scope_kind, scope_id, feature_family, out->counters,
                            (int)sizeof(out->counters), &out->item_count);
}

int db2_sketch_count_min_save(const sketch_count_min_t *cm, const char *scope_kind,
                              const char *scope_id, const char *feature_family)
{
   char params[128];
   snprintf(params, sizeof(params),
            "{\"width\":%d,\"depth\":%d,\"rotation\":{\"count_min_reset\":\"30d\"}}",
            SKETCH_COUNT_MIN_WIDTH, SKETCH_COUNT_MIN_DEPTH);
   return sketch_save_bytes("count_min", scope_kind, scope_id, feature_family, cm->counters,
                            (int)sizeof(cm->counters), cm->item_count, params);
}

/* ── HyperLogLog ────────────────────────────────────────────────────────── */

int db2_sketch_hll_load(sketch_hll_t *out, const char *scope_kind, const char *scope_id,
                        const char *feature_family)
{
   sketch_hll_init(out);
   return sketch_load_bytes("hll", scope_kind, scope_id, feature_family, out->registers,
                            (int)sizeof(out->registers), &out->item_count);
}

int db2_sketch_hll_save(const sketch_hll_t *hll, const char *scope_kind, const char *scope_id,
                        const char *feature_family)
{
   char params[128];
   snprintf(params, sizeof(params), "{\"precision\":%d,\"rotation\":{\"hll_reset\":\"never\"}}",
            SKETCH_HLL_PRECISION);
   return sketch_save_bytes("hll", scope_kind, scope_id, feature_family, hll->registers,
                            (int)sizeof(hll->registers), hll->item_count, params);
}
