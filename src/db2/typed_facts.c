/* db2/typed_facts.c: compatibility write gate over canonical assertions. */
#include "typed_facts.h"

#include "db2.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "fact_mutation.h"
#include "memory_ontology.h"
#include "memory_scope_query.h"

#include <stddef.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TF_ERRBUF 256

/* Seed ontology: the relations the gate accepts, with allowed subject/object
 * kinds ("ANY" = any kind, "SCALAR" = a literal value). Mirrors the proposal's
 * rel_types table as a fixed in-code seed for this slice. The CSS convention
 * relations (naming_convention, token_strategy, file_layout,
 * component_owns_styles, should_match) back the migration assistant's #2
 * upgrade; a couple of general identity relations show the layer is not
 * CSS-specific. */
typedef struct
{
   const char *relation;
   const char *head_kind; /* allowed subject kind, or "ANY" */
   const char *tail_kind; /* allowed object kind, or "SCALAR"/"ANY" */
} tf_rel_t;

static const tf_rel_t TF_ONTOLOGY[] = {
    {"naming_convention", "project", "SCALAR"},
    {"token_strategy", "project", "SCALAR"},
    {"file_layout", "project", "SCALAR"},
    {"component_owns_styles", "project", "SCALAR"},
    {"should_match", "component", "convention"},
    /* deepening sweep (Part B): a settled architecture decision about a code seam.
     * subject = canonical "<file>:<top-decl>"; object = "extracted@<commit>" or
     * "rejected:pass-through". The sweep reads active facts to exclude re-proposals. */
    {"architecture_settled", "code_site", "SCALAR"},
    /* general identity examples (the layer is not CSS-specific) */
    {"located_in", "person", "place"},
    {"has_ip", "device", "SCALAR"},
    {NULL, NULL, NULL},
};

static const tf_rel_t *tf_lookup(const char *relation)
{
   if (!relation)
      return NULL;
   for (int i = 0; TF_ONTOLOGY[i].relation; i++)
      if (strcmp(TF_ONTOLOGY[i].relation, relation) == 0)
         return &TF_ONTOLOGY[i];
   return NULL;
}

int typed_fact_relation_known(const char *relation)
{
   return tf_lookup(relation) != NULL;
}

static int tf_kind_ok(const char *allowed, const char *actual)
{
   if (strcmp(allowed, "ANY") == 0)
      return 1;
   /* SCALAR accepts any literal value kind ("" or "scalar"/"value"). */
   if (strcmp(allowed, "SCALAR") == 0)
      return !actual || actual[0] == '\0' || strcmp(actual, "scalar") == 0 ||
             strcmp(actual, "value") == 0;
   return actual && strcmp(allowed, actual) == 0;
}

