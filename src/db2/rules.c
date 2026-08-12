/* db2/rules.c: agent rules — Postgres via libpq. */

#include "config.h"
#include "rules.h"
#include "db2_internal.h"
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
   db2_copy_col_text(r->polarity, sizeof(r->polarity), st, 1);
   db2_copy_col_text(r->title, sizeof(r->title), st, 2);
   db2_copy_col_text(r->description, sizeof(r->description), st, 3);
   r->weight = aimee_pg_column_int(st, 4);
   db2_copy_col_text(r->domain, sizeof(r->domain), st, 5);
   db2_copy_col_text(r->created_at, sizeof(r->created_at), st, 6);
   db2_copy_col_text(r->updated_at, sizeof(r->updated_at), st, 7);
   db2_copy_col_text(r->directive_type, sizeof(r->directive_type), st, 8);
}

int db2_rules_insert(const char *polarity, const char *title, const char *description, int weight)
{
   if (!title || !*title)
      return -1;
   void *conn = db2_conn();
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

int db2_rules_export_jsonl(const char *path)
{
   if (!path || !*path)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT polarity, title, description, weight,"
                            " domain, created_at, updated_at FROM rules ORDER BY id";
   char err[RULES_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;

   FILE *f = fopen(path, "w");
   if (!f)
   {
      aimee_pg_finalize(st);
      return -1;
   }

   int count = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      cJSON *obj = cJSON_CreateObject();
      const char *polarity = aimee_pg_column_text(st, 0);
      const char *title = aimee_pg_column_text(st, 1);
      const char *description = aimee_pg_column_text(st, 2);
      const char *domain = aimee_pg_column_text(st, 4);
      const char *created_at = aimee_pg_column_text(st, 5);
      const char *updated_at = aimee_pg_column_text(st, 6);
      cJSON_AddStringToObject(obj, "polarity", polarity ? polarity : "");
      cJSON_AddStringToObject(obj, "title", title ? title : "");
      cJSON_AddStringToObject(obj, "description", description ? description : "");
      cJSON_AddNumberToObject(obj, "weight", aimee_pg_column_int(st, 3));
      cJSON_AddStringToObject(obj, "domain", domain ? domain : "");
      cJSON_AddStringToObject(obj, "created_at", created_at ? created_at : "");
      cJSON_AddStringToObject(obj, "updated_at", updated_at ? updated_at : "");

      char *line = cJSON_PrintUnformatted(obj);
      cJSON_Delete(obj);
      if (line)
      {
         fprintf(f, "%s\n", line);
         free(line);
         count++;
      }
   }
   fclose(f);
   aimee_pg_finalize(st);
   return count;
}

int db2_rules_list(rule_t *out, int max_rules)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return 0;

   char err[RULES_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT " RULES_SELECT_COLS " FROM rules ORDER BY weight DESC, title ASC", err,
       sizeof(err));
   if (!st)
      return 0;

   int count = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < max_rules)
      row_to_rule(st, &out[count++]);
   aimee_pg_finalize(st);
   return count;
}

int db2_rules_list_by_tier(int min_weight, rule_t *out, int max_rules)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return 0;

   char err[RULES_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT " RULES_SELECT_COLS " FROM rules "
                                          "WHERE weight >= ?1 ORDER BY weight DESC, title ASC",
                                          err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", min_weight);

   int count = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < max_rules)
      row_to_rule(st, &out[count++]);
   aimee_pg_finalize(st);
   return count;
}

int db2_rules_list_hard(rule_t *out, int max_rules)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return 0;

   char err[RULES_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT " RULES_SELECT_COLS " FROM rules "
                                          "WHERE directive_type = 'hard'"
                                          " ORDER BY weight DESC, title ASC",
                                          err, sizeof(err));
   if (!st)
      return 0;

   int count = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < max_rules)
      row_to_rule(st, &out[count++]);
   aimee_pg_finalize(st);
   return count;
}

