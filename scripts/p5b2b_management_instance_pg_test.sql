\set ON_ERROR_STOP on

-- CT260 supplies production-profile public metadata with -v.  The ordinary P1
-- gate uses deterministic metadata-only defaults; neither path stores PEM or
-- private/workload proof material.
\if :{?issuer}
\else
  \set issuer 'spiffe://p5b2b.test'
\endif
\if :{?subject_a}
\else
  \set subject_a 'kb-node-a'
\endif
\if :{?subject_b}
\else
  \set subject_b 'kb-node-b'
\endif
\if :{?proof_anchor_a}
\else
  \set proof_anchor_a '1111111111111111111111111111111111111111111111111111111111111111'
\endif
\if :{?proof_anchor_b}
\else
  \set proof_anchor_b '3333333333333333333333333333333333333333333333333333333333333333'
\endif
\if :{?custody_anchor_a}
\else
  \set custody_anchor_a '2222222222222222222222222222222222222222222222222222222222222222'
\endif
\if :{?custody_anchor_b}
\else
  \set custody_anchor_b '4444444444444444444444444444444444444444444444444444444444444444'
\endif
\if :{?binding_digest_a}
\else
  \set binding_digest_a 'c4962ba069dae6f9081d34b0215d53a06e3f5b470f02c7cd70ade8e1a110202e'
\endif
\if :{?binding_digest_b}
\else
  \set binding_digest_b '24e9b2c83cd1f18b01594a62c3e13971794f9d15f7d461a30bf9f8260dfdf15d'
\endif
\if :{?ca_fingerprint}
\else
  \set ca_fingerprint 'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc'
\endif
\if :{?leaf_a_issuer}
\else
  \set leaf_a_issuer '/CN=p5b2b-ca'
\endif
\if :{?leaf_b_issuer}
\else
  \set leaf_b_issuer '/CN=p5b2b-ca'
\endif
\if :{?leaf_a_serial}
\else
  \set leaf_a_serial 'a1'
\endif
\if :{?leaf_b_serial}
\else
  \set leaf_b_serial 'b1'
\endif
\if :{?leaf_a_fingerprint}
\else
  \set leaf_a_fingerprint 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
\endif
\if :{?leaf_b_fingerprint}
\else
  \set leaf_b_fingerprint 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'
\endif
\if :{?leaf_a_spki_digest}
\else
  \set leaf_a_spki_digest '7777777777777777777777777777777777777777777777777777777777777777'
\endif
\if :{?leaf_b_spki_digest}
\else
  \set leaf_b_spki_digest '8888888888888888888888888888888888888888888888888888888888888888'
\endif
\if :{?leaf_a_not_before}
\else
  SELECT extract(epoch FROM date_trunc('second',now()-interval '10 seconds'))::BIGINT AS leaf_a_not_before,
         extract(epoch FROM date_trunc('second',now()-interval '10 seconds')+interval '1 hour')::BIGINT AS leaf_a_not_after
  \gset
\endif
\if :{?leaf_b_not_before}
\else
  SELECT extract(epoch FROM date_trunc('second',now()-interval '10 seconds'))::BIGINT AS leaf_b_not_before,
         extract(epoch FROM date_trunc('second',now()-interval '10 seconds')+interval '1 hour')::BIGINT AS leaf_b_not_after
  \gset
\endif

BEGIN;
-- The production column is UTC TEXT.  Force a non-UTC session so every
-- certificate-validity read proves it does not inherit the database timezone.
SET LOCAL TIME ZONE 'Europe/Amsterdam';

DO $$ BEGIN
  IF public.aimee_utc_text_timestamptz('2026-01-02 03:04:05') IS DISTINCT FROM
       TIMESTAMPTZ '2026-01-02 03:04:05+00' OR
     public.aimee_utc_text_timestamptz('2026-01-02T03:04:05Z') IS DISTINCT FROM
       TIMESTAMPTZ '2026-01-02 03:04:05+00' THEN
    RAISE EXCEPTION 'UTC enrollment timestamp parsing inherited session timezone';
  END IF;
END $$;

-- Pin a transcript vector independently generated from the canonical network-
-- order length framing, then prove any CT-supplied B2a vectors agree with SQL.
DO $$ BEGIN
  IF public.kb_management_instance_binding_digest(
      'spiffe://p5b2b.test','kb-node-a',repeat('1',64),repeat('2',64)) <>
      'c4962ba069dae6f9081d34b0215d53a06e3f5b470f02c7cd70ade8e1a110202e' THEN
    RAISE EXCEPTION 'canonical binding vector mismatch';
  END IF;
END $$;

