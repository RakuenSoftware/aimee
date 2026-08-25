-- p6c_bedrock_catalog_test.sql: the mandatory DB-layer Bedrock catalog-routing +
-- fail-closed adapter-registry validation gate (P6c-catalog). Run against a Postgres
-- that has had schema_roles.sql + schema.sql + schema_grants.sql applied. Proves, at the
-- DB layer under the non-owner NOBYPASSRLS runtime role:
--   (a) a valid bedrock foundation model (converse, anthropic, aws, us-east-1) upserts via
--       org_catalog_bedrock_upsert and reads back with provider='bedrock' + the fields;
--   (b) (invoke, meta-llama) -> REJECTED (no native adapter), NO row written;
--   (c) (converse, 'typo-family') -> REJECTED (registry is fail-closed, not accept-any);
--   (d) a cross-region-inference-profile with empty underlying_fm_arns -> REJECTED;
--   (e) an underlying_fm_arns element that is a wildcard/cross-service ARN -> REJECTED;
--   (f) an invalid partition / invalid target_type -> REJECTED;
--   (g) aws_account='abc' on a provisioned target -> REJECTED;
--   (h) the PLAIN org_catalog_upsert with provider='bedrock' -> REJECTED (bypass closed);
--   (i) a non-bedrock provider via org_catalog_upsert still works (bedrock_* stay NULL);
--   (j) admin-only (a non-admin principal RAISEs 42501) + a WORM kb_audit_outbox row is
--       appended on a successful bedrock upsert;
--   (k) the runtime role gets permission denied on a direct SELECT of a bedrock_* column
--       (the new columns are NOT tenant-readable — same posture P2a proved).
-- Any failed assertion aborts with an ERROR (non-zero psql exit): a hard CI gate.
--
--   psql -v ON_ERROR_STOP=1 -d <db> -f scripts/p6c_bedrock_catalog_test.sql
--
-- Uses SET ROLE aimee_kb_runtime so a single superuser session exercises the runtime
-- role's RLS/grant view. The whole test is one transaction (so set_tenant_context's
-- transaction-local principal GUC persists across statements) and is rolled back.

\set ON_ERROR_STOP on

BEGIN;

-- Seed as the owner principal. The catalog definer functions run as the (superuser)
-- owner and bypass RLS for their internal writes, exactly like P2a/P3a/P10.
SELECT set_config('aimee.principal', 'owner', true);

INSERT INTO kb_team(id, name) VALUES (960001, 'p6c_alpha');
INSERT INTO kb_team(id, name) VALUES (960002, 'p6c_beta');
INSERT INTO kb_team_membership(identity_key, team, is_default)
  VALUES ('oidc:test:p6c_member_a', 960001, 1);
INSERT INTO kb_team_membership(identity_key, team, is_default)
  VALUES ('oidc:test:p6c_member_a', 960002, 0);

-- (a) A valid bedrock foundation model (converse, anthropic) upserts and reads back with
-- provider='bedrock', wire='bedrock', and the routing tuple intact.
SELECT org_catalog_bedrock_upsert(
  'p6c-claude-foundation', 'Claude on Bedrock', 'converse', 'anthropic', 'foundation',
  'aws', 'us-east-1', NULL, ARRAY['us-east-1']::text[], NULL, '', true);

DO $$
DECLARE r org_model_catalog%ROWTYPE;
BEGIN
  SELECT * INTO r FROM org_model_catalog WHERE model_id = 'p6c-claude-foundation';
  IF r.model_id IS NULL THEN
    RAISE EXCEPTION 'P6c FAIL: valid bedrock foundation model was not written';
  END IF;
  IF r.provider <> 'bedrock' OR r.wire <> 'bedrock' THEN
    RAISE EXCEPTION 'P6c FAIL: bedrock row provider/wire = %/% (want bedrock/bedrock)',
      r.provider, r.wire;
  END IF;
  IF r.bedrock_api <> 'converse' OR r.model_family <> 'anthropic'
     OR r.bedrock_target_type <> 'foundation' OR r.aws_partition <> 'aws' THEN
    RAISE EXCEPTION 'P6c FAIL: bedrock routing tuple not persisted (api=% family=% type=% part=%)',
      r.bedrock_api, r.model_family, r.bedrock_target_type, r.aws_partition;
  END IF;
  IF r.aws_invoke_region <> 'us-east-1' OR array_length(r.aws_region_set, 1) <> 1 THEN
    RAISE EXCEPTION 'P6c FAIL: aws_region_set not persisted (len=%)',
      array_length(r.aws_region_set, 1);
  END IF;
  IF r.aws_account IS NOT NULL OR r.underlying_fm_arns IS NOT NULL THEN
    RAISE EXCEPTION 'P6c FAIL: foundation target should carry no account/underlying ARNs';
  END IF;
