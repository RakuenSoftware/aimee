\set ON_ERROR_STOP on
\pset tuples_only on

-- Destructive by design, against a disposable database only.  The runner below
-- creates and drops that database around this script.
SET client_min_messages=warning;
CREATE OR REPLACE FUNCTION e2e_assert(ok BOOLEAN,msg TEXT) RETURNS VOID LANGUAGE plpgsql AS $$
BEGIN IF NOT ok THEN RAISE EXCEPTION 'e2e assertion failed: %',msg; END IF; END $$;
CREATE OR REPLACE FUNCTION e2e_expect_stale_document(doc_id BIGINT,token TEXT) RETURNS VOID
LANGUAGE plpgsql AS $$ BEGIN
 BEGIN
  PERFORM document_lifecycle_apply(doc_id,'purge',token,'erase');
  RAISE EXCEPTION 'stale preview unexpectedly accepted';
 EXCEPTION WHEN raise_exception THEN
  IF SQLERRM='stale preview unexpectedly accepted' THEN RAISE; END IF;
 END;
END $$;
CREATE OR REPLACE FUNCTION e2e_expect_bad_kind(memory_id BIGINT) RETURNS VOID LANGUAGE plpgsql AS $$
BEGIN
 BEGIN UPDATE memories SET epistemic_kind='rumour' WHERE id=memory_id;
  RAISE EXCEPTION 'unknown epistemic kind accepted'; EXCEPTION WHEN check_violation THEN NULL; END;
END $$;
CREATE OR REPLACE FUNCTION e2e_expect_unhooked(table_name TEXT,emitter_name TEXT,set_clause TEXT)
RETURNS VOID LANGUAGE plpgsql AS $$
BEGIN
 BEGIN
  EXECUTE format('DROP TRIGGER %I ON %I',emitter_name,table_name);
  EXECUTE format('UPDATE %I SET %s WHERE ctid=(SELECT ctid FROM %I LIMIT 1)',
    table_name,set_clause,table_name);
  RAISE EXCEPTION 'unhooked mutation unexpectedly accepted for %',table_name;
 EXCEPTION WHEN raise_exception THEN
  IF SQLERRM LIKE 'unhooked mutation unexpectedly accepted%' THEN RAISE; END IF;
  IF SQLERRM NOT LIKE 'evidence emitter % is absent or disabled%' THEN RAISE; END IF;
 END;
END $$;
CREATE OR REPLACE FUNCTION e2e_expect_operation_change(commit_id TEXT) RETURNS VOID
LANGUAGE plpgsql AS $$ BEGIN
 BEGIN UPDATE fact_graph_commits SET operation='different.operation' WHERE fact_graph_commits.commit_id=$1;
  RAISE EXCEPTION 'second changeset operation accepted';
 EXCEPTION WHEN raise_exception THEN
  IF SQLERRM='second changeset operation accepted' THEN RAISE; END IF;
 END;
END $$;
CREATE OR REPLACE FUNCTION e2e_expect_lower_authority_revert(commit_id TEXT) RETURNS VOID
LANGUAGE plpgsql AS $$ BEGIN
 BEGIN PERFORM knowledge_changeset_revert($1,false);
  RAISE EXCEPTION 'lower authority revert accepted';
 EXCEPTION WHEN raise_exception THEN
  IF SQLERRM='lower authority revert accepted' THEN RAISE; END IF;
  IF SQLERRM NOT LIKE 'revert applied no items:%lower-authority%' AND
     SQLERRM NOT LIKE 'revert conflicts:%lower-authority%' THEN RAISE; END IF;
 END;
END $$;
CREATE OR REPLACE FUNCTION e2e_expect_episode_rewrite(memory_id BIGINT) RETURNS VOID
LANGUAGE plpgsql AS $$ BEGIN
 BEGIN UPDATE memories SET content='rewritten observation' WHERE id=$1;
  RAISE EXCEPTION 'episode rewrite accepted';
 EXCEPTION WHEN raise_exception THEN
  IF SQLERRM='episode rewrite accepted' THEN RAISE; END IF;
  IF SQLERRM NOT LIKE '%immutable; add an annotation%' THEN RAISE; END IF;
 END;
END $$;
CREATE OR REPLACE FUNCTION e2e_expect_provisional_package(package_id TEXT,token TEXT) RETURNS VOID
LANGUAGE plpgsql AS $$ BEGIN
 BEGIN PERFORM ontology_package_migrate($1,$2);
  RAISE EXCEPTION 'provisional relation auto-promotion accepted';
 EXCEPTION WHEN raise_exception THEN
  IF SQLERRM='provisional relation auto-promotion accepted' THEN RAISE; END IF;
  IF SQLERRM NOT LIKE 'provisional relation promotion requires a P7 governance decision%' THEN RAISE; END IF;
 END;
END $$;
CREATE OR REPLACE FUNCTION e2e_expect_widening_package(package_id TEXT,token TEXT) RETURNS VOID
LANGUAGE plpgsql AS $$ BEGIN
 BEGIN PERFORM ontology_package_migrate($1,$2);
  RAISE EXCEPTION 'unacknowledged widening accepted';
 EXCEPTION WHEN raise_exception THEN
  IF SQLERRM='unacknowledged widening accepted' THEN RAISE; END IF;
  IF SQLERRM NOT LIKE 'ontology widening requires explicit dry-run acknowledgement%' THEN RAISE; END IF;
 END;
END $$;
CREATE OR REPLACE FUNCTION e2e_expect_stale_ontology(package_id TEXT,token TEXT) RETURNS VOID
LANGUAGE plpgsql AS $$ BEGIN
 BEGIN PERFORM ontology_package_migrate($1,$2);
  RAISE EXCEPTION 'stale ontology dry-run accepted';
 EXCEPTION WHEN raise_exception THEN
  IF SQLERRM='stale ontology dry-run accepted' THEN RAISE; END IF;
  IF SQLERRM NOT LIKE 'ontology dry-run is absent, mismatched, consumed, or stale%' THEN RAISE; END IF;
 END;
END $$;
CREATE OR REPLACE FUNCTION e2e_expect_bad_outcome(memory_id BIGINT) RETURNS VOID
LANGUAGE plpgsql AS $$ BEGIN
 PERFORM set_config('aimee.principal','user:bad-outcome-test',true);
 BEGIN PERFORM work_outcome_record('bad-outcome','bad-event','memory',memory_id::TEXT,'liked');
  RAISE EXCEPTION 'unknown outcome accepted'; EXCEPTION WHEN check_violation THEN NULL; END;
END $$;

DO $$ BEGIN
  BEGIN
    INSERT INTO memory_evidence_events(event_id,object_kind,object_id,operation,
      authenticated_actor,effective_authority,occurred_at)
    VALUES('bad-op','memory','1','invented','test','operator',pg_now_text());
    RAISE EXCEPTION 'unknown operation unexpectedly accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
  BEGIN
    INSERT INTO memory_evidence_events(event_id,object_kind,object_id,operation,before_ref,
      authenticated_actor,effective_authority,occurred_at)
    VALUES('bad-purge','memory','1','purge','memory:1:h=x','test','operator',pg_now_text());
    RAISE EXCEPTION 'purge payload unexpectedly accepted';
  EXCEPTION WHEN check_violation THEN NULL; END;
END $$;

