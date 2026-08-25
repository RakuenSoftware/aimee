\set ON_ERROR_STOP on
\echo '== Memory governance: row scope, tombstones, and WORM audit =='

DO $$
DECLARE memories_rls boolean; tombstones_rls boolean; policy_count integer;
BEGIN
  SELECT relrowsecurity INTO memories_rls FROM pg_class WHERE oid='public.memories'::regclass;
  SELECT relrowsecurity INTO tombstones_rls
    FROM pg_class WHERE oid='public.memory_rejection_tombstones'::regclass;
  SELECT count(*) INTO policy_count FROM pg_policies
   WHERE schemaname='public' AND tablename IN ('memories','memory_rejection_tombstones');
  IF NOT memories_rls OR NOT tombstones_rls OR policy_count<>2 THEN
    RAISE EXCEPTION 'memory governance RLS/policy posture is incomplete (%/%/%)',
      memories_rls,tombstones_rls,policy_count;
  END IF;
END $$;

-- Seed as the schema owner.  The runtime checks below must rely on row policy,
-- not on the legacy membership side tables.
INSERT INTO memories(id,key,content,lifecycle_state,scope_type,scope_value)
VALUES
  (990001,'atlas:project-a','project A value','active','project','project-a'),
  (990002,'atlas:project-b','project B value','active','project','project-b'),
  (990003,'atlas:shared','shared value','active','workspace','_shared');

SET ROLE aimee_kb_runtime;
SELECT set_config('aimee.memory_project','project-a',false);

DO $$
DECLARE own_n integer; other_n integer; shared_n integer;
BEGIN
  IF has_table_privilege(current_user,'memory_rejection_tombstones','DELETE') OR
     has_table_privilege(current_user,'memory_rejection_tombstones','TRUNCATE') OR
     NOT has_table_privilege(current_user,'memory_rejection_tombstones','SELECT,INSERT,UPDATE') THEN
    RAISE EXCEPTION 'memory tombstone runtime privileges permit erasure or prevent review';
  END IF;
  SELECT count(*) INTO own_n FROM memories WHERE id=990001;
  SELECT count(*) INTO other_n FROM memories WHERE id=990002;
  SELECT count(*) INTO shared_n FROM memories WHERE id=990003;
  IF own_n<>1 OR other_n<>0 OR shared_n<>1 THEN
    RAISE EXCEPTION 'memory row scope leaked (own %, other %, shared %)',own_n,other_n,shared_n;
  END IF;
END $$;

-- Rejecting preserves the row for review and installs a value-keyed refusal.
INSERT INTO memory_rejection_tombstones(
  object_kind,memory_key,memory_content,scope_type,scope_value,authority_rank,reason,rejected_by)
VALUES('memory','atlas:project-a','project A value','project','project-a',40,
       'operator rejected extraction','test:operator');
UPDATE memories SET lifecycle_state='rejected',confidence=0,
  archive_reason='operator rejected extraction' WHERE id=990001;

DO $$
BEGIN
  BEGIN
    INSERT INTO memories(key,content,lifecycle_state,scope_type,scope_value)
    VALUES('atlas:project-a','project A value','active','project','project-a');
    RAISE EXCEPTION 'active memory reassertion unexpectedly succeeded';
  EXCEPTION WHEN raise_exception THEN
    IF SQLERRM='active memory reassertion unexpectedly succeeded' THEN RAISE; END IF;
    IF position('rejection tombstone' in SQLERRM)=0 THEN RAISE; END IF;
  END;
END $$;

-- Only an explicit review action removes the refusal; the retained row becomes
-- recallable again without creating a second copy.
UPDATE memory_rejection_tombstones SET active=0,restored_at=pg_now_text(),
  restored_by='test:operator'
 WHERE object_kind='memory' AND memory_key='atlas:project-a' AND active=1;
UPDATE memories SET lifecycle_state='active',confidence=0.5,archive_reason='' WHERE id=990001;

RESET ROLE;

-- Typed facts use the exact source/relation/target value as their refusal key.
BEGIN;
INSERT INTO fact_graph_commits
  (commit_id,operation,actor_principal,actor_role,authority_rank,status,reversible)
VALUES('atlas-fact-assert','fact.assert','test:operator','operator',40,'open',1);
INSERT INTO entity_edges(id,source,relation,target,edge_class,lifecycle_state,commit_id)
VALUES(990011,'Atlas','deploys_to','Production','semantic','persistent','atlas-fact-assert');
INSERT INTO fact_graph_changes(commit_id,assertion_id,action,existed_after,after_lifecycle)
VALUES('atlas-fact-assert',990011,'insert',1,'persistent');
UPDATE fact_graph_commits SET status='applied',closed_at=pg_now_text()
 WHERE commit_id='atlas-fact-assert';
COMMIT;

INSERT INTO memory_rejection_tombstones(
  object_kind,source,relation,target,authority_rank,reason,rejected_by)
VALUES('fact','Atlas','deploys_to','Production',40,'wrong extraction','test:operator');
BEGIN;
INSERT INTO fact_graph_commits
  (commit_id,operation,actor_principal,actor_role,authority_rank,status,reversible)
VALUES('atlas-fact-reject','fact.reject','test:operator','operator',40,'open',1);
UPDATE entity_edges SET lifecycle_state='invalidated',invalidated_at=pg_now_text(),
  commit_id='atlas-fact-reject' WHERE id=990011;
INSERT INTO fact_graph_changes(commit_id,assertion_id,action,existed_before,existed_after,
  before_lifecycle,after_lifecycle,after_invalidated_at)
VALUES('atlas-fact-reject',990011,'invalidate',1,1,'persistent','invalidated',pg_now_text());
UPDATE fact_graph_commits SET status='applied',closed_at=pg_now_text()
 WHERE commit_id='atlas-fact-reject';
COMMIT;

INSERT INTO fact_graph_commits
  (commit_id,operation,actor_principal,actor_role,authority_rank,status,reversible)
VALUES('atlas-fact-reassert','fact.assert','test:extractor','model',10,'open',1);

DO $$
BEGIN
  BEGIN
    INSERT INTO entity_edges(source,relation,target,edge_class,lifecycle_state,commit_id)
    VALUES('Atlas','deploys_to','Production','semantic','persistent','atlas-fact-reassert');
    RAISE EXCEPTION 'typed fact reassertion unexpectedly succeeded';
  EXCEPTION WHEN raise_exception THEN
    IF SQLERRM='typed fact reassertion unexpectedly succeeded' THEN RAISE; END IF;
    IF position('rejection tombstone' in SQLERRM)=0 THEN RAISE; END IF;
  END;
END $$;
DELETE FROM fact_graph_commits WHERE commit_id='atlas-fact-reassert';

DO $$
DECLARE audit_n integer; retained_n integer;
BEGIN
  -- The request process proves durable submission here. Chain construction is
  -- intentionally asynchronous and is covered by run-worm-worker-pg-test.sh.
  SELECT count(*) INTO audit_n FROM kb_audit_outbox
   WHERE action IN ('memory.assert','memory.reject','memory.invalidate','memory.restore');
  SELECT count(*) INTO retained_n FROM memories WHERE id=990001 AND content='project A value';
  IF audit_n<5 OR retained_n<>1 THEN
    RAISE EXCEPTION 'memory review evidence incomplete (audit %, retained %)',audit_n,retained_n;
  END IF;
END $$;

\echo '== Memory governance assertions PASSED =='
