/* db2/entity_profiles.c: entity-profile cards — Postgres via libpq. */

#include "entity_profiles.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stddef.h>
#include <stdio.h>
#include <time.h>

#define EP_ERRBUF 256

static void now_utc_iso(char *buf, size_t len)
{
   time_t t = time(NULL);
   struct tm gmt;
   gmtime_r(&t, &gmt);
   strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &gmt);
}

int db2_entity_profile_upsert(const char *entity_id, const char *canonical_name,
                              int observation_count, const char *card_json)
{
   if (!entity_id || !*entity_id || !card_json)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char ts[32];
   now_utc_iso(ts, sizeof(ts));

   static const char *sql =
       "INSERT INTO entity_profiles (entity_id, canonical_name, observation_count, card_json,"
       " last_refreshed, created_at)"
       " VALUES (?1, ?2, ?3, ?4, ?5, ?6)"
       " ON CONFLICT(entity_id) DO UPDATE SET"
       "  observation_count = excluded.observation_count,"
       "  card_json = excluded.card_json,"
       "  last_refreshed = excluded.last_refreshed";
   char err[EP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", entity_id);
   aimee_pg_bind_text(st, "?2", canonical_name ? canonical_name : entity_id);
   aimee_pg_bind_int(st, "?3", observation_count);
   aimee_pg_bind_text(st, "?4", card_json);
   aimee_pg_bind_text(st, "?5", ts);
   aimee_pg_bind_text(st, "?6", ts);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int db2_entity_profile_is_fresh(const char *entity_id, const char *cutoff_modifier)
{
   if (!entity_id || !cutoff_modifier)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT 1 FROM entity_profiles"
                            " WHERE entity_id = ?1 AND last_refreshed > pg_now_text(?2)";
   char err[EP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", entity_id);
   aimee_pg_bind_text(st, "?2", cutoff_modifier);
   int fresh = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW) ? 1 : 0;
   aimee_pg_finalize(st);
   return fresh;
}

int db2_entity_profile_get_card(const char *entity_id, char *out_json, size_t out_len)
{
   if (!entity_id || !*entity_id || !out_json || out_len == 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT card_json FROM entity_profiles"
                            " WHERE LOWER(entity_id) = LOWER(?1)";
   char err[EP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", entity_id);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *json = aimee_pg_column_text(st, 0);
      if (json && json[0])
      {
         snprintf(out_json, out_len, "%s", json);
         rc = 0;
      }
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_entity_count_observations(const char *entity_id)
{
   if (!entity_id || !*entity_id)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT COUNT(DISTINCT memory_id) FROM memory_entities"
                            " WHERE LOWER(entity) = LOWER(?1)";
   char err[EP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", entity_id);
   int n = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return n;
}

int db2_entity_list_active(int min_obs, char (*names_out)[128], int *obs_out, int max)
{
   if (!names_out || !obs_out || max <= 0 || min_obs < 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT LOWER(entity), COUNT(DISTINCT memory_id) AS obs"
                            "  FROM memory_entities"
                            " GROUP BY LOWER(entity)"
                            " HAVING COUNT(DISTINCT memory_id) >= ?1";
   char err[EP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", min_obs);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *e = aimee_pg_column_text(st, 0);
      if (!e || !e[0])
         continue;
      snprintf(names_out[n], 128, "%s", e);
      obs_out[n] = aimee_pg_column_int(st, 1);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}
