\set ON_ERROR_STOP on

DO $$
BEGIN
  IF to_regclass('public.fact_graph_commits') IS NULL OR
     to_regclass('public.fact_evidence') IS NULL OR
     to_regclass('public.memory_fact_actors') IS NULL OR
     to_regclass('public.entity_mental_models') IS NULL THEN
    RAISE EXCEPTION 'fact mutation migration objects are missing';
  END IF;
END $$;

INSERT INTO fact_graph_commits
  (commit_id,parent_commit_id,operation,actor_principal,actor_role,authority_rank,status,reversible)
VALUES
  ('pg-fixture-commit','pg-fixture-run','fact.assert','test:system','system',20,'open',1);
INSERT INTO entity_edges
  (source,relation,target,edge_class,assertion_kind,lifecycle_state,authority_rank,
   actor_principal,commit_id,confidence,confidence_class)
VALUES
  ('pg-fixture','works_for','acme','semantic','world_fact','persistent',20,
   'test:system','pg-fixture-commit',0.8,'B');
UPDATE fact_graph_commits SET status='applied' WHERE commit_id='pg-fixture-commit';

DO $$
BEGIN
  IF (SELECT count(*) FROM entity_mental_models WHERE entity='pg-fixture') <> 1 THEN
    RAISE EXCEPTION 'derived mental-model view did not rebuild from current assertions';
  END IF;
  BEGIN
    INSERT INTO entity_edges(source,relation,target,edge_class)
      VALUES('bypass','works_for','acme','semantic');
    RAISE EXCEPTION 'direct semantic insert unexpectedly succeeded';
  EXCEPTION WHEN raise_exception THEN
    IF SQLERRM = 'direct semantic insert unexpectedly succeeded' THEN RAISE; END IF;
  END;
  BEGIN
    UPDATE entity_edges SET target='globex' WHERE source='pg-fixture';
    RAISE EXCEPTION 'direct semantic update unexpectedly succeeded';
  EXCEPTION WHEN raise_exception THEN
    IF SQLERRM = 'direct semantic update unexpectedly succeeded' THEN RAISE; END IF;
  END;
  BEGIN
    DELETE FROM entity_edges WHERE source='pg-fixture';
    RAISE EXCEPTION 'direct semantic delete unexpectedly succeeded';
  EXCEPTION WHEN raise_exception THEN
    IF SQLERRM = 'direct semantic delete unexpectedly succeeded' THEN RAISE; END IF;
  END;
END $$;

INSERT INTO fact_graph_commits
  (commit_id,operation,actor_principal,actor_role,authority_rank,status,reversible)
VALUES ('pg-mental-commit','fact.assert','test:system','system',20,'open',1);
DO $$
BEGIN
  BEGIN
    INSERT INTO entity_edges
      (source,relation,target,edge_class,assertion_kind,lifecycle_state,commit_id)
    VALUES ('unsourced-model','profile','replacement','semantic','mental_model',
            'persistent','pg-mental-commit');
    RAISE EXCEPTION 'stored mental model unexpectedly succeeded';
  EXCEPTION WHEN raise_exception THEN
    IF SQLERRM = 'stored mental model unexpectedly succeeded' THEN RAISE; END IF;
  END;
END $$;
DELETE FROM fact_graph_commits WHERE commit_id='pg-mental-commit';

SELECT 'fact mutation PostgreSQL migration/invariant checks passed' AS result;
