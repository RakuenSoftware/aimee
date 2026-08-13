/* vault_handlers_stub.c: stub the WP-C.1 vault route handlers for tests that link
 * server.o (whose dispatch table references handle_vault_*) but do not exercise
 * the vault — avoids pulling the crypto/store/config dependency chain into the
 * dispatch routing test. The real handlers are covered by unit-test-vault-* and
 * the live smoke. */
#include "server.h"
#include "cJSON.h"
#include "runtime_secret.h"

/* server_api_status.o uses the production Vault facade. Keep this routing
 * test hermetic while preserving its process-cache semantics. */
int vault_runtime_secret_set(const char *name, const char *value)
{
   return runtime_secret_store(name, value);
}

int vault_runtime_secret_delete(const char *name)
{
   runtime_secret_remove(name);
   return 0;
}

/* server_provider.o asks the vault whether a provider has a credential filed
 * under its name. Answering 0 keeps this test on the env-var path it already
 * exercised, so provider dispatch is measured without dragging the vault store
 * and its crypto chain into a routing test. The real lookup is covered by
 * unit-test-vault-provider-credential. */
int vault_provider_has_credential(const char *provider_name)
{
   (void)provider_name;
   return 0;
}

static int vault_stub(server_conn_t *conn)
{
   return server_send_error(conn, "vault: not available in this test build", NULL);
}

int handle_vault_unlock(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   return vault_stub(conn);
}
int handle_vault_rekey(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   return vault_stub(conn);
}
int handle_vault_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   return vault_stub(conn);
}
int handle_vault_set_server(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   return vault_stub(conn);
}
int handle_vault_capability(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   return vault_stub(conn);
}
int handle_vault_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   return vault_stub(conn);
}
int handle_vault_delete(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   return vault_stub(conn);
}
int handle_vault_lock(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   return vault_stub(conn);
}

/* mTLS client-cert lifecycle handlers (slice 2b) — stubbed for the same reason
 * as the vault handlers: server.o's dispatch table references them, but the
 * routing test does not exercise the PKI/crypto chain. Real coverage lives in
 * unit-test-pki and the live smoke. */
int handle_cert_issue(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   return vault_stub(conn);
}
int handle_cert_sign(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)conn;
   (void)req;
   return 0;
}
int handle_cert_revoke(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   return vault_stub(conn);
}
int handle_cert_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   return vault_stub(conn);
}
