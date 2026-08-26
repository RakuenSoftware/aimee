DO $$ BEGIN
  IF (SELECT count(*) FROM pg_catalog.pg_constraint
       WHERE conrelid='public.kb_management_read_intent'::pg_catalog.regclass
         AND conname IN ('preserve_selector_capability','preserve_path_method'))<>2 OR
     (SELECT count(*) FROM pg_catalog.pg_constraint
       WHERE conrelid='public.kb_management_read_key_use'::pg_catalog.regclass
         AND conname='preserve_key_use_selector_team')<>1 THEN
    RAISE EXCEPTION 'P5-D2b migration removed an unrelated local CHECK';
  END IF;
END $$;

INSERT INTO public.kb_team_membership(identity_key,team,is_default)
VALUES('cert:/CN=p5c2d-local-ca:11',97522,0);

SET LOCAL ROLE aimee_kb_status;
DO $$ DECLARE r RECORD; BEGIN
  SELECT * INTO STRICT r FROM public.kb_management_status_lookup(
    '/CN=p5c2d-local-ca','11',repeat('1',64),'srv-p5c2d','management.read.config.v1');
  IF r.revocation_generation<1 OR r.target_mgmt_fingerprint<>repeat('2',64) THEN
    RAISE EXCEPTION 'P5-D2b config-purpose status admission mismatch';
  END IF;
END $$;
RESET ROLE;

SET LOCAL ROLE aimee_kb_runtime;
DO $$ DECLARE r RECORD; BEGIN
  PERFORM set_config('aimee.principal','oidc:https%3A%25issuer:p5c2d-lead',true);
  PERFORM set_config('aimee.team','97522',true);
  SELECT * INTO STRICT r FROM public.kb_management_read_intent_start(
    repeat('f',64),repeat('e',64),97522,'srv-p5c2d','config','GET',
    '/v1/servers/srv-p5c2d/config',decode(repeat('02',32),'hex'),repeat('d',64),
    'https://kb.p5c2d.test',60,repeat('3',32));
  IF r.target_server_id<>'srv-p5c2d' OR r.publication_generation<>1 THEN
    RAISE EXCEPTION 'P5-D2b frozen config intent mismatch';
  END IF;
  BEGIN
    PERFORM * FROM public.kb_management_read_intent_start(
      repeat('1',64),repeat('2',64),97522,'srv-p5c2d','config','GET',
      '/v1/servers/srv-p5c2d/agents',decode(repeat('03',32),'hex'),repeat('4',64),
      'https://kb.p5c2d.test',60,repeat('3',32));
    RAISE EXCEPTION 'P5-D2b selector-confused path admitted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  BEGIN
    PERFORM * FROM public.kb_management_read_intent_start(
      repeat('5',64),repeat('6',64),97522,'srv-p5c2d','secrets','GET',
      '/v1/servers/srv-p5c2d/secrets',decode(repeat('04',32),'hex'),repeat('7',64),
      'https://kb.p5c2d.test',60,repeat('3',32));
    RAISE EXCEPTION 'P5-D2b unknown selector admitted';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
RESET ROLE;

SET LOCAL ROLE aimee_kb_token_authority_runtime;
DO $$ DECLARE r RECORD; h TEXT; p TEXT; s TEXT; jwt TEXT; BEGIN
  SELECT * INTO STRICT r FROM public.kb_management_read_authority_claim(
    repeat('f',64),repeat('e',64),repeat('c',64),5);
  IF NOT r.newly_admitted OR r.capability<>'remote_reads' THEN
    RAISE EXCEPTION 'P5-D2b authority claim mismatch';
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
  IF NOT public.kb_management_read_authority_finalize(
      repeat('f',64),repeat('e',64),repeat('c',64),jwt) THEN
    RAISE EXCEPTION 'P5-D2b finalize failed';
  END IF;
END $$;
RESET ROLE;

DO $$ BEGIN
  IF (SELECT count(*) FROM public.kb_management_read_key_use
       WHERE correlation_id=repeat('f',64) AND selector='config' AND result_status='issued')<>1 OR
     (SELECT count(*) FROM public.kb_audit_outbox
       WHERE action='vault.key_use' AND actor_principal='management-token-authority'
         AND detail LIKE '%"selector" : "config"%')<>1 THEN
    RAISE EXCEPTION 'P5-D2b config key-use/audit cardinality mismatch';
  END IF;
END $$;

ROLLBACK;
\echo 'P5-D2b safe config PostgreSQL 17 gate passed'
