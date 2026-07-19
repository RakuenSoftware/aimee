/* db2/db2_tenant.h: per-request tenant context for the RLS-hardened tier (I4).
 *
 * The ONLY runtime path that sets the Postgres tenant GUCs. It opens a
 * transaction on the thread's leased connection, calls the SECURITY DEFINER
 * set_tenant_context(principal, team) (which validates team membership under the
 * principal's own-rows policy), and — on commit, rollback, error, OR connection
 * reset — RESETs aimee.principal / aimee.team before the connection returns to the
 * pool, so a pooled shared runtime role never leaks one request's tenant context
 * into the next. Every tenant GUC is transaction-local (SET LOCAL / set_config
 * with is_local=true), so correctness holds regardless of PgBouncer pooling mode.
 *
 * Tenant-scoped db2 entrypoints (kb_team_*, kb_project_*, kb_membership_*, ...)
 * must call db2_tenant_require_pg() first: RLS cannot be enforced on the SQLite
 * test shim, so a tenant op on the shim is a hard, typed failure — never a silent
 * unprotected read (B1/N1). */
#ifndef DEC_DB2_TENANT_H
#define DEC_DB2_TENANT_H 1

#include <stdint.h>
#include "kb_identity.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Typed tenancy return codes (negative). */
   enum
   {
      DB2_TENANT_OK = 0,
      DB2_ERR_TENANT_REQUIRES_PG = -100, /* tenant op attempted on the SQLite shim */
      DB2_ERR_TENANT_UNAUTHENTICATED = -101, /* principal not verifier-produced */
      DB2_ERR_TENANT_NO_CONN = -102,
      DB2_ERR_TENANT_BEGIN = -103,       /* BEGIN / set_tenant_context failed */
      DB2_ERR_TENANT_DENIED = -104,      /* team not in principal memberships */
   };

   /* Fail-closed guard: 0 when the live backend is Postgres (RLS-enforcing),
    * DB2_ERR_TENANT_REQUIRES_PG when it is the SQLite shim or DB2 is down. Called
    * at the top of every tenant-scoped db2 entrypoint. */
   int db2_tenant_require_pg(void);

   /* Open a tenant-scoped unit of work: leases a connection, BEGIN, and sets the
    * tenant context from the AUTHENTICATED principal (its identity_key) + team.
    * `team` may be 0 to set only the principal (bootstrap / identity resolution,
    * before a billing team is known). Rejects an unauthenticated principal
    * (DB2_ERR_TENANT_UNAUTHENTICATED) and a team the principal is not in
    * (DB2_ERR_TENANT_DENIED). Returns 0 on success; on any failure the context is
    * left clear and no transaction is held. Pairs with exactly one
    * db2_tenant_scope_commit() or db2_tenant_scope_rollback(). */
   int db2_tenant_scope_begin(const kb_principal_t *p, int64_t team);

   /* Commit / roll back the tenant-scoped unit. BOTH reset the tenant GUCs and
    * release the transaction before the connection returns to the pool. Safe to
    * call rollback on an already-aborted transaction. */
   int db2_tenant_scope_commit(void);
   void db2_tenant_scope_rollback(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_TENANT_H */
