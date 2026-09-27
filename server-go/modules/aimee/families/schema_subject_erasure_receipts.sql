-- The receipt is captured while the actual private erasure holds its source
-- locks. Hash locators let the shared owner reconcile the erased session set
-- without retaining the session identifiers after deletion.
ALTER TABLE db1_subject_erasure_request ADD COLUMN session_digests JSONB NOT NULL DEFAULT '[]'::jsonb
 CHECK(jsonb_typeof(session_digests)='array');
ALTER TABLE db1_subject_erasure_request ADD COLUMN receipt_policy TEXT NOT NULL DEFAULT 'legacy';

CREATE TABLE user_memory_erasure_delegations (
 delegation_digest TEXT PRIMARY KEY CHECK(delegation_digest ~ '^[0-9a-f]{64}$')
);
REVOKE ALL ON user_memory_erasure_delegations FROM PUBLIC;

-- Session-owned payloads cannot be republished by a delayed writer after the
-- coordinator has committed deletion. The epoch also fences old RR snapshots.
CREATE FUNCTION user_memory_erasure_payload_guard() RETURNS trigger
LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog AS $$
DECLARE current_row JSONB:=to_jsonb(NEW); prior_row JSONB:='{}';
 sessions TEXT[]; delegations TEXT[]; observed BIGINT; prohibited BOOLEAN;
BEGIN
 IF TG_OP='UPDATE' THEN prior_row:=to_jsonb(OLD); END IF;
 IF TG_TABLE_NAME='server_sessions' THEN
  sessions:=ARRAY[current_row->>'id',prior_row->>'id'];
 ELSE
  sessions:=ARRAY[current_row->>'session_id',current_row->>'aimee_session_id',prior_row->>'session_id',prior_row->>'aimee_session_id'];
 END IF;
 delegations:=ARRAY[current_row->>'delegation_id',prior_row->>'delegation_id'];
 EXECUTE format('SELECT generation FROM %I.user_memory_erasure_epoch WHERE id=1 FOR SHARE',TG_TABLE_SCHEMA) INTO STRICT observed;
 EXECUTE format('SELECT EXISTS(SELECT 1 FROM %I.user_memory_erasure_sessions i
 WHERE i.session_digest IN(SELECT encode(sha256(convert_to(x,''UTF8'')),''hex'') FROM unnest($1::text[]) x))
 OR EXISTS(SELECT 1 FROM %I.user_memory_erasure_delegations i
 WHERE i.delegation_digest IN(SELECT encode(sha256(convert_to(x,''UTF8'')),''hex'') FROM unnest($2::text[]) x))',TG_TABLE_SCHEMA,TG_TABLE_SCHEMA)
 INTO prohibited USING sessions,delegations;
 IF prohibited THEN RAISE EXCEPTION 'session payload prohibited by surviving erasure intent' USING ERRCODE='23514'; END IF;
 RETURN NEW;
END $$;
REVOKE ALL ON FUNCTION user_memory_erasure_payload_guard() FROM PUBLIC;

-- This cache carries neither session nor source observations. Once privacy
-- erasure has occurred, unsafe cache writes become misses until its producer
-- supplies versioned ownership. Ordinary uncached execution remains available.
CREATE FUNCTION user_memory_unowned_cache_guard() RETURNS trigger
LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog AS $$
DECLARE observed BIGINT;
BEGIN
 EXECUTE format('SELECT generation FROM %I.user_memory_erasure_epoch WHERE id=1 FOR SHARE',TG_TABLE_SCHEMA) INTO STRICT observed;
 IF observed>0 THEN RETURN NULL; END IF;
 RETURN NEW;
END $$;
REVOKE ALL ON FUNCTION user_memory_unowned_cache_guard() FROM PUBLIC;
CREATE TRIGGER user_memory_unowned_cache_guard BEFORE INSERT OR UPDATE ON agent_cache
 FOR EACH ROW EXECUTE FUNCTION user_memory_unowned_cache_guard();

