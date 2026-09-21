-- Content-free retry identities survive parent erasure so deletion cannot free
-- an already committed key. Payloads remain only in governed canonical history
-- or proposals, whose existing erasure rules continue to apply.
CREATE TABLE user_memory_mutation_receipts (
 owner_id UUID NOT NULL,
 actor_principal TEXT NOT NULL CHECK(length(actor_principal) BETWEEN 1 AND 1024),
 key_hash TEXT NOT NULL CHECK(key_hash ~ '^[0-9a-f]{64}$'),
 request_hash TEXT NOT NULL CHECK(request_hash ~ '^[0-9a-f]{64}$'),
 commit_id UUID NOT NULL DEFAULT gen_random_uuid(),
 target_id BIGINT NOT NULL CHECK(target_id>0),
 target_revision BIGINT NOT NULL CHECK(target_revision>0),
 result_revision BIGINT NOT NULL CHECK(result_revision>=0),
 proposal_id UUID,
 created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
 PRIMARY KEY(owner_id,actor_principal,key_hash),
 CHECK((proposal_id IS NULL AND result_revision>=target_revision) OR
       (proposal_id IS NOT NULL AND result_revision=0))
);
CREATE FUNCTION user_memory_mutation_receipt_guard() RETURNS trigger
LANGUAGE plpgsql SET search_path=pg_catalog AS $$
DECLARE valid_result BOOLEAN;
BEGIN
 IF TG_OP<>'INSERT' THEN RAISE EXCEPTION 'private mutation receipts are immutable'; END IF;
 IF NEW.actor_principal<>COALESCE(current_setting('aimee.private_principal',true),'') OR
    COALESCE(current_setting('aimee.private_authority',true),'') NOT IN ('user','model') THEN
   RAISE EXCEPTION 'private mutation receipt requires verified actor context';
 END IF;
 IF NEW.proposal_id IS NULL THEN
   EXECUTE format('SELECT EXISTS(SELECT 1 FROM %I.user_memories m,%I.user_memory_collection_generation o
     WHERE o.id=1 AND o.owner_id=$1 AND m.id=$2 AND m.record_revision=$3 AND m.lifecycle_state=''active''
     AND (m.valid_until IS NULL OR m.valid_until>now()) AND (m.record_revision=$4 OR EXISTS(
       SELECT 1 FROM %I.user_memory_versions v WHERE v.memory_id=m.id AND v.record_revision=$4)))',TG_TABLE_SCHEMA,TG_TABLE_SCHEMA,TG_TABLE_SCHEMA)
     INTO valid_result USING NEW.owner_id,NEW.target_id,NEW.result_revision,NEW.target_revision;
 ELSE
   EXECUTE format('SELECT EXISTS(SELECT 1 FROM %I.user_memory_correction_proposals p,%I.user_memory_collection_generation o
     WHERE o.id=1 AND o.owner_id=$1 AND p.owner_id=o.owner_id AND p.proposal_id=$2 AND p.target_id=$3 AND p.target_revision=$4)',TG_TABLE_SCHEMA,TG_TABLE_SCHEMA)
     INTO valid_result USING NEW.owner_id,NEW.proposal_id,NEW.target_id,NEW.target_revision;
 END IF;
 IF NOT valid_result THEN RAISE EXCEPTION 'private mutation receipt has no canonical outcome'; END IF;
 RETURN NEW;
END $$;
CREATE TRIGGER user_memory_mutation_receipt_guard BEFORE INSERT OR UPDATE OR DELETE ON user_memory_mutation_receipts
 FOR EACH ROW EXECUTE FUNCTION user_memory_mutation_receipt_guard();
REVOKE ALL ON FUNCTION user_memory_mutation_receipt_guard() FROM PUBLIC;
DO $private_retry_acl$
DECLARE recipient RECORD; role_name TEXT;
BEGIN
 FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_class relation,LATERAL aclexplode(relation.relacl) acl
  WHERE relation.oid='user_memory_mutation_receipts'::regclass AND acl.grantee<>relation.relowner
 LOOP
  role_name:=CASE WHEN recipient.grantee=0 THEN 'PUBLIC' ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
  EXECUTE format('REVOKE ALL ON user_memory_mutation_receipts FROM %s',role_name);
  IF recipient.grantee<>0 THEN EXECUTE format('GRANT SELECT,INSERT ON user_memory_mutation_receipts TO %s',role_name); END IF;
 END LOOP;
 FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_proc routine,LATERAL aclexplode(routine.proacl) acl
  WHERE routine.oid='user_memory_mutation_receipt_guard()'::regprocedure AND acl.grantee<>routine.proowner
 LOOP
  role_name:=CASE WHEN recipient.grantee=0 THEN 'PUBLIC' ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
  EXECUTE format('REVOKE ALL ON FUNCTION user_memory_mutation_receipt_guard() FROM %s',role_name);
 END LOOP;
END
$private_retry_acl$;