SELECT set_config('p5b2b.issuer',:'issuer',false),
       set_config('p5b2b.subject_a',:'subject_a',false),
       set_config('p5b2b.subject_b',:'subject_b',false),
       set_config('p5b2b.proof_a',:'proof_anchor_a',false),
       set_config('p5b2b.proof_b',:'proof_anchor_b',false),
       set_config('p5b2b.custody_a',:'custody_anchor_a',false),
       set_config('p5b2b.custody_b',:'custody_anchor_b',false),
       set_config('p5b2b.binding_a',:'binding_digest_a',false),
       set_config('p5b2b.binding_b',:'binding_digest_b',false),
       set_config('p5b2b.ca_fp',:'ca_fingerprint',false),
       set_config('p5b2b.leaf_a_issuer',:'leaf_a_issuer',false),
       set_config('p5b2b.leaf_b_issuer',:'leaf_b_issuer',false),
       set_config('p5b2b.leaf_a_serial',:'leaf_a_serial',false),
       set_config('p5b2b.leaf_b_serial',:'leaf_b_serial',false),
       set_config('p5b2b.leaf_a_fp',:'leaf_a_fingerprint',false),
       set_config('p5b2b.leaf_b_fp',:'leaf_b_fingerprint',false),
       set_config('p5b2b.leaf_a_spki',:'leaf_a_spki_digest',false),
       set_config('p5b2b.leaf_b_spki',:'leaf_b_spki_digest',false),
       set_config('p5b2b.leaf_a_nb',:'leaf_a_not_before',false),
       set_config('p5b2b.leaf_a_na',:'leaf_a_not_after',false),
       set_config('p5b2b.leaf_b_nb',:'leaf_b_not_before',false),
       set_config('p5b2b.leaf_b_na',:'leaf_b_not_after',false);

DO $$ BEGIN
  IF public.kb_management_instance_binding_digest(current_setting('p5b2b.issuer'),
       current_setting('p5b2b.subject_a'),current_setting('p5b2b.proof_a'),
       current_setting('p5b2b.custody_a'))<>current_setting('p5b2b.binding_a') OR
     public.kb_management_instance_binding_digest(current_setting('p5b2b.issuer'),
       current_setting('p5b2b.subject_b'),current_setting('p5b2b.proof_b'),
       current_setting('p5b2b.custody_b'))<>current_setting('p5b2b.binding_b') THEN
    RAISE EXCEPTION 'B2a/SQL binding disagreement';
  END IF;
END $$;

INSERT INTO public.kb_team(name) VALUES('p5b2b-team-a'),('p5b2b-team-b');
SELECT set_config('p5b2b.team_a',(SELECT id::TEXT FROM public.kb_team WHERE name='p5b2b-team-a'),false),
       set_config('p5b2b.team_b',(SELECT id::TEXT FROM public.kb_team WHERE name='p5b2b-team-b'),false);

-- Hardened ownership, exact one owner-only policy per table, and negative ACLs.
DO $$
DECLARE t TEXT; n INTEGER;
BEGIN
  FOREACH t IN ARRAY ARRAY['kb_management_instance_grant','kb_management_instance',
                            'kb_management_instance_issue'] LOOP
    SELECT count(*) INTO n FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace ns ON ns.oid=c.relnamespace
     WHERE ns.nspname='public' AND c.relname=t AND c.relrowsecurity AND c.relforcerowsecurity
       AND pg_catalog.pg_get_userbyid(c.relowner)='aimee_kb_owner';
    IF n<>1 THEN RAISE EXCEPTION 'RLS/owner mismatch for %',t; END IF;
    SELECT count(*) INTO n FROM pg_catalog.pg_policy p JOIN pg_catalog.pg_class c ON c.oid=p.polrelid
     WHERE c.relname=t AND p.polcmd='*' AND p.polroles='{0}'::OID[]
       AND pg_catalog.pg_get_expr(p.polqual,p.polrelid)=pg_catalog.pg_get_expr(p.polwithcheck,p.polrelid)
       AND pg_catalog.pg_get_expr(p.polqual,p.polrelid) LIKE '%aimee_kb_owner%';
    IF n<>1 THEN RAISE EXCEPTION 'owner-only policy mismatch for %',t; END IF;
    IF has_table_privilege('aimee_kb_runtime','public.'||t,'SELECT') OR
       has_table_privilege('aimee_kb_runtime','public.'||t,'INSERT') OR
       has_table_privilege('aimee_kb_runtime','public.'||t,'UPDATE') OR
       has_table_privilege('aimee_kb_runtime','public.'||t,'DELETE') THEN
      RAISE EXCEPTION 'runtime has direct privilege on %',t;
    END IF;
  END LOOP;
  IF has_function_privilege('aimee_kb_runtime',
      'public.kb_management_instance_grant_create(text,bigint,text,text,text,text,text,text,text,text)','EXECUTE') OR
     NOT has_function_privilege('aimee_kb_runtime',
      'public.kb_management_instance_grant_preflight(text,text,text,text,text,text)','EXECUTE') OR
     NOT has_function_privilege('aimee_kb_runtime',
      'public.kb_management_instance_snapshot(text,text,text,text,text,text)','EXECUTE') THEN
    RAISE EXCEPTION 'runtime facade ACL mismatch';
  END IF;
  SELECT count(*) INTO n
    FROM pg_catalog.pg_proc p,
         LATERAL pg_catalog.aclexplode(COALESCE(
           p.proacl,pg_catalog.acldefault('f',p.proowner))) a
   WHERE p.oid='public.kb_management_instance_grant_preflight(text,text,text,text,text,text)'::REGPROCEDURE
     AND a.grantee=0 AND a.privilege_type='EXECUTE';
  IF n<>0 THEN RAISE EXCEPTION 'PUBLIC preflight execute privilege'; END IF;
  IF to_regprocedure(
      'public.kb_management_instance_begin_initial(text,text,text,text,text,text,text,text,text,text)')
      IS NOT NULL THEN
    RAISE EXCEPTION 'legacy begin_initial overload remains installed';
  END IF;
  IF NOT has_function_privilege('aimee_kb_migrate',
      'public.kb_management_instance_grant_create(text,bigint,text,text,text,text,text,text,text,text)','EXECUTE') THEN
    RAISE EXCEPTION 'migration provisioning ACL missing';
  END IF;
  SELECT count(*) INTO n
    FROM pg_catalog.pg_policy p
    JOIN pg_catalog.pg_class c ON c.oid=p.polrelid
    JOIN pg_catalog.pg_namespace ns ON ns.oid=c.relnamespace
   WHERE ns.nspname='public' AND c.relname='kb_team_membership'
     AND p.polname='p_member_management_definer_read' AND p.polcmd='r'
     AND p.polroles='{0}'::OID[] AND p.polwithcheck IS NULL
     AND pg_catalog.pg_get_expr(p.polqual,p.polrelid) LIKE '%aimee_kb_owner%'
     AND pg_catalog.pg_get_expr(p.polqual,p.polrelid) LIKE '%cert:%';
  IF n<>1 THEN RAISE EXCEPTION 'management membership read policy mismatch'; END IF;
  SELECT count(*) INTO n
    FROM pg_catalog.pg_policy p
    JOIN pg_catalog.pg_class c ON c.oid=p.polrelid
    JOIN pg_catalog.pg_namespace ns ON ns.oid=c.relnamespace
   WHERE ns.nspname='public' AND c.relname='kb_team_membership'
     AND p.polname='p_member_management_definer_insert' AND p.polcmd='a'
     AND p.polroles='{0}'::OID[] AND p.polqual IS NULL
     AND pg_catalog.pg_get_expr(p.polwithcheck,p.polrelid) LIKE '%aimee_kb_owner%'
     AND pg_catalog.pg_get_expr(p.polwithcheck,p.polrelid) LIKE '%cert:%'
     AND pg_catalog.pg_get_expr(p.polwithcheck,p.polrelid) LIKE '%is_default%';
  IF n<>1 THEN RAISE EXCEPTION 'management membership insert policy mismatch'; END IF;
