-- per-user-write-tier-rls-test.sql: DB-layer assertions for kb_write_tier_grant,
-- the authoritative {subject -> tier} map that replaces the process-global
-- aimee.api.remote_writes (proposal per-user-remote-writes-authz.md §6).
--
-- Run against a Postgres that has had schema_roles.sql + schema.sql +
-- schema_grants.sql applied. Every assertion aborts with an ERROR (non-zero psql
-- exit), so this is a hard gate, never a skip.
--
--   psql -v ON_ERROR_STOP=1 -d <db> -f scripts/per-user-write-tier-rls-test.sql
--
-- Like p1_rls_isolation_test.sql this uses SET ROLE aimee_kb_runtime so a single
-- superuser session sees exactly the runtime role's RLS view, including FORCE
-- ROW LEVEL SECURITY. What this file proves that the C layer cannot: that a
-- compromised or buggy runtime cannot read another team's grants, cannot award
-- itself a tier, and cannot erase the evidence of a grant.

\set ON_ERROR_STOP on

BEGIN;

INSERT INTO kb_team(id, name) VALUES
  (910001, 'wt_alpha'), (910002, 'wt_beta');
-- alice: plain member of alpha.  carol: team lead of alpha.  bob: beta only.
INSERT INTO kb_team_membership(identity_key, team, is_default) VALUES
  ('oidc:test:alice', 910001, 1),
  ('oidc:test:carol', 910001, 1),
  ('oidc:test:bob',   910002, 1);
INSERT INTO kb_team_lead(identity_key, team) VALUES ('oidc:test:carol', 910001);
INSERT INTO kb_server_registry(server_id, cert_cn, mgmt_cert_cn, team_id, endpoint, status)
  VALUES ('wt-srv-alpha','wt-cn-alpha','wt-mgmt-alpha',910001,'https://alpha','active'),
         ('wt-srv-beta', 'wt-cn-beta', 'wt-mgmt-beta', 910002,'https://beta', 'active');
-- One live grant per team, owner-written (as the bootstrap operator would).
INSERT INTO kb_write_tier_grant(server_id, team_id, subject, tier, granted_by) VALUES
  ('wt-srv-alpha', 910001, 'oidc:test:alice', 'data', 'owner'),
  ('wt-srv-beta',  910002, 'oidc:test:bob',   'full', 'owner');

-- Structural: the table must be FORCE RLS, and the runtime role must not be able
-- to erase a grant (revocation is a state change, not a delete).
DO $$
BEGIN
  IF (SELECT relforcerowsecurity FROM pg_class WHERE relname='kb_write_tier_grant') IS NOT TRUE THEN
    RAISE EXCEPTION 'FAIL: kb_write_tier_grant is not FORCE ROW LEVEL SECURITY';
  END IF;
  IF has_table_privilege('aimee_kb_runtime','public.kb_write_tier_grant','DELETE') THEN
    RAISE EXCEPTION 'FAIL: runtime can DELETE a write-tier grant (must revoke, not erase)';
  END IF;
  -- The definer functions are the only write path, so that a grant change can
  -- never happen without its WORM audit row. Direct DML must be unavailable.
  IF has_table_privilege('aimee_kb_runtime','public.kb_write_tier_grant','INSERT') OR
     has_table_privilege('aimee_kb_runtime','public.kb_write_tier_grant','UPDATE') THEN
    RAISE EXCEPTION 'FAIL: runtime holds direct DML on kb_write_tier_grant (bypasses the audit)';
  END IF;
  IF NOT has_table_privilege('aimee_kb_runtime','public.kb_write_tier_grant','SELECT') THEN
    RAISE EXCEPTION 'FAIL: runtime cannot read kb_write_tier_grant';
  END IF;
  -- Runtime must NOT be able to forge audit rows directly.
  IF has_function_privilege('aimee_kb_runtime',
       'public.kb_audit_worm_append(text,text,text,text,text,text)','EXECUTE') THEN
    RAISE EXCEPTION 'FAIL: runtime can call kb_audit_worm_append directly';
  END IF;
  IF NOT has_function_privilege('aimee_kb_runtime',
       'public.kb_write_tier_grant_set(text,bigint,text,text,text)','EXECUTE') OR
     NOT has_function_privilege('aimee_kb_runtime',
       'public.kb_write_tier_grant_revoke(text,bigint,text)','EXECUTE') THEN
    RAISE EXCEPTION 'FAIL: runtime cannot call the write-tier definer functions';
  END IF;
END $$;

-- Fail-closed, asserted FIRST: a runtime connection that has never installed a
-- tenant context sees no grants at all.  This must precede every
-- set_tenant_context below — the tenant GUC is transaction-local and survives
-- RESET ROLE, so a later "no context" check would silently inherit whatever
-- principal ran before it and prove nothing.
SET ROLE aimee_kb_runtime;
DO $$
DECLARE n INT;
BEGIN
  SELECT count(*) INTO n FROM kb_write_tier_grant;
  IF n <> 0 THEN
    RAISE EXCEPTION 'FAIL: % grants visible without a tenant context', n;
  END IF;