DO $payload_guards$
DECLARE target TEXT;
BEGIN
 FOREACH target IN ARRAY ARRAY['delegation_messages','delegation_checkpoint','delegation_spawns','delegate_learnings','working_profile_observations_local','context_cache','context_snapshots','context_activation_events','context_activation_turns','file_snapshots','execution_trace','workflow_binding','branch_ownership','session_feature_branch','economizer_state','checkpoints','agent_log','working_memory','payload_rewrite_state','conv_tool_events','conv_tool_chains','conv_context_state','windows','primary_sessions','webchat_claude_sessions','webchat_live','session_state_write_paths','session_state','server_sessions'] LOOP
  EXECUTE format('CREATE TRIGGER user_memory_erasure_payload_guard BEFORE INSERT OR UPDATE ON %I FOR EACH ROW EXECUTE FUNCTION user_memory_erasure_payload_guard()',target);
 END LOOP;
END $payload_guards$;

-- The store owner calls this on EVERY startup after migration, including a
-- startup with no pending versions. A content-only restore can have bypassed
-- triggers; its retained control metadata still prohibits serving those rows.
DO $restore_replay$
BEGIN
 EXECUTE format($definition$
 CREATE FUNCTION %1$I.user_memory_replay_erasure_intents() RETURNS BIGINT
 LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog,%1$I AS $body$
 DECLARE target TEXT; total BIGINT:=0; removed BIGINT; session_column TEXT;
 BEGIN
  LOCK TABLE %1$I.user_memories,%1$I.server_sessions IN SHARE ROW EXCLUSIVE MODE;
  PERFORM generation FROM %1$I.user_memory_erasure_epoch WHERE id=1 FOR UPDATE;
  IF NOT EXISTS(SELECT 1 FROM %1$I.user_memory_erasure_epoch WHERE generation>0)
   AND NOT EXISTS(SELECT 1 FROM %1$I.user_memory_erasure_intents)
   AND NOT EXISTS(SELECT 1 FROM %1$I.user_memory_erasure_sessions)
   AND NOT EXISTS(SELECT 1 FROM %1$I.user_memory_erasure_delegations) THEN RETURN 0; END IF;
  DELETE FROM %1$I.user_memories m WHERE EXISTS(SELECT 1 FROM %1$I.user_memory_erasure_intents i
   WHERE i.memory_id=m.id OR i.payload_digest=encode(sha256(convert_to(m.content,'UTF8')),'hex'))
   OR EXISTS(SELECT 1 FROM %1$I.user_memory_erasure_sessions i WHERE i.session_digest=encode(sha256(convert_to(m.source_session,'UTF8')),'hex'));
  GET DIAGNOSTICS removed=ROW_COUNT; total:=total+removed;
  DELETE FROM %1$I.user_memory_versions v WHERE EXISTS(SELECT 1 FROM %1$I.user_memory_erasure_intents i
   WHERE i.memory_id=v.memory_id OR i.payload_digest=encode(sha256(convert_to(v.record->>'content','UTF8')),'hex'))
   OR EXISTS(SELECT 1 FROM %1$I.user_memory_erasure_sessions i WHERE i.session_digest=encode(sha256(convert_to(v.record->>'source_session','UTF8')),'hex'));
  GET DIAGNOSTICS removed=ROW_COUNT; total:=total+removed;
  FOREACH target IN ARRAY ARRAY['delegation_messages','delegation_checkpoint','delegation_spawns','delegate_learnings','working_profile_observations_local','context_cache','context_snapshots','context_activation_events','context_activation_turns','file_snapshots','execution_trace','workflow_binding','branch_ownership','session_feature_branch','economizer_state','checkpoints','agent_log','working_memory','payload_rewrite_state','conv_tool_events','conv_tool_chains','conv_context_state','windows','primary_sessions','webchat_claude_sessions','webchat_live','session_state_write_paths','session_state','server_sessions'] LOOP
   session_column:=CASE target WHEN 'server_sessions' THEN 'id' WHEN 'workflow_binding' THEN 'aimee_session_id' WHEN 'webchat_claude_sessions' THEN 'aimee_session_id' ELSE 'session_id' END;
   IF target IN ('delegation_messages','delegation_checkpoint') THEN
    EXECUTE format('DELETE FROM %%I.%%I p WHERE EXISTS(SELECT 1 FROM %%I.user_memory_erasure_delegations i WHERE i.delegation_digest=encode(sha256(convert_to(p.delegation_id,''UTF8'')),''hex''))',%1$L,target,%1$L);
   ELSE
    EXECUTE format('DELETE FROM %%I.%%I p WHERE EXISTS(SELECT 1 FROM %%I.user_memory_erasure_sessions i WHERE i.session_digest=encode(sha256(convert_to(p.%%I,''UTF8'')),''hex''))',%1$L,target,%1$L,session_column);
   END IF;
   GET DIAGNOSTICS removed=ROW_COUNT; total:=total+removed;
  END LOOP;
  IF EXISTS(SELECT 1 FROM %1$I.user_memory_erasure_epoch WHERE generation>0) THEN
   DELETE FROM %1$I.agent_cache; GET DIAGNOSTICS removed=ROW_COUNT; total:=total+removed;
  END IF;
  RETURN total;
 END $body$;
 $definition$,current_schema());