END $$;

SET ROLE aimee_kb_runtime;
DO $$ BEGIN
  BEGIN PERFORM * FROM public.kb_management_instance_grant;
    RAISE EXCEPTION 'runtime read lineage table';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  BEGIN PERFORM public.kb_management_instance_binding_digest('a','b',repeat('1',64),repeat('2',64));
    RAISE EXCEPTION 'runtime executed internal digest helper';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
END $$;
RESET ROLE;

-- Two independent nodes on one team: owner grants, exact replay, and mismatch.
SET ROLE aimee_kb_migrate;
DO $$
DECLARE r RECORD;
BEGIN
  BEGIN
    PERFORM * FROM public.kb_management_instance_grant_create(repeat('0',32),
      9223372036854775807,current_setting('p5b2b.issuer'),'missing-team',repeat('1',64),
      repeat('2',64),public.kb_management_instance_binding_digest(
        current_setting('p5b2b.issuer'),'missing-team',repeat('1',64),repeat('2',64)),
      current_setting('p5b2b.leaf_a_issuer'),current_setting('p5b2b.ca_fp'),'owner:p5b2b-test');
    RAISE EXCEPTION 'missing-team grant accepted';
  EXCEPTION WHEN invalid_authorization_specification THEN NULL; END;
  SELECT * INTO r FROM public.kb_management_instance_grant_create(repeat('1',32),
    current_setting('p5b2b.team_a')::BIGINT,current_setting('p5b2b.issuer'),
    current_setting('p5b2b.subject_a'),current_setting('p5b2b.proof_a'),
    current_setting('p5b2b.custody_a'),current_setting('p5b2b.binding_a'),
    current_setting('p5b2b.leaf_a_issuer'),current_setting('p5b2b.ca_fp'),'owner:p5b2b-test');
  IF r.replayed OR r.grant_state<>'pending' THEN RAISE EXCEPTION 'initial grant result mismatch'; END IF;
  SELECT * INTO r FROM public.kb_management_instance_grant_create(repeat('1',32),
    current_setting('p5b2b.team_a')::BIGINT,current_setting('p5b2b.issuer'),
    current_setting('p5b2b.subject_a'),current_setting('p5b2b.proof_a'),
    current_setting('p5b2b.custody_a'),current_setting('p5b2b.binding_a'),
    current_setting('p5b2b.leaf_a_issuer'),current_setting('p5b2b.ca_fp'),'owner:p5b2b-test');
  IF NOT r.replayed THEN RAISE EXCEPTION 'grant exact replay missed'; END IF;
  BEGIN
    PERFORM * FROM public.kb_management_instance_grant_create(repeat('1',32),
      current_setting('p5b2b.team_a')::BIGINT,current_setting('p5b2b.issuer'),
      current_setting('p5b2b.subject_a'),current_setting('p5b2b.proof_a'),
      current_setting('p5b2b.custody_a'),current_setting('p5b2b.binding_a'),
      '/CN=wrong-ca',current_setting('p5b2b.ca_fp'),'owner:p5b2b-test');
    RAISE EXCEPTION 'grant mismatch replayed';
  EXCEPTION WHEN unique_violation THEN NULL; END;
  BEGIN
    PERFORM * FROM public.kb_management_instance_grant_create(repeat('6',32),
      current_setting('p5b2b.team_a')::BIGINT,current_setting('p5b2b.issuer'),
      current_setting('p5b2b.subject_a'),current_setting('p5b2b.proof_a'),
      current_setting('p5b2b.custody_a'),current_setting('p5b2b.binding_a'),
      current_setting('p5b2b.leaf_a_issuer'),current_setting('p5b2b.ca_fp'),'owner:p5b2b-test');
    RAISE EXCEPTION 'duplicate live custody/binding accepted';
  EXCEPTION WHEN unique_violation THEN NULL; END;
  PERFORM * FROM public.kb_management_instance_grant_create(repeat('2',32),
    current_setting('p5b2b.team_a')::BIGINT,current_setting('p5b2b.issuer'),
    current_setting('p5b2b.subject_b'),current_setting('p5b2b.proof_b'),
    current_setting('p5b2b.custody_b'),current_setting('p5b2b.binding_b'),
    current_setting('p5b2b.leaf_b_issuer'),current_setting('p5b2b.ca_fp'),'owner:p5b2b-test');
