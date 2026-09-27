-- Retained revisions belong to the same private owner as user_memories. They
-- are canonical history, never an independent recall source or public outbox.
-- Existing rows acquire history on their next governed change; migration must
-- not invent earlier content or authorship.
CREATE TABLE user_memory_versions (
  memory_id BIGINT NOT NULL REFERENCES user_memories(id) ON DELETE CASCADE,
  record_revision BIGINT NOT NULL CHECK (record_revision > 0),
  record JSONB NOT NULL CHECK (jsonb_typeof(record)='object'),
  retained_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY(memory_id,record_revision)
);

CREATE FUNCTION user_memory_retain_version() RETURNS trigger
LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog AS $$
BEGIN
  IF NEW.record_revision <> OLD.record_revision THEN
    EXECUTE format('INSERT INTO %I.user_memory_versions(memory_id,record_revision,record)
      VALUES($1,$2,$3)',TG_TABLE_SCHEMA)
      USING OLD.id,OLD.record_revision,
        to_jsonb(OLD)-ARRAY['use_count','last_used_at','updated_at'];
  END IF;
  RETURN NEW;
END $$;
REVOKE ALL ON FUNCTION user_memory_retain_version() FROM PUBLIC;

CREATE TRIGGER user_memory_retain_version
  AFTER UPDATE OF id,kind,tier,key,content,confidence,lifecycle_state,valid_until,
    source_session,created_at,record_revision ON user_memories
  FOR EACH ROW EXECUTE FUNCTION user_memory_retain_version();

-- A legacy in-place writer also retains history. Runtime roles cannot rewrite
-- or fabricate it, and a hard parent erasure removes every retained payload.
-- Apply this to actual default-grant recipients, not just PUBLIC.
DO $personal_version_acl$
DECLARE recipient record; role_name TEXT;
BEGIN
  FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_proc AS routine,
    LATERAL aclexplode(routine.proacl) AS acl
    WHERE routine.oid='user_memory_retain_version()'::regprocedure
      AND acl.grantee<>routine.proowner
  LOOP
    role_name:=CASE WHEN recipient.grantee=0 THEN 'PUBLIC'
      ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
    EXECUTE format('REVOKE ALL ON FUNCTION user_memory_retain_version() FROM %s',role_name);
  END LOOP;
  FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_class AS relation,
    LATERAL aclexplode(relation.relacl) AS acl
    WHERE relation.oid='user_memory_versions'::regclass AND acl.grantee<>relation.relowner
  LOOP
    role_name:=CASE WHEN recipient.grantee=0 THEN 'PUBLIC'
      ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
    EXECUTE format('REVOKE ALL ON TABLE user_memory_versions FROM %s',role_name);
    IF recipient.grantee<>0 THEN
      EXECUTE format('GRANT SELECT ON TABLE user_memory_versions TO %s',role_name);
    END IF;
  END LOOP;
END
$personal_version_acl$;