END $$;

-- Existing rows are never inferred/backfilled from array order: a migration-null tuple
-- remains unavailable until an admin re-upserts it through the new signature.
INSERT INTO org_model_catalog(model_id, display_name, provider, wire, endpoint, enabled,
  bedrock_api, model_family, bedrock_target_type, aws_partition, aws_account,
  aws_region_set, underlying_fm_arns, aws_invoke_region)
VALUES ('p6c-legacy-null', 'legacy', 'bedrock', 'bedrock', '', true, 'converse', 'anthropic',
  'foundation', 'aws', NULL, ARRAY['us-east-1'], NULL, NULL);

-- Adapter-registry truth table (the pure predicate directly).
DO $$
BEGIN
  IF NOT org_bedrock_adapter_supported('converse', 'cohere') THEN
    RAISE EXCEPTION 'P6c FAIL: (converse, cohere) should be supported';
  END IF;
  IF NOT org_bedrock_adapter_supported('invoke', 'anthropic') THEN
    RAISE EXCEPTION 'P6c FAIL: (invoke, anthropic) should be supported';
  END IF;
  IF org_bedrock_adapter_supported('invoke', 'meta-llama') THEN
    RAISE EXCEPTION 'P6c FAIL: (invoke, meta-llama) must NOT be supported';
  END IF;
  IF org_bedrock_adapter_supported('converse', 'typo-family') THEN
    RAISE EXCEPTION 'P6c FAIL: (converse, typo-family) must NOT be supported (fail-closed)';
  END IF;
  IF org_bedrock_adapter_supported('converse', NULL) THEN
    RAISE EXCEPTION 'P6c FAIL: (converse, NULL) must NOT be supported (no fail-open on NULL)';
  END IF;
  IF org_bedrock_adapter_supported(NULL, 'anthropic') THEN
    RAISE EXCEPTION 'P6c FAIL: (NULL, anthropic) must NOT be supported';
  END IF;
END $$;

SELECT org_catalog_bedrock_upsert('p6c-disabled', 'Disabled', 'converse', 'anthropic',
  'foundation', 'aws', 'us-east-1', NULL, ARRAY['us-east-1'], NULL, '', false);
SELECT org_catalog_bedrock_upsert('p6c-native-invoke', 'Invoke', 'invoke', 'anthropic',
  'foundation', 'aws', 'us-east-1', NULL, ARRAY['us-east-1'], NULL, '', true);

-- A small helper: assert that a bedrock upsert RAISEs (fail-closed) and writes no row.
-- Each case runs in its own BEGIN/EXCEPTION block; the SAVEPOINT is unnecessary because
-- a RAISE inside the definer rolls back only the definer's INSERT (the whole call fails).

-- (b) (invoke, meta-llama) -> REJECTED (unsupported adapter), no row.
DO $$
BEGIN
  BEGIN
    PERFORM org_catalog_bedrock_upsert('p6c-reject-b', 'x', 'invoke', 'meta-llama',
      'foundation', 'aws', 'us-east-1', NULL, ARRAY['us-east-1']::text[], NULL, '', true);
    RAISE EXCEPTION 'P6c FAIL: (invoke, meta-llama) was accepted';
  EXCEPTION WHEN data_exception THEN NULL;  -- expected 22023
  END;
  IF EXISTS (SELECT 1 FROM org_model_catalog WHERE model_id = 'p6c-reject-b') THEN
    RAISE EXCEPTION 'P6c FAIL: a row was written despite the rejected adapter';
  END IF;
END $$;

-- (c) (converse, 'typo-family') -> REJECTED (registry fail-closed).
DO $$
BEGIN
  BEGIN
    PERFORM org_catalog_bedrock_upsert('p6c-reject-c', 'x', 'converse', 'typo-family',
      'foundation', 'aws', 'us-east-1', NULL, ARRAY['us-east-1']::text[], NULL, '', true);
    RAISE EXCEPTION 'P6c FAIL: (converse, typo-family) was accepted';
  EXCEPTION WHEN data_exception THEN NULL;
  END;
  IF EXISTS (SELECT 1 FROM org_model_catalog WHERE model_id = 'p6c-reject-c') THEN
    RAISE EXCEPTION 'P6c FAIL: a row was written despite the unknown family';
  END IF;