END $$;
RESET ROLE;

-- A past-but-well-formed grant proves preflight uses primary authoritative time.
SELECT set_config('p5b2b.binding_expired',
  public.kb_management_instance_binding_digest('spiffe://p5b2b.expired','expired-node',
    repeat('8',64),repeat('9',64)),false);
INSERT INTO public.kb_management_instance_grant(installation_id,replacement_lineage_id,team_id,
  workload_issuer,workload_subject,proof_anchor,custody_anchor,binding_digest,
  expected_ca_issuer,expected_ca_fingerprint,creator_identity,created_at,expires_at)
VALUES(repeat('8',32),repeat('8',32),current_setting('p5b2b.team_b')::BIGINT,
  'spiffe://p5b2b.expired','expired-node',repeat('8',64),repeat('9',64),
  current_setting('p5b2b.binding_expired'),current_setting('p5b2b.leaf_a_issuer'),
  current_setting('p5b2b.ca_fp'),'owner:p5b2b-test',now()-interval '2 hours',
  now()-interval '1 hour');

SET ROLE aimee_kb_runtime;
DO $$
DECLARE r RECORD;
BEGIN
  SELECT * INTO r FROM public.kb_management_instance_grant_preflight(repeat('1',32),
    current_setting('p5b2b.issuer'),current_setting('p5b2b.subject_a'),
    current_setting('p5b2b.proof_a'),current_setting('p5b2b.custody_a'),
    current_setting('p5b2b.binding_a'));
  IF r.installation_id<>repeat('1',32) OR r.replacement_lineage_id<>repeat('1',32) OR
     r.expires_at_epoch<=extract(epoch FROM now())::BIGINT THEN
    RAISE EXCEPTION 'grant preflight result mismatch';
  END IF;
  BEGIN
    PERFORM * FROM public.kb_management_instance_grant_preflight(repeat('1',32),
      current_setting('p5b2b.issuer'),current_setting('p5b2b.subject_b'),
      current_setting('p5b2b.proof_b'),current_setting('p5b2b.custody_b'),
      current_setting('p5b2b.binding_b'));
    RAISE EXCEPTION 'wrong-binding preflight allowed';
  EXCEPTION WHEN invalid_authorization_specification THEN NULL; END;
  BEGIN
    PERFORM * FROM public.kb_management_instance_grant_preflight(repeat('8',32),
      'spiffe://p5b2b.expired','expired-node',repeat('8',64),repeat('9',64),
      current_setting('p5b2b.binding_expired'));
    RAISE EXCEPTION 'expired preflight allowed';
  EXCEPTION WHEN invalid_authorization_specification THEN NULL; END;
  BEGIN
    PERFORM * FROM public.kb_management_instance_begin_initial(repeat('9',64),repeat('1',32),
      repeat('1',32),repeat('9',32),current_setting('p5b2b.issuer'),
      current_setting('p5b2b.subject_a'),current_setting('p5b2b.proof_a'),
      current_setting('p5b2b.custody_a'),current_setting('p5b2b.binding_a'),repeat('5',64),
      current_setting('p5b2b.leaf_a_spki'));
    RAISE EXCEPTION 'changed-lineage begin allowed';
  EXCEPTION WHEN unique_violation THEN NULL; END;
  SELECT * INTO r FROM public.kb_management_instance_begin_initial(repeat('a',64),repeat('1',32),
    repeat('1',32),repeat('1',32),current_setting('p5b2b.issuer'),current_setting('p5b2b.subject_a'),
    current_setting('p5b2b.proof_a'),current_setting('p5b2b.custody_a'),
    current_setting('p5b2b.binding_a'),repeat('5',64),current_setting('p5b2b.leaf_a_spki'));
  IF r.replayed OR r.generation<>1 OR r.issue_state<>'pending' THEN RAISE EXCEPTION 'initial begin mismatch'; END IF;
  SELECT * INTO r FROM public.kb_management_instance_begin_initial(repeat('a',64),repeat('1',32),
    repeat('1',32),repeat('1',32),current_setting('p5b2b.issuer'),current_setting('p5b2b.subject_a'),
    current_setting('p5b2b.proof_a'),current_setting('p5b2b.custody_a'),
    current_setting('p5b2b.binding_a'),repeat('5',64),current_setting('p5b2b.leaf_a_spki'));
  IF NOT r.replayed THEN RAISE EXCEPTION 'initial begin replay missed'; END IF;
  BEGIN
    PERFORM * FROM public.kb_management_instance_begin_initial(repeat('9',64),repeat('9',32),
      repeat('1',32),repeat('1',32),current_setting('p5b2b.issuer'),
      current_setting('p5b2b.subject_a'),current_setting('p5b2b.proof_a'),
      current_setting('p5b2b.custody_a'),current_setting('p5b2b.binding_a'),repeat('9',64),
      current_setting('p5b2b.leaf_a_spki'));
    RAISE EXCEPTION 'other operation consumed grant';
  EXCEPTION WHEN unique_violation THEN NULL; END;
  BEGIN
    PERFORM * FROM public.kb_management_instance_begin_initial(repeat('a',64),repeat('1',32),
      repeat('1',32),repeat('9',32),current_setting('p5b2b.issuer'),
      current_setting('p5b2b.subject_a'),current_setting('p5b2b.proof_a'),
      current_setting('p5b2b.custody_a'),current_setting('p5b2b.binding_a'),repeat('5',64),
      current_setting('p5b2b.leaf_a_spki'));
    RAISE EXCEPTION 'replay changed lineage allowed';
  EXCEPTION WHEN unique_violation THEN NULL; END;
  BEGIN
    PERFORM * FROM public.kb_management_instance_begin_initial(repeat('a',64),repeat('1',32),
      repeat('1',32),repeat('1',32),current_setting('p5b2b.issuer'),current_setting('p5b2b.subject_a'),
      current_setting('p5b2b.proof_a'),current_setting('p5b2b.custody_a'),
      current_setting('p5b2b.binding_a'),repeat('6',64),current_setting('p5b2b.leaf_a_spki'));
    RAISE EXCEPTION 'initial begin mismatch replayed';
  EXCEPTION WHEN unique_violation THEN NULL; END;
  PERFORM * FROM public.kb_management_instance_begin_initial(repeat('b',64),repeat('2',32),
    repeat('2',32),repeat('2',32),current_setting('p5b2b.issuer'),current_setting('p5b2b.subject_b'),
    current_setting('p5b2b.proof_b'),current_setting('p5b2b.custody_b'),
    current_setting('p5b2b.binding_b'),repeat('6',64),current_setting('p5b2b.leaf_b_spki'));