END $restore_replay$;

-- Capture control metadata through the storage owner, not by granting runtime
-- code the right to edit or clear deletion intent. The Go family still admits
-- the request and deletes the remaining session graph in the enclosing transaction.
-- Protected canonical memory deletion stays inside this narrow storage definer.
DO $prepare_subject$
BEGIN
 EXECUTE format($definition$
 CREATE FUNCTION %1$I.user_memory_prepare_subject_erasure(p_principal TEXT)
 RETURNS TABLE(session_digests TEXT,session_count BIGINT)
 LANGUAGE plpgsql SECURITY DEFINER SET search_path=pg_catalog,%1$I AS $body$
 DECLARE receipt TEXT; n BIGINT;
 BEGIN
  IF p_principal IS NULL OR length(p_principal) NOT BETWEEN 1 AND 600 THEN
   RAISE EXCEPTION 'invalid erasure principal' USING ERRCODE='22023';
  END IF;
  LOCK TABLE %1$I.server_sessions,%1$I.user_memories IN SHARE ROW EXCLUSIVE MODE;
  SELECT COALESCE(jsonb_agg('sha256:'||encode(sha256(convert_to(id,'UTF8')),'hex') ORDER BY id),'[]'::jsonb)::text,count(*)
  INTO receipt,n FROM (SELECT id FROM %1$I.server_sessions WHERE principal=p_principal ORDER BY id LIMIT 4097) sessions;
  IF n>4096 THEN RETURN QUERY SELECT '[]'::text,n; RETURN; END IF;
  UPDATE %1$I.user_memory_erasure_epoch SET generation=generation+1 WHERE id=1;
  WITH sessions AS MATERIALIZED(SELECT id FROM %1$I.server_sessions WHERE principal=p_principal),
  memories AS MATERIALIZED(SELECT id,content FROM %1$I.user_memories WHERE author_principal=p_principal
   OR source_session IN(SELECT id FROM sessions) OR id IN(SELECT memory_id FROM %1$I.user_memory_versions
    WHERE record->>'author_principal'=p_principal OR record->>'source_session' IN(SELECT id FROM sessions)))
  INSERT INTO %1$I.user_memory_erasure_intents(memory_id,payload_digest)
   SELECT id,encode(sha256(convert_to(content,'UTF8')),'hex') FROM memories
   UNION SELECT memory_id,encode(sha256(convert_to(record->>'content','UTF8')),'hex')
   FROM %1$I.user_memory_versions WHERE memory_id IN(SELECT id FROM memories)
  ON CONFLICT DO NOTHING;
  INSERT INTO %1$I.user_memory_erasure_sessions(session_digest)
   SELECT encode(sha256(convert_to(id,'UTF8')),'hex') FROM %1$I.server_sessions WHERE principal=p_principal ON CONFLICT DO NOTHING;
  INSERT INTO %1$I.user_memory_erasure_delegations(delegation_digest)
   SELECT encode(sha256(convert_to(delegation_id,'UTF8')),'hex') FROM %1$I.delegation_spawns
   WHERE delegation_id<>'' AND session_id IN(SELECT id FROM %1$I.server_sessions WHERE principal=p_principal) ON CONFLICT DO NOTHING;
  DELETE FROM %1$I.user_memories m WHERE m.author_principal=p_principal
   OR m.source_session IN(SELECT id FROM %1$I.server_sessions WHERE principal=p_principal)
   OR m.id IN(SELECT memory_id FROM %1$I.user_memory_versions WHERE record->>'author_principal'=p_principal
    OR record->>'source_session' IN(SELECT id FROM %1$I.server_sessions WHERE principal=p_principal));
  RETURN QUERY SELECT receipt,n;
 END $body$;
 $definition$,current_schema());
