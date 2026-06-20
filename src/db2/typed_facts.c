/* db2/typed_facts.c: typed-fact store + write gate. See typed_facts.h. */
#include "typed_facts.h"

#include "db2.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
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
   "id, subject, subject_kind, relation, object, object_kind, confidence, source, asserted_at"

int db2_typed_fact_recall(const char *subject, const char *relation_filter, typed_fact_t *out,
                          int max)
{
   if (!subject || !out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   int filt = (relation_filter && relation_filter[0]) ? 1 : 0;
   static const char *q_all = "SELECT " TF_COLS " FROM typed_facts"
                              " WHERE subject = ?1 AND active = 1 ORDER BY relation, id LIMIT ?2";
   static const char *q_rel = "SELECT " TF_COLS " FROM typed_facts"
                              " WHERE subject = ?1 AND relation = ?3 AND active = 1"
                              " ORDER BY id LIMIT ?2";
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
   static const char *q = "SELECT " TF_COLS " FROM typed_facts"
                          " WHERE relation = ?1 AND active = 1 ORDER BY subject, id LIMIT ?2";
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
