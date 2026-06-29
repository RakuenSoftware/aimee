/* db2_reembed.c — embedder-runtime-fetch-autodim §2c: the double-gated
 * dim-change reset. When the configured/derived embedding dim differs from the
 * recorded schema_embedding_dim on a populated DB, db2_init refuses (never serve a
 * new-dim embedder against an old-dim corpus). This is the attended, explicit
 * recovery: drop the DERIVED vector tables, recreate them at the new dim (via the
 * idempotent schema-apply), record the new dim, and re-trigger each subsystem's
 * re-embed from its AUTHORITATIVE source. Destructive; the caller gates it
 * (config flag + --confirm). */
#include "db2.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "db_schema.h"
#include "lifecycle.h"
#include "artifacts.h"        /* db2_curator_reembed_all */
#include "evidence_vectors.h" /* db2_evidence_reembed_all */
#include "kb_payload.h"       /* db2_kb_pdf_reembed_all */
#include "../headers/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The DERIVED halfvec vector tables: each is rebuilt from an authoritative source
 * elsewhere (kb_documents, artifacts, evidence_index_ops, memories), so dropping +
 * recreating + re-deriving loses no source data. Any halfvec table NOT on this list
 * is unknown -> the reset REFUSES rather than risk destroying source it doesn't
 * understand. Keep in sync with schema.sql's halfvec(__EMBED_DIM__) tables. */
static const char *DERIVED_VECTOR_TABLES[] = {"kb_embeddings",          "kb_pdf_embeddings",
                                              "memory_embeddings",      "curator_entity_vectors",
                                              "curator_narrative_vectors", "curator_claim_vectors",
                                              "curator_code_unit_vectors", "exemplar_vectors",
                                              "evidence_vectors",       "code_embeddings",
                                              NULL};

static int is_known_vector_table(const char *t)
{
   for (int i = 0; DERIVED_VECTOR_TABLES[i]; i++)
      if (strcmp(DERIVED_VECTOR_TABLES[i], t) == 0)
         return 1;
   return 0;
}

/* Append to the human report buffer (best-effort, never overflows). */
static void rpt(db2_reembed_plan_t *p, const char *fmt, ...)
{
   if (!p)
      return;
   size_t len = strlen(p->detail);
   if (len >= sizeof(p->detail) - 1)
      return;
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(p->detail + len, sizeof(p->detail) - len, fmt, ap);
   va_end(ap);
}

/* Does any halfvec table in the live schema fall outside the known set? Fills the
 * report with the discovered tables. Returns 0 if all known, -1 if an unknown one
 * exists (refuse), -2 on a query error. */
static int discover_and_check(void *conn, db2_reembed_plan_t *p)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT DISTINCT table_name FROM information_schema.columns"
       " WHERE table_schema = 'public' AND udt_name = 'halfvec' ORDER BY table_name",
       err, sizeof(err));
   if (!st)
      return -2;
   int unknown = 0;
   p->n_tables = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *t = aimee_pg_column_text(st, 0);
      if (!t)
         continue;
      if (p->n_tables < (int)(sizeof(p->tables) / sizeof(p->tables[0])))
         snprintf(p->tables[p->n_tables++], sizeof(p->tables[0]), "%s", t);
      if (!is_known_vector_table(t))
      {
         unknown = 1;
         rpt(p, "  UNKNOWN halfvec table (refusing): %s\n", t);
      }
   }
   aimee_pg_finalize(st);
   return unknown ? -1 : 0;
}

/* Count rows in a table (best-effort; -1 on error). */
static long long table_rows(void *conn, const char *table)
{
   char sql[256];
   snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s", table);
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   long long n = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = (long long)aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}

/* Are there FK constraints referencing `table` (which DROP TABLE would need
 * CASCADE for)? Returns 1 if any inbound FK, 0 if none, -1 on error. */
static int has_inbound_fk(void *conn, const char *table)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT count(*) FROM information_schema.constraint_column_usage ccu"
       " JOIN information_schema.table_constraints tc ON tc.constraint_name = ccu.constraint_name"
       " WHERE tc.constraint_type = 'FOREIGN KEY' AND ccu.table_name = ?1",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", table);
   int n = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return n < 0 ? -1 : (n > 0 ? 1 : 0);
}

static int exec1(void *conn, const char *sql, char *err, size_t errlen)
{
   return aimee_pg_exec(conn, sql, err, errlen);
}

/* Read the reembed_in_progress maintenance marker ("<target_dim>:<started_epoch>").
 * Returns 1 if set (filling out_dim and out_started when non-NULL), 0 if not set,
 * -1 on error. The kb's health + search paths consult this to report `maintenance`
 * and 503 vector search while a dim-change re-embed is in flight. */
