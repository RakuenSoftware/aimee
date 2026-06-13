/* fact_lifecycle.c: confidence classes (§5) + correction/retraction (§4) over
 * the semantic edges written through db2_fact_commit. P3. See fact_lifecycle.h. */
#include "../headers/aimee.h" /* edge_t (pulled in transitively by entity_edges.h) */
#include "fact_lifecycle.h"
#include "../headers/rel_types.h" /* rel_types_seed_lookup, correction_behavior_t */
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define FL_ERRBUF 256

/* Shared "now" expression (matches asserted_at's stored format). */
#define FL_NOW_SQL "to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')"

double fact_class_confidence(const char *cls)
{
   if (cls && cls[0] == 'A' && cls[1] == '\0')
      return 1.0;
   if (cls && cls[0] == 'B' && cls[1] == '\0')
      return 0.6;
   return 0.4; /* C and any unknown -> conservative floor */
}

int fact_class_rank(const char *cls)
{
   if (!cls || !cls[0])
      return 0;
   if (cls[0] == 'A' && cls[1] == '\0')
      return 3;
   if (cls[0] == 'B' && cls[1] == '\0')
      return 2;
   if (cls[0] == 'C' && cls[1] == '\0')
      return 1;
   return 0;
}

const char *fact_class_for(fact_authority_t authority, fact_gate_verdict_t verdict)
{
   /* A novel rel_type is speculation — Class C even from a user turn (it is the
    * ontology, not the speaker, that is unproven). Checked first so this is the
    * single source of truth and db2_fact_commit need not special-case NOVEL. */
   if (verdict == FACT_GATE_NOVEL)
      return FACT_CLASS_C;
   if (authority == FACT_AUTHORITY_USER)
      return FACT_CLASS_A; /* a direct user assertion of a known relation earns A */
   if (verdict == FACT_GATE_ACCEPT)
      return FACT_CLASS_B; /* model inference consistent with the ontology */
   return FACT_CLASS_C;    /* anything else (reject/badarg) — conservative */
}

int db2_fact_expire_speculative(int ttl_days)
{
   if (ttl_days <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   /* Cutoff = now - ttl_days, in the same UTC text format as asserted_at, so the
    * lexical comparison is also chronological. */
   time_t cutoff_t = time(NULL) - (time_t)ttl_days * 86400;
   struct tm tmv;
   gmtime_r(&cutoff_t, &tmv);
   char cutoff[32];
   strftime(cutoff, sizeof(cutoff), "%Y-%m-%d %H:%M:%S", &tmv);

   /* Supersede unconfirmed (weight<=1) Class C edges asserted before the cutoff
    * and still active. The row is retained (only stamped) per "always keep the
    * origin artifact". asserted_at='' (legacy/un-stamped) is left alone. */
   static const char *sql = "UPDATE entity_edges SET superseded_at = " FL_NOW_SQL
                            " WHERE edge_class = 'semantic' AND confidence_class = 'C'"
                            "   AND superseded_at = '' AND suppressed = 0 AND weight <= 1"
                            "   AND asserted_at <> '' AND asserted_at < ?1";
   char err[FL_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", cutoff);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_DONE ? changes : -1;
}

int db2_fact_promote_durable(int threshold)
{
   if (threshold <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   /* Class B confirmed >= threshold times becomes durable (confidence 0.8). It
    * stays Class B (never promoted to A, §5 R2-3); durability means it no longer
    * expires (expiry targets only Class C). */
   static const char *sql = "UPDATE entity_edges SET confidence = 0.8"
                            " WHERE edge_class = 'semantic' AND confidence_class = 'B'"
                            "   AND superseded_at = '' AND suppressed = 0"
                            "   AND weight >= ?1 AND confidence < 0.8";
   char err[FL_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", threshold);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_DONE ? changes : -1;
}

int db2_fact_retract(const char *source, const char *relation, fact_authority_t authority)
{
   if (!source || !source[0] || !relation || !relation[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char norm[REL_TYPE_NAME_MAX];
   rel_type_normalize(relation, norm, sizeof(norm));
   if (!norm[0])
      return -1;

   /* correction_behavior is declared on the rel_type (seed). Unknown / novel
    * types default to supersede. */
   const rel_type_def_t *def = rel_types_seed_lookup(norm);
   correction_behavior_t behavior = def ? def->correction_behavior : CORR_SUPERSEDE;

   /* Authority rule (§4 R1-B1): immutable blocks model/inferred edits, but a user
    * always wins — a user retraction supersedes even an immutable fact. */
   if (behavior == CORR_IMMUTABLE && authority != FACT_AUTHORITY_USER)
      return FACT_RETRACT_IMMUTABLE;

   /* supersede (and user-overridden immutable): stamp superseded_at.
    * hard_delete: tombstone (suppressed=1) + stamp, still retained + auditable. */
   const char *sql = (behavior == CORR_HARD_DELETE)
                         ? "UPDATE entity_edges SET suppressed = 1, superseded_at = " FL_NOW_SQL
                           " WHERE source = ?1 AND relation = ?2 AND edge_class = 'semantic'"
                           "   AND superseded_at = '' AND suppressed = 0"
                         : "UPDATE entity_edges SET superseded_at = " FL_NOW_SQL
                           " WHERE source = ?1 AND relation = ?2 AND edge_class = 'semantic'"
                           "   AND superseded_at = '' AND suppressed = 0";
   char err[FL_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", source);
   aimee_pg_bind_text(st, "?2", norm);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_DONE ? changes : -1;
}

int db2_fact_current_count(const char *entity)
{
   if (!entity || !entity[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT COUNT(*) FROM entity_edges"
                            " WHERE (source = ?1 OR target = ?2) AND edge_class = 'semantic'"
                            "   AND superseded_at = '' AND suppressed = 0";
   char err[FL_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", entity);
   aimee_pg_bind_text(st, "?2", entity);
   int c = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      c = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return c;
}