-- A rollback proves the row and its trigger-created event share a transaction.
BEGIN;
SELECT set_config('aimee.principal','user:rollback',true);
SELECT set_config('aimee.authority','user',true);
INSERT INTO memories(key,content) VALUES('p1-rolled-back','secret');
DO $$ BEGIN ASSERT EXISTS(SELECT 1 FROM memory_evidence_events
  WHERE authenticated_actor='user:rollback' AND object_kind='memory'); END $$;
ROLLBACK;
DO $$ BEGIN
 ASSERT NOT EXISTS(SELECT 1 FROM memories WHERE key='p1-rolled-back');
 ASSERT NOT EXISTS(SELECT 1 FROM memory_evidence_events WHERE authenticated_actor='user:rollback');
END $$;

-- All core object kinds emit through the same database seam.  The scoped rows
-- intentionally share one authenticated principal but use their natural keys.
BEGIN;
SELECT set_config('aimee.principal','operator:e2e',true);
SELECT set_config('aimee.authority','operator',true);
INSERT INTO memories(key,content,content_hash) VALUES('p1-memory','memory text','mh') RETURNING id \gset mem_
INSERT INTO memory_scopes(memory_id,scope_type,scope_value) VALUES(:mem_id,'project','e2e');
INSERT INTO memory_links(source_id,target_id) VALUES(:mem_id,:mem_id);
INSERT INTO docs(content_hash,filename,normalized_text,state) VALUES('dh1','p1.md','body','active') RETURNING id \gset doc_
INSERT INTO document_versions(doc_key,doc_id,content_hash) VALUES('p1.md',:doc_id,'dh1');
INSERT INTO entity_registry(kind,created_at) VALUES(1,pg_now_text()) RETURNING canonical_id \gset ent_
INSERT INTO entity_aliases(name,name_norm,canonical_id,is_preferred) VALUES('P1','p1',:ent_canonical_id,1);
INSERT INTO rel_types(rel_type,head_kinds,tail_kinds,status) VALUES('p1_rel','1','1','active');
INSERT INTO derived_memory_registry(derived_kind,derived_memory_id) VALUES('summary','p1');
INSERT INTO ontology_packages(package_id,version,package_json,author,review_record,created_at)
 VALUES('p1-package',9001,'{}','operator:e2e','reviewed',pg_now_text());
COMMIT;
DO $$ DECLARE missing TEXT;
BEGIN
 SELECT string_agg(k,',') INTO missing FROM unnest(ARRAY['memory','scope','link','document',
   'document_version','entity','alias','rel_type','derived','ontology_package']) k
 WHERE NOT EXISTS(SELECT 1 FROM memory_evidence_events e WHERE e.object_kind=k
   AND e.authenticated_actor='operator:e2e');
 ASSERT missing IS NULL,format('missing object-kind events: %s',missing);
 ASSERT NOT EXISTS(SELECT 1 FROM memory_evidence_events WHERE authenticated_actor='operator:e2e'
   AND transport_identity='');
END $$;

-- Dropping any covered emitter makes that table fail closed. Each DROP and
-- rejected UPDATE lives in a PL/pgSQL subtransaction, so the emitter is restored
-- automatically before the next object kind is tested.
SELECT e2e_expect_unhooked('memories','evidence_memories','key=key');
SELECT e2e_expect_unhooked('docs','evidence_docs','filename=filename');
SELECT e2e_expect_unhooked('document_versions','evidence_document_versions','doc_key=doc_key');
SELECT e2e_expect_unhooked('entity_registry','evidence_entities','kind=kind');
SELECT e2e_expect_unhooked('entity_aliases','evidence_aliases','name=name');
SELECT e2e_expect_unhooked('rel_types','evidence_rel_types','rel_type=rel_type');
SELECT e2e_expect_unhooked('derived_memory_registry','evidence_derived','response_policy=response_policy');
SELECT e2e_expect_unhooked('memory_scopes','evidence_scopes','scope_value=scope_value');
SELECT e2e_expect_unhooked('memory_links','evidence_links','relation=relation');
SELECT e2e_expect_unhooked('ontology_packages','evidence_ontology_packages','author=author');

-- Maintenance changes N rows and preserves the caller's correlation id on N events.
BEGIN;
SELECT set_config('aimee.principal','internal:maintenance-e2e',true);
SELECT set_config('aimee.authority','system',true);
SELECT set_config('aimee.correlation_id','sweep:e2e',true);
INSERT INTO memories(key,content) VALUES('sweep-1','a'),('sweep-2','b'),('sweep-3','c');
UPDATE memories SET lifecycle_state='archived' WHERE key LIKE 'sweep-%';
COMMIT;
DO $$ BEGIN ASSERT (SELECT count(*) FROM memory_evidence_events
 WHERE correlation_id='sweep:e2e' AND operation='supersede')=3; END $$;

-- A turn uses the retrieval event/turn identifier as the same join key on its
-- writes; no special interactive writer path is needed.
BEGIN;
SELECT set_config('aimee.principal','user:turn-e2e',true);
SELECT set_config('aimee.authority','user',true);
SELECT set_config('aimee.correlation_id','retrieval-event:turn-e2e',true);
INSERT INTO memories(key,content) VALUES('turn-correlated-write','joined');
COMMIT;
SELECT e2e_assert(EXISTS(SELECT 1 FROM memory_evidence_events
 WHERE authenticated_actor='user:turn-e2e' AND correlation_id='retrieval-event:turn-e2e'),
 'turn mutation joins retrieval evidence');

-- Changeset show/preview are immutable projections, including honest purge refusal.
DO $$ DECLARE cid TEXT; before_row TEXT; preview JSONB;
BEGIN
 SELECT changeset_id,to_jsonb(k)::TEXT INTO cid,before_row FROM knowledge_changesets k
  WHERE operation='memory.assert' ORDER BY created_at LIMIT 1;
 ASSERT knowledge_changeset_show(cid)->'items' IS NOT NULL;
 INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,
   status,reversible,irreversible_why,created_at,closed_at)
 VALUES('irreversible-e2e','document.purge','operator:e2e','operator',40,'applied',0,
   'content physically removed',pg_now_text(),pg_now_text());
 preview:=knowledge_changeset_preview_revert('irreversible-e2e');
 ASSERT preview->>'reversible'='false';
 ASSERT preview->>'refusal'='content physically removed';
 ASSERT before_row=(SELECT to_jsonb(k)::TEXT FROM knowledge_changesets k WHERE changeset_id=cid);
END $$;
SELECT e2e_expect_operation_change((SELECT commit_id FROM fact_graph_commits
 WHERE status='applied' ORDER BY created_at LIMIT 1));

-- Per-item authority refusal: an operator (rank 40) cannot revert a changeset
-- whose authenticated writer had a stronger rank. No target item is changed.
BEGIN;
SELECT set_config('aimee.principal','operator:authority-test',true);
SELECT set_config('aimee.authority','operator',true);
INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status,
 reversible,created_at) VALUES('higher-authority-e2e','memory.assert','root:governance','operator',
 50,'open',1,pg_now_text());
SELECT set_config('aimee.changeset_id','higher-authority-e2e',true);
INSERT INTO memories(key,content) VALUES('higher-authority-item','preserve') RETURNING id \gset ha_
UPDATE fact_graph_commits SET status='applied',closed_at=pg_now_text() WHERE commit_id='higher-authority-e2e';
SELECT set_config('aimee.changeset_id','',true);
SELECT e2e_expect_lower_authority_revert('higher-authority-e2e');
COMMIT;
SELECT e2e_assert((SELECT lifecycle_state FROM memories WHERE id=:ha_id)='active','lower authority changes nothing');