END $$;
RESET ROLE;

-- Activate both, replay A exactly, and reject changed activation metadata.
SET ROLE aimee_kb_runtime;
DO $$
DECLARE r RECORD;
BEGIN
  SELECT * INTO r FROM public.kb_management_instance_activate(repeat('a',64),repeat('1',32),
    current_setting('p5b2b.issuer'),current_setting('p5b2b.subject_a'),current_setting('p5b2b.proof_a'),
    current_setting('p5b2b.custody_a'),current_setting('p5b2b.binding_a'),'initial',1,
    NULL::BIGINT,NULL::TEXT,NULL::TEXT,NULL::TEXT,repeat('5',64),current_setting('p5b2b.leaf_a_spki'),
    repeat('9',64),current_setting('p5b2b.leaf_a_issuer'),current_setting('p5b2b.ca_fp'),
    current_setting('p5b2b.leaf_a_issuer'),current_setting('p5b2b.leaf_a_serial'),
    current_setting('p5b2b.leaf_a_fp'),current_setting('p5b2b.leaf_a_spki'),
    current_setting('p5b2b.leaf_a_nb')::BIGINT,current_setting('p5b2b.leaf_a_na')::BIGINT);
  IF r.replayed OR r.issue_state<>'active' OR r.generation<>1 THEN RAISE EXCEPTION 'activation mismatch'; END IF;
  SELECT * INTO r FROM public.kb_management_instance_activate(repeat('a',64),repeat('1',32),
    current_setting('p5b2b.issuer'),current_setting('p5b2b.subject_a'),current_setting('p5b2b.proof_a'),
    current_setting('p5b2b.custody_a'),current_setting('p5b2b.binding_a'),'initial',1,
    NULL::BIGINT,NULL::TEXT,NULL::TEXT,NULL::TEXT,repeat('5',64),current_setting('p5b2b.leaf_a_spki'),
    repeat('9',64),current_setting('p5b2b.leaf_a_issuer'),current_setting('p5b2b.ca_fp'),
    current_setting('p5b2b.leaf_a_issuer'),current_setting('p5b2b.leaf_a_serial'),
    current_setting('p5b2b.leaf_a_fp'),current_setting('p5b2b.leaf_a_spki'),
    current_setting('p5b2b.leaf_a_nb')::BIGINT,current_setting('p5b2b.leaf_a_na')::BIGINT);
  IF NOT r.replayed THEN RAISE EXCEPTION 'activation replay missed'; END IF;
  BEGIN
    PERFORM * FROM public.kb_management_instance_activate(repeat('a',64),repeat('1',32),
      current_setting('p5b2b.issuer'),current_setting('p5b2b.subject_a'),current_setting('p5b2b.proof_a'),
      current_setting('p5b2b.custody_a'),current_setting('p5b2b.binding_a'),'initial',1,
      NULL::BIGINT,NULL::TEXT,NULL::TEXT,NULL::TEXT,repeat('5',64),current_setting('p5b2b.leaf_a_spki'),
      repeat('8',64),current_setting('p5b2b.leaf_a_issuer'),current_setting('p5b2b.ca_fp'),
      current_setting('p5b2b.leaf_a_issuer'),current_setting('p5b2b.leaf_a_serial'),
      current_setting('p5b2b.leaf_a_fp'),current_setting('p5b2b.leaf_a_spki'),
      current_setting('p5b2b.leaf_a_nb')::BIGINT,current_setting('p5b2b.leaf_a_na')::BIGINT);
    RAISE EXCEPTION 'activation mismatch replayed';
  EXCEPTION WHEN unique_violation THEN NULL; END;
  PERFORM * FROM public.kb_management_instance_activate(repeat('b',64),repeat('2',32),
    current_setting('p5b2b.issuer'),current_setting('p5b2b.subject_b'),current_setting('p5b2b.proof_b'),
    current_setting('p5b2b.custody_b'),current_setting('p5b2b.binding_b'),'initial',1,
    NULL::BIGINT,NULL::TEXT,NULL::TEXT,NULL::TEXT,repeat('6',64),current_setting('p5b2b.leaf_b_spki'),
    repeat('a',64),current_setting('p5b2b.leaf_b_issuer'),current_setting('p5b2b.ca_fp'),
    current_setting('p5b2b.leaf_b_issuer'),current_setting('p5b2b.leaf_b_serial'),
    current_setting('p5b2b.leaf_b_fp'),current_setting('p5b2b.leaf_b_spki'),
    current_setting('p5b2b.leaf_b_nb')::BIGINT,current_setting('p5b2b.leaf_b_na')::BIGINT);