int db2_reembed_in_progress_get(int *target_dim, long *started_epoch)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT value FROM kb_meta WHERE key = 'reembed_in_progress'", err, sizeof(err));
   if (!st)
      return 0; /* table/row absent -> not in progress */
   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *v = aimee_pg_column_text(st, 0);
      if (v && v[0])
      {
         found = 1;
         int td = 0;
         long se = 0;
         if (sscanf(v, "%d:%ld", &td, &se) >= 1)
         {
            if (target_dim)
               *target_dim = td;
            if (started_epoch)
               *started_epoch = se;
         }
      }
   }
   aimee_pg_finalize(st);
   return found;
}

/* Clear the maintenance marker (reconciliation done, or operator override). */
int db2_reembed_in_progress_clear(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   return aimee_pg_exec(conn, "DELETE FROM kb_meta WHERE key = 'reembed_in_progress'", err,
                        sizeof(err));
}

/* §2c operator escape hatch: clear a stuck maintenance marker, but first check the
 * store is in a recoverable state. If the recorded schema dim disagrees with the
 * running in-memory dim, clearing would resume search against a store still mid-
 * transition — so REFUSE unless force, making that dangerous case explicit (the
 * common stuck case — re-embed finished but the marker outlived a crash — has
 * matching dims and clears without force). Returns 0 cleared, -1 refused (mismatch,
 * no force), -2 error. Out params was_in_progress, recorded, running filled when
 * non-NULL. */
int db2_reembed_clear_maintenance(int force, int *was_in_progress, int *recorded, int *running)
{
   void *conn = db2_conn();
   if (!conn)
      return -2;
   if (was_in_progress)
      *was_in_progress = db2_reembed_in_progress_get(NULL, NULL) == 1;
   int rec = 0;
   (void)db2_embedding_dim_read(conn, &rec); /* rec stays 0 when none recorded */
   int run = db2_embedding_dim();
   if (recorded)
      *recorded = rec;
   if (running)
      *running = run;
   if (rec > 0 && rec != run && !force)
      return -1; /* inconsistent: require force to clear into a mid-transition store */
   return db2_reembed_in_progress_clear() == 0 ? 0 : -2;
}

