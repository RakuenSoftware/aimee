/* db2/db2_hardening.h: hardened-tier boot assertions (I1 verify-full, B4 runtime
 * role privilege floor).
 *
 * The hardened kb tier (multi-tenant, holds team data) MUST connect to Postgres
 * over verify-full TLS under a non-owner, NOBYPASSRLS, no-DDL runtime role — else
 * the team-scoped RLS is defeated. These checks are activated when the deployment
 * opts into the hardened tier (AIMEE_KB_HARDENED=1); the dev/personal profile
 * (single-owner, plaintext local Postgres, no tenant data) runs without them, the
 * same dev-vs-hardened split the vault custody profiles use. A hardened kb that
 * fails either check fails closed at boot rather than run with defeated isolation. */
#ifndef DEC_DB2_HARDENING_H
#define DEC_DB2_HARDENING_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* 1 when hardened-tier boot checks are enabled (AIMEE_KB_HARDENED truthy). */
   int db2_hardening_enabled(void);

   /* Pure DSN check (I1): 1 iff the libpq URL selects sslmode=verify-full. Rejects
    * a missing/weaker sslmode. Unit-testable without a live server. */
   int db2_hardening_dsn_verify_full(const char *libpq_url);

   /* Runtime-role privilege floor (B4): queries the connected role and writes 0 on
    * success, or a nonzero code with a message into err when the role has
    * BYPASSRLS, is a superuser, or holds CREATE on the public schema (a runtime
    * role must be DML-only). `conn` is a db2 pg connection handle. */
   int db2_hardening_assert_runtime_role(void *conn, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_HARDENING_H */
