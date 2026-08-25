/* aimee-kb-worm: separately credentialed WORM audit consumer.
 *
 * Producers commit immutable kb_audit_outbox rows. This process is the only
 * online principal allowed to call aimee_kb_worm_api.drain(), which constructs the
 * hash chain and writes the delivery ledger atomically. It deliberately links
 * no KB request, mutation, provider, vault, or HTTP code. */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libpq-fe.h>

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

static int drain(PGconn *conn, int batch, int *count)
{
   char limit[16];
   snprintf(limit, sizeof(limit), "%d", batch);
   PGresult *result = PQexecParams(conn, "SELECT aimee_kb_worm_api.drain($1::integer)", 1, NULL,
                                   (const char *[]){limit}, NULL, NULL, 0);
   if (!result || PQresultStatus(result) != PGRES_TUPLES_OK || PQntuples(result) != 1 ||
       bounded_int(PQgetvalue(result, 0, 0), 0, batch, count) != 0)
   {
      if (result)
         PQclear(result);
      return -1;
   }
   PQclear(result);
   return 0;
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

   PGconn *conn = PQconnectdb(db_url);
   memset(db_url, 0, strlen(db_url));
   free(db_url);
   if (!conn || PQstatus(conn) != CONNECTION_OK)
   {
      fputs("aimee-kb-worm: database connection failed\n", stderr);
      if (conn)
         PQfinish(conn);
      return 66;
   }
   if (assert_role(conn) != 0)
   {
      fprintf(stderr,
              "aimee-kb-worm: database principal must be isolated role %s "
              "with only the WORM API schema\n",
              WORM_ROLE);
      PQfinish(conn);
      return 67;
   }
   if (exec_ok(conn, "SET application_name='aimee-kb-worm'; LISTEN kb_audit_worm") != 0)
   {
      fputs("aimee-kb-worm: initialization failed\n", stderr);
      PQfinish(conn);
      return 68;
   }

   (void)signal(SIGINT, stop_worker);
   (void)signal(SIGTERM, stop_worker);
   int rc = 0;
   while (g_running)
   {
      int count = 0;
      if (drain(conn, batch, &count) != 0)
      {
         fputs("aimee-kb-worm: drain failed\n", stderr);
         rc = 69;
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
         rc = 70;
         break;
      }
   }
   PQfinish(conn);
   return rc;
}
