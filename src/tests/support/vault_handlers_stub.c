/* vault_handlers_stub.c: stub the WP-C.1 vault route handlers for tests that link
 * server.o (whose dispatch table references handle_vault_*) but do not exercise
 * the vault — avoids pulling the crypto/store/config dependency chain into the
 * dispatch routing test. The real handlers are covered by unit-test-vault-* and
 * the live smoke. */
#include "server.h"
#include "cJSON.h"

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
