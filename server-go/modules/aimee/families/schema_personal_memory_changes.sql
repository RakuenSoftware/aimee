-- Storage mechanics for the Go memory owner's private placement. Keep these
-- separate from shared-KB tables, including when both schemas share a database.
-- This is an invalidation stream, not a second audit or content history store.
ALTER TABLE user_memories ADD COLUMN record_revision BIGINT NOT NULL DEFAULT 1
  CHECK (record_revision > 0);

CREATE TABLE user_memory_collection_generation (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  owner_id UUID NOT NULL DEFAULT gen_random_uuid(),
  generation BIGINT NOT NULL DEFAULT 0 CHECK (generation >= 0)
);
INSERT INTO user_memory_collection_generation(id) VALUES (1);

CREATE TABLE user_memory_invalidation_outbox (
  generation BIGINT PRIMARY KEY CHECK (generation > 0),
  memory_id BIGINT NOT NULL,
  record_revision BIGINT NOT NULL CHECK (record_revision > 0),
  operation TEXT NOT NULL CHECK (operation IN ('insert','update','delete')),
  recorded_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- Updating the generation row serializes change positions in commit order.
-- A sequence alone would let a later position commit before an earlier one,
-- allowing a consumer to skip the late commit permanently. Rollback restores
-- both the generation and its event. Old content is never copied to the outbox.
CREATE FUNCTION user_memory_capture_change() RETURNS trigger
LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog AS $$
DECLARE position BIGINT; revision BIGINT; target BIGINT;
BEGIN
  IF TG_OP = 'UPDATE' THEN
    IF NEW.id <> OLD.id THEN
      RAISE EXCEPTION 'memory identity is immutable';
    END IF;
    IF (to_jsonb(OLD) - ARRAY['record_revision','use_count','last_used_at','updated_at'])
       IS NOT DISTINCT FROM
       (to_jsonb(NEW) - ARRAY['record_revision','use_count','last_used_at','updated_at']) THEN
      NEW.record_revision := OLD.record_revision;
      RETURN NEW;
    END IF;
    NEW.record_revision := OLD.record_revision + 1;
  ELSIF TG_OP = 'INSERT' THEN
    NEW.record_revision := 1;
  END IF;

  target := CASE WHEN TG_OP = 'DELETE' THEN OLD.id ELSE NEW.id END;
  revision := CASE WHEN TG_OP = 'DELETE' THEN OLD.record_revision + 1 ELSE NEW.record_revision END;
  -- The trigger relation supplies the namespace, never caller input. Fully
  -- qualified names also prevent a runtime temporary table from shadowing the
  -- protected generation or outbox when this function runs as its owner.
  EXECUTE format('UPDATE %I.user_memory_collection_generation SET generation = generation + 1
    WHERE id = 1 RETURNING generation', TG_TABLE_SCHEMA) INTO STRICT position;
  EXECUTE format('INSERT INTO %I.user_memory_invalidation_outbox(generation,memory_id,record_revision,operation)
    VALUES($1,$2,$3,$4)', TG_TABLE_SCHEMA) USING position,target,revision,lower(TG_OP);
  RETURN CASE WHEN TG_OP = 'DELETE' THEN OLD ELSE NEW END;
END $$;
REVOKE ALL ON FUNCTION user_memory_capture_change() FROM PUBLIC;

CREATE TRIGGER user_memory_capture_change
  BEFORE INSERT OR DELETE OR UPDATE OF id,kind,tier,key,content,confidence,
    lifecycle_state,valid_until,source_session,created_at,record_revision ON user_memories
  FOR EACH ROW EXECUTE FUNCTION user_memory_capture_change();

-- Counter-only reads never invoke the trigger or serialize memory content.
-- New governed columns must join this attribute list in their migration; the
-- owner conformance test checks the installed catalog for omitted columns.

-- Deployment defaults grant runtime access to ordinary application tables.
-- Readers may inspect change positions, but only the trigger owner may advance
-- or erase them. Preserve read grants for existing non-public recipients.
DO $personal_change_acl$
DECLARE recipient record; target record; role_name TEXT;
BEGIN
  FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_proc AS routine,
    LATERAL aclexplode(routine.proacl) AS acl
    WHERE routine.oid='user_memory_capture_change()'::regprocedure
      AND acl.grantee<>routine.proowner
  LOOP
    role_name := CASE WHEN recipient.grantee=0 THEN 'PUBLIC'
      ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
    EXECUTE format('REVOKE ALL ON FUNCTION user_memory_capture_change() FROM %s',role_name);
  END LOOP;
  FOR target IN SELECT oid,relname,relowner FROM pg_class
    WHERE oid IN ('user_memory_collection_generation'::regclass,
                  'user_memory_invalidation_outbox'::regclass)
  LOOP
    FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_class AS relation,
      LATERAL aclexplode(relation.relacl) AS acl
      WHERE relation.oid=target.oid AND acl.grantee<>target.relowner
    LOOP
      role_name := CASE WHEN recipient.grantee=0 THEN 'PUBLIC'
        ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
      EXECUTE format('REVOKE ALL ON TABLE %I FROM %s',target.relname,role_name);
      IF recipient.grantee<>0 THEN
        EXECUTE format('GRANT SELECT ON TABLE %I TO %s',target.relname,role_name);
      END IF;
    END LOOP;
  END LOOP;
END
$personal_change_acl$;
