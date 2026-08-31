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

#include <libpq-fe.h>

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

static int exec_ok(PGconn *conn, const char *sql)
{
   PGresult *result = PQexec(conn, sql);
   int ok = result && PQresultStatus(result) == PGRES_COMMAND_OK;
   if (result)
      PQclear(result);
   return ok ? 0 : -1;
}

static int assert_role(PGconn *conn)
{
   PGresult *result =
       PQexecParams(conn,
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
                    1, NULL, (const char *[]){WORM_ROLE}, NULL, NULL, 0);
   int ok = result && PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) == 1 &&
            strcmp(PQgetvalue(result, 0, 0), "t") == 0;
   if (result)
      PQclear(result);
   return ok ? 0 : -1;
}

static int ack(PGconn *conn, const char *outbox_id, long long audit_seq)
{
   char seq[32];
   snprintf(seq, sizeof(seq), "%lld", audit_seq);
   PGresult *result = PQexecParams(conn, "SELECT aimee_kb_worm_api.ack($1::bigint,$2::bigint)", 2,
                                   NULL, (const char *[]){outbox_id, seq}, NULL, NULL, 0);
   int ok = result && PQresultStatus(result) == PGRES_TUPLES_OK;
   if (result)
      PQclear(result);
   return ok ? 0 : -1;
}

static int drain(PGconn *conn, int batch, int *count)
{
   *count = 0;
   if (exec_ok(conn, "BEGIN") != 0)
      return -1;

   char limit[16];
   snprintf(limit, sizeof(limit), "%d", batch);
   PGresult *rows = PQexecParams(conn, "SELECT * FROM aimee_kb_worm_api.claim($1::integer)", 1,
                                 NULL, (const char *[]){limit}, NULL, NULL, 0);
   if (!rows || PQresultStatus(rows) != PGRES_TUPLES_OK || PQnfields(rows) != 8)
      goto fail;

   int n = PQntuples(rows);
   for (int i = 0; i < n; ++i)
   {
      const char *outbox_id = PQgetvalue(rows, i, 0);
      char event_id[96];
      snprintf(event_id, sizeof(event_id), "kb:%s", outbox_id);
      long long seq = 0;
      if (audit_worm_append_idempotent(
              event_id, PQgetvalue(rows, i, 1), PQgetvalue(rows, i, 2), PQgetvalue(rows, i, 3),
              PQgetvalue(rows, i, 4), PQgetvalue(rows, i, 5), PQgetvalue(rows, i, 6),
              PQgetvalue(rows, i, 7), &seq) != 0 ||
          ack(conn, outbox_id, seq) != 0)
         goto fail;
   }
   PQclear(rows);
   rows = NULL;
   if (exec_ok(conn, "COMMIT") != 0)
      goto fail_no_rows;
   *count = n;
   return 0;

fail:
   if (rows)
      PQclear(rows);
fail_no_rows:
   (void)exec_ok(conn, "ROLLBACK");
   return -1;
}

static void clear_notifications(PGconn *conn)
{
   if (!PQconsumeInput(conn))
      return;
   PGnotify *notification;
   while ((notification = PQnotifies(conn)) != NULL)
      PQfreemem(notification);
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

   const char *configured = getenv("AIMEE_WORM_DB2_URL");
   if (!configured || !*configured)
   {
      fputs("aimee-kb-worm: AIMEE_WORM_DB2_URL is required; refusing runtime credential "
            "fallback\n",
            stderr);
      return 65;
   }
   char *db_url = strdup(configured);
   if (!db_url)
      return 70;
   (void)unsetenv("AIMEE_WORM_DB2_URL");

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

   PGconn *conn = PQconnectdb(db_url);
   memset(db_url, 0, strlen(db_url));
   free(db_url);
   if (!conn || PQstatus(conn) != CONNECTION_OK)
   {
      fputs("aimee-kb-worm: outbox connection failed\n", stderr);
      if (conn)
         PQfinish(conn);
      audit_worm_close();
      return 67;
   }
   if (assert_role(conn) != 0)
   {
      fprintf(stderr,
              "aimee-kb-worm: database principal must be isolated role %s "
              "with only the WORM claim/ack API\n",
              WORM_ROLE);
      PQfinish(conn);
      audit_worm_close();
      return 68;
   }
   char verify_err[256] = "";
   if (audit_worm_startup_verify(verify_err, sizeof(verify_err), NULL, NULL) != 0)
   {
      fprintf(stderr, "aimee-kb-worm: SQLite WORM verification failed: %s\n", verify_err);
      PQfinish(conn);
      audit_worm_close();
      return 71;
   }
   if (exec_ok(conn, "SET application_name='aimee-kb-worm'; LISTEN kb_audit_worm") != 0)
   {
      fputs("aimee-kb-worm: initialization failed\n", stderr);
      PQfinish(conn);
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
      struct pollfd fd = {.fd = PQsocket(conn), .events = POLLIN, .revents = 0};
      int wait_rc;
      do
         wait_rc = poll(&fd, 1, poll_ms);
      while (wait_rc < 0 && errno == EINTR && g_running);
      if (wait_rc > 0)
         clear_notifications(conn);
      else if (wait_rc < 0 && errno != EINTR)
      {
         fputs("aimee-kb-worm: notification wait failed\n", stderr);
         rc = 72;
         break;
      }
   }
   PQfinish(conn);
   audit_worm_close();
   return rc;
}