-- P2 conflict refusal and force-partial compensation. Historical target rows
-- and P1 events remain append-only; the revert is a new linked changeset.
BEGIN;
SELECT set_config('aimee.principal','operator:revert',true);
SELECT set_config('aimee.authority','operator',true);
INSERT INTO fact_graph_commits(commit_id,operation,origin_ref,actor_principal,actor_role,
 authority_rank,status,reversible,created_at)
 VALUES('e2e-revert-target','memory.assert','e2e:revert','operator:revert','operator',40,'open',1,pg_now_text());
SELECT set_config('aimee.changeset_id','e2e-revert-target',true);
INSERT INTO memories(key,content) VALUES('revert-a','a') RETURNING id \gset rv_a_
INSERT INTO memories(key,content) VALUES('revert-b','b') RETURNING id \gset rv_b_
UPDATE fact_graph_commits SET status='applied',closed_at=pg_now_text() WHERE commit_id='e2e-revert-target';
SELECT set_config('aimee.changeset_id','',true);
COMMIT;
SELECT md5(string_agg(to_jsonb(x)::text,'' ORDER BY id)) AS items_hash FROM fact_graph_changes x
 WHERE commit_id='e2e-revert-target' \gset rv_items_
SELECT md5(string_agg(to_jsonb(x)::text,'' ORDER BY event_id)) AS events_hash FROM memory_evidence_events x
 WHERE changeset_id='e2e-revert-target' \gset rv_events_
BEGIN;
SELECT set_config('aimee.principal','operator:later',true);
SELECT set_config('aimee.authority','operator',true);
UPDATE memories SET content='later-a' WHERE id=:rv_a_id;
COMMIT;
DO $$ BEGIN
 PERFORM set_config('aimee.principal','operator:revert',true);
 PERFORM set_config('aimee.authority','operator',true);
 BEGIN
  PERFORM knowledge_changeset_revert('e2e-revert-target',false);
  RAISE EXCEPTION 'default conflicting revert accepted';
 EXCEPTION WHEN raise_exception THEN
  IF SQLERRM='default conflicting revert accepted' THEN RAISE; END IF;
 END;
END $$;
BEGIN;
SELECT set_config('aimee.principal','operator:revert',true);
SELECT set_config('aimee.authority','operator',true);
SELECT knowledge_changeset_revert('e2e-revert-target',true) AS partial \gset rv_partial_
COMMIT;
SELECT e2e_assert((:'rv_partial_partial'::jsonb)->>'partial'='true','force partial reports partial');
SELECT e2e_assert((:'rv_partial_partial'::jsonb)->'skipped_items' @>
 '[{"object_kind":"memory","why":"later-change"}]'::jsonb,'force partial names conflict');
SELECT e2e_assert((SELECT lifecycle_state FROM memories WHERE id=:rv_a_id)='active','conflict skipped');
SELECT e2e_assert((SELECT lifecycle_state FROM memories WHERE id=:rv_b_id)='archived','remaining item reverted');
SELECT e2e_assert((SELECT reverts_changeset FROM fact_graph_commits
 WHERE commit_id=((:'rv_partial_partial'::jsonb)->>'changeset_id'))='e2e-revert-target','revert linkage');
SELECT e2e_assert((SELECT md5(string_agg(to_jsonb(x)::text,'' ORDER BY id)) FROM fact_graph_changes x
 WHERE commit_id='e2e-revert-target')=:'rv_items_items_hash','target items immutable');
SELECT e2e_assert((SELECT md5(string_agg(to_jsonb(x)::text,'' ORDER BY event_id)) FROM memory_evidence_events x
 WHERE changeset_id='e2e-revert-target')=:'rv_events_events_hash','target events immutable');

-- Seed an assertion through the guarded graph seam and attach document evidence.
BEGIN;
SELECT set_config('aimee.principal','user:fact-owner',true);
SELECT set_config('aimee.authority','user',true);
INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status,
 reversible,created_at) VALUES('fact-seed-e2e','fact.assert','user:fact-owner','user',30,'open',1,pg_now_text());
INSERT INTO entity_edges(source,relation,target,edge_class,confidence_class,confidence,asserted_at,
 assertion_kind,epistemic_kind,lifecycle_state,authority_rank,actor_principal,version,commit_id,
 subject_kind,object_kind)
VALUES('alice','p1_rel','answer','semantic','A',0.9,pg_now_text(),'world_fact','world_fact',
 'persistent',30,'user:fact-owner',1,'fact-seed-e2e',1,1) RETURNING id \gset edge_
INSERT INTO fact_graph_changes(commit_id,assertion_id,action,existed_after,after_lifecycle,
 after_confidence,after_authority_rank,after_version) VALUES('fact-seed-e2e',:edge_id,'insert',1,
 'persistent',0.9,30,1);
INSERT INTO fact_evidence(assertion_id,source_kind,source_id,source_span,evidence_hash,actor_principal,
 commit_id,created_at) VALUES(:edge_id,'document',:doc_id::TEXT,'1:1','span-hash','user:fact-owner',
 'fact-seed-e2e',pg_now_text());
UPDATE fact_graph_commits SET status='applied',closed_at=pg_now_text() WHERE commit_id='fact-seed-e2e';
COMMIT;

-- P3 invalidation is preview-bound, changes no fact identity, keeps user facts in
-- recall with re-verification, and marks dependencies without deleting them.
BEGIN;
SELECT set_config('aimee.principal','operator:document',true);
SELECT set_config('aimee.authority','operator',true);
INSERT INTO derived_memory_registry(derived_kind,derived_memory_id,current_status)
 VALUES('summary','doc-summary','fresh');
INSERT INTO derived_memory_dependencies(derived_kind,derived_memory_id,input_kind,input_id,
 input_version,source_hash,contribution) VALUES('summary','doc-summary','document',:doc_id::TEXT,'1','dh1','essential');
SELECT document_lifecycle_preview(:doc_id,'invalidate') AS p \gset inv_
SELECT document_lifecycle_apply(:doc_id,'invalidate',(:'inv_p'::jsonb)->>'preview_token','bad source') AS a \gset inva_
COMMIT;
SELECT e2e_assert((SELECT state FROM docs WHERE id=:doc_id)='invalidated','document invalidated');
SELECT e2e_assert((SELECT support_status FROM entity_edges WHERE id=:edge_id)='unsupported','fact unsupported');
SELECT e2e_assert((SELECT reverify_needed FROM entity_edges WHERE id=:edge_id)=1,'user fact reverify');
SELECT e2e_assert(EXISTS(SELECT 1 FROM fact_assertions_current WHERE id=:edge_id),'user fact recalled');
SELECT e2e_assert(EXISTS(SELECT 1 FROM derived_memory_dependencies WHERE derived_memory_id='doc-summary'),'dependency retained');
SELECT e2e_assert((SELECT current_status FROM derived_memory_registry WHERE derived_memory_id='doc-summary')='unsupported','derived unsupported');
SELECT e2e_assert(EXISTS(SELECT 1 FROM operator_review_surface WHERE item_id='document:'||:doc_id::TEXT),'document review item');
SELECT e2e_assert(((:'inv_p'::jsonb)->'facts_only_ids') @> to_jsonb(:edge_id),
 'blast radius names affected assertion');

