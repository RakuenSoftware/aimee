/* aimee-kb-worm: separately credentialed SQLite WORM audit consumer.
 *
 * Producers commit immutable PostgreSQL outbox rows. This process can only
 * claim those rows and acknowledge their shared-audit_worm SQLite sequence; it
 * cannot reach KB application tables or construct a second chain format. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aimee/postgres/client.h>

#include "aimee_home.h"
#include <aimee/audit/audit_worm.h>

#define WORM_ROLE "aimee_kb_worm_worker"

static volatile sig_atomic_t g_running = 1;

static void stop_worker(int sig)
{
   (void)sig;
   g_running = 0;
}

static int bounded_int(const char *text, int min, int max, int *out)
{
   if (!text || !*text || !out)
      return -1;
   char *end = NULL;
   errno = 0;
   long value = strtol(text, &end, 10);
   if (errno || !end || *end || value < min || value > max)
      return -1;
   *out = (int)value;
   return 0;
}

static int exec_ok(aimee_postgres_session_t *session, const char *sql)
{
   return aimee_postgres_session_exec(session, sql, NULL, 0, NULL, NULL, NULL, 0);
}
static aimee_postgres_result_t *query(aimee_postgres_session_t *session, const char *sql,
                                      size_t count, const char *const *values)
{
   if (count > 2)
      return NULL;
   aimee_postgres_value_t args[2] = {0};
   for (size_t i = 0; i < count; i++)
      if (values[i])
      {
         args[i].kind = AIMEE_POSTGRES_TEXT;
         args[i].data = values[i];
         args[i].length = strlen(values[i]);
      }
   return aimee_postgres_session_query(session, sql, args, count, NULL, NULL, 0);
}
static const char *value(const aimee_postgres_result_t *result, int row, int column)
{
   const char *text = aimee_postgres_result_cell(result, (size_t)row, (size_t)column, NULL);
   return text ? text : "";
}
static int assert_role(aimee_postgres_session_t *conn)
{
   aimee_postgres_result_t *result =
       query(conn,
             "SELECT current_user=$1 AND rolcanlogin AND NOT rolinherit "
             "AND NOT rolsuper AND NOT rolbypassrls AND NOT rolcreatedb "
             "AND NOT rolcreaterole AND NOT rolreplication "
             "AND NOT has_schema_privilege(current_user,'public','USAGE') "
             "AND has_schema_privilege(current_user,'aimee_kb_worm_api','USAGE') "
             "AND has_function_privilege(current_user,"
             " 'aimee_kb_worm_api.claim(integer)','EXECUTE') "
             "AND has_function_privilege(current_user,"
             " 'aimee_kb_worm_api.ack(bigint,bigint)','EXECUTE') "
             "AND pg_try_advisory_lock(5752444001::bigint) "
             "AND NOT EXISTS (SELECT 1 FROM pg_auth_members "
             " WHERE roleid=(SELECT oid FROM pg_roles WHERE rolname=current_user) "
             "    OR member=(SELECT oid FROM pg_roles WHERE rolname=current_user)) "
             "FROM pg_roles WHERE rolname=current_user",
             1, (const char *[]){WORM_ROLE});
   int ok =
       result && aimee_postgres_result_rows(result) == 1 && strcmp(value(result, 0, 0), "t") == 0;
   if (result)
      aimee_postgres_result_free(result);
   return ok ? 0 : -1;
}

static int ack(aimee_postgres_session_t *conn, const char *outbox_id, long long audit_seq)
{
   char seq[32];
   snprintf(seq, sizeof(seq), "%lld", audit_seq);
   aimee_postgres_result_t *result =
       query(conn, "SELECT aimee_kb_worm_api.ack($1::bigint,$2::bigint)", 2,
             (const char *[]){outbox_id, seq});
   int ok = result != NULL;
   if (result)
      aimee_postgres_result_free(result);
   return ok ? 0 : -1;
}

static int drain(aimee_postgres_session_t *conn, int batch, int *count)
{
   *count = 0;
   if (exec_ok(conn, "BEGIN") != 0)
      return -1;

   char limit[16];
   snprintf(limit, sizeof(limit), "%d", batch);
   aimee_postgres_result_t *rows = query(conn, "SELECT * FROM aimee_kb_worm_api.claim($1::integer)",
                                         1, (const char *[]){limit});
   if (!rows || aimee_postgres_result_columns(rows) != 8)
      goto fail;

   int n = aimee_postgres_result_rows(rows);
   for (int i = 0; i < n; ++i)
   {
      const char *outbox_id = value(rows, i, 0);
      char event_id[96];
      snprintf(event_id, sizeof(event_id), "kb:%s", outbox_id);
      long long seq = 0;
      if (audit_worm_append_idempotent(event_id, value(rows, i, 1), value(rows, i, 2),
                                       value(rows, i, 3), value(rows, i, 4), value(rows, i, 5),
                                       value(rows, i, 6), value(rows, i, 7), &seq) != 0 ||
          ack(conn, outbox_id, seq) != 0)
         goto fail;
   }
   aimee_postgres_result_free(rows);
   rows = NULL;
   if (exec_ok(conn, "COMMIT") != 0)
      goto fail_no_rows;
   *count = n;
   return 0;

fail:
   if (rows)
      aimee_postgres_result_free(rows);
fail_no_rows:
   (void)exec_ok(conn, "ROLLBACK");
   return -1;
}

int main(int argc, char **argv)
{
   int once = 0, batch = 128, poll_ms = 1000;
   for (int i = 1; i < argc; ++i)
   {
      if (strcmp(argv[i], "--once") == 0)
         once = 1;
      else if (strncmp(argv[i], "--batch=", 8) == 0 &&
               bounded_int(argv[i] + 8, 1, 1000, &batch) == 0)
         ;
      else if (strncmp(argv[i], "--poll-ms=", 10) == 0 &&
               bounded_int(argv[i] + 10, 10, 60000, &poll_ms) == 0)
         ;
      else
      {
         fputs("usage: aimee-kb-worm [--once] [--batch=1..1000] "
               "[--poll-ms=10..60000]\n",
               stderr);
         return 64;
      }
   }

   const char *configured = getenv("AIMEE_WORM_POSTGRES_URL");
   if (!configured || !*configured)
   {
      fputs("aimee-kb-worm: AIMEE_WORM_POSTGRES_URL is required; refusing runtime credential "
            "fallback\n",
            stderr);
      return 65;
   }
   char *db_url = strdup(configured);
   if (!db_url)
      return 70;
   (void)unsetenv("AIMEE_WORM_POSTGRES_URL");

   char default_path[1024];
   const char *worm_path = getenv("AIMEE_WORM_PATH");
   if (!worm_path || !worm_path[0])
   {
      const char *home = aimee_home();
      if (!home)
      {
         fputs("aimee-kb-worm: AIMEE_HOME or HOME is required\n", stderr);
         memset(db_url, 0, strlen(db_url));
         free(db_url);
         return 66;
      }
      snprintf(default_path, sizeof(default_path), "%s/audit/kb-worm-live.db", home);
      worm_path = default_path;
   }
   if (audit_worm_init_at(worm_path) != 0)
   {
      fputs("aimee-kb-worm: SQLite WORM initialization failed\n", stderr);
      memset(db_url, 0, strlen(db_url));
      free(db_url);
      return 66;
   }

   aimee_postgres_session_t *conn = aimee_postgres_session_open_local(db_url, NULL, 0);
   memset(db_url, 0, strlen(db_url));
   free(db_url);
   if (!conn)
   {
      fputs("aimee-kb-worm: outbox connection failed\n", stderr);
      if (conn)
         aimee_postgres_session_close(conn);
      audit_worm_close();
      return 67;
   }
   if (assert_role(conn) != 0)
   {
      fprintf(stderr,
              "aimee-kb-worm: database principal must be isolated role %s "
              "with only the WORM claim/ack API\n",
              WORM_ROLE);
      aimee_postgres_session_close(conn);
      audit_worm_close();
      return 68;
   }
   char verify_err[256] = "";
   if (audit_worm_startup_verify(verify_err, sizeof(verify_err), NULL, NULL) != 0)
   {
      fprintf(stderr, "aimee-kb-worm: SQLite WORM verification failed: %s\n", verify_err);
      aimee_postgres_session_close(conn);
      audit_worm_close();
      return 71;
   }
   if (exec_ok(conn, "SET application_name='aimee-kb-worm'; LISTEN kb_audit_worm") != 0)
   {
      fputs("aimee-kb-worm: initialization failed\n", stderr);
      aimee_postgres_session_close(conn);
      audit_worm_close();
      return 69;
   }

   (void)signal(SIGINT, stop_worker);
   (void)signal(SIGTERM, stop_worker);
   int rc = 0;
   while (g_running)
   {
      int count = 0;
      if (drain(conn, batch, &count) != 0)
      {
         fputs("aimee-kb-worm: SQLite delivery failed\n", stderr);
         rc = 70;
         break;
      }
      if (count > 0 && audit_worm_checkpoint() != 0)
      {
         fputs("aimee-kb-worm: checkpoint failed\n", stderr);
         rc = 71;
         break;
      }
      if (once)
         break;
      if (count == batch)
         continue;
      int wait_rc = 0, remaining = poll_ms;
      while (g_running && remaining > 0 && wait_rc == 0)
      {
         unsigned slice = (unsigned)(remaining > 1000 ? 1000 : remaining);
         wait_rc = aimee_postgres_session_wait(conn, slice);
         remaining -= (int)slice;
      }
      if (wait_rc < 0)
      {
         fputs("aimee-kb-worm: notification wait failed\n", stderr);
         rc = 72;
         break;
      }
   }
   aimee_postgres_session_close(conn);
   audit_worm_close();
   return rc;
}
