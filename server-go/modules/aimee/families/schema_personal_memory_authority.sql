-- Existing payloads have no verified author. Never infer user authorship from
-- private placement or backfill it from a later caller.
ALTER TABLE user_memories
 ADD COLUMN provenance_category TEXT NOT NULL DEFAULT 'unknown'
   CHECK(provenance_category IN ('unknown','agent_message','user_stated','reviewed_model')),
 ADD COLUMN author_principal TEXT NOT NULL DEFAULT '',
 ADD COLUMN author_transport TEXT NOT NULL DEFAULT '';

-- Defense for compatibility SQL writers. Go performs actionable admission
-- before issuing writes; the trigger prevents bypass and assigns metadata from
-- the transaction's host-verified context, never from a record payload.
CREATE FUNCTION user_memory_guard_authority() RETURNS trigger
LANGUAGE plpgsql SET search_path=pg_catalog AS $$
DECLARE authority TEXT:=current_setting('aimee.private_authority',true);
        principal TEXT:=COALESCE(current_setting('aimee.private_principal',true),'');
        transport TEXT:=COALESCE(current_setting('aimee.private_transport',true),'');
        authored_change BOOLEAN;
BEGIN
  IF authority='user' AND principal='' THEN authority:=''; END IF;
  IF TG_OP='INSERT' THEN
    IF NEW.provenance_category<>'unknown' OR NEW.author_principal<>'' OR NEW.author_transport<>'' THEN
      RAISE EXCEPTION 'private authorship must come from verified mutation context';
    END IF;
    authored_change:=true;
  ELSE
    IF (NEW.provenance_category,NEW.author_principal,NEW.author_transport)
       IS DISTINCT FROM (OLD.provenance_category,OLD.author_principal,OLD.author_transport) THEN
      RAISE EXCEPTION 'private authorship cannot be assigned by a record payload';
    END IF;
    IF (to_jsonb(NEW)-ARRAY['record_revision','use_count','last_used_at','updated_at'])
       IS NOT DISTINCT FROM (to_jsonb(OLD)-ARRAY['record_revision','use_count','last_used_at','updated_at']) THEN
      RETURN NEW;
    END IF;
    IF COALESCE(authority,'')<>'user' AND OLD.provenance_category<>'agent_message' THEN
      RAISE EXCEPTION 'private authoritative or unknown-origin mutation requires user review';
    END IF;
    IF OLD.lifecycle_state NOT IN ('active','retired','expired') OR
       (COALESCE(authority,'')<>'user' AND OLD.lifecycle_state<>'active') THEN
      RAISE EXCEPTION 'private revoked content requires explicit restoration';
    END IF;
    authored_change:=(NEW.kind,NEW.tier,NEW.key,NEW.content,NEW.confidence)
      IS DISTINCT FROM (OLD.kind,OLD.tier,OLD.key,OLD.content,OLD.confidence);
    IF (authored_change OR COALESCE(authority,'')<>'user') AND OLD.kind IN ('episode','experience','instruction','policy') THEN
      RAISE EXCEPTION 'private protected kind requires annotation or revocation';
    END IF;
  END IF;
  IF authored_change THEN
    IF authority='user' AND principal<>'' THEN
      NEW.provenance_category:='user_stated';
    ELSIF authority='model' THEN
      NEW.provenance_category:='agent_message';
      NEW.confidence:=LEAST(NEW.confidence,CASE WHEN NEW.tier='L5' THEN 0.5 ELSE 0.8 END);
    ELSE
      NEW.provenance_category:='unknown';
      principal:=''; transport:='';
    END IF;
    NEW.author_principal:=principal;
    NEW.author_transport:=transport;
  END IF;
  RETURN NEW;
END $$;
REVOKE ALL ON FUNCTION user_memory_guard_authority() FROM PUBLIC;

-- The guard sorts before revision assignment. Counter-only reads still avoid
-- every content trigger. The history trigger retains the previous authorship.
DO $private_authority_triggers$
DECLARE attributes TEXT; recipient RECORD; role_name TEXT;
BEGIN
 SELECT string_agg(quote_ident(attname),',' ORDER BY attnum) INTO attributes
 FROM pg_attribute WHERE attrelid='user_memories'::regclass AND attnum>0 AND NOT attisdropped
   AND attgenerated='' AND attname NOT IN ('use_count','last_used_at','updated_at');
 DROP TRIGGER user_memory_assign_revision ON user_memories;
 DROP TRIGGER user_memory_capture_change ON user_memories;
 DROP TRIGGER user_memory_retain_version ON user_memories;
 EXECUTE format('CREATE TRIGGER user_memory_00_authority BEFORE INSERT OR UPDATE OF %s ON user_memories FOR EACH ROW EXECUTE FUNCTION user_memory_guard_authority()',attributes);
 EXECUTE format('CREATE TRIGGER user_memory_assign_revision BEFORE INSERT OR UPDATE OF %s ON user_memories FOR EACH ROW EXECUTE FUNCTION user_memory_assign_revision()',attributes);
 EXECUTE format('CREATE TRIGGER user_memory_capture_change AFTER INSERT OR DELETE OR UPDATE OF %s ON user_memories FOR EACH ROW EXECUTE FUNCTION user_memory_capture_change()',attributes);
 EXECUTE format('CREATE TRIGGER user_memory_retain_version AFTER UPDATE OF %s ON user_memories FOR EACH ROW EXECUTE FUNCTION user_memory_retain_version()',attributes);
 FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_proc routine,
   LATERAL aclexplode(routine.proacl) acl
   WHERE routine.oid='user_memory_guard_authority()'::regprocedure AND acl.grantee<>routine.proowner
 LOOP
   role_name:=CASE WHEN recipient.grantee=0 THEN 'PUBLIC' ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
   EXECUTE format('REVOKE ALL ON FUNCTION user_memory_guard_authority() FROM %s',role_name);
 END LOOP;
END
$private_authority_triggers$;

-- Ordinary runtime deletion is retirement through the Go owner. Physical erase
-- and TRUNCATE require the schema owner; neither may bypass authority/history.
DO $private_erase_privileges$
DECLARE recipient RECORD; role_name TEXT;
BEGIN
 FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_class relation,
   LATERAL aclexplode(relation.relacl) acl
   WHERE relation.oid='user_memories'::regclass AND acl.grantee<>relation.relowner
 LOOP
   role_name:=CASE WHEN recipient.grantee=0 THEN 'PUBLIC' ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
   EXECUTE format('REVOKE DELETE,TRUNCATE ON user_memories FROM %s',role_name);
 END LOOP;
END
$private_erase_privileges$;
