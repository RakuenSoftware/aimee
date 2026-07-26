-- per-user-identity-authority-pg17-test.sql: assertions for the data-plane
-- identity token authority (proposal per-user-remote-writes-authz.md §4).
--
-- Run against a Postgres that has had schema_roles.sql + schema.sql +
-- schema_grants.sql applied. Every assertion aborts with an ERROR (non-zero psql
-- exit), so this is a hard gate, never a skip.
--
-- Why this file exists: CREATE FUNCTION only syntax-checks a plpgsql body. It
-- does NOT resolve %ROWTYPE declarations or column references, so a schema load
-- that "applies clean" proves almost nothing about these functions. Invoking
-- them forces compilation and catches a mistyped column or a renamed table that
-- would otherwise surface the first time someone tries to mint a token.

\set ON_ERROR_STOP on
BEGIN;

DO $$
DECLARE c TEXT := repeat('a',64); j TEXT := repeat('b',64); ok BOOLEAN := false;
BEGIN
  -- Absent intent: compile the body AND prove the fail-closed guard fires.
  BEGIN
    PERFORM * FROM kb_management_identity_authority_snapshot(c, j);
  EXCEPTION
    WHEN sqlstate 'P0002' THEN ok := true;   -- expected: intent unavailable
    WHEN undefined_column OR undefined_table OR undefined_function OR syntax_error THEN
      RAISE EXCEPTION 'identity snapshot body failed to compile: %', SQLERRM;
  END;
  IF NOT ok THEN
    RAISE EXCEPTION 'FAIL: identity snapshot did not fail closed on an absent intent';
  END IF;

  ok := false;
  BEGIN
    PERFORM * FROM kb_management_identity_authority_admit(c, j);
  EXCEPTION
    WHEN sqlstate 'P0002' THEN ok := true;
    WHEN undefined_column OR undefined_table OR undefined_function OR syntax_error THEN
      RAISE EXCEPTION 'identity admit body failed to compile: %', SQLERRM;
  END;
  IF NOT ok THEN
    RAISE EXCEPTION 'FAIL: identity admit did not fail closed on an absent intent';
  END IF;

  -- Malformed identifiers are rejected before any lookup happens.
  ok := false;
  BEGIN
    PERFORM * FROM kb_management_identity_authority_snapshot('nothex', j);
  EXCEPTION WHEN sqlstate '22023' THEN ok := true;
  END;
  IF NOT ok THEN
    RAISE EXCEPTION 'FAIL: identity snapshot accepted a malformed correlation_id';
  END IF;

  -- readback must not raise on an absent key-use intent: it exists to resolve a
  -- lost COMMIT without private-key use, so "nothing admitted" is a normal
  -- answer (zero rows), not an error.
  BEGIN
    PERFORM * FROM kb_management_identity_authority_readback(c, j);
  EXCEPTION
    WHEN undefined_column OR undefined_table OR undefined_function OR syntax_error THEN
      RAISE EXCEPTION 'identity readback body failed to compile: %', SQLERRM;
    WHEN OTHERS THEN
      RAISE EXCEPTION 'FAIL: identity readback raised on an absent intent: %', SQLERRM;
  END;

  -- use and finalize demand REPEATABLE READ. This test transaction is READ
  -- COMMITTED, so both must refuse with 25001 — which also compiles them.
  ok := false;
  BEGIN
    PERFORM * FROM kb_management_identity_authority_use(c, j);
  EXCEPTION
    WHEN sqlstate '25001' THEN ok := true;
    WHEN undefined_column OR undefined_table OR undefined_function OR syntax_error THEN
      RAISE EXCEPTION 'identity use body failed to compile: %', SQLERRM;
  END;
  IF NOT ok THEN
    RAISE EXCEPTION 'FAIL: identity use ran outside REPEATABLE READ';
  END IF;

  ok := false;
  BEGIN
    PERFORM kb_management_identity_authority_finalize(c, j);
  EXCEPTION
    WHEN sqlstate '25001' THEN ok := true;
    WHEN undefined_column OR undefined_table OR undefined_function OR syntax_error THEN
      RAISE EXCEPTION 'identity finalize body failed to compile: %', SQLERRM;
  END;
  IF NOT ok THEN
    RAISE EXCEPTION 'FAIL: identity finalize ran outside REPEATABLE READ';
  END IF;
END $$;

