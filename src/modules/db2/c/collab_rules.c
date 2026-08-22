/* db2/collab_rules.c: collaborative agent rules — Postgres via libpq. */

#include "collab_rules.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include "cJSON.h"
#include "dstr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CR_ERRBUF 256

static const char *status_str(collab_rule_status_t status)
{
   switch (status)
   {
   case COLLAB_PROPOSED:
      return "proposed";
   case COLLAB_ACTIVE:
      return "active";
   case COLLAB_REJECTED:
      return "rejected";
   case COLLAB_RETIRED:
      return "retired";
   }
   return "unknown";
}

static collab_rule_status_t status_from_str(const char *status)
{
   if (!status)
      return COLLAB_PROPOSED;
   if (strcmp(status, "active") == 0)
      return COLLAB_ACTIVE;
   if (strcmp(status, "rejected") == 0)
      return COLLAB_REJECTED;
   if (strcmp(status, "retired") == 0)
      return COLLAB_RETIRED;
   return COLLAB_PROPOSED;
}

static void row_to_collab_rule(aimee_pg_stmt_t *st, collab_rule_t *rule)
{
   memset(rule, 0, sizeof(*rule));
   rule->id = aimee_pg_column_int(st, 0);
   db2_copy_col_text(rule->text, sizeof(rule->text), st, 1);
   db2_copy_col_text(rule->reason, sizeof(rule->reason), st, 2);
   db2_copy_col_text(rule->proposed_by, sizeof(rule->proposed_by), st, 3);
   rule->status = status_from_str(aimee_pg_column_text(st, 4));
   db2_copy_col_text(rule->created_at, sizeof(rule->created_at), st, 5);
   db2_copy_col_text(rule->decided_at, sizeof(rule->decided_at), st, 6);
}

