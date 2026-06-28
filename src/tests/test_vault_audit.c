/* test_vault_audit.c — pin the D2/D2c security control: a server-principal
 * credential write is recorded in the DEDICATED append-only 0600 audit sink
 * (audit.log), NOT the operator-readable general server log.
 *
 * This test fails if vault_audit_server_write() is ever reverted to
 * aimee_log(LOG_WARN, "vault.audit", ...): the general log does not write
 * audit.log, so the file would stay empty. It also asserts the two properties
 * the dedicated sink exists for — 0600 perms (access separation) — and that the
 * secret itself is never written (fingerprint only). */
#include "server.h" /* server_conn_t, vault_audit_server_write */
#include "log.h"    /* audit_log_open/close */
#include "config.h" /* config_default_dir */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* server_vault.o references these (other handlers in the same TU); the audit
 * path under test never calls them, so trivial stubs satisfy the linker. */
int server_send_error(server_conn_t *conn, const char *msg, const char *detail)
{
   (void)conn;
   (void)msg;
   (void)detail;
   return 0;
}
int server_send_response(server_conn_t *conn, cJSON *resp)
{
   (void)conn;
   (void)resp;
   return 0;
}

static char *slurp(const char *path, size_t *len_out)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   char *buf = malloc((size_t)n + 1);
   assert(buf);
   size_t got = fread(buf, 1, (size_t)n, f);
   buf[got] = '\0';
   fclose(f);
   if (len_out)
      *len_out = got;
   return buf;
}

int main(void)
{
   char home[256];
   snprintf(home, sizeof(home), "/tmp/aimee-vaultaudit-test-%d", (int)getpid());
   char cmd[640];
   snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", home, home);
   assert(system(cmd) == 0);
   setenv("AIMEE_HOME", home, 1);

   audit_log_open();

   /* A server-principal write over an attested UDS connection. */
   server_conn_t conn;
   memset(&conn, 0, sizeof(conn));
   conn.attested_transport = ATTEST_UDS_PEERCRED;
   snprintf(conn.vault_principal, sizeof(conn.vault_principal), "uid:1000");

   const char *secret = "sk-supersecret-must-never-be-logged-9f3a";
   vault_audit_server_write(&conn, "glm", "api_key", secret);

   audit_log_close();

   char path[320];
   snprintf(path, sizeof(path), "%s/audit.log", config_default_dir());

   /* (1) The dedicated sink received the record — fails if routed to aimee_log. */
   size_t len = 0;
   char *body = slurp(path, &len);
   assert(body && "audit.log not written — server-principal write was not routed to the dedicated sink");
   assert(strstr(body, "\"event\":\"VAULT_SERVER_WRITE\"") && "missing VAULT_SERVER_WRITE event");
   assert(strstr(body, "agent=glm") && "missing agent field");
   assert(strstr(body, "transport=uds") && "missing/incorrect attested transport");
   assert(strstr(body, "fp=") && "missing credential fingerprint");

   /* (2) The secret is NEVER written — only the fingerprint. */
   assert(!strstr(body, secret) && "the raw secret leaked into the audit log");

   /* (3) Access separation: the audit file is 0600. */
   struct stat st;
   assert(stat(path, &st) == 0);
   assert((st.st_mode & 0777) == 0600 && "audit.log must be 0600");

   free(body);
   snprintf(cmd, sizeof(cmd), "rm -rf %s", home);
   (void)system(cmd);

   printf("PASS: vault server-principal write routes to the dedicated 0600 audit sink (D2/D2c)\n");
   return 0;
}