-- Re-ingest retires the prior document/version while retaining assertion and
-- corroboration identity. The version batches remain discoverable as one
-- origin_ref group keyed by the stable document key.
BEGIN;
SELECT set_config('aimee.principal','user:reingest',true);
SELECT set_config('aimee.authority','user',true);
INSERT INTO docs(content_hash,filename,normalized_text,state) VALUES('re-old','same.md','old','active') RETURNING id \gset re_old_
INSERT INTO document_versions(doc_key,doc_id,content_hash,version_no) VALUES('stable-doc-key',:re_old_id,'re-old',1) RETURNING id \gset re_v1_
INSERT INTO fact_graph_commits(commit_id,operation,origin_ref,actor_principal,actor_role,
 authority_rank,status,reversible,created_at) VALUES('reingest-fact','fact.assert','stable-doc-key',
 'user:reingest','user',30,'open',1,pg_now_text());
INSERT INTO entity_edges(source,relation,target,edge_class,confidence_class,confidence,asserted_at,
 assertion_kind,epistemic_kind,lifecycle_state,authority_rank,actor_principal,version,commit_id,
 subject_kind,object_kind)
VALUES('reingest','p1_rel','identity','semantic','A',0.9,pg_now_text(),'world_fact','world_fact',
 'persistent',30,'user:reingest',1,'reingest-fact',1,1) RETURNING id \gset re_edge_
INSERT INTO fact_graph_changes(commit_id,assertion_id,action,existed_after,after_lifecycle,
 after_confidence,after_authority_rank,after_version) VALUES('reingest-fact',:re_edge_id,'insert',1,
 'persistent',0.9,30,1);
INSERT INTO fact_evidence(assertion_id,source_kind,source_id,evidence_hash,actor_principal,commit_id,
 created_at) VALUES(:re_edge_id,'document',:re_old_id::TEXT,'stable-span','user:reingest',
 'reingest-fact',pg_now_text());
UPDATE fact_graph_commits SET status='applied',closed_at=pg_now_text() WHERE commit_id='reingest-fact';
COMMIT;
BEGIN;
SELECT set_config('aimee.principal','user:reingest',true);
SELECT set_config('aimee.authority','user',true);
INSERT INTO docs(content_hash,filename,normalized_text,state) VALUES('re-new','same.md','new','active') RETURNING id \gset re_new_
INSERT INTO document_versions(doc_key,doc_id,content_hash,version_no) VALUES('stable-doc-key',:re_new_id,'re-new',2) RETURNING id \gset re_v2_
COMMIT;
SELECT e2e_assert((SELECT state FROM docs WHERE id=:re_old_id)='retired','prior doc retired');
SELECT e2e_assert(NOT (SELECT is_current FROM document_versions WHERE id=:re_v1_id),'prior version retired');
SELECT e2e_assert((SELECT is_current FROM document_versions WHERE id=:re_v2_id),'new version current');
SELECT e2e_assert(EXISTS(SELECT 1 FROM fact_assertions_current WHERE id=:re_edge_id),'fact identity retained');
SELECT e2e_assert(EXISTS(SELECT 1 FROM fact_evidence WHERE assertion_id=:re_edge_id AND evidence_hash='stable-span'),'corroboration retained');
SELECT e2e_assert(jsonb_array_length(knowledge_changeset_show('stable-doc-key')->'changesets')>=2,'origin group spans batches');

-- Purge deletes all content-bearing stores, traces and vectors but retains its
-- shell, content-free receipt, and lifecycle event.  A stale token is refused.
BEGIN;
SELECT set_config('aimee.principal','operator:purge',true);
SELECT set_config('aimee.authority','operator',true);
INSERT INTO docs(content_hash,filename,normalized_text,state) VALUES('dh-purge','purge.md','private','active') RETURNING id \gset purge_
INSERT INTO document_sections(doc_id,heading_path,heading,content_hash) VALUES(:purge_id,'/h','secret heading','sh');
INSERT INTO kb_documents(project,file_path,file_hash,chunk_index,content) VALUES('e2e','purge.md','dh-purge',0,'private') RETURNING id \gset chunk_
INSERT INTO kb_doc_regions(chunk_id,document_key,quote) VALUES(:chunk_id,'purge.md','private') RETURNING id \gset region_
INSERT INTO kb_table_cells(region_id,document_key,cell_text) VALUES(:region_id,'purge.md','private');
INSERT INTO kb_embeddings(point_id) VALUES(:chunk_id);
INSERT INTO kb_pdf_embeddings(point_id) VALUES(:chunk_id);
SELECT document_lifecycle_preview(:purge_id,'purge') AS p \gset pp_
INSERT INTO memories(key,content) VALUES('intervening-head','x');
SELECT e2e_expect_stale_document(:purge_id,(:'pp_p'::jsonb)->>'preview_token');
SELECT document_lifecycle_preview(:purge_id,'purge') AS p \gset pp2_
SELECT document_lifecycle_apply(:purge_id,'purge',(:'pp2_p'::jsonb)->>'preview_token','erase') AS a \gset purge_apply_
COMMIT;
SELECT e2e_assert((SELECT state FROM docs WHERE id=:purge_id)='purged','purged state');
SELECT e2e_assert((SELECT normalized_text FROM docs WHERE id=:purge_id)='','purged body');
SELECT e2e_assert(NOT EXISTS(SELECT 1 FROM document_sections WHERE doc_id=:purge_id),'sections removed');
SELECT e2e_assert(NOT EXISTS(SELECT 1 FROM kb_documents WHERE file_path='purge.md'),'chunks removed');
SELECT e2e_assert(NOT EXISTS(SELECT 1 FROM kb_doc_regions WHERE document_key='purge.md'),'regions removed');
SELECT e2e_assert(NOT EXISTS(SELECT 1 FROM kb_table_cells WHERE document_key='purge.md'),'cells removed');
SELECT e2e_assert(NOT EXISTS(SELECT 1 FROM kb_embeddings WHERE point_id=:chunk_id),'vectors removed');
SELECT e2e_assert(NOT EXISTS(SELECT 1 FROM kb_pdf_embeddings WHERE point_id=:chunk_id),'pdf vectors removed');
SELECT e2e_assert(EXISTS(SELECT 1 FROM document_purge_receipts WHERE doc_id=:purge_id AND selector='doc_id='||:purge_id::TEXT),'purge receipt');
SELECT e2e_assert(EXISTS(SELECT 1 FROM memory_evidence_events WHERE object_kind='document'
 AND object_id=:purge_id::TEXT AND operation='purge' AND before_ref='' AND after_ref=''),'purge event');

-- P4 one shared status function, reverse lookup, and re-derivation scheduling.
INSERT INTO derived_memory_registry(derived_kind,derived_memory_id)
 VALUES('summary','dependency-not-recorded');
SELECT e2e_assert((SELECT status FROM derived_memory_freshness
 WHERE derived_memory_id='dependency-not-recorded')='dependencies:not-recorded',
 'undeclared dependencies are explicit');
SELECT e2e_assert(knowledge_input_moved('document',:doc_id::TEXT,'','dh1','','summary'),'shared stale predicate');
SELECT e2e_assert((SELECT status FROM derived_memory_freshness WHERE derived_memory_id='doc-summary')='unsupported','freshness projection');
SELECT e2e_assert(EXISTS(SELECT 1 FROM derived_memory_dependencies WHERE input_kind='document'
 AND input_id=:doc_id::TEXT AND derived_memory_id='doc-summary'),'reverse dependency');