int db2_dim_change_reset(int target_dim, int force, int dry_run, db2_reembed_plan_t *out)
{
   db2_reembed_plan_t local;
   db2_reembed_plan_t *p = out ? out : &local;
   memset(p, 0, sizeof(*p));
   void *conn = db2_conn();
   if (!conn)
      return -1;
   if (target_dim <= 0 || target_dim > EMBED_MAX_DIM)
   {
      rpt(p, "invalid target dim %d (must be 1..%d)\n", target_dim, EMBED_MAX_DIM);
      return -1;
   }
   p->target_dim = target_dim;
   int recorded = 0;
   if (db2_embedding_dim_read(conn, &recorded) == DB2_DIM_FOUND)
      p->recorded_dim = recorded;
   if (p->recorded_dim == target_dim)
   {
      rpt(p, "no dim change needed (recorded == target == %d)\n", target_dim);
      return 0; /* no-op: caller reports "nothing to do" */
   }

   int chk = discover_and_check(conn, p);
   if (chk == -2)
   {
      rpt(p, "could not enumerate halfvec tables\n");
      return -1;
   }
   if (chk == -1)
      return -2; /* unknown halfvec table -> refuse (distinct code so the CLI explains) */

   rpt(p, "dim change: recorded=%d -> target=%d; %d derived vector table(s)\n",
       p->recorded_dim ? p->recorded_dim : -1, target_dim, p->n_tables);
   for (int i = 0; i < p->n_tables; i++)
   {
      long long rows = table_rows(conn, p->tables[i]);
      p->rows_cleared += (rows > 0 ? rows : 0);
      rpt(p, "  DROP %s (%lld rows) -> recreate halfvec(%d)\n", p->tables[i], rows, target_dim);
   }
   rpt(p,
       "  then: re-record schema_embedding_dim=%d, set reembed_in_progress, re-trigger "
       "doc-embed + curator + evidence re-embed\n",
       target_dim);

   if (dry_run)
   {
      rpt(p, "[dry-run] no changes made\n");
      return 0;
   }

   /* ---- EXECUTE (destructive) ---- */
   char err[512] = "";
   /* FK guard before any drop. */
   for (int i = 0; i < p->n_tables; i++)
   {
      int fk = has_inbound_fk(conn, p->tables[i]);
      if (fk == 1 && !force)
      {
         rpt(p, "  %s has inbound FK references; re-run with --force to DROP ... CASCADE\n",
             p->tables[i]);
         return -3;
      }
   }
   /* Atomicity: drop + recreate-at-new-dim + re-record + marker run in ONE
    * transaction (PG DDL is transactional). A failure at any step ROLLBACKs all of
    * it, so the schema is never left half-dropped — either the whole reset lands or
    * nothing changes. db2_set_embedding_dim (in-memory) is deferred until after
    * COMMIT so a rollback cannot leave the process pointing at a dim the DB lacks.
    * (db_apply_schema_postgres takes the dim as an arg, so it does not need the
    * global pre-set.)
    *
    * Serialization: hold the db2_init mutex across the whole execute + the deferred
    * in-memory swap, so this cannot interleave with db2_init or a concurrent reset
    * (the other writers of schema + recorded/in-memory dim). The residual COMMIT->
    * set window seen by lock-free db2_embedding_dim() readers (pgvec insert/query) is
    * sub-millisecond, during maintenance (search 503s), and before the re-embed
    * inserters are re-triggered below — so no writer observes the new schema with the
    * old in-memory dim in practice. Every exit from here must db2_init_unlock(). */
   db2_init_lock();
   if (exec1(conn, "BEGIN", err, sizeof(err)) != 0)
   {
      rpt(p, "  BEGIN failed: %s\n", err);
      db2_init_unlock();
      return -1;
   }
#define RESET_ABORT(...)                                                                           \
   do                                                                                              \
   {                                                                                               \
      rpt(p, __VA_ARGS__);                                                                         \
      char rberr[256] = "";                                                                        \
      (void)exec1(conn, "ROLLBACK", rberr, sizeof(rberr));                                         \
      p->n_dropped = 0;                                                                            \
      db2_init_unlock();                                                                           \
      return -1;                                                                                   \
   } while (0)
   for (int i = 0; i < p->n_tables; i++)
   {
      char sql[256];
      snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS %s%s", p->tables[i],
               force ? " CASCADE" : "");
      if (exec1(conn, sql, err, sizeof(err)) != 0)
         RESET_ABORT("  DROP %s failed: %s\n", p->tables[i], err);
      p->n_dropped++;
   }
   /* Clear the recorded dim so the schema-apply records the new one fresh (no
    * mismatch refusal), then re-apply the schema -> recreates every dropped table
    * at the new dim WITH its indexes (CREATE TABLE/INDEX IF NOT EXISTS), and records
    * schema_embedding_dim = target_dim (record-after-DDL). */
   if (exec1(conn, "DELETE FROM kb_meta WHERE key = 'schema_embedding_dim'", err, sizeof(err)) != 0)
      RESET_ABORT("  clearing recorded dim failed: %s\n", err);
   if (db_apply_schema_postgres(conn, target_dim, err, sizeof(err)) != 0)
      RESET_ABORT("  schema re-apply at dim %d failed: %s\n", target_dim, err);
   /* Mark maintenance: "<target_dim>:<started_at_epoch>". The kb's health surfaces
    * this as `maintenance` and the search path 503s until reconciliation clears it. */
   {
      char val[64];
      snprintf(val, sizeof(val), "%d:%ld", target_dim, (long)time(NULL));
      char sql[256];
      snprintf(sql, sizeof(sql),
               "INSERT INTO kb_meta (key, value) VALUES ('reembed_in_progress', '%s')"
               " ON CONFLICT (key) DO UPDATE SET value = '%s'",
               val, val);
      if (exec1(conn, sql, err, sizeof(err)) != 0)
         RESET_ABORT("  setting maintenance marker failed: %s\n", err);
   }
   if (exec1(conn, "COMMIT", err, sizeof(err)) != 0)
      RESET_ABORT("  COMMIT failed: %s\n", err);
#undef RESET_ABORT
   db2_set_embedding_dim(target_dim);
   db2_init_unlock();
   /* Re-trigger the derive-from-source re-embeds. kb_embeddings is auto-backfilled
    * by the doc-embed drain (kb_documents survive). curator vectors rebuild from the
    * authoritative `artifacts` (state->proposed) and evidence from
    * evidence_index_ops (->pending). memory_embeddings re-embeds via memory's own
    * path (`aimee memory reembed`); surfaced in the command's guidance. */
   p->curator_requeued = db2_curator_reembed_all();
   p->evidence_requeued = db2_evidence_reembed_all();
   (void)db2_kb_pdf_reembed_all(); /* PDF vectors re-derive from embed_pdf jobs (no auto-backfill) */
   rpt(p,
       "reset done: dropped %d table(s), recorded dim=%d, requeued curator=%d evidence=%d; "
       "doc-embed backfill re-embeds kb chunks; run `aimee memory reembed --start` for memory\n",
       p->n_dropped, target_dim, p->curator_requeued, p->evidence_requeued);
   LOG_WARN("db2", "§2c dim-change reset executed: %d -> %d (%d tables, %lld rows cleared)",
            p->recorded_dim, target_dim, p->n_dropped, p->rows_cleared);
   return 0;
}
