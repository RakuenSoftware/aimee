-- Private correction drafts are outside serving memory and are erased with
-- their parent. A final decision is immutable; retries reuse its reference.
CREATE TABLE user_memory_correction_proposals (
 proposal_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
 owner_id UUID NOT NULL,
 target_id BIGINT NOT NULL REFERENCES user_memories(id) ON DELETE CASCADE,
 target_revision BIGINT NOT NULL CHECK(target_revision>0),
 actor_principal TEXT NOT NULL CHECK(length(actor_principal) BETWEEN 1 AND 1024),
 actor_transport TEXT NOT NULL DEFAULT '',
 payload TEXT NOT NULL CHECK(length(payload)>0),
 payload_digest TEXT NOT NULL CHECK(payload_digest ~ '^[0-9a-f]{64}$'),
 state TEXT NOT NULL DEFAULT 'pending' CHECK(state IN ('pending','approved','rejected')),
 reviewer_principal TEXT NOT NULL DEFAULT '',
 reviewer_transport TEXT NOT NULL DEFAULT '',
 decision_id TEXT NOT NULL DEFAULT '',
 review_commit_id TEXT,
 result_id BIGINT NOT NULL DEFAULT 0,
 result_revision BIGINT NOT NULL DEFAULT 0,
 created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
 UNIQUE(owner_id,target_id,target_revision,payload_digest),
 CHECK((state='pending' AND reviewer_principal='' AND reviewer_transport='' AND decision_id='' AND review_commit_id IS NULL AND result_id=0 AND result_revision=0)
    OR (state='rejected' AND reviewer_principal<>'' AND decision_id<>'' AND review_commit_id IS NOT NULL AND result_id=0 AND result_revision=0)
    OR (state='approved' AND reviewer_principal<>'' AND decision_id<>'' AND review_commit_id IS NOT NULL AND result_id=target_id AND result_revision>target_revision))
);
CREATE INDEX user_memory_correction_proposals_recent ON user_memory_correction_proposals(created_at DESC,proposal_id);
CREATE INDEX user_memory_correction_proposals_parent ON user_memory_correction_proposals(target_id);
ALTER TABLE user_memories
 ADD COLUMN review_proposal_id TEXT NOT NULL DEFAULT '',
 ADD COLUMN reviewer_principal TEXT NOT NULL DEFAULT '',
 ADD COLUMN reviewer_transport TEXT NOT NULL DEFAULT '';

CREATE FUNCTION user_memory_correction_proposal_guard() RETURNS trigger
LANGUAGE plpgsql SET search_path=pg_catalog AS $$
DECLARE valid_parent BOOLEAN;
BEGIN
 IF TG_OP='INSERT' THEN
   EXECUTE format('SELECT EXISTS(SELECT 1 FROM %I.user_memories m,%I.user_memory_collection_generation o WHERE m.id=$1 AND m.record_revision=$2 AND m.lifecycle_state=''active'' AND (m.valid_until IS NULL OR m.valid_until>now()) AND o.id=1 AND o.owner_id=$3)',TG_TABLE_SCHEMA,TG_TABLE_SCHEMA)
     INTO valid_parent USING NEW.target_id,NEW.target_revision,NEW.owner_id;
   IF NOT valid_parent OR NEW.state<>'pending' OR
      NEW.actor_principal<>COALESCE(current_setting('aimee.private_principal',true),'') OR
      NEW.actor_transport<>COALESCE(current_setting('aimee.private_transport',true),'') OR
      COALESCE(current_setting('aimee.private_authority',true),'')<>'model' OR
      NEW.payload_digest<>encode(sha256(convert_to(NEW.payload,'UTF8')),'hex') THEN
     RAISE EXCEPTION 'invalid private correction proposal';
   END IF;
 ELSE
   IF (to_jsonb(NEW)-ARRAY['state','reviewer_principal','reviewer_transport','decision_id','review_commit_id','result_id','result_revision'])
      IS DISTINCT FROM (to_jsonb(OLD)-ARRAY['state','reviewer_principal','reviewer_transport','decision_id','review_commit_id','result_id','result_revision'])
      OR OLD.state<>'pending' OR NEW.state NOT IN ('approved','rejected')
      OR COALESCE(current_setting('aimee.private_authority',true),'')<>'review'
      OR NEW.reviewer_principal<>COALESCE(current_setting('aimee.private_reviewer',true),'')
      OR NEW.reviewer_transport<>COALESCE(current_setting('aimee.private_review_transport',true),'')
      OR NEW.proposal_id::text<>COALESCE(current_setting('aimee.private_review_proposal',true),'') THEN
     RAISE EXCEPTION 'private correction payload and decisions are immutable';
   END IF;
   IF NEW.state='approved' THEN
     EXECUTE format('SELECT EXISTS(SELECT 1 FROM %I.user_memories WHERE id=$1 AND record_revision=$2 AND review_proposal_id=$3 AND provenance_category=''reviewed_model'')',TG_TABLE_SCHEMA)
       INTO valid_parent USING NEW.result_id,NEW.result_revision,NEW.proposal_id::text;
     IF NOT valid_parent THEN RAISE EXCEPTION 'private correction decision has no canonical result'; END IF;
   END IF;
 END IF;
 RETURN NEW;