static int count_total(void *conn)
{
   char err[CR_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT COUNT(*) FROM collab_rules", err, sizeof(err));
   if (!st)
      return 0;
   int count = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      count = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return count;
}

static int increment_epoch(void *conn)
{
   char err[CR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO collab_rules_meta(key, value) VALUES('epoch', '1')"
       " ON CONFLICT(key) DO UPDATE SET value = CAST(CAST(collab_rules_meta.value AS INTEGER) + 1 "
       "AS TEXT)",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (rc != AIMEE_PG_DONE)
      return -1;
   return db2_collab_rules_epoch();
}

static cJSON *collab_rule_to_json(const collab_rule_t *rule)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddNumberToObject(obj, "id", rule->id);
   cJSON_AddStringToObject(obj, "text", rule->text);
   cJSON_AddStringToObject(obj, "reason", rule->reason);
   cJSON_AddStringToObject(obj, "proposed_by", rule->proposed_by);
   cJSON_AddStringToObject(obj, "status", status_str(rule->status));
   cJSON_AddStringToObject(obj, "created_at", rule->created_at);
   cJSON_AddStringToObject(obj, "decided_at", rule->decided_at);
   return obj;
}

int db2_collab_rules_epoch(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char err[CR_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT value FROM collab_rules_meta WHERE key = 'epoch'", err, sizeof(err));
   if (!st)
      return 0;
   int epoch = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      epoch = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return epoch;
}

int db2_collab_rules_list(collab_rule_t *out, int max)
{
   void *conn = db2_conn();
   if (!conn || !out || max <= 0)
      return 0;
   char err[CR_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT id, text, reason, proposed_by, status, created_at, decided_at"
                        " FROM collab_rules ORDER BY id ASC",
                        err, sizeof(err));
   if (!st)
      return 0;
   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      row_to_collab_rule(st, &out[count]);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_collab_rules_list_active(collab_rule_t *out, int max)
{
   void *conn = db2_conn();
   if (!conn || !out || max <= 0)
      return 0;
   char err[CR_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT id, text, reason, proposed_by, status, created_at, decided_at"
                        " FROM collab_rules WHERE status = 'active' ORDER BY id ASC",
                        err, sizeof(err));
   if (!st)
      return 0;
   int count = 0;
   while (count < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      row_to_collab_rule(st, &out[count]);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_collab_rules_propose(const char *text, const char *reason, const char *proposed_by)
{
   void *conn = db2_conn();
   if (!conn || !text || !text[0])
      return -1;
   if (count_total(conn) >= COLLAB_MAX_TOTAL_RULES)
      return -1;
   if (strlen(text) > COLLAB_RULE_TEXT_LEN)
      return -1;
   if (reason && strlen(reason) > COLLAB_RULE_REASON_LEN)
      return -1;

   char err[CR_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO collab_rules(text, reason, proposed_by, status)"
                        " VALUES(?1, ?2, ?3, 'proposed') RETURNING id",
                        err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", text);
   aimee_pg_bind_text(st, "?2", reason ? reason : "");
   aimee_pg_bind_text(st, "?3", proposed_by ? proposed_by : "");

   int id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return id;
}

/* Applies a status change that must match exactly one row and then advances the
 * epoch, as one transaction.
 *
 * Both have to land together. db2_collab_rules_inject returns nothing at all
 * when the agent's epoch equals the stored one, so a status change that commits
 * without its bump leaves a rule active and enforced that no agent is ever told
 * about -- and it does not resolve on its own, because only the next approve or
 * retire moves the epoch. Unwrapped, a failure between the two produced exactly
 * that.
 *
 * The row count is the outcome, not statement success: a transition that
 * matches nothing means the rule is absent or was in another status, and
 * bumping the epoch for that tells every agent to re-read a set that has not
 * moved. */
static int transition_and_bump(void *conn, const char *sql, int rule_id, int cap)
{
   char err[CR_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;

   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   aimee_pg_bind_int(st, "?1", rule_id);
   if (cap > 0)
      aimee_pg_bind_int(st, "?2", cap);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changed = (rc == AIMEE_PG_DONE) ? aimee_pg_stmt_changes(st) : 0;
   aimee_pg_finalize(st);

   if (changed != 1 || increment_epoch(conn) < 0)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      return -1;
   }
   return 0;
}

int db2_collab_rules_approve(int rule_id)
{
   void *conn = db2_conn();
   if (!conn || rule_id <= 0)
      return -1;
   /* The cap is inside the UPDATE rather than a count read before it. Reading
    * the count, comparing, and then writing is a check that has stopped being
    * true by the time it is acted on.
    *
    * It does not make concurrent approvals safe: at READ COMMITTED two
    * statements can each see one short of the cap and each admit one. Closing
    * that needs a lock or SERIALIZABLE, and this is strictly tighter than what
    * it replaces rather than a claim to be airtight. The caller cannot tell the
    * two refusals apart, and could not before either. */
   return transition_and_bump(
       conn,
       "UPDATE collab_rules SET status = 'active', decided_at = pg_now_text()"
       " WHERE id = ?1 AND status = 'proposed'"
       "   AND (SELECT COUNT(*) FROM collab_rules WHERE status = 'active') < ?2",
       rule_id, COLLAB_MAX_ACTIVE_RULES);
}

/* No epoch bump, and that is not an omission: a proposal was never in the
 * active set, so rejecting it changes nothing an agent is holding and a bump
 * would make every agent re-read a set that has not moved. */
int db2_collab_rules_reject(int rule_id)
{
   void *conn = db2_conn();
   if (!conn || rule_id <= 0)
      return -1;
   char err[CR_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "UPDATE collab_rules SET status = 'rejected', decided_at = pg_now_text()"
                        " WHERE id = ?1 AND status = 'proposed'",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", rule_id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changed = (rc == AIMEE_PG_DONE) ? aimee_pg_stmt_changes(st) : 0;
   aimee_pg_finalize(st);
   return changed > 0 ? 0 : -1;
}

int db2_collab_rules_retire(int rule_id)
{
   void *conn = db2_conn();
   if (!conn || rule_id <= 0)
      return -1;
   return transition_and_bump(
       conn,
       "UPDATE collab_rules SET status = 'retired', decided_at = pg_now_text()"
       " WHERE id = ?1 AND status = 'active'",
       rule_id, 0);
}

char *db2_collab_rules_inject(int agent_last_epoch)
{
   collab_rule_t rules[COLLAB_MAX_ACTIVE_RULES];
   int epoch = db2_collab_rules_epoch();
   int count;
   dstr_t out;

   if (!db2_conn())
      return NULL;
   if (agent_last_epoch >= 0 && agent_last_epoch == epoch)
      return NULL;

   count = db2_collab_rules_list_active(rules, COLLAB_MAX_ACTIVE_RULES);
   if (count <= 0)
      return NULL;

   dstr_init(&out);
   dstr_appendf(&out, "\n# Collaborative Rules (epoch %d)\n", epoch);
   for (int i = 0; i < count; i++)
      dstr_appendf(&out, "%d. %s\n", i + 1, rules[i].text);
   return dstr_steal(&out);
}

char *db2_collab_rules_json_all(void)
{
   if (!db2_conn())
      return NULL;
   collab_rule_t rules[COLLAB_MAX_TOTAL_RULES];
   int count = db2_collab_rules_list(rules, COLLAB_MAX_TOTAL_RULES);
   cJSON *arr = cJSON_CreateArray();

   for (int i = 0; i < count; i++)
      cJSON_AddItemToArray(arr, collab_rule_to_json(&rules[i]));

   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return json;
}

char *db2_collab_rules_json_active(void)
{
   if (!db2_conn())
      return NULL;
   collab_rule_t rules[COLLAB_MAX_ACTIVE_RULES];
   int count = db2_collab_rules_list_active(rules, COLLAB_MAX_ACTIVE_RULES);
   int epoch = db2_collab_rules_epoch();
   cJSON *obj = cJSON_CreateObject();
   cJSON *arr;

   cJSON_AddNumberToObject(obj, "epoch", epoch);
   arr = cJSON_AddArrayToObject(obj, "rules");
   for (int i = 0; i < count; i++)
      cJSON_AddItemToArray(arr, collab_rule_to_json(&rules[i]));

   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json;
}