END $prepare_subject$;

DO $receipt_control_acl$
DECLARE recipient RECORD; role_name TEXT; target TEXT;
BEGIN
 FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_class relation,LATERAL aclexplode(relation.relacl) acl
 WHERE relation.oid='user_memory_erasure_delegations'::regclass AND acl.grantee<>relation.relowner LOOP
  role_name:=CASE WHEN recipient.grantee=0 THEN 'PUBLIC' ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
  EXECUTE format('REVOKE ALL ON user_memory_erasure_delegations FROM %s',role_name);
 END LOOP;
 FOREACH target IN ARRAY ARRAY['user_memory_prepare_subject_erasure(text)','user_memory_replay_erasure_intents()'] LOOP
  FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_proc routine,LATERAL aclexplode(routine.proacl) acl
   WHERE routine.oid=target::regprocedure AND acl.grantee<>routine.proowner LOOP
   role_name:=CASE WHEN recipient.grantee=0 THEN 'PUBLIC' ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
   EXECUTE format('REVOKE ALL ON FUNCTION %s FROM %s',target,role_name);
  END LOOP;
 END LOOP;
 FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_class relation,LATERAL aclexplode(relation.relacl) acl
 WHERE relation.oid='server_sessions'::regclass AND acl.privilege_type='DELETE' AND acl.grantee<>0 AND acl.grantee<>relation.relowner LOOP
  role_name:=quote_ident(pg_get_userbyid(recipient.grantee));
  EXECUTE format('GRANT EXECUTE ON FUNCTION user_memory_prepare_subject_erasure(text),user_memory_replay_erasure_intents() TO %s',role_name);
 END LOOP;
 -- Runtime may append the committed receipt and read it for retries, never
 -- erase or revise an earlier coverage claim.
 FOR recipient IN SELECT DISTINCT acl.grantee FROM pg_class relation,LATERAL aclexplode(relation.relacl) acl
 WHERE relation.oid='db1_subject_erasure_request'::regclass AND acl.grantee<>relation.relowner LOOP
  role_name:=CASE WHEN recipient.grantee=0 THEN 'PUBLIC' ELSE quote_ident(pg_get_userbyid(recipient.grantee)) END;
  EXECUTE format('REVOKE ALL ON db1_subject_erasure_request FROM %s',role_name);
  IF recipient.grantee<>0 THEN EXECUTE format('GRANT SELECT,INSERT ON db1_subject_erasure_request TO %s',role_name); END IF;
 END LOOP;
END $receipt_control_acl$;