END $$;
CREATE TRIGGER user_memory_correction_proposal_guard BEFORE INSERT OR UPDATE ON user_memory_correction_proposals
 FOR EACH ROW EXECUTE FUNCTION user_memory_correction_proposal_guard();

CREATE OR REPLACE FUNCTION user_memory_guard_authority() RETURNS trigger
LANGUAGE plpgsql SET search_path=pg_catalog AS $$
DECLARE authority TEXT:=current_setting('aimee.private_authority',true);
        principal TEXT:=COALESCE(current_setting('aimee.private_principal',true),'');
        transport TEXT:=COALESCE(current_setting('aimee.private_transport',true),'');
        authored_change BOOLEAN;
        proposal JSONB; draft JSONB;
        review_id TEXT:=COALESCE(current_setting('aimee.private_review_proposal',true),'');
        reviewer TEXT:=COALESCE(current_setting('aimee.private_reviewer',true),'');
        reviewer_transport TEXT:=COALESCE(current_setting('aimee.private_review_transport',true),'');
BEGIN
  IF authority='user' AND principal='' THEN authority:=''; END IF;
  IF TG_OP='INSERT' THEN
    IF NEW.provenance_category<>'unknown' OR NEW.author_principal<>'' OR NEW.author_transport<>'' OR NEW.review_proposal_id<>'' OR NEW.reviewer_principal<>'' OR NEW.reviewer_transport<>'' THEN
      RAISE EXCEPTION 'private authorship must come from verified mutation context';
    END IF;
    authored_change:=true;
  ELSE
    IF (NEW.provenance_category,NEW.author_principal,NEW.author_transport,NEW.review_proposal_id,NEW.reviewer_principal,NEW.reviewer_transport)
       IS DISTINCT FROM (OLD.provenance_category,OLD.author_principal,OLD.author_transport,OLD.review_proposal_id,OLD.reviewer_principal,OLD.reviewer_transport) THEN
      RAISE EXCEPTION 'private authorship cannot be assigned by a record payload';
    END IF;
    IF authority='review' THEN
      EXECUTE format('SELECT to_jsonb(p) FROM %I.user_memory_correction_proposals p WHERE proposal_id::text=$1 AND target_id=$2 AND target_revision=$3 AND state=''pending'' AND owner_id=(SELECT owner_id FROM %I.user_memory_collection_generation WHERE id=1)',TG_TABLE_SCHEMA,TG_TABLE_SCHEMA)
        INTO proposal USING review_id,OLD.id,OLD.record_revision;
      draft:=(proposal->>'payload')::jsonb;
      IF proposal IS NULL OR reviewer='' OR OLD.lifecycle_state<>'active' OR
         (OLD.valid_until IS NOT NULL AND OLD.valid_until<=now()) OR
         OLD.kind IN ('episode','experience','instruction','policy') OR
         (to_jsonb(NEW)-ARRAY['content','tier','confidence','updated_at']) IS DISTINCT FROM
         (to_jsonb(OLD)-ARRAY['content','tier','confidence','updated_at']) OR
         NEW.content IS DISTINCT FROM draft->>'content' OR NEW.tier IS DISTINCT FROM draft->>'tier' OR
         NEW.kind IS DISTINCT FROM draft->>'epistemic_kind' OR
         NEW.confidence IS DISTINCT FROM (draft->>'confidence')::double precision OR
         NEW.confidence>(CASE WHEN NEW.tier='L5' THEN 0.5 ELSE 0.8 END) THEN
        RAISE EXCEPTION 'private review no longer matches the admitted draft';
      END IF;
      NEW.provenance_category:='reviewed_model';
      NEW.author_principal:=proposal->>'actor_principal';
      NEW.author_transport:=proposal->>'actor_transport';
      NEW.review_proposal_id:=review_id;
      NEW.reviewer_principal:=reviewer;
      NEW.reviewer_transport:=reviewer_transport;
      RETURN NEW;
    END IF;
    IF (to_jsonb(NEW)-ARRAY['record_revision','use_count','last_used_at','updated_at'])
       IS NOT DISTINCT FROM (to_jsonb(OLD)-ARRAY['record_revision','use_count','last_used_at','updated_at']) THEN
      RETURN NEW;
    END IF;
    IF COALESCE(authority,'')<>'user' AND OLD.provenance_category<>'agent_message' THEN
      RAISE EXCEPTION 'private authoritative or unknown-origin mutation requires user review';
    END IF;
    IF OLD.lifecycle_state NOT IN ('active','retired','expired') OR
       (COALESCE(authority,'')<>'user' AND (OLD.lifecycle_state<>'active' OR (OLD.valid_until<=now() AND (NEW.valid_until IS NULL OR NEW.valid_until>now())))) THEN
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
    NEW.review_proposal_id:=''; NEW.reviewer_principal:=''; NEW.reviewer_transport:='';
    NEW.author_principal:=principal;
    NEW.author_transport:=transport;
  END IF;
  RETURN NEW;
