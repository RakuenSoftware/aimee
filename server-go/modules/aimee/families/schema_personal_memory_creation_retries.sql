-- Store retry identities use the same immutable actor/key namespace. Existing
-- correction/retirement receipts and request hashes remain unchanged.
ALTER TABLE user_memory_mutation_receipts DROP CONSTRAINT user_memory_mutation_receipts_operation_check;
ALTER TABLE user_memory_mutation_receipts ADD CONSTRAINT user_memory_mutation_receipts_operation_check
 CHECK(operation IN ('store','supersede','delete'));
CREATE OR REPLACE FUNCTION user_memory_mutation_receipt_guard() RETURNS trigger
LANGUAGE plpgsql SET search_path=pg_catalog AS $$
DECLARE valid_result BOOLEAN;
BEGIN
 IF TG_OP<>'INSERT' THEN RAISE EXCEPTION 'private mutation receipts are immutable'; END IF;
 IF NEW.actor_principal<>COALESCE(current_setting('aimee.private_principal',true),'') OR
    COALESCE(current_setting('aimee.private_authority',true),'') NOT IN ('user','model') THEN
   RAISE EXCEPTION 'private mutation receipt requires verified actor context';
 END IF;
 IF NEW.operation='store' AND NEW.proposal_id IS NULL AND NEW.target_revision<>NEW.result_revision THEN
   RAISE EXCEPTION 'private store receipt must bind its canonical result';
 END IF;
 IF NEW.operation='delete' THEN
   IF NEW.proposal_id IS NOT NULL OR NEW.result_revision<=NEW.target_revision THEN
     RAISE EXCEPTION 'private retirement receipt requires a version transition';
   END IF;
   EXECUTE format('SELECT EXISTS(SELECT 1 FROM %I.user_memories m,%I.user_memory_collection_generation o
     WHERE o.id=1 AND o.owner_id=$1 AND m.id=$2 AND m.record_revision=$3 AND m.lifecycle_state=''retired''
     AND EXISTS(SELECT 1 FROM %I.user_memory_versions v WHERE v.memory_id=m.id AND v.record_revision=$4
       AND v.record->>''lifecycle_state''=''active''))',TG_TABLE_SCHEMA,TG_TABLE_SCHEMA,TG_TABLE_SCHEMA)
     INTO valid_result USING NEW.owner_id,NEW.target_id,NEW.result_revision,NEW.target_revision;
 ELSIF NEW.proposal_id IS NULL THEN
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
REVOKE ALL ON FUNCTION user_memory_mutation_receipt_guard() FROM PUBLIC;
