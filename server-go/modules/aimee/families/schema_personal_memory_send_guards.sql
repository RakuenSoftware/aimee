-- BEGIN memory send guards
-- A durable, bounded send lease survives owner/connection loss. Mutations take
-- a shared row lock; acquisition takes an exclusive row lock before its source
-- checks. The host must finish its write within a shorter monotonic budget,
-- measured BEFORE requesting either owner's lease.
CREATE TABLE IF NOT EXISTS memory_send_barrier (
 id INTEGER PRIMARY KEY CHECK(id=1),
 blocked_until TIMESTAMPTZ NOT NULL DEFAULT '-infinity'
);
INSERT INTO memory_send_barrier(id) VALUES(1) ON CONFLICT DO NOTHING;
CREATE TABLE IF NOT EXISTS memory_send_leases (
 token TEXT PRIMARY KEY CHECK(token ~ '^[a-f0-9]{32}$'),
 expires_at TIMESTAMPTZ NOT NULL
);
DO $install_send_guard$
DECLARE schema_name TEXT:=current_schema(); recipient record; role_name TEXT;
BEGIN
 EXECUTE format($ddl$
 CREATE OR REPLACE FUNCTION %1$I.memory_send_guard_begin(p_token TEXT,p_duration_ms INTEGER)
 RETURNS void LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog,%1$I,pg_temp AS $body$
 DECLARE lease_until TIMESTAMPTZ;
 BEGIN
  IF p_token IS NULL OR p_token !~ '^[a-f0-9]{32}$' OR p_duration_ms IS DISTINCT FROM 5000 THEN
   RAISE EXCEPTION 'invalid memory send guard' USING ERRCODE='22023';
  END IF;
  PERFORM set_config('synchronous_commit','on',true);
  PERFORM id FROM memory_send_barrier WHERE id=1 FOR UPDATE;
  IF NOT FOUND THEN RAISE EXCEPTION 'memory send guard unavailable'; END IF;
  DELETE FROM memory_send_leases WHERE expires_at<=clock_timestamp();
  IF (SELECT count(*) FROM memory_send_leases)>=64 THEN
   RAISE EXCEPTION 'memory send guard capacity' USING ERRCODE='55P03';
  END IF;
  lease_until:=clock_timestamp()+interval '5 seconds';
  INSERT INTO memory_send_leases(token,expires_at) VALUES(p_token,lease_until);
  UPDATE memory_send_barrier SET blocked_until=greatest(blocked_until,lease_until) WHERE id=1;
 END $body$;
 $ddl$,schema_name);
 EXECUTE format($ddl$
 CREATE OR REPLACE FUNCTION %1$I.memory_send_guard_end(p_token TEXT)
 RETURNS void LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog,%1$I,pg_temp AS $body$
 BEGIN
  IF p_token IS NULL OR p_token !~ '^[a-f0-9]{32}$' THEN
   RAISE EXCEPTION 'invalid memory send guard' USING ERRCODE='22023';
  END IF;
  PERFORM id FROM memory_send_barrier WHERE id=1 FOR UPDATE;
  IF NOT FOUND THEN RAISE EXCEPTION 'memory send guard unavailable'; END IF;
  DELETE FROM memory_send_leases WHERE token=p_token;
  UPDATE memory_send_barrier SET blocked_until=coalesce(
   (SELECT max(expires_at) FROM memory_send_leases),'-infinity'::timestamptz) WHERE id=1;
 END $body$;
 $ddl$,schema_name);
 EXECUTE format($ddl$
 CREATE OR REPLACE FUNCTION %1$I.memory_send_mutation_guard()
 RETURNS trigger LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog,%1$I,pg_temp AS $body$
 DECLARE until_at TIMESTAMPTZ;
 BEGIN
  -- Usage counters do not alter source versions or eligibility. Keep ordinary
  -- recall accounting available while a provider write is guarded.
  IF TG_OP='UPDATE' AND
   (to_jsonb(NEW)-ARRAY['use_count','last_used_at','updated_at','surfaced_count','last_surfaced_at','trigger_count','last_triggered_at',TG_TABLE_NAME||'_fts_tsv']) =
   (to_jsonb(OLD)-ARRAY['use_count','last_used_at','updated_at','surfaced_count','last_surfaced_at','trigger_count','last_triggered_at',TG_TABLE_NAME||'_fts_tsv']) THEN
   RETURN NEW;
  END IF;
  -- FOR SHARE also detects an obsolete REPEATABLE READ snapshot through a
  -- serialization error after an acquisition changes this singleton row.
  SELECT blocked_until INTO until_at FROM memory_send_barrier WHERE id=1 FOR SHARE;
  IF NOT FOUND THEN RAISE EXCEPTION 'memory send guard unavailable'; END IF;
  IF until_at>clock_timestamp() THEN
   RAISE EXCEPTION 'memory provider send in progress; retry mutation' USING ERRCODE='55P03';
  END IF;
  IF TG_OP='DELETE' THEN RETURN OLD; END IF;
  IF TG_OP='TRUNCATE' THEN RETURN NULL; END IF;
  RETURN NEW;
 END $body$;
 $ddl$,schema_name);
 -- Default grants must not expose another request's opaque release token or
 -- permit clients to shorten the barrier directly.
 FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_class AS relation,
  LATERAL aclexplode(relation.relacl) AS acl
  WHERE relation.oid IN ('memory_send_barrier'::regclass,'memory_send_leases'::regclass)
   AND acl.grantee<>relation.relowner
 LOOP
  role_name:=CASE WHEN recipient.grantee=0 THEN 'PUBLIC'
   ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
  EXECUTE format('REVOKE ALL ON TABLE memory_send_barrier,memory_send_leases FROM %s',role_name);
  IF recipient.grantee<>0 THEN
   EXECUTE format('GRANT EXECUTE ON FUNCTION memory_send_guard_begin(TEXT,INTEGER),memory_send_guard_end(TEXT) TO %s',role_name);
  END IF;
 END LOOP;
 REVOKE ALL ON TABLE memory_send_barrier,memory_send_leases FROM PUBLIC;
 REVOKE ALL ON FUNCTION memory_send_guard_begin(TEXT,INTEGER),memory_send_guard_end(TEXT),memory_send_mutation_guard() FROM PUBLIC;
 IF EXISTS(SELECT 1 FROM pg_roles WHERE rolname='aimee_store_runtime') THEN
  GRANT EXECUTE ON FUNCTION memory_send_guard_begin(TEXT,INTEGER),memory_send_guard_end(TEXT) TO aimee_store_runtime;
 END IF;
END $install_send_guard$;
DROP TRIGGER IF EXISTS memory_send_guard ON user_memories;
CREATE TRIGGER memory_send_guard BEFORE INSERT OR UPDATE OR DELETE ON user_memories FOR EACH ROW EXECUTE FUNCTION memory_send_mutation_guard();
DROP TRIGGER IF EXISTS memory_send_truncate_guard ON user_memories;
CREATE TRIGGER memory_send_truncate_guard BEFORE TRUNCATE ON user_memories FOR EACH STATEMENT EXECUTE FUNCTION memory_send_mutation_guard();
DROP TRIGGER IF EXISTS memory_send_guard ON user_memory_collection_generation;
CREATE TRIGGER memory_send_guard BEFORE INSERT OR UPDATE OR DELETE ON user_memory_collection_generation FOR EACH ROW EXECUTE FUNCTION memory_send_mutation_guard();
DROP TRIGGER IF EXISTS memory_send_truncate_guard ON user_memory_collection_generation;
CREATE TRIGGER memory_send_truncate_guard BEFORE TRUNCATE ON user_memory_collection_generation FOR EACH STATEMENT EXECUTE FUNCTION memory_send_mutation_guard();
-- END memory send guards