int db2_typed_fact_assert(const char *subject, const char *subject_kind, const char *relation,
                          const char *object, const char *object_kind, int confidence,
                          const char *source, const char *now_iso)
{
   if (!subject || !subject[0] || !relation || !object)
      return TYPED_FACT_ERROR;
   const tf_rel_t *rel = tf_lookup(relation);
   if (!rel)
      return TYPED_FACT_REJECTED_REL;
   if (!tf_kind_ok(rel->head_kind, subject_kind) || !tf_kind_ok(rel->tail_kind, object_kind))
      return TYPED_FACT_REJECTED_KIND;
   if (confidence < 0)
      confidence = 0;
   if (confidence > 100)
      confidence = 100;

   fact_actor_t actor;
   if (db2_fact_actor_internal(FACT_ACTOR_SYSTEM, &actor) != 0)
      return TYPED_FACT_ERROR;
   fact_evidence_input_t evidence = {.source_kind = "typed_fact_source",
                                     .source_id = source && source[0] ? source : NULL,
                                     .observed_at = now_iso,
                                     .stance = "supports"};
   fact_assertion_input_t input = {
       .source = subject,
       .relation = relation,
       .target = object,
       .relation_id = 0,
       .subject_kind = NODE_OTHER,
       .object_kind =
           object_kind && (strcmp(object_kind, "scalar") == 0 || strcmp(object_kind, "value") == 0)
               ? NODE_SCALAR
               : NODE_OTHER,
       .confidence_class = "B",
       .confidence = (double)confidence / 100.0,
       .assertion_kind = FACT_KIND_WORLD_FACT,
       .valid_from = now_iso,
       .evidence = &evidence,
       .functional = 1};
   fact_mutation_result_t result;
   if (db2_fact_mutation_assert(&actor, &input, &result) != 0)
      return TYPED_FACT_ERROR;
   return result.changed || result.evidence_added ? TYPED_FACT_OK : TYPED_FACT_UNCHANGED;
#if 0 /* legacy parallel fact table retired; semantic assertions are canonical */

   void *conn = db2_conn();
   if (!conn)
      return TYPED_FACT_ERROR;

   char err[TF_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return TYPED_FACT_ERROR;
   int rc = TYPED_FACT_OK;

   /* Examine the current active fact for (subject, relation). */
   static const char *sel = "SELECT id, object FROM typed_facts"
                            " WHERE subject = ?1 AND relation = ?2 AND active = 1"
                            " ORDER BY id DESC LIMIT 1";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sel, err, sizeof(err));
   int64_t prior_id = -1;
   int identical = 0;
   if (st)
   {
      aimee_pg_bind_text(st, "?1", subject);
      aimee_pg_bind_text(st, "?2", relation);
      if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         prior_id = aimee_pg_column_int64(st, 0);
         const char *prior_obj = aimee_pg_column_text(st, 1);
         identical = prior_obj && strcmp(prior_obj, object) == 0;
      }
      aimee_pg_finalize(st);
   }
   else
      rc = TYPED_FACT_ERROR;

   if (rc == TYPED_FACT_OK && identical)
   {
      aimee_pg_exec(conn, "COMMIT", err, sizeof(err));
      return TYPED_FACT_UNCHANGED;
   }

   /* Insert the new fact. */
   int64_t new_id = -1;
   if (rc == TYPED_FACT_OK)
   {
      static const char *ins =
          "INSERT INTO typed_facts"
          " (subject, subject_kind, relation, object, object_kind, confidence, source, asserted_at)"
          " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) RETURNING id";
      aimee_pg_stmt_t *si = aimee_pg_prepare(conn, ins, err, sizeof(err));
      if (!si)
         rc = TYPED_FACT_ERROR;
      else
      {
         aimee_pg_bind_text(si, "?1", subject);
         aimee_pg_bind_text(si, "?2", subject_kind ? subject_kind : "");
         aimee_pg_bind_text(si, "?3", relation);
         aimee_pg_bind_text(si, "?4", object);
         aimee_pg_bind_text(si, "?5", object_kind ? object_kind : "");
         aimee_pg_bind_int(si, "?6", confidence);
         aimee_pg_bind_text(si, "?7", source ? source : "");
         aimee_pg_bind_text(si, "?8", now_iso ? now_iso : "");
         if (aimee_pg_step(si, err, sizeof(err)) == AIMEE_PG_ROW)
            new_id = aimee_pg_column_int64(si, 0);
         else
            rc = TYPED_FACT_ERROR;
         aimee_pg_finalize(si);
      }
   }

   /* Supersede the prior contradicting fact (retain it, mark inactive). */
   if (rc == TYPED_FACT_OK && prior_id >= 0 && new_id >= 0)
   {
      static const char *sup =
          "UPDATE typed_facts SET active = 0, superseded_by = ?2 WHERE id = ?1";
      aimee_pg_stmt_t *su = aimee_pg_prepare(conn, sup, err, sizeof(err));
      if (su)
      {
         aimee_pg_bind_int64(su, "?1", prior_id);
         aimee_pg_bind_int64(su, "?2", new_id);
         if (aimee_pg_step(su, err, sizeof(err)) != AIMEE_PG_DONE)
            rc = TYPED_FACT_ERROR;
         aimee_pg_finalize(su);
      }
      else
         rc = TYPED_FACT_ERROR;
   }

   if (rc == TYPED_FACT_OK)
      aimee_pg_exec(conn, "COMMIT", err, sizeof(err));
   else
      aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   return rc;