-- Structural: the identity intent and its key-use record are authority-owned and
-- immutable, exactly like the management and read intents they sit beside.
DO $$
BEGIN
  IF (SELECT relforcerowsecurity FROM pg_class
        WHERE relname='kb_management_identity_intent') IS NOT TRUE OR
     (SELECT relforcerowsecurity FROM pg_class
        WHERE relname='kb_management_identity_key_use_intent') IS NOT TRUE THEN
    RAISE EXCEPTION 'FAIL: an identity authority table is not FORCE ROW LEVEL SECURITY';
  END IF;
  IF has_table_privilege('aimee_kb_runtime','public.kb_management_identity_intent','SELECT') OR
     has_table_privilege('aimee_kb_runtime',
       'public.kb_management_identity_key_use_intent','SELECT') THEN
    RAISE EXCEPTION 'FAIL: ordinary runtime can read identity authority state';
  END IF;
  -- 'identity' must be an accepted intent kind, or no identity token can be filed.
  IF NOT EXISTS (SELECT 1 FROM pg_constraint
                  WHERE conname='kb_management_token_intent_namespace_kind_check'
                    AND pg_get_constraintdef(oid) LIKE '%identity%') THEN
    RAISE EXCEPTION 'FAIL: the intent namespace does not accept the identity kind';
  END IF;
END $$;

-- NO GRANT MEANS DENY, asserted where it is actually enforced.
--
-- This is the load-bearing claim of the whole feature: after the hard cutover a
-- subject with no live kb_write_tier_grant row must not be able to obtain a
-- token. Everything above only proves the fail-closed paths for a MISSING
-- intent; this builds a real, well-formed identity intent and shows the mint
-- still refuses, for the specific reason that the subject is not granted.
INSERT INTO kb_team(id, name) VALUES (930001, 'idauth_team');
INSERT INTO kb_enrollments(id, scope, fingerprint, state, authority_id)
  VALUES (930101, 'p5-kb-management', repeat('c',64), 'active', repeat('1',32)),
         (930102, 'p5-server-management', repeat('d',64), 'active', repeat('2',32));
INSERT INTO kb_server_registry(server_id, cert_cn, mgmt_cert_cn, team_id, endpoint, status)
  VALUES ('idsrv', 'idauth-cn', 'idauth-mgmt-cn', 930001, 'https://idsrv', 'active');
INSERT INTO kb_management_token_intent_namespace(correlation_id, jti, kind)
  VALUES (repeat('e',64), repeat('f',64), 'identity');
INSERT INTO kb_management_identity_intent(
  correlation_id, jti, kind, token_jti, team_id, subject, auth_mode, target_server_id,
  token_issuer, audience, kid, issued_at, expires_at, installation_id,
  installation_generation, installation_enrollment_id, target_enrollment_id,
  revocation_generation, state)
VALUES (repeat('e',64), repeat('f',64), 'identity', 'id-jti-00000042', 930001,
        'oidc:idp.test:ungranted', 'oidc', 'idsrv', 'kb', 'idsrv', 'kid-test',
        1000, 1300, repeat('a',32), 1, 930101, 930102, 1, 'pending');

DO $$
DECLARE ok BOOLEAN := false; msg TEXT;
BEGIN
  BEGIN
    PERFORM * FROM kb_management_identity_authority_snapshot(repeat('e',64), repeat('f',64));
    RAISE EXCEPTION 'FAIL: the mint accepted a subject with no write-tier grant';
  EXCEPTION
    WHEN sqlstate '42501' THEN
      ok := true;  -- expected: "subject no longer granted"
    WHEN OTHERS THEN
      GET STACKED DIAGNOSTICS msg = MESSAGE_TEXT;
      -- Any other refusal would mean the snapshot stopped for an unrelated
      -- reason and this assertion proved nothing about grants.
      RAISE EXCEPTION 'FAIL: expected a grant refusal (42501), got: %', msg;
  END;
  IF NOT ok THEN
    RAISE EXCEPTION 'FAIL: no-grant refusal did not fire';
  END IF;
END $$;

-- With a live grant the snapshot gets PAST the grant check. It still refuses
-- (this fixture has no JWKS publication or token root), but the refusal must no
-- longer be the grant one — otherwise the check above would pass even if the
-- grant lookup were broken and always denied.
INSERT INTO kb_write_tier_grant(server_id, team_id, subject, tier, granted_by)
  VALUES ('idsrv', 930001, 'oidc:idp.test:ungranted', 'data', 'owner');
DO $$
DECLARE msg TEXT;
BEGIN
  BEGIN
    PERFORM * FROM kb_management_identity_authority_snapshot(repeat('e',64), repeat('f',64));
  EXCEPTION WHEN OTHERS THEN
    GET STACKED DIAGNOSTICS msg = MESSAGE_TEXT;
    IF msg LIKE '%no longer granted%' THEN
      RAISE EXCEPTION 'FAIL: a live grant still refused as ungranted (grant lookup is broken)';
    END IF;
  END;
END $$;

\echo '== per-user identity token authority assertions PASSED =='
ROLLBACK;
