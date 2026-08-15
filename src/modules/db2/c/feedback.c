/* db2/feedback.c: feedback recording — Postgres via libpq. */

#include "feedback.h"
#include "rules.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "aimee.h" /* now_utc */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *db2_feedback_parse_polarity(const char *input)
{
   if (!input || !*input)
      return NULL;

   if (strcmp(input, "+") == 0 || strcmp(input, "posi") == 0 || strcmp(input, "positive") == 0)
      return "positive";

   if (strcmp(input, "-") == 0 || strcmp(input, "negi") == 0 || strcmp(input, "negative") == 0)
      return "negative";

   if (strcmp(input, "principle") == 0)
      return "principle";

   return NULL;
}

int db2_feedback_record(const char *polarity, const char *title, const char *description,
                        int weight_override, int *reinforced)
{
   void *conn = db2_conn();
   if (!conn || !polarity || !title)
      return -1;

   *reinforced = 0;

   rule_t existing;
   char err[256] = "";
   if (db2_rules_find_by_title(title, &existing) == 0)
   {
      int new_weight = existing.weight + 50;
      if (new_weight > 100)
         new_weight = 100;

      char ts[32];
      now_utc(ts, sizeof(ts));

      aimee_pg_stmt_t *st =
          aimee_pg_prepare(conn,
                           "UPDATE rules SET weight = ?1, description = ?2,"
                           " updated_at = ?3, last_reinforced_at = ?4 WHERE id = ?5",
                           err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_int(st, "?1", new_weight);
      aimee_pg_bind_text(st, "?2", description ? description : existing.description);
      aimee_pg_bind_text(st, "?3", ts);
      aimee_pg_bind_text(st, "?4", ts);
      aimee_pg_bind_int(st, "?5", existing.id);
      (void)aimee_pg_step(st, err, sizeof(err));
      aimee_pg_finalize(st);

      *reinforced = 1;
      db2_rules_cache_invalidate();
      return existing.id;
   }

   int weight = 50;
   if (weight_override >= 0)
      weight = weight_override;
   if (weight > 100)
      weight = 100;

   char ts[32];
   now_utc(ts, sizeof(ts));

   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "INSERT INTO rules (polarity, title, description, weight,"
                                          " domain, created_at, updated_at)"
                                          " VALUES (?1, ?2, ?3, ?4, '', ?5, ?6) RETURNING id",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", polarity);
   aimee_pg_bind_text(st, "?2", title);
   aimee_pg_bind_text(st, "?3", description ? description : "");
   aimee_pg_bind_int(st, "?4", weight);
   aimee_pg_bind_text(st, "?5", ts);
   aimee_pg_bind_text(st, "?6", ts);

   int id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);

   if (id < 0)
      return -1;
   db2_rules_cache_invalidate();
   return id;
}