END $$;
RESET ROLE;

-- A target server makes the new cert membership observable through the existing
-- status-admission join, without any direct runtime lineage-table read.
INSERT INTO public.kb_enrollments(scope,fingerprint,serial,state,expires_at,legacy,
  cert_issuer,cert_serial_norm,authority_id)
VALUES('p5-server-management',repeat('f',64),'ff','active',(now()+interval '1 day')::TEXT,0,
       '/CN=p5b2b-target-ca','ff',repeat('f',32));
INSERT INTO public.kb_server_registry(server_id,cert_cn,mgmt_cert_cn,team_id,endpoint,status,
  mgmt_issuer,mgmt_serial_norm,mgmt_fingerprint)
VALUES('p5b2b-target','p5b2b-target-client','p5b2b-target-mgmt',
       current_setting('p5b2b.team_a')::BIGINT,'https://p5b2b.invalid','active',
       '/CN=p5b2b-target-ca','ff',repeat('f',64));

SET ROLE aimee_kb_runtime;
DO $$ BEGIN
  PERFORM * FROM public.kb_management_instance_snapshot(repeat('1',32),
    current_setting('p5b2b.issuer'),current_setting('p5b2b.subject_a'),
    current_setting('p5b2b.proof_a'),current_setting('p5b2b.custody_a'),
    current_setting('p5b2b.binding_a'));
  PERFORM * FROM public.kb_management_status_lookup(current_setting('p5b2b.leaf_a_issuer'),
    current_setting('p5b2b.leaf_a_serial'),current_setting('p5b2b.leaf_a_fp'),
    'p5b2b-target','management.health.v1');
END $$;

RESET ROLE;

DO $$ BEGIN
  IF (SELECT count(*) FROM public.kb_management_instance WHERE team_id=current_setting('p5b2b.team_a')::BIGINT
       AND state='active' AND current_generation=1)<>2 THEN RAISE EXCEPTION 'two-node activation missing'; END IF;
  IF (SELECT count(*) FROM public.kb_team_membership WHERE team=current_setting('p5b2b.team_a')::BIGINT
       AND identity_key LIKE 'cert:%')<>2 THEN RAISE EXCEPTION 'certificate memberships missing'; END IF;
END $$;

-- Renewal is denied outside the inclusive <=1200-second window.
SELECT set_config('p5b2b.a_enrollment',(SELECT current_enrollment_id::TEXT FROM public.kb_management_instance
  WHERE installation_id=repeat('1',32)),false);
SET ROLE aimee_kb_runtime;
DO $$ BEGIN
  BEGIN
    PERFORM * FROM public.kb_management_instance_begin_renewal(repeat('d',64),repeat('1',32),
      current_setting('p5b2b.issuer'),current_setting('p5b2b.subject_a'),current_setting('p5b2b.proof_a'),
      current_setting('p5b2b.custody_a'),current_setting('p5b2b.binding_a'),2,
      current_setting('p5b2b.a_enrollment')::BIGINT,current_setting('p5b2b.leaf_a_issuer'),
      current_setting('p5b2b.leaf_a_serial'),current_setting('p5b2b.leaf_a_fp'),repeat('d',64),repeat('e',64));
    RAISE EXCEPTION 'early renewal allowed';
  EXCEPTION WHEN invalid_authorization_specification THEN NULL; END;
END $$;
RESET ROLE;

