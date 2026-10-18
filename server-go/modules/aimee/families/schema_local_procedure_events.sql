-- Append-only migration 46. Server-local procedure observations, independent of KB.
-- Learning owns admission/projection policy; DB1 owns immutable rows and privacy.
CREATE TABLE local_procedure_events (
  principal TEXT NOT NULL CHECK (length(principal) BETWEEN 1 AND 600),
  event_id TEXT NOT NULL CHECK (length(event_id) BETWEEN 1 AND 256),
  procedure JSONB NOT NULL CHECK (jsonb_typeof(procedure)='object'),
  event JSONB NOT NULL CHECK (jsonb_typeof(event)='object'),
  exposure JSONB NOT NULL CHECK (jsonb_typeof(exposure)='object'),
  scope_kind TEXT NOT NULL CHECK (scope_kind IN ('global','project','workspace')),
  scope_id TEXT NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (principal,event_id)
);
CREATE INDEX local_procedure_events_cohort ON local_procedure_events(principal,(procedure::text));

-- Surviving payload-free erasure intent also fences delayed writers/restores.
CREATE TABLE local_procedure_erasure_intents (
 event_digest TEXT PRIMARY KEY CHECK(event_digest ~ '^[0-9a-f]{64}$')
);
CREATE FUNCTION local_procedure_event_guard() RETURNS trigger
LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog AS $$
DECLARE observed BIGINT; prohibited BOOLEAN;
BEGIN
 IF TG_OP='UPDATE' AND NEW IS DISTINCT FROM OLD THEN
  RAISE EXCEPTION 'procedure observations are immutable' USING ERRCODE='23514';
 END IF;
 EXECUTE format('SELECT generation FROM %I.user_memory_erasure_epoch WHERE id=1 FOR SHARE',TG_TABLE_SCHEMA) INTO STRICT observed;
 EXECUTE format('SELECT EXISTS(SELECT 1 FROM %I.local_procedure_erasure_intents WHERE event_digest=$1)',TG_TABLE_SCHEMA)
 INTO prohibited USING encode(sha256(convert_to(jsonb_build_array(NEW.principal,NEW.event_id)::text,'UTF8')),'hex');
 IF prohibited THEN RAISE EXCEPTION 'procedure observation restoration prohibited by surviving erasure intent' USING ERRCODE='23514'; END IF;
 RETURN NEW;
END $$;
CREATE TRIGGER local_procedure_event_guard BEFORE INSERT OR UPDATE ON local_procedure_events
 FOR EACH ROW EXECUTE FUNCTION local_procedure_event_guard();

DO $local_procedure_erasure$
BEGIN
 EXECUTE format($definition$
 CREATE FUNCTION %1$I.local_procedure_prepare_subject_erasure(p_principal TEXT) RETURNS BIGINT
 LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog,%1$I AS $body$
 DECLARE removed BIGINT;
 BEGIN
  IF p_principal IS NULL OR length(p_principal) NOT BETWEEN 1 AND 600 THEN
   RAISE EXCEPTION 'invalid erasure principal' USING ERRCODE='22023';
  END IF;
  UPDATE %1$I.user_memory_erasure_epoch SET generation=generation+1 WHERE id=1;
  INSERT INTO %1$I.local_procedure_erasure_intents(event_digest)
   SELECT encode(sha256(convert_to(jsonb_build_array(principal,event_id)::text,'UTF8')),'hex')
   FROM %1$I.local_procedure_events WHERE principal=p_principal ON CONFLICT DO NOTHING;
  DELETE FROM %1$I.local_procedure_events WHERE principal=p_principal;
  GET DIAGNOSTICS removed=ROW_COUNT; RETURN removed;
 END $body$;
 $definition$,current_schema());
 EXECUTE format($definition$
 CREATE FUNCTION %1$I.local_procedure_replay_erasure_intents() RETURNS BIGINT
 LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog,%1$I AS $body$
 DECLARE removed BIGINT;
 BEGIN
  PERFORM generation FROM %1$I.user_memory_erasure_epoch WHERE id=1 FOR UPDATE;
  DELETE FROM %1$I.local_procedure_events e WHERE EXISTS(
   SELECT 1 FROM %1$I.local_procedure_erasure_intents i
   WHERE i.event_digest=encode(sha256(convert_to(jsonb_build_array(e.principal,e.event_id)::text,'UTF8')),'hex'));
  GET DIAGNOSTICS removed=ROW_COUNT; RETURN removed;
 END $body$;
 $definition$,current_schema());
END $local_procedure_erasure$;
REVOKE ALL ON local_procedure_erasure_intents FROM PUBLIC;
REVOKE ALL ON FUNCTION local_procedure_event_guard(),local_procedure_prepare_subject_erasure(text),local_procedure_replay_erasure_intents() FROM PUBLIC;
DO $local_procedure_acl$
DECLARE recipient RECORD; role_name TEXT; target TEXT;
BEGIN
 FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_class relation,LATERAL aclexplode(relation.relacl) acl
 WHERE relation.oid='local_procedure_erasure_intents'::regclass AND acl.grantee<>relation.relowner LOOP
  role_name:=CASE WHEN recipient.grantee=0 THEN 'PUBLIC' ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
  EXECUTE format('REVOKE ALL ON local_procedure_erasure_intents FROM %s',role_name);
 END LOOP;
 FOREACH target IN ARRAY ARRAY['local_procedure_event_guard()','local_procedure_prepare_subject_erasure(text)','local_procedure_replay_erasure_intents()'] LOOP
  FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_proc routine,LATERAL aclexplode(routine.proacl) acl
   WHERE routine.oid=target::regprocedure AND acl.grantee<>routine.proowner LOOP
   role_name:=CASE WHEN recipient.grantee=0 THEN 'PUBLIC' ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
   EXECUTE format('REVOKE ALL ON FUNCTION %s FROM %s',target,role_name);
  END LOOP;
 END LOOP;
 FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_class relation,LATERAL aclexplode(relation.relacl) acl
 WHERE relation.oid='server_sessions'::regclass AND acl.privilege_type='DELETE' AND acl.grantee<>0 AND acl.grantee<>relation.relowner LOOP
  role_name:=quote_ident(pg_get_userbyid(recipient.grantee));
  EXECUTE format('REVOKE UPDATE,DELETE,TRUNCATE,TRIGGER ON local_procedure_events FROM %s',role_name);
  EXECUTE format('GRANT SELECT,INSERT ON local_procedure_events TO %s',role_name);
  EXECUTE format('GRANT EXECUTE ON FUNCTION local_procedure_prepare_subject_erasure(text),local_procedure_replay_erasure_intents() TO %s',role_name);
 END LOOP;
END $local_procedure_acl$;