END $$;

-- (d) A cross-region-inference-profile with empty underlying_fm_arns -> REJECTED.
DO $$
BEGIN
  BEGIN
    PERFORM org_catalog_bedrock_upsert('p6c-reject-d', 'x', 'converse', 'anthropic',
      'cross-region-inference-profile', 'aws', 'us-east-1', '123456789012',
      ARRAY['us-east-1']::text[], ARRAY[]::text[], '', true);
    RAISE EXCEPTION 'P6c FAIL: profile target with empty underlying_fm_arns was accepted';
  EXCEPTION WHEN data_exception THEN NULL;
  END;
  IF EXISTS (SELECT 1 FROM org_model_catalog WHERE model_id = 'p6c-reject-d') THEN
    RAISE EXCEPTION 'P6c FAIL: a row was written despite empty underlying_fm_arns';
  END IF;
END $$;

-- (e) An underlying_fm_arns element that is a wildcard / cross-service ARN -> REJECTED.
DO $$
BEGIN
  BEGIN
    PERFORM org_catalog_bedrock_upsert('p6c-reject-e', 'x', 'converse', 'anthropic',
      'application-inference-profile', 'aws', 'us-east-1', '123456789012',
      ARRAY['us-east-1']::text[],
      ARRAY['arn:aws:bedrock:us-east-1::foundation-model/anthropic.claude-3',
            'arn:aws:s3:us-east-1::foundation-model/nope']::text[], '', true);
    RAISE EXCEPTION 'P6c FAIL: a cross-service underlying ARN was accepted';
  EXCEPTION WHEN data_exception THEN NULL;
  END;
  IF EXISTS (SELECT 1 FROM org_model_catalog WHERE model_id = 'p6c-reject-e') THEN
    RAISE EXCEPTION 'P6c FAIL: a row was written despite a cross-service underlying ARN';
  END IF;
END $$;

-- A wildcard '*' in the ARN is rejected by the charset check before the shape check.
DO $$
BEGIN
  BEGIN
    PERFORM org_catalog_bedrock_upsert('p6c-reject-e2', 'x', 'converse', 'anthropic',
      'application-inference-profile', 'aws', 'us-east-1', '123456789012',
      ARRAY['us-east-1']::text[],
      ARRAY['arn:aws:bedrock:us-east-1::foundation-model/*']::text[], '', true);
    RAISE EXCEPTION 'P6c FAIL: a wildcard underlying ARN was accepted';
  EXCEPTION WHEN data_exception THEN NULL;
  END;
END $$;

-- (f) Invalid partition 'gcp' and invalid target_type -> REJECTED.
DO $$
BEGIN
  BEGIN
    PERFORM org_catalog_bedrock_upsert('p6c-reject-f1', 'x', 'converse', 'anthropic',
      'foundation', 'gcp', 'us-east-1', NULL, ARRAY['us-east-1']::text[], NULL, '', true);
    RAISE EXCEPTION 'P6c FAIL: invalid partition gcp was accepted';
  EXCEPTION WHEN data_exception THEN NULL;
  END;
  BEGIN
    PERFORM org_catalog_bedrock_upsert('p6c-reject-f2', 'x', 'converse', 'anthropic',
      'not-a-type', 'aws', 'us-east-1', NULL, ARRAY['us-east-1']::text[], NULL, '', true);
    RAISE EXCEPTION 'P6c FAIL: invalid target_type was accepted';
  EXCEPTION WHEN data_exception THEN NULL;
  END;
END $$;

-- Empty region_set -> REJECTED (completeness gate).
DO $$
BEGIN
  BEGIN
    PERFORM org_catalog_bedrock_upsert('p6c-reject-region', 'x', 'converse', 'anthropic',
      'foundation', 'aws', 'us-east-1', NULL, ARRAY[]::text[], NULL, '', true);
    RAISE EXCEPTION 'P6c FAIL: empty region_set was accepted';
  EXCEPTION WHEN data_exception THEN NULL;
  END;
END $$;

