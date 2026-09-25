-- Independently retained control metadata; exclude it from content-only restores.
-- No erased payload or raw session identifier is retained here.
CREATE TABLE user_memory_erasure_epoch (
 id INTEGER PRIMARY KEY CHECK(id=1), generation BIGINT NOT NULL DEFAULT 0
);
INSERT INTO user_memory_erasure_epoch(id) VALUES(1);
CREATE TABLE user_memory_erasure_intents (
 memory_id BIGINT NOT NULL,
 payload_digest TEXT NOT NULL CHECK(payload_digest ~ '^[0-9a-f]{64}$'),
 erased_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
 PRIMARY KEY(memory_id,payload_digest)
);
CREATE INDEX user_memory_erasure_digest_idx ON user_memory_erasure_intents(payload_digest);
CREATE TABLE user_memory_erasure_sessions (
 session_digest TEXT PRIMARY KEY CHECK(session_digest ~ '^[0-9a-f]{64}$')
);
CREATE FUNCTION user_memory_erasure_restore_guard() RETURNS trigger
LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog AS $$
DECLARE prohibited BOOLEAN; observed BIGINT;
BEGIN
 EXECUTE format('SELECT generation FROM %I.user_memory_erasure_epoch WHERE id=1 FOR SHARE',TG_TABLE_SCHEMA) INTO STRICT observed;
 EXECUTE format('SELECT EXISTS(SELECT 1 FROM %I.user_memory_erasure_intents
 WHERE memory_id=$1 OR payload_digest=$2) OR EXISTS(SELECT 1 FROM %I.user_memory_erasure_sessions WHERE session_digest=$3)',TG_TABLE_SCHEMA,TG_TABLE_SCHEMA)
 INTO prohibited USING NEW.id,encode(sha256(convert_to(NEW.content,'UTF8')),'hex'),
   encode(sha256(convert_to(NEW.source_session,'UTF8')),'hex');
 IF prohibited THEN RAISE EXCEPTION 'private memory restoration prohibited by surviving erasure intent'
   USING ERRCODE='23514'; END IF;
 RETURN NEW;
END $$;
CREATE TRIGGER user_memory_erasure_restore_guard BEFORE INSERT OR UPDATE ON user_memories
 FOR EACH ROW EXECUTE FUNCTION user_memory_erasure_restore_guard();
REVOKE ALL ON user_memory_erasure_intents,user_memory_erasure_sessions,user_memory_erasure_epoch FROM PUBLIC;
REVOKE ALL ON FUNCTION user_memory_erasure_restore_guard() FROM PUBLIC;
DO $private_erasure_acl$
DECLARE recipient RECORD; role_name TEXT;
BEGIN
 FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_class relation,
   LATERAL aclexplode(relation.relacl) acl
   WHERE relation.oid IN ('user_memory_erasure_intents'::regclass,'user_memory_erasure_sessions'::regclass,'user_memory_erasure_epoch'::regclass)
     AND acl.grantee<>relation.relowner
 LOOP
  role_name:=CASE WHEN recipient.grantee=0 THEN 'PUBLIC' ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
  EXECUTE format('REVOKE ALL ON user_memory_erasure_intents,user_memory_erasure_sessions,user_memory_erasure_epoch FROM %s',role_name);
 END LOOP;
 FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_proc routine,
   LATERAL aclexplode(routine.proacl) acl
   WHERE routine.oid='user_memory_erasure_restore_guard()'::regprocedure AND acl.grantee<>routine.proowner
 LOOP
  role_name:=CASE WHEN recipient.grantee=0 THEN 'PUBLIC' ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
  EXECUTE format('REVOKE ALL ON FUNCTION user_memory_erasure_restore_guard() FROM %s',role_name);
 END LOOP;
END $private_erasure_acl$;