-- Revocation between activation and replay fails closed.
UPDATE public.kb_enrollments SET state='revoked',revoked_at=public.pg_now_text()
 WHERE id=(SELECT current_enrollment_id FROM public.kb_management_instance WHERE installation_id=repeat('2',32));
SET ROLE aimee_kb_runtime;
DO $$ BEGIN
  BEGIN
    PERFORM * FROM public.kb_management_instance_activate(repeat('b',64),repeat('2',32),
      current_setting('p5b2b.issuer'),current_setting('p5b2b.subject_b'),current_setting('p5b2b.proof_b'),
      current_setting('p5b2b.custody_b'),current_setting('p5b2b.binding_b'),'initial',1,
      NULL::BIGINT,NULL::TEXT,NULL::TEXT,NULL::TEXT,repeat('6',64),current_setting('p5b2b.leaf_b_spki'),
      repeat('a',64),current_setting('p5b2b.leaf_b_issuer'),current_setting('p5b2b.ca_fp'),
      current_setting('p5b2b.leaf_b_issuer'),current_setting('p5b2b.leaf_b_serial'),
      current_setting('p5b2b.leaf_b_fp'),current_setting('p5b2b.leaf_b_spki'),
      current_setting('p5b2b.leaf_b_nb')::BIGINT,current_setting('p5b2b.leaf_b_na')::BIGINT);
    RAISE EXCEPTION 'revoked activation replay allowed';
  EXCEPTION WHEN invalid_authorization_specification THEN NULL; END;
END $$;
RESET ROLE;

-- Explicit replacement preserves lineage and revokes through the canonical
-- trigger, which advances the global generation exactly once.
SELECT set_config('p5b2b.rev_before',(SELECT generation::TEXT FROM public.kb_cert_revocation_generation WHERE singleton=1),false);
SELECT set_config('p5b2b.binding_c',public.kb_management_instance_binding_digest(
  current_setting('p5b2b.issuer'),'kb-node-c',repeat('5',64),repeat('6',64)),false);
SET ROLE aimee_kb_migrate;
DO $$
DECLARE r RECORD;
BEGIN
  SELECT * INTO r FROM public.kb_management_instance_replacement_grant_create(repeat('c',64),
    repeat('1',32),repeat('1',32),1,current_setting('p5b2b.a_enrollment')::BIGINT,
    current_setting('p5b2b.leaf_a_issuer'),current_setting('p5b2b.leaf_a_serial'),
    current_setting('p5b2b.leaf_a_fp'),repeat('3',32),current_setting('p5b2b.team_a')::BIGINT,
    current_setting('p5b2b.issuer'),'kb-node-c',repeat('5',64),repeat('6',64),
    current_setting('p5b2b.binding_c'),current_setting('p5b2b.leaf_a_issuer'),
    current_setting('p5b2b.ca_fp'),'owner:p5b2b-test');
  IF r.replayed OR r.old_instance_state<>'replaced' THEN RAISE EXCEPTION 'replacement result mismatch'; END IF;
  SELECT * INTO r FROM public.kb_management_instance_replacement_grant_create(repeat('c',64),
    repeat('1',32),repeat('1',32),1,current_setting('p5b2b.a_enrollment')::BIGINT,
    current_setting('p5b2b.leaf_a_issuer'),current_setting('p5b2b.leaf_a_serial'),
    current_setting('p5b2b.leaf_a_fp'),repeat('3',32),current_setting('p5b2b.team_a')::BIGINT,
    current_setting('p5b2b.issuer'),'kb-node-c',repeat('5',64),repeat('6',64),
    current_setting('p5b2b.binding_c'),current_setting('p5b2b.leaf_a_issuer'),
    current_setting('p5b2b.ca_fp'),'owner:p5b2b-test');
  IF NOT r.replayed THEN RAISE EXCEPTION 'replacement replay missed'; END IF;
END $$;
RESET ROLE;

DO $$ BEGIN
  IF (SELECT generation FROM public.kb_cert_revocation_generation WHERE singleton=1)<>
       current_setting('p5b2b.rev_before')::BIGINT+1 OR
     (SELECT state FROM public.kb_management_instance WHERE installation_id=repeat('1',32))<>'replaced' OR
     (SELECT replacement_lineage_id FROM public.kb_management_instance_grant WHERE installation_id=repeat('3',32))<>repeat('1',32) THEN
    RAISE EXCEPTION 'replacement lineage/revocation mismatch';
  END IF;
END $$;

-- Activate C, then replace it with D to prove a three-hop A -> C -> D chain
-- retains A's immutable lineage root.
SET ROLE aimee_kb_runtime;
SELECT * FROM public.kb_management_instance_begin_initial(repeat('e',64),repeat('3',32),
  repeat('3',32),repeat('1',32),current_setting('p5b2b.issuer'),'kb-node-c',repeat('5',64),repeat('6',64),
  current_setting('p5b2b.binding_c'),repeat('7',64),repeat('9',64));
SELECT * FROM public.kb_management_instance_activate(repeat('e',64),repeat('3',32),
  current_setting('p5b2b.issuer'),'kb-node-c',repeat('5',64),repeat('6',64),
  current_setting('p5b2b.binding_c'),'initial',1,NULL::BIGINT,NULL::TEXT,NULL::TEXT,NULL::TEXT,
  repeat('7',64),repeat('9',64),repeat('b',64),current_setting('p5b2b.leaf_a_issuer'),
  current_setting('p5b2b.ca_fp'),current_setting('p5b2b.leaf_a_issuer'),'c1',repeat('c',64),
  repeat('9',64),current_setting('p5b2b.leaf_a_nb')::BIGINT,
  current_setting('p5b2b.leaf_a_na')::BIGINT);