#endif
}

static void tf_row(aimee_pg_stmt_t *st, typed_fact_t *f)
{
   memset(f, 0, sizeof(*f));
   f->id = aimee_pg_column_int64(st, 0);
   const char *s = aimee_pg_column_text(st, 1);
   const char *sk = aimee_pg_column_text(st, 2);
   const char *r = aimee_pg_column_text(st, 3);
   const char *o = aimee_pg_column_text(st, 4);
   const char *ok = aimee_pg_column_text(st, 5);
   const char *src = aimee_pg_column_text(st, 7);
   const char *at = aimee_pg_column_text(st, 8);
   snprintf(f->subject, sizeof(f->subject), "%s", s ? s : "");
   snprintf(f->subject_kind, sizeof(f->subject_kind), "%s", sk ? sk : "");
   snprintf(f->relation, sizeof(f->relation), "%s", r ? r : "");
   snprintf(f->object, sizeof(f->object), "%s", o ? o : "");
   snprintf(f->object_kind, sizeof(f->object_kind), "%s", ok ? ok : "");
   f->confidence = aimee_pg_column_int(st, 6);
   snprintf(f->source, sizeof(f->source), "%s", src ? src : "");
   snprintf(f->asserted_at, sizeof(f->asserted_at), "%s", at ? at : "");
}

#define TF_COLS                                                                                    \
   "e.id, e.source, CAST(e.subject_kind AS TEXT), e.relation, e.target,"                           \
   " CAST(e.object_kind AS TEXT), CAST(e.confidence * 100 AS INTEGER),"                            \
   " COALESCE((SELECT fe.source_id FROM fact_evidence fe WHERE fe.assertion_id=e.id"               \
   " AND fe.invalidated_at='' ORDER BY fe.id DESC LIMIT 1),''), e.asserted_at"

