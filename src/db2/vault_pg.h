#ifndef DEC_VAULT_PG_H
#define DEC_VAULT_PG_H 1

/* db2/vault_pg.h: the Postgres storage backend for the kb credential vault (P10
 * slice 2). Implements the vault_store_backend_t vtable over DB2 (db2_conn() +
 * aimee_pg_*), storing ONLY ciphertext in org_vault_secret / org_vault_current /
 * org_vault_salt. The envelope crypto is byte-identical to the jsonfile backend
 * (same vault_crypto); only the persistence differs (Postgres rows vs a JSON file),
 * which is the P10 anti-drift property.
 *
 * kb-only: this source joins the KB/DB2 link, NEVER SERVER_SRCS. aimee-kb binds it
 * at startup with vault_store_set_backend(&vault_pg_backend) after db2_init.
 *
 * AAD = "principal|agent|cred|version" (binds the ciphertext to its identity slot
 * AND version, so a restored older-version ciphertext cannot be presented as
 * current). The server-autonomous dual-wrap ops (set_dual/set_server/get_server/
 * add_server_wraps) are unsupported on this backend — they encode the server's
 * "server can read a user credential without the user unlocking" model, which the
 * single-KEK org vault does not use. They log and return -1. */

#include "vault_internal.h" /* vault_store_backend_t (-Imodules/vault) */

/* The exported backend. Bound by kb_main via vault_store_set_backend(). */
extern const vault_store_backend_t vault_pg_backend;

#endif /* DEC_VAULT_PG_H */