RESET ROLE;
SELECT set_config('p5b2b.c_enrollment',(SELECT current_enrollment_id::TEXT
  FROM public.kb_management_instance WHERE installation_id=repeat('3',32)),false);
SET ROLE aimee_kb_migrate;
SELECT * FROM public.kb_management_instance_replacement_grant_create(repeat('f',64),
  repeat('1',32),repeat('3',32),1,current_setting('p5b2b.c_enrollment')::BIGINT,
  current_setting('p5b2b.leaf_a_issuer'),'c1',repeat('c',64),repeat('7',32),
  current_setting('p5b2b.team_a')::BIGINT,current_setting('p5b2b.issuer'),'kb-node-d',
  repeat('b',64),repeat('d',64),public.kb_management_instance_binding_digest(
    current_setting('p5b2b.issuer'),'kb-node-d',repeat('b',64),repeat('d',64)),
  current_setting('p5b2b.leaf_a_issuer'),current_setting('p5b2b.ca_fp'),'owner:p5b2b-test');
RESET ROLE;
DO $$ BEGIN
  IF (SELECT replacement_lineage_id FROM public.kb_management_instance_grant
       WHERE installation_id=repeat('7',32))<>repeat('1',32) OR
     (SELECT replaces_installation_id FROM public.kb_management_instance_grant
       WHERE installation_id=repeat('7',32))<>repeat('3',32) THEN
    RAISE EXCEPTION 'three-hop replacement lineage mismatch';
  END IF;
END $$;

-- Once C has a successor, a distinct operation cannot fork the lineage.
SET ROLE aimee_kb_migrate;
DO $$ BEGIN
  BEGIN
    PERFORM * FROM public.kb_management_instance_replacement_grant_create(repeat('d',64),
      repeat('1',32),repeat('3',32),1,current_setting('p5b2b.c_enrollment')::BIGINT,
      current_setting('p5b2b.leaf_a_issuer'),'c1',repeat('c',64),repeat('6',32),
      current_setting('p5b2b.team_a')::BIGINT,
      current_setting('p5b2b.issuer'),'kb-node-f',repeat('7',64),repeat('8',64),
      public.kb_management_instance_binding_digest(current_setting('p5b2b.issuer'),
        'kb-node-f',repeat('7',64),repeat('8',64)),current_setting('p5b2b.leaf_a_issuer'),
      current_setting('p5b2b.ca_fp'),'owner:p5b2b-test');
    RAISE EXCEPTION 'replacement lineage fork accepted';
  EXCEPTION WHEN invalid_authorization_specification OR unique_violation THEN NULL; END;
END $$;
RESET ROLE;

-- Bounded expiry processes no more than the caller limit.
SET ROLE aimee_kb_migrate;
SELECT * FROM public.kb_management_instance_grant_create(repeat('4',32),
  current_setting('p5b2b.team_b')::BIGINT,'spiffe://p5b2b.expire','node-d',repeat('7',64),repeat('8',64),
  public.kb_management_instance_binding_digest('spiffe://p5b2b.expire','node-d',repeat('7',64),repeat('8',64)),
  '/CN=p5b2b-ca',current_setting('p5b2b.ca_fp'),'owner:p5b2b-test');
SELECT * FROM public.kb_management_instance_grant_create(repeat('5',32),
  current_setting('p5b2b.team_b')::BIGINT,'spiffe://p5b2b.expire','node-e',repeat('9',64),repeat('a',64),
  public.kb_management_instance_binding_digest('spiffe://p5b2b.expire','node-e',repeat('9',64),repeat('a',64)),
  '/CN=p5b2b-ca',current_setting('p5b2b.ca_fp'),'owner:p5b2b-test');
RESET ROLE;
SET LOCAL session_replication_role=replica;
UPDATE public.kb_management_instance_grant SET created_at=now()-interval '2 days',expires_at=now()-interval '1 day'
 WHERE installation_id IN (repeat('4',32),repeat('5',32));
SET LOCAL session_replication_role=origin;
SET ROLE aimee_kb_runtime;
DO $$
DECLARE r RECORD;
BEGIN
  BEGIN PERFORM * FROM public.kb_management_instance_expire_quarantine(0);
    RAISE EXCEPTION 'unbounded maintenance accepted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  SELECT * INTO r FROM public.kb_management_instance_expire_quarantine(1);
  IF r.expired_grants<>1 OR r.expired_issues<>0 OR r.quarantined_issues<>0 THEN
    RAISE EXCEPTION 'bounded expiry result mismatch';
  END IF;
END $$;
RESET ROLE;
DO $$ BEGIN
  IF (SELECT count(*) FROM public.kb_management_instance_grant
       WHERE installation_id IN (repeat('4',32),repeat('5',32)) AND state='expired')<>1 THEN
    RAISE EXCEPTION 'expiry limit exceeded';
  END IF;
END $$;

SELECT 'p5b2b_management_instance_pg: ok';
ROLLBACK;