-- Stored Bedrock endpoints and malformed/oversized/NULL-bearing arrays are rejected.
DO $$
BEGIN
  BEGIN
    PERFORM org_catalog_bedrock_upsert('p6c-endpoint', 'x', 'converse', 'anthropic',
      'foundation', 'aws', 'us-east-1', NULL, ARRAY['us-east-1'], NULL,
      'https://127.0.0.1:9999', true);
    RAISE EXCEPTION 'P6c FAIL: stored Bedrock endpoint was accepted';
  EXCEPTION WHEN data_exception THEN NULL;
  END;
  BEGIN
    PERFORM org_catalog_bedrock_upsert('p6c-rank', 'x', 'converse', 'anthropic',
      'foundation', 'aws', 'us-east-1', NULL, ARRAY[['us-east-1']], NULL, '', true);
    RAISE EXCEPTION 'P6c FAIL: multidimensional region array was accepted';
  EXCEPTION WHEN data_exception THEN NULL;
  END;
  BEGIN
    PERFORM org_catalog_bedrock_upsert('p6c-null-elem', 'x', 'converse', 'anthropic',
      'foundation', 'aws', 'us-east-1', NULL, ARRAY['us-east-1',NULL]::text[], NULL, '', true);
    RAISE EXCEPTION 'P6c FAIL: NULL region element was accepted';
  EXCEPTION WHEN data_exception THEN NULL;
  END;
  BEGIN
    PERFORM org_catalog_bedrock_upsert('p6c-duplicate-region', 'x', 'converse', 'anthropic',
      'application-inference-profile', 'aws', 'us-east-1', '123456789012',
      ARRAY['us-east-1','us-east-1']::text[],
      ARRAY['arn:aws:bedrock:us-east-1::foundation-model/anthropic.claude'], '', true);
    RAISE EXCEPTION 'P6c FAIL: duplicate destination region was accepted';
  EXCEPTION WHEN data_exception THEN NULL;
  END;
  BEGIN
    PERFORM org_catalog_bedrock_upsert('p6c-too-many', 'x', 'converse', 'anthropic',
      'foundation', 'aws', 'us-east-1', NULL,
      ARRAY(SELECT 'r-' || i::text FROM generate_series(1,65) AS i), NULL, '', true);
    RAISE EXCEPTION 'P6c FAIL: 65-element region array was accepted';
  EXCEPTION WHEN data_exception THEN NULL;
  END;
END $$;

-- (g) aws_account='abc' on a provisioned target -> REJECTED (needs 12 digits).
DO $$
BEGIN
  BEGIN
    PERFORM org_catalog_bedrock_upsert('p6c-reject-g', 'x', 'converse', 'anthropic',
      'provisioned', 'aws', 'us-east-1', 'abc', ARRAY['us-east-1']::text[], NULL, '', true);
    RAISE EXCEPTION 'P6c FAIL: provisioned target with account=abc was accepted';
  EXCEPTION WHEN data_exception THEN NULL;
  END;
  IF EXISTS (SELECT 1 FROM org_model_catalog WHERE model_id = 'p6c-reject-g') THEN
    RAISE EXCEPTION 'P6c FAIL: a row was written despite a bad account';
  END IF;
END $$;

-- A valid provisioned target (12-digit account) upserts fine (positive control for g).
SELECT org_catalog_bedrock_upsert('p6c-provisioned-ok', 'Prov', 'converse', 'anthropic',
  'provisioned', 'aws', 'us-east-1', '123456789012', ARRAY['us-east-1']::text[], NULL, '', true);
DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM org_model_catalog
                 WHERE model_id = 'p6c-provisioned-ok' AND aws_account = '123456789012') THEN
    RAISE EXCEPTION 'P6c FAIL: valid provisioned target was not written';
  END IF;
END $$;

-- A valid application-inference-profile with well-formed underlying FM ARNs upserts fine.
SELECT org_catalog_bedrock_upsert('p6c-profile-ok', 'Profile', 'converse', 'anthropic',
  'application-inference-profile', 'aws', 'us-west-2', '123456789012',
  ARRAY['us-east-1','us-west-2']::text[],
  ARRAY['arn:aws:bedrock:us-east-1::foundation-model/anthropic.claude-3-sonnet',
        'arn:aws:bedrock:us-west-2::foundation-model/anthropic.claude-3-haiku']::text[],
  '', true);
DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM org_model_catalog
                 WHERE model_id = 'p6c-profile-ok'
                   AND array_length(underlying_fm_arns, 1) = 2) THEN
    RAISE EXCEPTION 'P6c FAIL: valid inference-profile target was not written';
  END IF;
