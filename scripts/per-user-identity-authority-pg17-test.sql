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

-- ============================================================================
-- kb_management_identity_intent_start — the writer a login mode calls once it
-- has an authenticated subject. Everything above assumed an intent already
-- existed; this covers the only thing that may create one.
-- ============================================================================

-- Both fixture subjects are team members, so that what the assertions below
-- isolate really is the GRANT. The writer reads kb_write_tier_grant as the
-- logged-in principal under FORCE RLS, and that table's read policy is scoped to
-- the principal's teams — without a membership row every subject would refuse
-- for lack of visibility and the grant checks would prove nothing.
INSERT INTO kb_team_membership(identity_key, team)
  VALUES ('oidc:idp.test:ungranted', 930001),
         ('oidc:idp.test:nogrant', 930001);

-- Structural: the subject is NOT a parameter. This is the guarantee that a
-- login front-end cannot mint on behalf of anyone but whoever it authenticated
-- into the scope, so it is asserted on the signature rather than trusted to the
-- body. Ten arguments, none of them a subject.
DO $$
DECLARE args TEXT;
BEGIN
  SELECT pg_get_function_arguments(oid) INTO args FROM pg_proc
    WHERE proname='kb_management_identity_intent_start';
  IF args IS NULL THEN
    RAISE EXCEPTION 'FAIL: the identity intent writer does not exist';
  END IF;
  IF args LIKE '%subject%' THEN
    RAISE EXCEPTION 'FAIL: the identity intent writer takes a caller-supplied subject: %', args;
  END IF;
  IF NOT has_function_privilege('aimee_kb_runtime',
       'public.kb_management_identity_intent_start(TEXT,TEXT,TEXT,BIGINT,TEXT,TEXT,TEXT,TEXT,INTEGER,TEXT)',
       'EXECUTE') THEN
    RAISE EXCEPTION 'FAIL: runtime cannot execute the identity intent writer (no login can mint)';
  END IF;
END $$;

DO $$
DECLARE ok BOOLEAN; msg TEXT;
BEGIN
  -- No aimee.principal in scope: there is no authenticated subject to record an
  -- intent for, so the writer must refuse rather than invent one. Also compiles
  -- the body (%ROWTYPE and column references are not resolved at CREATE).
  PERFORM set_config('aimee.principal','',true);
  PERFORM set_config('aimee.team','',true);
  ok := false;
  BEGIN
    PERFORM * FROM kb_management_identity_intent_start(
      repeat('1',64), repeat('2',64), 'id-jti-00000001', 930001,
      'idsrv', 'oidc', 'kb', 'kid-test', 300, repeat('a',32));
  EXCEPTION
    WHEN sqlstate '42501' THEN ok := true;
    WHEN undefined_column OR undefined_table OR undefined_function OR syntax_error THEN
      RAISE EXCEPTION 'identity intent writer body failed to compile: %', SQLERRM;
  END;
  IF NOT ok THEN
    RAISE EXCEPTION 'FAIL: the identity intent writer ran with no authenticated principal';
  END IF;

  -- Malformed identifiers are rejected before any lookup.
  ok := false;
  BEGIN
    PERFORM * FROM kb_management_identity_intent_start(
      'nothex', repeat('2',64), 'id-jti-00000001', 930001,
      'idsrv', 'oidc', 'kb', 'kid-test', 300, repeat('a',32));
  EXCEPTION WHEN sqlstate '22023' THEN ok := true;
  END;
  IF NOT ok THEN
    RAISE EXCEPTION 'FAIL: the identity intent writer accepted a malformed correlation_id';
  END IF;

  -- A TTL the server's verifier would throw away must never be recorded.
  ok := false;
  BEGIN
    PERFORM * FROM kb_management_identity_intent_start(
      repeat('1',64), repeat('2',64), 'id-jti-00000001', 930001,
      'idsrv', 'oidc', 'kb', 'kid-test', 3601, repeat('a',32));
  EXCEPTION WHEN sqlstate '22023' THEN ok := true;
  END;
  IF NOT ok THEN
    RAISE EXCEPTION 'FAIL: the identity intent writer accepted an unverifiable TTL';
  END IF;

  -- Only the two declared auth modes exist; a third would silently widen the
  -- login surface.
  ok := false;
  BEGIN
    PERFORM * FROM kb_management_identity_intent_start(
      repeat('1',64), repeat('2',64), 'id-jti-00000001', 930001,
      'idsrv', 'ldap', 'kb', 'kid-test', 300, repeat('a',32));
  EXCEPTION WHEN sqlstate '22023' THEN ok := true;
  END;
  IF NOT ok THEN
    RAISE EXCEPTION 'FAIL: the identity intent writer accepted an undeclared auth mode';
  END IF;

  -- A principal whose scope names a different team than the intent does.
  PERFORM set_config('aimee.principal','oidc:idp.test:ungranted',true);
  PERFORM set_config('aimee.team','930002',true);
  ok := false;
  BEGIN
    PERFORM * FROM kb_management_identity_intent_start(
      repeat('1',64), repeat('2',64), 'id-jti-00000001', 930001,
      'idsrv', 'oidc', 'kb', 'kid-test', 300, repeat('a',32));
  EXCEPTION WHEN sqlstate '42501' THEN ok := true;
  END;
  IF NOT ok THEN
    RAISE EXCEPTION 'FAIL: the identity intent writer crossed team scopes';
  END IF;

  -- THE load-bearing case: an authenticated subject with no live grant cannot
  -- even file an intent, so the ungranted path never reaches the signing
  -- pipeline at all. 'oidc:idp.test:nogrant' has no kb_write_tier_grant row.
  PERFORM set_config('aimee.principal','oidc:idp.test:nogrant',true);
  PERFORM set_config('aimee.team','930001',true);
  ok := false;
  BEGIN
    PERFORM * FROM kb_management_identity_intent_start(
      repeat('1',64), repeat('2',64), 'id-jti-00000001', 930001,
      'idsrv', 'oidc', 'kb', 'kid-test', 300, repeat('a',32));
  EXCEPTION
    WHEN sqlstate '42501' THEN
      GET STACKED DIAGNOSTICS msg = MESSAGE_TEXT;
      IF msg NOT LIKE '%not granted%' THEN
        RAISE EXCEPTION 'FAIL: expected a grant refusal, got: %', msg;
      END IF;
      ok := true;
  END;
  IF NOT ok THEN
    RAISE EXCEPTION 'FAIL: an ungranted subject filed an identity intent';
  END IF;

  -- And the mirror of the snapshot assertion above: with a live grant the
  -- writer must get PAST the grant check, or the refusal above would pass even
  -- if the grant lookup were a blanket deny. This fixture has no management
  -- instance, so it still refuses (28000) — just not as ungranted.
  PERFORM set_config('aimee.principal','oidc:idp.test:ungranted',true);
  BEGIN
    PERFORM * FROM kb_management_identity_intent_start(
      repeat('1',64), repeat('2',64), 'id-jti-00000001', 930001,
      'idsrv', 'oidc', 'kb', 'kid-test', 300, repeat('a',32));
  EXCEPTION WHEN OTHERS THEN
    GET STACKED DIAGNOSTICS msg = MESSAGE_TEXT;
    IF msg LIKE '%not granted%' THEN
      RAISE EXCEPTION 'FAIL: a live grant still refused as ungranted (grant lookup is broken)';
    END IF;
  END;
  PERFORM set_config('aimee.principal','',true);
  PERFORM set_config('aimee.team','',true);