-- Fine-grained code and policy invalidation touches only recorded inputs, and
-- the read-only reconciler proves that event-driven maintenance left no drift.
BEGIN;
INSERT INTO projects(name,root,workspace,scanned_at) VALUES('e2e-p4','/e2e/p4','e2e',pg_now_text()) RETURNING id \gset p4_project_
INSERT INTO files(project_id,generation,path,hash,scanned_at) VALUES(:p4_project_id,1,'a.c','hash-a',pg_now_text()) RETURNING id \gset p4_file_a_
INSERT INTO files(project_id,generation,path,hash,scanned_at) VALUES(:p4_project_id,1,'b.c','hash-b',pg_now_text()) RETURNING id \gset p4_file_b_
SELECT derived_memory_declare('recommendation','rec-a',jsonb_build_array(jsonb_build_object(
 'input_kind','code_unit','input_id',:p4_file_a_id::TEXT,'input_version','1','source_hash','hash-a')));
SELECT derived_memory_declare('recommendation','rec-b',jsonb_build_array(jsonb_build_object(
 'input_kind','code_unit','input_id',:p4_file_b_id::TEXT,'input_version','1','source_hash','hash-b')));
INSERT INTO derivation_policy_versions(derived_kind,current_version,updated_at)
 VALUES('policy-old','v1',pg_now_text()),('policy-new','v2',pg_now_text());
SELECT derived_memory_declare('policy-old','old-item',jsonb_build_array(jsonb_build_object(
 'input_kind','memory','input_id',:mem_id::TEXT,'derivation_policy_version','v1')));
SELECT derived_memory_declare('policy-new','new-item',jsonb_build_array(jsonb_build_object(
 'input_kind','memory','input_id',:mem_id::TEXT,'derivation_policy_version','v2')));
UPDATE files SET hash='hash-a2' WHERE id=:p4_file_a_id;
UPDATE derivation_policy_versions SET current_version='v3' WHERE derived_kind='policy-old';
COMMIT;
SELECT e2e_assert((SELECT current_status FROM derived_memory_registry WHERE derived_memory_id='rec-a')='stale','changed file stale');
SELECT e2e_assert((SELECT current_status FROM derived_memory_registry WHERE derived_memory_id='rec-b')='fresh','unrelated file fresh');
SELECT e2e_assert((SELECT current_status FROM derived_memory_registry WHERE derived_memory_id='old-item')='stale','old policy stale');
SELECT e2e_assert((SELECT current_status FROM derived_memory_registry WHERE derived_memory_id='new-item')='fresh','new policy untouched');
SELECT e2e_assert(derived_memory_reconcile()=0,'event-driven derived state has zero drift');

-- P5 separates outcomes from truth and produces workflow-local preference plus
-- global contest. Default ranking weight stays zero.
SELECT md5(to_jsonb(x)::TEXT) AS structural_hash FROM (SELECT tier,confidence,lifecycle_state,
 epistemic_kind,governance_promoted FROM memories WHERE id=:mem_id) x \gset outcome_before_
BEGIN;
SELECT set_config('aimee.principal','user:evaluator-1',true);
SELECT work_outcome_record('out-1','retr-1','memory',:mem_id::TEXT,'useful','t1','workflow-a');
SELECT work_outcome_record('out-2','retr-2','memory',:mem_id::TEXT,'useful','t2','workflow-a');
SELECT work_outcome_record('out-3','retr-3','memory',:mem_id::TEXT,'dead_end','t3','workflow-b');
SELECT work_outcome_record('out-4','retr-4','memory',:mem_id::TEXT,'dead_end','t4','workflow-b');
COMMIT;
SELECT e2e_assert((SELECT status FROM work_outcome_projection WHERE subject_kind='memory'
 AND subject_id=:mem_id::TEXT AND workflow='workflow-a')='preferred','workflow preferred');
SELECT e2e_assert((SELECT status FROM work_outcome_projection WHERE subject_kind='memory'
 AND subject_id=:mem_id::TEXT AND workflow='*')='contested','global contested');
SELECT e2e_assert((SELECT outcome_rank_weight FROM evidence_lifecycle_settings)=0,'default zero rank weight');
SELECT e2e_assert((SELECT lifecycle_state FROM memories WHERE id=:mem_id)='active','truth unchanged');
SELECT e2e_assert((SELECT md5(to_jsonb(x)::TEXT) FROM (SELECT tier,confidence,lifecycle_state,
 epistemic_kind,governance_promoted FROM memories WHERE id=:mem_id) x)=:'outcome_before_structural_hash','outcome structural isolation');
SELECT e2e_expect_bad_outcome(:mem_id);
BEGIN;
SELECT set_config('aimee.principal','user:stale-evaluator',true);
SELECT work_outcome_record('stale-code-outcome','stale-code-event','code_unit',:p4_file_a_id::TEXT,
 'useful','code task','code-flow','global','','','','hash-a',1);
COMMIT;
SELECT e2e_assert((SELECT status FROM work_outcome_projection WHERE subject_kind='code_unit'
 AND subject_id=:p4_file_a_id::TEXT AND workflow='code-flow')='stale','outcome reuses P4 stale predicate');

-- P6 closed kind values and the mental-model governance gate.
SELECT e2e_expect_bad_kind(:mem_id);
SELECT e2e_assert((SELECT epistemic_kind FROM memories WHERE id=:mem_id)='world_fact','kind unchanged');
BEGIN;
SELECT set_config('aimee.principal','user:kind-axis',true);
SELECT set_config('aimee.authority','user',true);
INSERT INTO memories(key,content,epistemic_kind,provenance_category)
 VALUES('kind-axis-user','same axis content','hypothesis','user_stated') RETURNING id \gset kind_user_
COMMIT;
BEGIN;
SELECT set_config('aimee.principal','model:kind-axis',true);
SELECT set_config('aimee.authority','model',true);
INSERT INTO memories(key,content,epistemic_kind,provenance_category)
 VALUES('kind-axis-model','same axis content','hypothesis','agent_message') RETURNING id \gset kind_model_
COMMIT;
SELECT e2e_assert((SELECT epistemic_kind FROM memories WHERE id=:kind_user_id)=
 (SELECT epistemic_kind FROM memories WHERE id=:kind_model_id),'kind independent of authority');
SELECT e2e_assert((SELECT effective_authority FROM memory_evidence_events WHERE object_kind='memory'
 AND object_id=:kind_user_id::TEXT ORDER BY recorded_at DESC LIMIT 1)<>
 (SELECT effective_authority FROM memory_evidence_events WHERE object_kind='memory'
 AND object_id=:kind_model_id::TEXT ORDER BY recorded_at DESC LIMIT 1),'authority remains distinct');
INSERT INTO memories(key,content,epistemic_kind) VALUES('immutable-episode','observed once','episode') RETURNING id \gset episode_
SELECT e2e_expect_episode_rewrite(:episode_id);
SELECT e2e_assert((SELECT count(*) FROM kind_lifecycle WHERE kind IN ('world_fact','episode','experience',
 'mental_model','preference','instruction','policy','hypothesis'))=8,'all epistemic lifecycle policies exist');
-- A row-local migration override yields exactly the same expiry set that the
-- legacy free-text kind policy would have selected.
INSERT INTO memories(key,content,tier,kind,epistemic_kind,last_used_at,expiry_days_migration_override)
 VALUES('legacy-task-expired','x','L1','task','world_fact',pg_now_text('-8 days'),7),
       ('legacy-fact-current','x','L1','fact','world_fact',pg_now_text('-8 days'),30);
SELECT e2e_assert((SELECT array_agg(key ORDER BY key) FROM memories m WHERE key LIKE 'legacy-%'
 AND m.last_used_at < pg_now_text('-'||(SELECT expire_days FROM kind_lifecycle k
   WHERE k.kind=m.kind)||' days'))=
 (SELECT array_agg(key ORDER BY key) FROM memories m WHERE key LIKE 'legacy-%'
 AND m.last_used_at < pg_now_text('-'||COALESCE(m.expiry_days_migration_override,
   (SELECT expire_days FROM kind_lifecycle WHERE kind=m.epistemic_kind))||' days')),
 'epistemic migration preserves expiry set');