int db2_rules_get(int id, rule_t *out)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return -1;

   char err[RULES_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT " RULES_SELECT_COLS " FROM rules WHERE id = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", id);

   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      row_to_rule(st, out);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_rules_find_by_title(const char *title, rule_t *out)
{
   void *conn = db2_conn();
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

int db2_rules_delete(int id)
{
   void *conn = db2_conn();
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
      db2_rules_cache_invalidate();
   return changes > 0 ? 0 : -1;
}

int db2_rules_delete_by_directive_type(const char *directive_type)
{
   void *conn = db2_conn();
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
      db2_rules_cache_invalidate();
   return changes;
}

int db2_rules_update_weight(int id, int weight)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   char err[RULES_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE rules SET weight = ?1, updated_at = ?2 WHERE id = ?3", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", weight);
   aimee_pg_bind_text(st, "?2", ts);
   aimee_pg_bind_int(st, "?3", id);
   aimee_pg_step_t step_rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   if (step_rc != AIMEE_PG_DONE)
      return -1;
   if (changes > 0)
      db2_rules_cache_invalidate();
   return changes > 0 ? 0 : -1;
}

int db2_rules_update_directive_type(int id, const char *directive_type)
{
   void *conn = db2_conn();
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
   db2_rules_cache_invalidate();
   return 0;
}

int db2_rules_reinforce_directive(int id, const char *directive_type, int weight_override)
{
   void *conn = db2_conn();
   if (!conn || !directive_type)
      return -1;

   const char *sql_with_weight =
       "UPDATE rules SET directive_type = ?1, weight = ?2, updated_at = pg_now_text(),"
       " last_reinforced_at = pg_now_text() WHERE id = ?3";
   const char *sql_no_weight = "UPDATE rules SET directive_type = ?1, updated_at = pg_now_text(),"
                               " last_reinforced_at = pg_now_text() WHERE id = ?2";
   const char *sql = (weight_override >= 0) ? sql_with_weight : sql_no_weight;

   char err[RULES_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", directive_type);
   if (weight_override >= 0)
   {
      aimee_pg_bind_int(st, "?2", weight_override);
      aimee_pg_bind_int(st, "?3", id);
   }
   else
   {
      aimee_pg_bind_int(st, "?2", id);
   }
   aimee_pg_step_t step_rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (step_rc != AIMEE_PG_DONE)
      return -1;
   db2_rules_cache_invalidate();
   return 0;
}

const char *db2_rules_tier(int weight)
{
   if (weight >= 75)
      return "Rule";
   if (weight >= 50)
      return "Inclination";
   return "Archived";
}

char db2_rules_polarity_symbol(const char *polarity)
{
   if (!polarity)
      return '~';
   if (strcmp(polarity, "positive") == 0)
      return '+';
   if (strcmp(polarity, "negative") == 0)
      return '-';
   return '~';
}

static char g_rules_cache_hash[32];
static char *g_rules_cache_output;

void db2_rules_cache_invalidate(void)
{
   g_rules_cache_hash[0] = '\0';
}

void db2_rules_signature(char *buf, size_t len)
{
   if (!buf || len == 0)
      return;
   buf[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return;
   char err[RULES_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT COUNT(*), MAX(updated_at) FROM rules", err, sizeof(err));
   if (!st)
      return;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      int cnt = aimee_pg_column_int(st, 0);
      const char *ts = aimee_pg_column_text(st, 1);
      snprintf(buf, len, "%d:%s", cnt, ts ? ts : "");
   }
   aimee_pg_finalize(st);
}

char *db2_rules_generate(void)
{
   void *conn = db2_conn();
   if (!conn)
      return NULL;

   if (!config_cache_disabled() && g_rules_cache_hash[0])
   {
      char cur[32];
      db2_rules_signature(cur, sizeof(cur));
      if (strcmp(cur, g_rules_cache_hash) == 0 && g_rules_cache_output)
         return strdup(g_rules_cache_output);
   }

   rule_t rules[128];
   int count = db2_rules_list(rules, 128);

   size_t cap = MAX_SESSION_CHARS + 256;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;

   int pos = snprintf(buf, cap, "# Rules\n\n");
   int emitted = 0;

   int tiers[] = {75, 50, 0};
   int tier_max[] = {100, 74, 49};

   for (int t = 0; t < 3 && emitted < MAX_SESSION_RULES; t++)
   {
      for (int i = 0; i < count && emitted < MAX_SESSION_RULES; i++)
      {
         int w = rules[i].weight;
         if (w < tiers[t] || w > tier_max[t])
            continue;

         char sym = db2_rules_polarity_symbol(rules[i].polarity);
         const char *text = rules[i].description;
         if (strlen(text) == 0)
            text = rules[i].title;

         char truncated[MAX_RULE_TEXT_LEN + 4];
         if (strlen(text) > MAX_RULE_TEXT_LEN)
         {
            memcpy(truncated, text, MAX_RULE_TEXT_LEN);
            memcpy(truncated + MAX_RULE_TEXT_LEN, "...", 4);
            text = truncated;
         }

         const char *prefix = "";
         if (rules[i].directive_type[0] == 'h')
            prefix = "MUST: ";
         else if (rules[i].directive_type[0] == 's' && rules[i].directive_type[1] == 'o')
            prefix = "SHOULD: ";

         char line[512];
         int llen = snprintf(line, sizeof(line), "- (%c %d) %s%s\n", sym, w, prefix, text);

         if (pos + llen >= (int)MAX_SESSION_CHARS)
            break;

         memcpy(buf + pos, line, llen);
         pos += llen;
         emitted++;
      }
   }

   buf[pos] = '\0';

   if (!config_cache_disabled())
   {
      free(g_rules_cache_output);
      g_rules_cache_output = strdup(buf);
      db2_rules_signature(g_rules_cache_hash, sizeof(g_rules_cache_hash));
   }

   return buf;
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

int db2_rules_decay(void)
{
   void *conn = db2_conn();
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
      db2_rules_cache_invalidate();

   return total;
}