END $$;
RESET ROLE;

-- Cross-team read isolation: alice sees alpha's grant and nothing of beta's.
SET ROLE aimee_kb_runtime;
SELECT set_tenant_context('oidc:test:alice', 910001);
DO $$
DECLARE n INT;
BEGIN
  SELECT count(*) INTO n FROM kb_write_tier_grant WHERE revoked_at IS NULL;
  IF n <> 1 THEN
    RAISE EXCEPTION 'FAIL: alice sees % live write-tier grants, expected exactly alpha''s 1', n;
  END IF;
  IF NOT EXISTS(SELECT 1 FROM kb_write_tier_grant WHERE team_id=910001) THEN
    RAISE EXCEPTION 'FAIL: alice cannot see her own team''s grant';
  END IF;
  IF EXISTS(SELECT 1 FROM kb_write_tier_grant WHERE team_id=910002) THEN
    RAISE EXCEPTION 'FAIL: alice can read beta''s write-tier grants';
  END IF;
END $$;

-- A plain member cannot award themselves (or anyone) a tier.
DO $$
BEGIN
  BEGIN
    PERFORM kb_write_tier_grant_set('wt-srv-alpha', 910001, 'oidc:test:mallory', 'full',
                                    'oidc:test:alice');
    RAISE EXCEPTION 'FAIL: a plain team member minted a write-tier grant';
  EXCEPTION WHEN insufficient_privilege THEN
    NULL; -- expected: the definer's admin/lead check refused
  END;
END $$;
RESET ROLE;

-- A plain member cannot escalate an existing grant either.
SET ROLE aimee_kb_runtime;
SELECT set_tenant_context('oidc:test:alice', 910001);
DO $$
BEGIN
  BEGIN
    PERFORM kb_write_tier_grant_set('wt-srv-alpha', 910001, 'oidc:test:alice', 'full', 'self');
    RAISE EXCEPTION 'FAIL: a plain team member escalated their own tier';
  EXCEPTION WHEN insufficient_privilege THEN
    NULL;
  END;
END $$;
RESET ROLE;

-- The team lead may administer grants within their own team, and every change
-- lands a WORM audit row in the same transaction.
SET ROLE aimee_kb_runtime;
SELECT set_tenant_context('oidc:test:carol', 910001);
DO $$
DECLARE t TEXT; before_n INT; after_n INT;
BEGIN
  SELECT count(*) INTO before_n FROM kb_audit_event WHERE action='authz.write_tier.set';
  PERFORM kb_write_tier_grant_set('wt-srv-alpha', 910001, 'oidc:test:dave', 'data',
                                  'oidc:test:carol');
  SELECT tier INTO t FROM kb_write_tier_grant WHERE subject='oidc:test:dave';
  IF t <> 'data' THEN
    RAISE EXCEPTION 'FAIL: team lead grant did not land (tier=%)', t;
  END IF;
  SELECT count(*) INTO after_n FROM kb_audit_event WHERE action='authz.write_tier.set';
  IF after_n <> before_n + 1 THEN
    RAISE EXCEPTION 'FAIL: granting a write tier left no WORM audit row';
  END IF;

  -- Revocation is the removal path, it preserves the row, and it audits.
  PERFORM kb_write_tier_grant_revoke('wt-srv-alpha', 910001, 'oidc:test:dave');
  IF NOT EXISTS(SELECT 1 FROM kb_write_tier_grant
                 WHERE subject='oidc:test:dave' AND revoked_at IS NOT NULL) THEN
    RAISE EXCEPTION 'FAIL: revocation did not retain the grant row';
  END IF;
  IF NOT EXISTS(SELECT 1 FROM kb_audit_event WHERE action='authz.write_tier.revoke') THEN
    RAISE EXCEPTION 'FAIL: revoking a write tier left no WORM audit row';
  END IF;
END $$;
RESET ROLE;

-- A lead's authority stops at their own team boundary.
SET ROLE aimee_kb_runtime;
SELECT set_tenant_context('oidc:test:carol', 910001);
DO $$
BEGIN
  BEGIN
    PERFORM kb_write_tier_grant_set('wt-srv-beta', 910002, 'oidc:test:mallory', 'full',
                                    'oidc:test:carol');
    RAISE EXCEPTION 'FAIL: alpha''s team lead wrote a grant scoped to beta';
  EXCEPTION WHEN insufficient_privilege THEN
    NULL; -- expected: the definer checks lead-ship against the TARGET team
  END;
END $$;
RESET ROLE;

\echo '== per-user write-tier grant RLS assertions PASSED =='
ROLLBACK;