BEGIN;
INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status,
 reversible,created_at) VALUES('mental-direct','fact.assert','operator:e2e','operator',40,'open',1,pg_now_text());
DO $$ BEGIN
 BEGIN
  INSERT INTO entity_edges(source,relation,target,edge_class,assertion_kind,epistemic_kind,
   lifecycle_state,authority_rank,commit_id) VALUES('m','p1_rel','x','semantic','mental_model',
   'mental_model','candidate',40,'mental-direct');
  RAISE EXCEPTION 'mental model bypass accepted';
 EXCEPTION WHEN raise_exception THEN
  IF SQLERRM='mental model bypass accepted' THEN RAISE; END IF;
 END;
END $$;
ROLLBACK;

-- P7 exposes explicit markers and kind-dependent decision sets. Requesting
-- evidence is itself a changeset/event and records the settling condition.
BEGIN;
SELECT set_config('aimee.principal','operator:review',true);
SELECT set_config('aimee.authority','operator',true);
INSERT INTO memories(key,content,epistemic_kind) VALUES('conflict-a','a','episode') RETURNING id \gset ca_
INSERT INTO memories(key,content) VALUES('conflict-b','b') RETURNING id \gset cb_
INSERT INTO memory_conflicts(memory_a,memory_b,detected_at) VALUES(:ca_id,:cb_id,pg_now_text()) RETURNING id \gset conflict_
SELECT item_head FROM operator_review_surface WHERE item_id='conflict:'||:conflict_id::TEXT \gset review_
SELECT operator_review_decide('conflict:'||:conflict_id::TEXT,:'review_item_head',
 'request_evidence','two independent sources','') AS decision \gset decision_
COMMIT;
SELECT e2e_assert(EXISTS(SELECT 1 FROM review_evidence_requests WHERE item_id='conflict:'||:conflict_id::TEXT
 AND condition='two independent sources'),'evidence condition');
SELECT e2e_assert(EXISTS(SELECT 1 FROM knowledge_review_decisions WHERE item_id='conflict:'||:conflict_id::TEXT),'review decision');
SELECT e2e_assert(EXISTS(SELECT 1 FROM memory_evidence_events WHERE object_kind='review'
 AND object_id='conflict:'||:conflict_id::TEXT),'review event');
BEGIN;
SELECT set_config('aimee.principal','operator:review',true);
SELECT set_config('aimee.authority','operator',true);
SELECT knowledge_changeset_revert((:'decision_decision'::jsonb)->>'changeset_id',false) AS reverted \gset review_revert_
COMMIT;
SELECT e2e_assert((SELECT resolved FROM memory_conflicts WHERE id=:conflict_id)=0,'review queue state reverted');
SELECT e2e_assert((SELECT state FROM review_evidence_requests WHERE item_id='conflict:'||:conflict_id::TEXT)='reverted','evidence request reverted');
SELECT e2e_assert((SELECT reverted_by_changeset FROM knowledge_review_decisions
 WHERE changeset_id=(:'decision_decision'::jsonb)->>'changeset_id')<>'','review decision linked to compensation');
SELECT e2e_assert(EXISTS(SELECT 1 FROM memory_evidence_events WHERE changeset_id=
 ((:'review_revert_reverted'::jsonb)->>'changeset_id') AND object_kind='review'),'review revert ledger event');

-- Offered decisions are kind-specific, and a stale head returns a recomputed
-- item rather than mutating the queue.
SELECT e2e_assert((SELECT offered_decisions ? 'annotate' AND NOT offered_decisions ? 'correct'
 FROM operator_review_surface WHERE item_id='conflict:'||:conflict_id::TEXT),'episode decisions');
BEGIN;
SELECT set_config('aimee.principal','operator:review',true);
SELECT set_config('aimee.authority','operator',true);
INSERT INTO memories(key,content,epistemic_kind) VALUES('instruction-review','do this','instruction') RETURNING id \gset instruction_
INSERT INTO memories(key,content) VALUES('instruction-peer','peer') RETURNING id \gset instruction_peer_
INSERT INTO memory_conflicts(memory_a,memory_b,detected_at) VALUES(:instruction_id,:instruction_peer_id,pg_now_text()) RETURNING id \gset instruction_conflict_
INSERT INTO memories(key,content,epistemic_kind) VALUES('hypothesis-review','maybe','hypothesis') RETURNING id \gset hypothesis_
INSERT INTO memories(key,content) VALUES('hypothesis-peer','peer') RETURNING id \gset hypothesis_peer_
INSERT INTO memory_conflicts(memory_a,memory_b,detected_at) VALUES(:hypothesis_id,:hypothesis_peer_id,pg_now_text()) RETURNING id \gset hypothesis_conflict_
COMMIT;
SELECT e2e_assert((SELECT offered_decisions ? 'revoke' AND NOT offered_decisions ? 'retire'
 FROM operator_review_surface WHERE item_id='conflict:'||:instruction_conflict_id::TEXT),'instruction decisions');
SELECT e2e_assert((SELECT offered_decisions ? 'resolve'
 FROM operator_review_surface WHERE item_id='conflict:'||:hypothesis_conflict_id::TEXT),'hypothesis decisions');
BEGIN;
SELECT set_config('aimee.principal','operator:review',true);
SELECT set_config('aimee.authority','operator',true);
SELECT operator_review_decide('conflict:'||:conflict_id::TEXT,:'review_item_head',
 'request_evidence','new condition','') AS stale_decision \gset stale_
COMMIT;
SELECT e2e_assert((:'stale_stale_decision'::jsonb)->>'stale'='true','stale review refused');
SELECT e2e_assert((:'stale_stale_decision'::jsonb)->'recomputed_item' IS NOT NULL,'stale review recomputed');

-- Policy promotion is an explicit operator decision with an evidence snapshot;
-- reverting it removes policy force and reopens the source queue.
BEGIN;
SELECT set_config('aimee.principal','model:policy-author',true);
SELECT set_config('aimee.authority','model',true);
INSERT INTO memories(key,content,epistemic_kind,governance_promoted)
 VALUES('policy-candidate','candidate policy','policy',0) RETURNING id \gset policy_
INSERT INTO memories(key,content) VALUES('policy-peer','peer') RETURNING id \gset policy_peer_
INSERT INTO memory_conflicts(memory_a,memory_b,detected_at) VALUES(:policy_id,:policy_peer_id,pg_now_text()) RETURNING id \gset policy_conflict_
COMMIT;
SELECT item_head FROM operator_review_surface WHERE item_id='conflict:'||:policy_conflict_id::TEXT \gset policy_review_
BEGIN;
SELECT set_config('aimee.principal','operator:policy-review',true);
SELECT set_config('aimee.authority','operator',true);
SELECT operator_review_decide('conflict:'||:policy_conflict_id::TEXT,:'policy_review_item_head',
 'promote','','') AS promotion \gset policy_promote_
COMMIT;
SELECT e2e_assert((SELECT governance_promoted FROM memories WHERE id=:policy_id)=1,'policy force promoted');
SELECT e2e_assert(EXISTS(SELECT 1 FROM knowledge_review_decisions WHERE changeset_id=
 (:'policy_promote_promotion'::jsonb)->>'changeset_id' AND authenticated_actor='operator:policy-review'
 AND evidence_snapshot<>'' AND resulting_authority='operator'),'promotion provenance');
