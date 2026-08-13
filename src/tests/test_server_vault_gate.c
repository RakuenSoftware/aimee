/* test_server_vault_gate.c: the transport gate on /v1/vault/list, driven through
 * handle_vault_list itself rather than through the predicate it calls.
 *
 * This level matters. The predicates were fully covered while the HANDLER was
 * wired to the wrong one, which silently regressed the browser GUI's credential
 * page for every webuser: runtime-web reaches /v1/vault/list over the root-owned
 * webchat hop, and runtime-web's own tests MOCK that endpoint, so they exercise
 * the relay and never the gate. Nothing in either suite could see it.
 *
 * The assertions below are deliberately about WHICH gate the handler consults,
 * not about the vault's contents: a refusal is identified by its message, and an
 * admitted caller is identified by the ABSENCE of one. That keeps the test free of
 * a real credential store — an admitted caller may still fail downstream on an
 * uninitialised store, which is not what is under test here. */
#include "cJSON.h"
#include "server.h"
#include "vault_capability.h"
#include "vault_principal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static char g_message[1024];
static int g_was_error;

/* server_send_ok is a static inline over server_send_response, so this one stub
 * captures both outcomes. */
int server_send_response(server_conn_t *conn, cJSON *response)
{
   (void)conn;
   g_was_error = 0;
   g_message[0] = '\0';
   cJSON *err = response ? cJSON_GetObjectItemCaseSensitive(response, "error") : NULL;
   if (cJSON_IsString(err))
   {
      g_was_error = 1;
      snprintf(g_message, sizeof(g_message), "%s", err->valuestring);
   }
   return 0;
}

int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   (void)conn;
   (void)request_id;
   g_was_error = 1;
   snprintf(g_message, sizeof(g_message), "%s", message ? message : "");
   return 0;
}

/* The enumeration audit sink. Stubbed rather than linked: it drags the audit
 * stack in, and what is under test here is which callers reach the store at all.
 * Counted so the ordering stays honest — a refused caller must not have been
 * audited as having enumerated anything. */
static int g_audited;
void vault_audit_bridge_server_list(const char *principal, int count)
{
   (void)principal;
   (void)count;
   g_audited++;
}

/* True iff the handler refused at its own gate, as opposed to getting past it.
 * Both refusals name themselves; nothing downstream produces these strings. */
static int refused_by_gate(void)
{
   return g_was_error && (strstr(g_message, "vault:write:server") != NULL ||
                          strstr(g_message, "requires an attested connection") != NULL);
}

static int list_refused(attested_transport_t transport, const char *principal)
{
   server_conn_t conn;
   memset(&conn, 0, sizeof(conn));
   conn.attested_transport = transport;
   snprintf(conn.vault_principal, sizeof(conn.vault_principal), "%s", principal ? principal : "");
   g_was_error = 0;
   g_message[0] = '\0';
   cJSON *req = cJSON_CreateObject();
   assert(req != NULL);
   (void)handle_vault_list(NULL, &conn, req);
   cJSON_Delete(req);
   return refused_by_gate();
}

int main(void)
{
   char path[256];
   snprintf(path, sizeof path, "%s/aimee-vault-gate-XXXXXX", platform_tmpdir());
   int fd = mkstemp(path);
   assert(fd >= 0);
   close(fd);
   vault_capability_set_path_for_test(path);

   /* Host-local authority enumerates with NO grant. The webchat case is the one
    * whose absence broke the GUI: an ordinary logged-in webuser holds no grant,
    * and gating this like a write meant an operator had to mint one per user,
    * over UDS, before the credential page would render at all. */
   assert(!list_refused(ATTEST_WEBCHAT_TRUSTED, "webuser:alice"));
   assert(!list_refused(ATTEST_WEBCHAT_TRUSTED, ""));
   assert(!list_refused(ATTEST_UDS_PEERCRED, "uid:1000"));
   assert(!list_refused(ATTEST_UDS_PEERCRED, "uid:0"));
   assert(!list_refused(ATTEST_TLS_BEARER, ""));

   /* A client cert is a network credential on arbitrary remote machines. It is
    * what reaches /v1, not what opens the credential store — so it enumerates
    * only with an explicit grant. This is the `curl --cert` case. */
   assert(list_refused(ATTEST_MTLS_CLIENT, "cert:thin-abc"));
   assert(vault_capability_grant("cert:thin-abc") == 0);
   assert(!list_refused(ATTEST_MTLS_CLIENT, "cert:thin-abc"));
   assert(vault_capability_revoke("cert:thin-abc") == 0);
   assert(list_refused(ATTEST_MTLS_CLIENT, "cert:thin-abc"));

   /* An unsanitizable CN keeps the MTLS classification with NO principal, so no
    * grant can ever name it: refused, and it must not inherit the bearer's
    * admission. */
   assert(list_refused(ATTEST_MTLS_CLIENT, ""));

   /* D2b: never over an unencrypted channel, however well-granted. */
   assert(vault_capability_grant("webuser:alice") == 0);
   assert(list_refused(ATTEST_TCP_BEARER, "webuser:alice"));
   assert(list_refused(ATTEST_NONE, "webuser:alice"));

   /* The plaintext refusal is the transport one, not the capability one — the
    * message must not tell an operator to grant their way out of D2b. */
   (void)list_refused(ATTEST_TCP_BEARER, "webuser:alice");
   assert(strstr(g_message, "requires an attested connection") != NULL);
   assert(strstr(g_message, "vault:write:server") == NULL);

   /* The gate runs BEFORE the store is touched. A refused caller must not appear
    * in the enumeration audit trail as having listed anything — an ordering the
    * predicate tests cannot observe, since they never reach the handler. */
   g_audited = 0;
   assert(list_refused(ATTEST_MTLS_CLIENT, "cert:never-granted"));
   assert(list_refused(ATTEST_TCP_BEARER, "webuser:alice"));
   assert(g_audited == 0);

   vault_capability_set_path_for_test(NULL);
   unlink(path);
   printf("PASS: server_vault_gate list gate per transport (webchat/UDS/TLS admit, "
          "mTLS needs a grant, plaintext refused)\n");
   return 0;
}
