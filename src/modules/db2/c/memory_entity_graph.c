/* db2/memory_entity_graph.c: write primitives for the per-memory entity /
 * temporal / coref / negation graph. Postgres via libpq.
 *
 * The negation search projection is maintained automatically by the
 * memory_negation_fts_tsv GENERATED ALWAYS column on memories; updating
 * memories.negation_tokens triggers the recompute. */

#include "../headers/aimee.h" /* memory_t / memory_lineage_t for header consumers */
#include "memory_query.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define MEG_ERRBUF 256

void db2_memory_coref_audit_insert(int64_t memory_id, const char *session_id, const char *outcome,
                                   const char *entity, const char *mode, double confidence)
{
   if (memory_id <= 0)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql =
       "INSERT INTO memory_coref_audit (memory_id, session_id, outcome, entity, mode, confidence)"
       " VALUES (?1, ?2, ?3, ?4, ?5, ?6)";
   char err[MEG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", session_id ? session_id : "");
   aimee_pg_bind_text(st, "?3", outcome ? outcome : "none");
   aimee_pg_bind_text(st, "?4", entity ? entity : "");
   aimee_pg_bind_text(st, "?5", mode ? mode : "");
   aimee_pg_bind_double(st, "?6", confidence);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void db2_memory_negation_tokens_update(int64_t memory_id, const char *new_tokens)
{
   if (memory_id <= 0)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   const char *tokens = new_tokens ? new_tokens : "";

   static const char *sql = "UPDATE memories SET negation_tokens = ?1 WHERE id = ?2";
   char err[MEG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", tokens);
   aimee_pg_bind_int64(st, "?2", memory_id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void db2_memory_entity_insert(int64_t memory_id, const char *entity, const char *role,
                              double weight)
{
   if (memory_id <= 0 || !entity || !*entity)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql = "INSERT INTO memory_entities (memory_id, entity, role, weight)"
                            " VALUES (?1, ?2, ?3, ?4) ON CONFLICT DO NOTHING";
   char err[MEG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", entity);
   aimee_pg_bind_text(st, "?3", role ? role : "mention");
   aimee_pg_bind_double(st, "?4", weight);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

void db2_memory_temporal_insert(int64_t memory_id, const char *ref_key, const char *granularity,
                                double weight)
{
   if (memory_id <= 0 || !ref_key || !*ref_key)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql =
       "INSERT INTO memory_temporal_refs (memory_id, ref_key, granularity, weight)"
       " VALUES (?1, ?2, ?3, ?4) ON CONFLICT DO NOTHING";
   char err[MEG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", memory_id);
   aimee_pg_bind_text(st, "?2", ref_key);
   aimee_pg_bind_text(st, "?3", granularity ? granularity : "relative");
   aimee_pg_bind_double(st, "?4", weight);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}