int db2_typed_fact_recall(const char *subject, const char *relation_filter, typed_fact_t *out,
                          int max)
{
   if (!subject || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   int filt = (relation_filter && relation_filter[0]) ? 1 : 0;
   static const char *q_all = "SELECT " TF_COLS " FROM entity_edges e"
                              " WHERE e.source = ?1 AND e.edge_class='semantic'"
                              " AND e.lifecycle_state IN ('persistent','promoted')"
                              " AND e.superseded_at='' AND e.invalidated_at='' AND e.suppressed=0"
                              " ORDER BY e.relation, e.id LIMIT ?2";
   static const char *q_rel = "SELECT " TF_COLS " FROM entity_edges e"
                              " WHERE e.source = ?1 AND e.relation = ?3 AND e.edge_class='semantic'"
                              " AND e.lifecycle_state IN ('persistent','promoted')"
                              " AND e.superseded_at='' AND e.invalidated_at='' AND e.suppressed=0"
                              " ORDER BY e.id LIMIT ?2";
   char err[TF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, filt ? q_rel : q_all, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", subject);
   aimee_pg_bind_int(st, "?2", max);
   if (filt)
      aimee_pg_bind_text(st, "?3", relation_filter);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      tf_row(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int db2_typed_fact_by_relation(const char *relation, typed_fact_t *out, int max)
{
   if (!relation || !relation[0] || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *q = "SELECT " TF_COLS " FROM entity_edges e"
                          " WHERE e.relation = ?1 AND e.edge_class='semantic'"
                          " AND e.lifecycle_state IN ('persistent','promoted')"
                          " AND e.superseded_at='' AND e.invalidated_at='' AND e.suppressed=0"
                          " ORDER BY e.source, e.id LIMIT ?2";
   char err[TF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, q, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", relation);
   aimee_pg_bind_int(st, "?2", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      tf_row(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

static int tf_timestamp_valid(const char *value)
{
   if (!value || !value[0])
      return 1;
   size_t n = strlen(value);
   if (n != 19 && n != 20)
      return 0;
   if (n == 20 && value[19] != 'Z')
      return 0;
   for (size_t i = 0; i < 19; i++)
   {
      if (i == 4 || i == 7)
      {
         if (value[i] != '-')
            return 0;
      }
      else if (i == 10)
      {
         if (value[i] != 'T' && value[i] != ' ')
            return 0;
      }
      else if (i == 13 || i == 16)
      {
         if (value[i] != ':')
            return 0;
      }
      else if (!isdigit((unsigned char)value[i]))
         return 0;
   }
   int year = atoi(value), month = atoi(value + 5), day = atoi(value + 8);
   int hour = atoi(value + 11), minute = atoi(value + 14), second = atoi(value + 17);
   if (year < 1 || month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59)
      return 0;
   static const int days_in_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
   int max_day = days_in_month[month];
   if (month == 2 && (year % 400 == 0 || (year % 4 == 0 && year % 100 != 0)))
      max_day = 29;
   return day >= 1 && day <= max_day;
}

static void tf_copy(char *dst, size_t cap, const char *src)
{
   snprintf(dst, cap, "%s", src ? src : "");
}

static void tf_load_semantic_row(aimee_pg_stmt_t *st, semantic_assertion_hit_t *hit)
{
   memset(hit, 0, sizeof(*hit));
   hit->assertion_id = aimee_pg_column_int64(st, 0);
   hit->version = aimee_pg_column_int(st, 1);
   tf_copy(hit->subject, sizeof(hit->subject), aimee_pg_column_text(st, 2));
   tf_copy(hit->relation, sizeof(hit->relation), aimee_pg_column_text(st, 3));
   tf_copy(hit->object, sizeof(hit->object), aimee_pg_column_text(st, 4));
   tf_copy(hit->assertion_kind, sizeof(hit->assertion_kind), aimee_pg_column_text(st, 5));
   tf_copy(hit->lifecycle_state, sizeof(hit->lifecycle_state), aimee_pg_column_text(st, 6));
   hit->authority_rank = aimee_pg_column_int(st, 7);
   tf_copy(hit->confidence_class, sizeof(hit->confidence_class), aimee_pg_column_text(st, 8));
   hit->confidence = aimee_pg_column_double(st, 9);
   tf_copy(hit->valid_from, sizeof(hit->valid_from), aimee_pg_column_text(st, 10));
   tf_copy(hit->valid_until, sizeof(hit->valid_until), aimee_pg_column_text(st, 11));
   tf_copy(hit->asserted_at, sizeof(hit->asserted_at), aimee_pg_column_text(st, 12));
   tf_copy(hit->superseded_at, sizeof(hit->superseded_at), aimee_pg_column_text(st, 13));
   hit->historical = aimee_pg_column_int(st, 14);
   hit->support_count = aimee_pg_column_int(st, 15);
   hit->contradiction_count = aimee_pg_column_int(st, 16);
   hit->raw_score = aimee_pg_column_double(st, 17);
   hit->fused_score = hit->raw_score;
   tf_copy(hit->inclusion_reason, sizeof(hit->inclusion_reason),
           "lexical semantic match after lifecycle, authority, and temporal filters");
}

static void tf_add_retrieval_trace(semantic_assertion_hit_t *hit, const char *channel,
                                   double raw_score, double fused_score, int rank)
{
   if (!hit || hit->retrieval_count >= SEMANTIC_ASSERTION_TRACE_MAX)
      return;
   semantic_assertion_retrieval_trace_t *trace = &hit->retrieval[hit->retrieval_count++];
   tf_copy(trace->channel, sizeof(trace->channel), channel);
   trace->raw_score = raw_score;
   trace->fused_score = fused_score;
   trace->rank = rank;
}

static void tf_load_evidence(void *conn, semantic_assertion_hit_t *hit)
{
   static const char *sql =
       "SELECT source_kind, source_id, source_span, observed_at, stance"
       " FROM fact_evidence WHERE assertion_id=?1 AND invalidated_at=''"
       " ORDER BY CASE WHEN stance='supports' THEN 0 ELSE 1 END, id DESC LIMIT ?2";
   char err[TF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", hit->assertion_id);
   aimee_pg_bind_int(st, "?2", SEMANTIC_ASSERTION_EVIDENCE_MAX);
   while (hit->evidence_count < SEMANTIC_ASSERTION_EVIDENCE_MAX &&
          aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      semantic_assertion_evidence_t *ev = &hit->evidence[hit->evidence_count++];
      tf_copy(ev->source_kind, sizeof(ev->source_kind), aimee_pg_column_text(st, 0));
      tf_copy(ev->source_id, sizeof(ev->source_id), aimee_pg_column_text(st, 1));
      tf_copy(ev->source_span, sizeof(ev->source_span), aimee_pg_column_text(st, 2));
      tf_copy(ev->observed_at, sizeof(ev->observed_at), aimee_pg_column_text(st, 3));
      tf_copy(ev->stance, sizeof(ev->stance), aimee_pg_column_text(st, 4));
   }
   aimee_pg_finalize(st);
}

int db2_semantic_assertion_search(const char *query, const char *valid_at, const char *believed_at,
                                  int include_historical, int limit, semantic_assertion_hit_t *out,
                                  int max)
{
   if (!query || !query[0] || !out || max <= 0)
      return SEMANTIC_ASSERTION_SEARCH_ERROR;
   if (!tf_timestamp_valid(valid_at) || !tf_timestamp_valid(believed_at))
      return SEMANTIC_ASSERTION_SEARCH_INVALID_TIME;
   void *conn = db2_conn();
   if (!conn)
      return SEMANTIC_ASSERTION_SEARCH_ERROR;
   if (limit <= 0 || limit > max)
      limit = max;

   char pattern[TYPED_FACT_STR_MAX + 3], exact[TYPED_FACT_STR_MAX + 1];
   size_t qn = strlen(query);
   if (qn > TYPED_FACT_STR_MAX)
      qn = TYPED_FACT_STR_MAX;
   pattern[0] = '%';
   for (size_t i = 0; i < qn; i++)
   {
      exact[i] = (char)tolower((unsigned char)query[i]);
      pattern[i + 1] = exact[i];
   }
   exact[qn] = '\0';
   pattern[qn + 1] = '%';
   pattern[qn + 2] = '\0';

   /* Empty time parameters are bound twice so query semantics remain explicit:
    * neither axis can accidentally degrade into an unfiltered historical scan. */
   static const char *sql =
       "SELECT e.id,e.version,e.source,e.relation,e.target,e.assertion_kind,"
       " e.lifecycle_state,e.authority_rank,e.confidence_class,e.confidence,"
       " e.valid_from,e.valid_until,e.asserted_at,e.superseded_at,"
       " CASE WHEN NOT ((e.asserted_at='' OR"
       "      REPLACE(SUBSTR(e.asserted_at,1,19),'T',' ')<=pg_now_text())"
       "   AND (e.superseded_at='' OR pg_now_text()<"
       "      REPLACE(SUBSTR(e.superseded_at,1,19),'T',' '))"
       "   AND (e.invalidated_at='' OR pg_now_text()<"
       "      REPLACE(SUBSTR(e.invalidated_at,1,19),'T',' '))"
       "   AND (e.assertion_kind<>'world_fact' OR ((e.valid_from='' OR"
       "      REPLACE(SUBSTR(e.valid_from,1,19),'T',' ')<=pg_now_text())"
       "      AND (e.valid_until='' OR pg_now_text()<"
       "      REPLACE(SUBSTR(e.valid_until,1,19),'T',' '))))) THEN 1 ELSE 0 END,"
       " (SELECT COUNT(*) FROM fact_evidence fe WHERE fe.assertion_id=e.id"
       "    AND fe.invalidated_at='' AND fe.stance='supports'),"
       " (SELECT COUNT(*) FROM fact_evidence fe WHERE fe.assertion_id=e.id"
       "    AND fe.invalidated_at='' AND fe.stance='contradicts'),"
       " (CASE WHEN LOWER(e.source)=?1 OR LOWER(e.target)=?1 THEN 4.0"
       "       WHEN LOWER(e.relation)=?1 THEN 3.5 ELSE 1.0 END"
       "  + e.confidence + CAST(e.authority_rank AS DOUBLE PRECISION)/100.0) AS score"
       " FROM entity_edges e"
       " WHERE e.edge_class='semantic' AND e.suppressed=0"
       " AND e.lifecycle_state IN ('persistent','promoted')"
       " AND (LOWER(e.source) LIKE ?2 OR LOWER(e.relation) LIKE ?2"
       "      OR LOWER(e.target) LIKE ?2"
       "      OR LOWER(e.source || ' ' || e.relation || ' ' || e.target) LIKE ?2)"
       " AND (?4<>'' OR ?3=1 OR ((e.asserted_at='' OR"
       "      REPLACE(SUBSTR(e.asserted_at,1,19),'T',' ')<=pg_now_text())"
       "      AND (e.superseded_at='' OR pg_now_text()<"
       "           REPLACE(SUBSTR(e.superseded_at,1,19),'T',' '))"
       "      AND (e.invalidated_at='' OR pg_now_text()<"
       "           REPLACE(SUBSTR(e.invalidated_at,1,19),'T',' '))))"
       " AND (?4='' OR ((e.asserted_at='' OR"
       "      REPLACE(SUBSTR(e.asserted_at,1,19),'T',' ')<=REPLACE(SUBSTR(?4,1,19),'T',' '))"
       "      AND (e.superseded_at='' OR REPLACE(SUBSTR(?4,1,19),'T',' ')<"
       "           REPLACE(SUBSTR(e.superseded_at,1,19),'T',' '))"
       "      AND (e.invalidated_at='' OR REPLACE(SUBSTR(?4,1,19),'T',' ')<"
       "           REPLACE(SUBSTR(e.invalidated_at,1,19),'T',' '))))"
       " AND (?5<>'' OR ?3=1 OR e.assertion_kind<>'world_fact' OR"
       "      ((e.valid_from='' OR REPLACE(SUBSTR(e.valid_from,1,19),'T',' ')<=pg_now_text())"
       "       AND (e.valid_until='' OR pg_now_text()<"
       "            REPLACE(SUBSTR(e.valid_until,1,19),'T',' '))))"
       " AND (?5='' OR ((e.valid_from='' OR"
       "      REPLACE(SUBSTR(e.valid_from,1,19),'T',' ')<=REPLACE(SUBSTR(?5,1,19),'T',' '))"
       "      AND (e.valid_until='' OR REPLACE(SUBSTR(?5,1,19),'T',' ')<"
       "           REPLACE(SUBSTR(e.valid_until,1,19),'T',' '))))"
       /* Deny-dominant derivation: if any live memory evidence is outside the
        * request-local visibility partition, the synthesized assertion is not
        * returned. Evidence from non-memory global/system sources is unaffected. */
       " AND (?101=0 OR ?102=1 OR NOT EXISTS ("
       "   SELECT 1 FROM fact_evidence se JOIN memories sm"
       "     ON se.source_kind='memory'"
       "    AND se.source_id=('memory:' || CAST(sm.id AS TEXT))"
       "   WHERE se.assertion_id=e.id AND se.invalidated_at='' AND (" DB2_MEMORY_SCOPE_RANK_SQL(
           "sm.id") ")=0))"
                    " ORDER BY score DESC,e.authority_rank DESC,e.id DESC LIMIT ?6";
   char err[TF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return SEMANTIC_ASSERTION_SEARCH_ERROR;
   aimee_pg_bind_text(st, "?1", exact);
   aimee_pg_bind_text(st, "?2", pattern);
   aimee_pg_bind_int(st, "?3", include_historical ? 1 : 0);
   aimee_pg_bind_text(st, "?4", believed_at ? believed_at : "");
   aimee_pg_bind_text(st, "?5", valid_at ? valid_at : "");
   aimee_pg_bind_int(st, "?6", limit);
   db2_memory_scope_bind_current(st);
   int n = 0;
   while (n < limit && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      tf_load_semantic_row(st, &out[n]);
      out[n].rank = n + 1;
      tf_add_retrieval_trace(&out[n], "lexical", out[n].raw_score, out[n].fused_score, out[n].rank);
      n++;
   }
   aimee_pg_finalize(st);
   for (int i = 0; i < n; i++)
      tf_load_evidence(conn, &out[i]);
   return n;
}

int db2_semantic_assertion_get_filtered(int64_t assertion_id, const char *valid_at,
                                        const char *believed_at, int include_historical,
                                        semantic_assertion_hit_t *out)
{
   if (assertion_id <= 0 || !out)
      return SEMANTIC_ASSERTION_SEARCH_ERROR;
   if (!tf_timestamp_valid(valid_at) || !tf_timestamp_valid(believed_at))
      return SEMANTIC_ASSERTION_SEARCH_INVALID_TIME;
   void *conn = db2_conn();
   if (!conn)
      return SEMANTIC_ASSERTION_SEARCH_ERROR;
   static const char *sql =
       "SELECT e.id,e.version,e.source,e.relation,e.target,e.assertion_kind,"
       " e.lifecycle_state,e.authority_rank,e.confidence_class,e.confidence,"
       " e.valid_from,e.valid_until,e.asserted_at,e.superseded_at,"
       " CASE WHEN NOT ((e.asserted_at='' OR"
       "      REPLACE(SUBSTR(e.asserted_at,1,19),'T',' ')<=pg_now_text())"
       "   AND (e.superseded_at='' OR pg_now_text()<"
       "      REPLACE(SUBSTR(e.superseded_at,1,19),'T',' '))"
       "   AND (e.invalidated_at='' OR pg_now_text()<"
       "      REPLACE(SUBSTR(e.invalidated_at,1,19),'T',' '))"
       "   AND (e.assertion_kind<>'world_fact' OR ((e.valid_from='' OR"
       "      REPLACE(SUBSTR(e.valid_from,1,19),'T',' ')<=pg_now_text())"
       "      AND (e.valid_until='' OR pg_now_text()<"
       "      REPLACE(SUBSTR(e.valid_until,1,19),'T',' '))))) THEN 1 ELSE 0 END,"
       " (SELECT COUNT(*) FROM fact_evidence fe WHERE fe.assertion_id=e.id"
       "    AND fe.invalidated_at='' AND fe.stance='supports'),"
       " (SELECT COUNT(*) FROM fact_evidence fe WHERE fe.assertion_id=e.id"
       "    AND fe.invalidated_at='' AND fe.stance='contradicts'),"
       " (1.0+e.confidence+CAST(e.authority_rank AS DOUBLE PRECISION)/100.0)"
       " FROM entity_edges e WHERE e.id=?1 AND e.edge_class='semantic'"
       " AND e.suppressed=0 AND e.lifecycle_state IN ('persistent','promoted')"
       " AND (?3<>'' OR ?2=1 OR ((e.asserted_at='' OR"
       "      REPLACE(SUBSTR(e.asserted_at,1,19),'T',' ')<=pg_now_text())"
       "      AND (e.superseded_at='' OR pg_now_text()<"
       "           REPLACE(SUBSTR(e.superseded_at,1,19),'T',' '))"
       "      AND (e.invalidated_at='' OR pg_now_text()<"
       "           REPLACE(SUBSTR(e.invalidated_at,1,19),'T',' '))))"
       " AND (?3='' OR ((e.asserted_at='' OR"
       "      REPLACE(SUBSTR(e.asserted_at,1,19),'T',' ')<=REPLACE(SUBSTR(?3,1,19),'T',' '))"
       "      AND (e.superseded_at='' OR REPLACE(SUBSTR(?3,1,19),'T',' ')<"
       "           REPLACE(SUBSTR(e.superseded_at,1,19),'T',' '))"
       "      AND (e.invalidated_at='' OR REPLACE(SUBSTR(?3,1,19),'T',' ')<"
       "           REPLACE(SUBSTR(e.invalidated_at,1,19),'T',' '))))"
       " AND (?4<>'' OR ?2=1 OR e.assertion_kind<>'world_fact' OR"
       "      ((e.valid_from='' OR REPLACE(SUBSTR(e.valid_from,1,19),'T',' ')<=pg_now_text())"
       "       AND (e.valid_until='' OR pg_now_text()<"
       "            REPLACE(SUBSTR(e.valid_until,1,19),'T',' '))))"
       " AND (?4='' OR ((e.valid_from='' OR"
       "      REPLACE(SUBSTR(e.valid_from,1,19),'T',' ')<=REPLACE(SUBSTR(?4,1,19),'T',' '))"
       "      AND (e.valid_until='' OR REPLACE(SUBSTR(?4,1,19),'T',' ')<"
       "           REPLACE(SUBSTR(e.valid_until,1,19),'T',' '))))"
       " AND (?101=0 OR ?102=1 OR NOT EXISTS ("
       "   SELECT 1 FROM fact_evidence se JOIN memories sm"
       "     ON se.source_kind='memory'"
       "    AND se.source_id=('memory:' || CAST(sm.id AS TEXT))"
       "   WHERE se.assertion_id=e.id AND se.invalidated_at='' AND (" DB2_MEMORY_SCOPE_RANK_SQL(
           "sm.id") ")=0))";
   char err[TF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return SEMANTIC_ASSERTION_SEARCH_ERROR;
   aimee_pg_bind_int64(st, "?1", assertion_id);
   aimee_pg_bind_int(st, "?2", include_historical ? 1 : 0);
   aimee_pg_bind_text(st, "?3", believed_at ? believed_at : "");
   aimee_pg_bind_text(st, "?4", valid_at ? valid_at : "");
   db2_memory_scope_bind_current(st);
   int rc = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      tf_load_semantic_row(st, out);
      rc = 1;
   }
   aimee_pg_finalize(st);
   if (rc == 1)
      tf_load_evidence(conn, out);
   return rc;
}

int db2_semantic_assertion_index_list(int64_t after_assertion_id,
                                      semantic_assertion_index_row_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql =
       "SELECT e.id,e.version,e.source,e.relation,e.target,e.assertion_kind FROM entity_edges e"
       " LEFT JOIN memory_embeddings me"
       "   ON me.point_id=(2000000000000+e.id) AND me.record_type='semantic_assertion'"
       " WHERE e.id>?1 AND e.edge_class='semantic' AND e.suppressed=0"
       " AND e.lifecycle_state IN ('persistent','promoted')"
       " AND (me.point_id IS NULL OR me.kind<>( 'assertion_v' || CAST(e.version AS TEXT)))"
       " ORDER BY e.id LIMIT ?2";
   char err[TF_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", after_assertion_id);
   aimee_pg_bind_int(st, "?2", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out[n].assertion_id = aimee_pg_column_int64(st, 0);
      out[n].version = aimee_pg_column_int(st, 1);
      snprintf(out[n].canonical_rendering, sizeof(out[n].canonical_rendering), "%s %s %s [%s]",
               aimee_pg_column_text(st, 2) ? aimee_pg_column_text(st, 2) : "",
               aimee_pg_column_text(st, 3) ? aimee_pg_column_text(st, 3) : "",
               aimee_pg_column_text(st, 4) ? aimee_pg_column_text(st, 4) : "",
               aimee_pg_column_text(st, 5) ? aimee_pg_column_text(st, 5) : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}
