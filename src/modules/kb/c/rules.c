/* kb_store/rules.c: agent rules — Postgres via libpq. */

#include "config.h"
#include "rules.h"
#include "kb_store_internal.h"
#include "db_postgres.h"
#include "aimee.h" /* now_utc, MAX_SESSION_CHARS, MAX_SESSION_RULES, MAX_RULE_TEXT_LEN */
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RULES_ERRBUF 256

#define RULES_SELECT_COLS                                                                          \
   "id, polarity, title, description, weight, domain, "                                            \
   "created_at, updated_at, directive_type"

static void row_to_rule(aimee_pg_stmt_t *st, rule_t *r)
{
   memset(r, 0, sizeof(*r));
   r->id = aimee_pg_column_int(st, 0);
   kb_store_copy_col_text(r->polarity, sizeof(r->polarity), st, 1);
   kb_store_copy_col_text(r->title, sizeof(r->title), st, 2);
   kb_store_copy_col_text(r->description, sizeof(r->description), st, 3);
   r->weight = aimee_pg_column_int(st, 4);
   kb_store_copy_col_text(r->domain, sizeof(r->domain), st, 5);
   kb_store_copy_col_text(r->created_at, sizeof(r->created_at), st, 6);
   kb_store_copy_col_text(r->updated_at, sizeof(r->updated_at), st, 7);
   kb_store_copy_col_text(r->directive_type, sizeof(r->directive_type), st, 8);
}

int kb_store_rules_insert(const char *polarity, const char *title, const char *description,
                          int weight)
{
   if (!title || !*title)
      return -1;
   void *conn = kb_store_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "INSERT INTO rules (polarity, title, description, weight, created_at, updated_at)"
       " VALUES (?1, ?2, ?3, ?4, pg_now_text(), pg_now_text())";
   char err[RULES_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", (polarity && polarity[0]) ? polarity : "positive");
   aimee_pg_bind_text(st, "?2", title);
   aimee_pg_bind_text(st, "?3", description ? description : "");
   aimee_pg_bind_int(st, "?4", weight);
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int kb_store_rules_find_by_title(const char *title, rule_t *out)
{
   void *conn = kb_store_conn();
   if (!conn || !title || !out)
      return -1;

   char err[RULES_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT " RULES_SELECT_COLS " FROM rules "
                                          "WHERE LOWER(title) = LOWER(?1)",
                                          err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", title);

   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      row_to_rule(st, out);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int kb_store_rules_delete(int id)
{
   void *conn = kb_store_conn();
   if (!conn)
      return -1;

   char err[RULES_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "DELETE FROM rules WHERE id = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", id);
   aimee_pg_step_t step_rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   if (step_rc != AIMEE_PG_DONE)
      return -1;
   if (changes > 0)
      kb_store_rules_cache_invalidate();
   return changes > 0 ? 0 : -1;
}

int kb_store_rules_delete_by_directive_type(const char *directive_type)
{
   void *conn = kb_store_conn();
   if (!conn || !directive_type)
      return -1;

   char err[RULES_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "DELETE FROM rules WHERE directive_type = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", directive_type);
   aimee_pg_step_t step_rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   if (step_rc != AIMEE_PG_DONE)
      return -1;
   if (changes > 0)
      kb_store_rules_cache_invalidate();
   return changes;
}

int kb_store_rules_update_directive_type(int id, const char *directive_type)
{
   void *conn = kb_store_conn();
   if (!conn || !directive_type)
      return -1;

   char err[RULES_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE rules SET directive_type = ?1 WHERE id = ?2", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", directive_type);
   aimee_pg_bind_int(st, "?2", id);
   aimee_pg_step_t step_rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (step_rc != AIMEE_PG_DONE)
      return -1;
   kb_store_rules_cache_invalidate();
   return 0;
}

static char g_rules_cache_hash[32];

void kb_store_rules_cache_invalidate(void)
{
   g_rules_cache_hash[0] = '\0';
}

#define DECAY_INTERVAL_SOFT 14
#define DECAY_INTERVAL_HARD 42
#define DECAY_AMOUNT        5
#define ARCHIVE_THRESHOLD   10
#define ARCHIVE_GRACE_DAYS  30

/* Run an UPDATE/DELETE that takes (decay_amount_or_threshold, days)
 * bound at ?1 and ?2. Returns the row count from aimee_pg_stmt_changes. */
static int rules_decay_step(void *conn, const char *sql, int amount, int days_signed_neg)
{
   char err[RULES_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   char interval[16];
   snprintf(interval, sizeof(interval), "-%d", days_signed_neg);
   aimee_pg_bind_int(st, "?1", amount);
   aimee_pg_bind_text(st, "?2", interval);
   (void)aimee_pg_step(st, err, sizeof(err));
   int n = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return n;
}

int kb_store_rules_decay(void)
{
   void *conn = kb_store_conn();
   if (!conn)
      return 0;

   int total = 0;

   /* Soft directives: gentle decay every DECAY_INTERVAL_SOFT days. */
   total += rules_decay_step(conn,
                             "UPDATE rules SET weight = GREATEST(weight - ?1, 0),"
                             " updated_at = pg_now_text()"
                             " WHERE (directive_type IS NULL OR directive_type != 'hard')"
                             " AND last_reinforced_at IS NOT NULL"
                             " AND last_reinforced_at < pg_now_text(?2 || ' days')",
                             DECAY_AMOUNT, DECAY_INTERVAL_SOFT);

   /* Hard directives: stickier; decay only after DECAY_INTERVAL_HARD days. */
   total += rules_decay_step(conn,
                             "UPDATE rules SET weight = GREATEST(weight - ?1, 0),"
                             " updated_at = pg_now_text()"
                             " WHERE directive_type = 'hard'"
                             " AND last_reinforced_at IS NOT NULL"
                             " AND last_reinforced_at < pg_now_text(?2 || ' days')",
                             DECAY_AMOUNT, DECAY_INTERVAL_HARD);

   total += rules_decay_step(conn,
                             "DELETE FROM rules WHERE weight < ?1"
                             " AND updated_at < pg_now_text(?2 || ' days')",
                             ARCHIVE_THRESHOLD, ARCHIVE_GRACE_DAYS);

   if (total > 0)
      kb_store_rules_cache_invalidate();

   return total;
}