END $$;

-- A grant for a subject who is not a member of the team it names must not
-- authorize anything. The writer reads the grant as the principal under FORCE
-- RLS, so the membership scope is a second, independent barrier: a grant row
-- planted for a non-member is invisible and the intent is refused.
INSERT INTO kb_write_tier_grant(server_id, team_id, subject, tier, granted_by)
  VALUES ('idsrv', 930001, 'oidc:idp.test:outsider', 'full', 'owner');
DO $$
DECLARE ok BOOLEAN := false; msg TEXT;
BEGIN
  PERFORM set_config('aimee.principal','oidc:idp.test:outsider',true);
  PERFORM set_config('aimee.team','930001',true);
  BEGIN
    PERFORM * FROM kb_management_identity_intent_start(
      repeat('3',64), repeat('4',64), 'id-jti-00000002', 930001,
      'idsrv', 'oidc', 'kb', 'kid-test', 300, repeat('a',32));
  EXCEPTION
    WHEN sqlstate '42501' THEN
      GET STACKED DIAGNOSTICS msg = MESSAGE_TEXT;
      IF msg NOT LIKE '%not granted%' THEN
        RAISE EXCEPTION 'FAIL: expected a grant refusal for a non-member, got: %', msg;
      END IF;
      ok := true;
  END;
  IF NOT ok THEN
    RAISE EXCEPTION 'FAIL: a grant planted for a non-member authorized an identity intent';
  END IF;
  PERFORM set_config('aimee.principal','',true);
  PERFORM set_config('aimee.team','',true);
END $$;

-- The subject grammar is asserted against the shared corpus by
-- scripts/gen/subject-corpus.sql, which the gate runs next. It is generated from
-- src/tests/subject_corpus.h — the same list the two C validators are tested
-- against by src/tests/test_subject_grammar.c — and is self-contained, so it does
-- not depend on this file's fixture surviving the ROLLBACK below.

\echo '== per-user identity token authority assertions PASSED =='
ROLLBACK;