END $$;

-- (h) The PLAIN org_catalog_upsert with provider='bedrock' -> REJECTED (bypass closed).
DO $$
BEGIN
  BEGIN
    PERFORM org_catalog_upsert('p6c-bypass', 'Sneak', 'bedrock', 'anthropic', '', true);
    RAISE EXCEPTION 'P6c FAIL: plain org_catalog_upsert accepted provider=bedrock (bypass OPEN)';
  EXCEPTION WHEN data_exception THEN NULL;  -- expected 22023
  END;
  IF EXISTS (SELECT 1 FROM org_model_catalog WHERE model_id = 'p6c-bypass') THEN
    RAISE EXCEPTION 'P6c FAIL: a bedrock row leaked through the plain upsert path';
  END IF;
  -- (h2) case-/whitespace-variant provider must ALSO be rejected (no near-canonical sneak).
  BEGIN
    PERFORM org_catalog_upsert('p6c-bypass2', 'Sneak', 'Bedrock', 'anthropic', '', true);
    RAISE EXCEPTION 'P6c FAIL: plain upsert accepted provider=Bedrock (case bypass OPEN)';
  EXCEPTION WHEN data_exception THEN NULL;
  END;
  BEGIN
    PERFORM org_catalog_upsert('p6c-bypass3', 'Sneak', '  bedrock  ', 'anthropic', '', true);
    RAISE EXCEPTION 'P6c FAIL: plain upsert accepted provider=" bedrock " (ws bypass OPEN)';
  EXCEPTION WHEN data_exception THEN NULL;
  END;
  IF EXISTS (SELECT 1 FROM org_model_catalog WHERE model_id IN ('p6c-bypass2','p6c-bypass3')) THEN
    RAISE EXCEPTION 'P6c FAIL: a case/ws-variant bedrock row leaked through the plain path';
  END IF;
END $$;

-- (d2) an application-inference-profile (not only cross-region) with empty underlying_fm_arns
-- -> REJECTED (both *-inference-profile target types fail closed).
DO $$
BEGIN
  BEGIN
    PERFORM org_catalog_bedrock_upsert('p6c-appprofile-empty', 'AppProf', 'converse', 'anthropic',
      'application-inference-profile', 'aws', 'us-east-1', '123456789012',
      ARRAY['us-east-1'], NULL, '', true);
    RAISE EXCEPTION 'P6c FAIL: application-inference-profile with empty fm-arns accepted';
  EXCEPTION WHEN data_exception THEN NULL;
  END;
END $$;

-- (i) A non-bedrock provider via the plain path still works; bedrock_* stay NULL.
SELECT org_catalog_upsert('p6c-openai', 'GPT', 'openai', 'openai', 'https://api.openai.test', true);
DO $$
DECLARE r org_model_catalog%ROWTYPE;
BEGIN
  SELECT * INTO r FROM org_model_catalog WHERE model_id = 'p6c-openai';
  IF r.provider <> 'openai' OR r.bedrock_api IS NOT NULL OR r.aws_region_set IS NOT NULL
     OR r.underlying_fm_arns IS NOT NULL THEN
    RAISE EXCEPTION 'P6c FAIL: non-bedrock row is not clean (provider=% api=%)',
      r.provider, r.bedrock_api;
  END IF;
END $$;

-- (j) A WORM kb_audit_outbox row is appended atomically on a successful bedrock upsert.
DO $$
DECLARE n0 bigint; n1 bigint; last_action text;
BEGIN
  SELECT count(*) INTO n0 FROM kb_audit_outbox;
  PERFORM org_catalog_bedrock_upsert('p6c-audited', 'Audited', 'converse', 'amazon-nova',
    'foundation', 'aws', 'us-east-1', NULL, ARRAY['us-east-1']::text[], NULL, '', true);
  SELECT count(*) INTO n1 FROM kb_audit_outbox;
  IF n1 <> n0 + 1 THEN
    RAISE EXCEPTION 'P6c FAIL: bedrock upsert did not append exactly one audit row (% -> %)', n0, n1;
  END IF;
  SELECT action INTO last_action FROM kb_audit_outbox ORDER BY outbox_id DESC LIMIT 1;
  IF last_action <> 'org_catalog_bedrock_upsert' THEN
    RAISE EXCEPTION 'P6c FAIL: newest audit action = % (want org_catalog_bedrock_upsert)', last_action;
  END IF;
END $$;

