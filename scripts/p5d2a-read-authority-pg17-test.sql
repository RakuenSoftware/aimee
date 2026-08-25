INSERT INTO public.kb_team_membership(identity_key,team,is_default)
VALUES('cert:/CN=p5c2d-local-ca:11',97522,0);

SET LOCAL ROLE aimee_kb_status;
DO $$ DECLARE r RECORD; BEGIN
  SELECT * INTO STRICT r FROM public.kb_management_status_lookup(
    '/CN=p5c2d-local-ca','11',repeat('1',64),'srv-p5c2d','management.read.v1');
  IF r.revocation_generation<1 OR r.target_mgmt_fingerprint<>repeat('2',64) THEN
    RAISE EXCEPTION 'P5-D2a read-purpose status admission mismatch';
  END IF;
END $$;
RESET ROLE;

SET LOCAL ROLE aimee_kb_runtime;
DO $$ DECLARE r RECORD; BEGIN
  IF public.kb_management_read_publication_generation()<>1 THEN
    RAISE EXCEPTION 'P5-D2a authoritative publication generation mismatch';
  END IF;
  PERFORM set_config('aimee.principal','oidc:https%3A%25issuer:p5c2d-lead',true);
  PERFORM set_config('aimee.team','97522',true);
  SELECT * INTO STRICT r FROM public.kb_management_read_intent_start(
    repeat('9',64),repeat('a',64),97522,'srv-p5c2d','agents','GET',
    '/v1/servers/srv-p5c2d/agents',decode(repeat('01',32),'hex'),repeat('b',64),
    'https://kb.p5c2d.test',60,repeat('3',32));
  IF r.target_server_id<>'srv-p5c2d' OR r.publication_generation<>1 OR
     r.local_cert_serial_norm<>'11' OR r.target_mgmt_serial_norm<>'22' THEN
    RAISE EXCEPTION 'P5-D2a frozen intent mismatch';
  END IF;
END $$;
RESET ROLE;

SET LOCAL ROLE aimee_kb_status;
DO $$ BEGIN
  BEGIN
    PERFORM public.kb_management_read_publication_generation();
    RAISE EXCEPTION 'status role read publication generation';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
END $$;
RESET ROLE;

SET LOCAL ROLE aimee_kb_token_authority_runtime;
DO $$ DECLARE r RECORD; h TEXT; p TEXT; s TEXT; jwt TEXT; BEGIN
  SELECT * INTO STRICT r FROM public.kb_management_read_authority_claim(
    repeat('9',64),repeat('a',64),repeat('c',64),5);
  IF NOT r.newly_admitted OR r.capability<>'remote_reads' OR
     r.token_version<>2 OR octet_length(r.wrapped_dek)<>40 THEN
    RAISE EXCEPTION 'P5-D2a claim mismatch';
  END IF;
  h:=rtrim(translate(replace(encode(convert_to(
    jsonb_build_object('alg','RS256','typ','JWT','kid',r.kid)::TEXT,'UTF8'),
    'base64'),chr(10),''),'+/','-_'),'=');
  p:=rtrim(translate(replace(encode(convert_to(jsonb_build_object(
    'v',1,'iss',r.token_issuer,'aud',r.audience,'sub',r.actor_identity,'team_id',r.team_id,
    'cap',r.capability,'jti',r.jti,'correlation_id',r.correlation_id,
    'request_sha256',r.request_sha256,'peer_issuer',r.local_cert_issuer,
    'peer_serial',r.local_cert_serial_norm,'peer_fingerprint',r.local_cert_fingerprint,
    'iat',r.issued_at,'exp',r.expires_at)::TEXT,'UTF8'),'base64'),chr(10),''),'+/','-_'),'=');
  s:=rtrim(translate(replace(encode(decode(repeat('00',384),'hex'),'base64'),chr(10),''),
    '+/','-_'),'=');
  jwt:=h||'.'||p||'.'||s;
  BEGIN
    PERFORM public.kb_management_read_authority_finalize(
      repeat('9',64),repeat('a',64),repeat('c',64),h||'.'||p||'.'||'bad');
    RAISE EXCEPTION 'P5-D2a malformed token finalized';
  EXCEPTION WHEN serialization_failure THEN NULL; END;
  IF NOT public.kb_management_read_authority_finalize(
      repeat('9',64),repeat('a',64),repeat('c',64),jwt) THEN
    RAISE EXCEPTION 'P5-D2a finalize failed';
  END IF;
END $$;
RESET ROLE;

SET LOCAL ROLE aimee_kb_runtime;
DO $$ BEGIN
  BEGIN
    PERFORM * FROM public.kb_management_read_token_readback(repeat('9',64),repeat('a',64));
    RAISE EXCEPTION 'ordinary runtime read retained bearer bytes';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  BEGIN
    PERFORM * FROM public.kb_management_read_authority_claim(
      repeat('9',64),repeat('a',64),repeat('d',64),5);
    RAISE EXCEPTION 'ordinary runtime invoked authority claim';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
END $$;
RESET ROLE;

SET LOCAL ROLE aimee_kb_token_authority_runtime;
DO $$ DECLARE r RECORD; BEGIN
  SELECT * INTO STRICT r FROM public.kb_management_read_authority_readback(
    repeat('9',64),repeat('a',64));
  IF r.jwt IS NULL OR r.jwt_sha256<>sha256(convert_to(r.jwt,'UTF8')) THEN
    RAISE EXCEPTION 'P5-D2a retained authority readback mismatch';
  END IF;
END $$;
RESET ROLE;

DO $$ BEGIN
  IF (SELECT count(*) FROM public.kb_management_read_key_use
       WHERE correlation_id=repeat('9',64) AND result_status='issued')<>1 OR
     (SELECT count(*) FROM public.kb_audit_outbox
       WHERE action='vault.key_use' AND actor_principal='management-token-authority'
         AND detail LIKE '%"selector" : "agents"%')<>1 THEN
    RAISE EXCEPTION 'P5-D2a atomic key-use/audit cardinality mismatch';
  END IF;
END $$;

ROLLBACK;
\echo 'P5-D2a read authority PostgreSQL 17 gate passed'