END $$;

DO $private_authority_triggers$
DECLARE attributes TEXT; recipient RECORD; role_name TEXT;
BEGIN
 SELECT string_agg(quote_ident(attname),',' ORDER BY attnum) INTO attributes
 FROM pg_attribute WHERE attrelid='user_memories'::regclass AND attnum>0 AND NOT attisdropped
   AND attgenerated='' AND attname NOT IN ('use_count','last_used_at','updated_at');
 DROP TRIGGER user_memory_00_authority ON user_memories;
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


REVOKE ALL ON FUNCTION user_memory_correction_proposal_guard() FROM PUBLIC;
DO $private_proposal_privileges$
DECLARE recipient RECORD; role_name TEXT;
BEGIN
 FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_class relation,
  LATERAL aclexplode(relation.relacl) acl
  WHERE relation.oid='user_memory_correction_proposals'::regclass AND acl.grantee<>relation.relowner
 LOOP
  role_name:=CASE WHEN recipient.grantee=0 THEN 'PUBLIC' ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
  EXECUTE format('REVOKE ALL ON user_memory_correction_proposals FROM %s',role_name);
  IF recipient.grantee<>0 THEN
   EXECUTE format('GRANT SELECT,INSERT ON user_memory_correction_proposals TO %s',role_name);
   EXECUTE format('GRANT UPDATE(state,reviewer_principal,reviewer_transport,decision_id,review_commit_id,result_id,result_revision) ON user_memory_correction_proposals TO %s',role_name);
  END IF;
 END LOOP;
 FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_proc routine,
  LATERAL aclexplode(routine.proacl) acl
  WHERE routine.oid='user_memory_correction_proposal_guard()'::regprocedure AND acl.grantee<>routine.proowner
 LOOP
  role_name:=CASE WHEN recipient.grantee=0 THEN 'PUBLIC' ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
  EXECUTE format('REVOKE ALL ON FUNCTION user_memory_correction_proposal_guard() FROM %s',role_name);
 END LOOP;
END
$private_proposal_privileges$;