-- Bind representative models to the exact team used by the runtime authority checks.
SELECT org_model_entitle('p6c-claude-foundation', 960001);
SELECT org_model_entitle('p6c-profile-ok', 960001);
SELECT org_model_entitle('p6c-legacy-null', 960001);
SELECT org_model_entitle('p6c-disabled', 960001);
SELECT org_model_entitle('p6c-native-invoke', 960001);
SELECT org_model_entitle('p6c-openai', 960001);

-- ----------------------------------------------------------------------------
-- Drop to the runtime role; RLS + grants now govern every access.
-- ----------------------------------------------------------------------------
SET ROLE aimee_kb_runtime;

-- (k) The runtime role has NO direct SELECT on org_model_catalog — a direct read of a new
-- bedrock_* column is permission denied (the columns are admin/definer-only; P2a already
-- REVOKED runtime's direct catalog SELECT, and the entitled projection excludes them).
SELECT set_tenant_context('oidc:test:p6c_member_a', 960001);
DO $$
BEGIN
  BEGIN
    PERFORM bedrock_api FROM org_model_catalog LIMIT 1;
    RAISE EXCEPTION 'P6c FAIL: runtime performed a direct SELECT of bedrock_api';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected: no SELECT grant
  END;
END $$;

-- The sole runtime resolver returns the exact bounded tuple only for this actor, this
-- request team, this entitlement, and a complete Converse target.
DO $$
DECLARE r record;
BEGIN
  SELECT * INTO r FROM org_catalog_bedrock_target(960001, 'p6c-profile-ok');
  IF r.model_id IS NULL OR r.invoke_region <> 'us-west-2' OR r.partition <> 'aws'
     OR r.account <> '123456789012' OR r.target_type <> 'application-inference-profile'
     OR r.region_set_json::jsonb <> '["us-east-1","us-west-2"]'::jsonb
     OR jsonb_array_length(r.underlying_json::jsonb) <> 2 OR r.endpoint <> '' THEN
    RAISE EXCEPTION 'P6c FAIL: resolver tuple mismatch: %', row_to_json(r);
  END IF;
  IF EXISTS (SELECT 1 FROM org_catalog_bedrock_target(960002, 'p6c-profile-ok')) THEN
    RAISE EXCEPTION 'P6c FAIL: resolver crossed request-team GUC boundary';
  END IF;
  IF EXISTS (SELECT 1 FROM org_catalog_bedrock_target(960001, 'p6c-provisioned-ok'))
     OR EXISTS (SELECT 1 FROM org_catalog_bedrock_target(960001, 'p6c-disabled'))
     OR EXISTS (SELECT 1 FROM org_catalog_bedrock_target(960001, 'p6c-native-invoke'))
     OR EXISTS (SELECT 1 FROM org_catalog_bedrock_target(960001, 'p6c-openai'))
     OR EXISTS (SELECT 1 FROM org_catalog_bedrock_target(960001, 'p6c-legacy-null'))
     OR EXISTS (SELECT 1 FROM org_catalog_bedrock_target(960001, 'does-not-exist')) THEN
    RAISE EXCEPTION 'P6c FAIL: unavailable resolver cases leaked a row';
  END IF;
END $$;

SELECT set_tenant_context('oidc:test:p6c_other_actor', NULL);
DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM org_catalog_bedrock_target(960001, 'p6c-profile-ok')) THEN
    RAISE EXCEPTION 'P6c FAIL: cross-actor resolver access succeeded';
  END IF;
END $$;
SELECT set_tenant_context('oidc:test:p6c_member_a', 960001);

-- (j-cont) admin-only: a non-admin principal calling org_catalog_bedrock_upsert RAISEs
-- 42501 (the runtime role's principal is a plain member, not an org admin).
DO $$
BEGIN
  BEGIN
    PERFORM org_catalog_bedrock_upsert('p6c-evil', 'Evil', 'converse', 'anthropic',
      'foundation', 'aws', 'us-east-1', NULL, ARRAY['us-east-1']::text[], NULL, '', true);
    RAISE EXCEPTION 'P6c FAIL: a non-admin principal wrote a bedrock catalog row';
  EXCEPTION WHEN insufficient_privilege THEN NULL;  -- expected: admin-only (errcode 42501)
  END;
END $$;

RESET ROLE;

\echo '== P6c bedrock catalog routing + fail-closed adapter-registry validation assertions PASSED =='
ROLLBACK;