BEGIN;
SELECT set_config('aimee.principal','operator:policy-review',true);
SELECT set_config('aimee.authority','operator',true);
SELECT knowledge_changeset_revert((:'policy_promote_promotion'::jsonb)->>'changeset_id',false);
COMMIT;
SELECT e2e_assert((SELECT governance_promoted FROM memories WHERE id=:policy_id)=0,'policy promotion reverted');

-- P8 stable export/import, mandatory bound dry run, one changeset migration,
-- exact count reconciliation, then rollback without deletion.
BEGIN;
SELECT set_config('aimee.principal','operator:ontology',true);
SELECT set_config('aimee.authority','operator',true);
SELECT ontology_package_export() AS package \gset op_
SELECT ontology_package_import(:'op_package'::jsonb,'operator:ontology','e2e review','') AS id \gset opid_
SELECT ontology_package_export() AS package2 \gset op2_
SELECT e2e_assert((:'op_package'::jsonb)-'package_id'=(:'op2_package2'::jsonb)-'package_id','stable ontology export');
SELECT ontology_package_dry_run(:'opid_id',true) AS dry \gset opdry_
SELECT ontology_package_migrate(:'opid_id',(:'opdry_dry'::jsonb)->>'preview_token') AS migration \gset opmig_
COMMIT;
SELECT e2e_assert((SELECT active FROM ontology_package_versions WHERE package_id=:'opid_id')=1,'package active');
SELECT e2e_assert(ontology_package_report((:'opmig_migration'::jsonb)->>'changeset_id') IS NOT NULL,'migration report');

-- A declared endpoint widening is explained and cannot activate unless the
-- bound dry-run explicitly acknowledges it.
BEGIN;
SELECT set_config('aimee.principal','operator:ontology',true);
SELECT set_config('aimee.authority','operator',true);
SELECT jsonb_set(jsonb_set(ontology_package_export()-'package_id','{version}','9002'::jsonb),
 '{relation_types}',COALESCE((SELECT jsonb_agg(CASE WHEN x->>'rel_type'='p1_rel'
   THEN jsonb_set(x,'{head_kinds}','"any"'::jsonb) ELSE x END ORDER BY x->>'rel_type')
   FROM jsonb_array_elements((ontology_package_export()->'relation_types')) x),'[]'::jsonb)) AS package \gset wide_pkg_
SELECT ontology_package_import(:'wide_pkg_package'::jsonb,'operator:ontology','widening review','') AS id \gset wide_id_
SELECT ontology_package_dry_run(:'wide_id_id',false) AS dry \gset wide_dry_
SELECT e2e_assert(((:'wide_dry_dry'::jsonb)->>'widenings')::bigint>0,'ontology widening reported');
SELECT e2e_assert(jsonb_array_length((:'wide_dry_dry'::jsonb)->'widenings_detail')>0,'ontology widening explained');
SELECT e2e_expect_widening_package(:'wide_id_id',(:'wide_dry_dry'::jsonb)->>'preview_token');
ROLLBACK;

-- A second package deliberately removes p1_rel (endpoint narrowing) and tries
-- to activate a provisional relation. Migration refuses until P7 records the
-- governance decision; its dry-run count then equals the exact retired facts.
BEGIN;
SELECT set_config('aimee.principal','operator:ontology',true);
SELECT set_config('aimee.authority','operator',true);
INSERT INTO rel_types(rel_type,head_kinds,tail_kinds,status)
 VALUES('governed_new_rel','any','any','provisional') RETURNING id \gset governed_rel_
INSERT INTO ontology_evaluations(rel_type,occurrence_count,status,created_at)
 VALUES('governed_new_rel',999,'pending',pg_now_text()) RETURNING id \gset governed_eval_
SELECT ontology_package_export() AS raw \gset opnext_
SELECT jsonb_set(:'opnext_raw'::jsonb-'package_id','{relation_types}',COALESCE((SELECT jsonb_agg(
 CASE WHEN x->>'rel_type'='governed_new_rel' THEN jsonb_set(x,'{status}','"active"'::jsonb)
      ELSE x END ORDER BY x->>'rel_type') FROM jsonb_array_elements(
 (:'opnext_raw'::jsonb)->'relation_types') x WHERE x->>'rel_type'<>'p1_rel'),'[]'::jsonb)) AS package \gset opnext_pkg_
SELECT ontology_package_import(:'opnext_pkg_package'::jsonb,'operator:ontology','narrowing review','') AS id \gset opnext_id_
SELECT ontology_package_dry_run(:'opnext_id_id',true) AS dry \gset opnext_dry_
SELECT e2e_expect_provisional_package(:'opnext_id_id',(:'opnext_dry_dry'::jsonb)->>'preview_token');
COMMIT;
SELECT item_head FROM operator_review_surface WHERE item_id='ontology:'||:governed_eval_id::TEXT \gset governed_review_
BEGIN;
SELECT set_config('aimee.principal','operator:ontology-governance',true);
SELECT set_config('aimee.authority','operator',true);
SELECT operator_review_decide('ontology:'||:governed_eval_id::TEXT,:'governed_review_item_head',
 'promote','','');
COMMIT;
BEGIN;
SELECT set_config('aimee.principal','operator:ontology',true);
SELECT set_config('aimee.authority','operator',true);
SELECT e2e_expect_stale_ontology(:'opnext_id_id',(:'opnext_dry_dry'::jsonb)->>'preview_token');
SELECT ontology_package_dry_run(:'opnext_id_id',true) AS dry \gset opnext_dry2_
SELECT ontology_package_migrate(:'opnext_id_id',(:'opnext_dry2_dry'::jsonb)->>'preview_token') AS migration \gset opnext_mig_
COMMIT;
SELECT e2e_assert(((:'opnext_dry2_dry'::jsonb)->>'facts_invalid')::bigint=
 ((:'opnext_mig_migration'::jsonb)->>'retired')::bigint,'ontology dry-run exact count');
SELECT e2e_assert(jsonb_array_length((:'opnext_dry2_dry'::jsonb)->'facts_invalid_detail')>0,
 'ontology dry-run explains invalid facts by rule');
SELECT e2e_assert(NOT EXISTS(SELECT 1 FROM entity_edges WHERE relation='p1_rel' AND invalidated_at=''),
 'narrowed facts retired without deletion');

-- A fact admitted only under the newer package is quarantined on rollback;
-- facts retired by that package are restored through the rollback changeset.
BEGIN;
SELECT set_config('aimee.principal','operator:new-contract',true);
SELECT set_config('aimee.authority','operator',true);
INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status,
 reversible,created_at) VALUES('new-contract-fact','fact.assert','operator:new-contract','operator',
 40,'open',1,pg_now_text());
INSERT INTO entity_edges(source,relation,target,edge_class,confidence_class,confidence,asserted_at,
 assertion_kind,epistemic_kind,lifecycle_state,authority_rank,actor_principal,version,commit_id)
VALUES('new','governed_new_rel','contract','semantic','A',1.0,pg_now_text(),'world_fact',
 'world_fact','persistent',40,'operator:new-contract',1,'new-contract-fact') RETURNING id \gset new_contract_
INSERT INTO fact_graph_changes(commit_id,assertion_id,action,existed_after,after_lifecycle,
 after_confidence,after_authority_rank,after_version) VALUES('new-contract-fact',:new_contract_id,
 'insert',1,'persistent',1.0,40,1);
UPDATE fact_graph_commits SET status='applied',closed_at=pg_now_text() WHERE commit_id='new-contract-fact';
COMMIT;
BEGIN;
SELECT set_config('aimee.principal','operator:ontology',true);
SELECT set_config('aimee.authority','operator',true);
SELECT ontology_package_rollback() AS rollback \gset op_rollback_
COMMIT;
SELECT e2e_assert((SELECT ontology_quarantined FROM entity_edges WHERE id=:new_contract_id)=1,
 'new-contract fact quarantined');
SELECT e2e_assert(EXISTS(SELECT 1 FROM entity_edges WHERE relation='p1_rel' AND invalidated_at=''),
 'prior-contract facts restored');
SELECT e2e_assert((:'op_rollback_rollback'::jsonb)->>'rollback'='true','ontology rollback reported');

-- P9 exact score reconstruction, rejected candidate, scope isolation and caps.
BEGIN;
SELECT set_config('aimee.principal','user:trace',true);
SELECT recall_trace_record('retr-trace','turn-trace','query-hash','project','e2e','normal',
 '[{"subject_kind":"memory","subject_id":"1","lane":"semantic","lane_rank":1,"final_rank":1,"scope_decision":"project-match","semantic_value":0.8,"semantic_weight":0.5,"keyword_value":0.2,"keyword_weight":0.5,"final_score":0.5,"epistemic_kind":"world_fact"},{"subject_kind":"memory","subject_id":"2","lane":"graph","lane_rank":2,"final_rank":0,"scope_decision":"rejected","graph_value":0.4,"graph_weight":0.5,"final_score":0.2,"rejected":true,"rejection_gate":"scope_rule_7"}]'::jsonb,true) AS trace \gset trace_
COMMIT;
SELECT e2e_assert(recall_trace_get((:'trace_trace'::jsonb)->>'trace_id','project','e2e') IS NOT NULL,'trace scope read');
SELECT e2e_assert(recall_trace_get((:'trace_trace'::jsonb)->>'trace_id','project','other') IS NULL,'trace scope isolation');
SELECT e2e_assert(EXISTS(SELECT 1 FROM recall_trace_results WHERE trace_id=(:'trace_trace'::jsonb)->>'trace_id'
 AND rejected=1 AND rejection_gate='scope_rule_7'),'rejection recorded');
SELECT e2e_assert((SELECT abs(sum(semantic_value*semantic_weight+keyword_value*keyword_weight+
 graph_value*graph_weight+temporal_value*temporal_weight+outcome_value*outcome_weight-final_score))<0.000000001
 FROM recall_trace_results WHERE trace_id=(:'trace_trace'::jsonb)->>'trace_id'),'score reconstruction');

-- The dynamic feature map is authoritative for exact reconstruction, including
-- features added after the original five-column trace shape.
BEGIN;
SELECT set_config('aimee.principal','user:trace',true);
SELECT recall_trace_record('retr-trace-dynamic','turn-trace','query-hash-2','project','e2e','normal',
 '[{"subject_kind":"memory","subject_id":"dynamic","lane":"hybrid","lane_rank":1,"final_rank":1,"scope_decision":"allowed","feature_values":{"lexical":0.2,"coverage":0.1,"graph":0.4,"outcome":-0.05,"post_rank_residual":0.01},"feature_weights":{"lexical":1,"coverage":1,"graph":0.5,"outcome":1,"post_rank_residual":1},"feature_contributions":{"lexical":0.2,"coverage":0.1,"graph":0.2,"outcome":-0.05,"post_rank_residual":0.01},"final_score":0.46}]'::jsonb,true) AS trace \gset trace_dynamic_
SELECT work_outcome_record('trace-fault-outcome','retr-trace-dynamic','memory','dynamic','misleading',
 'fault task','fault-flow','project','e2e','','','',0,'feature','graph');
COMMIT;
SELECT e2e_assert(EXISTS(SELECT 1 FROM work_outcomes o JOIN recall_traces t
 ON t.retrieval_event_id=o.retrieval_event_id JOIN recall_trace_results r ON r.trace_id=t.trace_id
 WHERE o.outcome_id='trace-fault-outcome' AND r.subject_id='dynamic' AND o.fault_value='graph'),
 'outcome fault joins exact trace element');

-- Sustained forced persistence cannot exceed the configured row cap, even when
-- one response contains more candidates than the entire budget. Age pruning is
-- independently enforced.
UPDATE evidence_lifecycle_settings SET trace_max_rows=7,trace_top_k=20,trace_max_age_days=1;
DO $$ DECLARE i BIGINT;payload JSONB;
BEGIN
 SELECT jsonb_agg(jsonb_build_object('subject_kind','memory','subject_id','cap-'||g,
   'lane','hybrid','lane_rank',g,'final_rank',g,'scope_decision','allowed',
   'feature_values',jsonb_build_object('score',1.0),'feature_weights',jsonb_build_object('score',1.0),
   'feature_contributions',jsonb_build_object('score',1.0),'final_score',1.0)) INTO payload
 FROM generate_series(1,12) g;
 FOR i IN 1..100 LOOP
  PERFORM recall_trace_record('cap-event-'||i,'cap-turn','cap-query','project','e2e','normal',payload,true);
 END LOOP;
END $$;
SELECT e2e_assert((SELECT count(*) FROM recall_trace_results)<=7,'trace row cap');
SELECT recall_trace_record('old-event','old-turn','old-query','project','e2e','normal',
 '[{"subject_kind":"memory","subject_id":"old","lane":"hybrid","lane_rank":1,"final_rank":1,"scope_decision":"allowed","semantic_value":1,"semantic_weight":1,"final_score":1}]'::jsonb,true) AS old_trace \gset old_trace_
UPDATE recall_traces SET created_at='2000-01-01 00:00:00'
 WHERE trace_id=(:'old_trace_old_trace'::jsonb)->>'trace_id';
SELECT recall_trace_prune();
SELECT e2e_assert(NOT EXISTS(SELECT 1 FROM recall_traces WHERE retrieval_event_id='old-event'),
 'trace age cap');
UPDATE evidence_lifecycle_settings SET trace_max_rows=10000,trace_top_k=20,trace_max_age_days=30;

-- P3 owns trace erasure for a purged document and does it in the same changeset.
BEGIN;
SELECT set_config('aimee.principal','operator:trace-purge',true);
SELECT set_config('aimee.authority','operator',true);
INSERT INTO docs(content_hash,filename,normalized_text,state)
 VALUES('trace-doc-hash','trace-doc.md','trace body','active') RETURNING id \gset trace_doc_
SELECT recall_trace_record('trace-doc-event','trace-doc-turn','trace-doc-query','global','','normal',
 jsonb_build_array(jsonb_build_object('subject_kind','document','subject_id',:trace_doc_id::TEXT,
 'lane','keyword','lane_rank',1,'final_rank',1,'scope_decision','allowed','keyword_value',1,
 'keyword_weight',1,'final_score',1)),true) AS document_trace \gset trace_doc_trace_
SELECT document_lifecycle_preview(:trace_doc_id,'purge') AS preview \gset trace_doc_preview_
SELECT document_lifecycle_apply(:trace_doc_id,'purge',
 (:'trace_doc_preview_preview'::jsonb)->>'preview_token','trace subject purge');
COMMIT;
SELECT e2e_assert(NOT EXISTS(SELECT 1 FROM recall_traces WHERE retrieval_event_id='trace-doc-event'),
 'purge removes subject trace');

SELECT 'evidence-lifecycle-pg-test: ok';
