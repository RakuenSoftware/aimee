\set ON_ERROR_STOP on

-- Reproducible P1/P9 bounded-growth benchmark. Run only in the disposable
-- database created by run-evidence-lifecycle-benchmark.sh.
CREATE TEMP TABLE evidence_benchmark_result (
  metric TEXT PRIMARY KEY,
  value DOUBLE PRECISION NOT NULL,
  unit TEXT NOT NULL
);

DO $$
DECLARE started TIMESTAMPTZ; before_bytes BIGINT; after_bytes BIGINT;
BEGIN
  PERFORM set_config('aimee.principal','internal:evidence-benchmark',true);
  PERFORM set_config('aimee.authority','system',true);
  SELECT pg_total_relation_size('memory_evidence_events') INTO before_bytes;
  started:=clock_timestamp();
  INSERT INTO memories(key,content,content_hash)
    SELECT 'evidence-bench-'||g,'bounded benchmark row '||g,md5(g::TEXT)
      FROM generate_series(1,10000) g;
  SELECT pg_total_relation_size('memory_evidence_events') INTO after_bytes;
  INSERT INTO evidence_benchmark_result VALUES
    ('ledger_10k_elapsed_ms',extract(epoch FROM clock_timestamp()-started)*1000,'ms'),
    ('ledger_10k_growth_bytes',after_bytes-before_bytes,'bytes'),
    ('ledger_bytes_per_event',(after_bytes-before_bytes)/10000.0,'bytes/event'),
    ('ledger_events',(SELECT count(*) FROM memory_evidence_events
       WHERE authenticated_actor='internal:evidence-benchmark'),'events');
END $$;

DO $$
DECLARE started TIMESTAMPTZ; before_bytes BIGINT; after_bytes BIGINT;i BIGINT;
        rows JSONB:='[
          {"subject_kind":"memory","subject_id":"1","lane":"semantic","lane_rank":1,"final_rank":1,"scope_decision":"project-match","semantic_value":0.8,"semantic_weight":0.5,"keyword_value":0.2,"keyword_weight":0.5,"final_score":0.5},
          {"subject_kind":"memory","subject_id":"2","lane":"semantic","lane_rank":2,"final_rank":2,"scope_decision":"project-match","semantic_value":0.7,"semantic_weight":0.5,"keyword_value":0.3,"keyword_weight":0.5,"final_score":0.5},
          {"subject_kind":"memory","subject_id":"3","lane":"graph","lane_rank":1,"final_rank":3,"scope_decision":"project-match","graph_value":0.6,"graph_weight":0.5,"temporal_value":0.4,"temporal_weight":0.5,"final_score":0.5},
          {"subject_kind":"memory","subject_id":"4","lane":"keyword","lane_rank":1,"final_rank":4,"scope_decision":"project-match","semantic_value":0.2,"semantic_weight":0.5,"keyword_value":0.8,"keyword_weight":0.5,"final_score":0.5},
          {"subject_kind":"memory","subject_id":"5","lane":"graph","lane_rank":2,"final_rank":0,"scope_decision":"rejected","graph_value":0.4,"graph_weight":0.5,"final_score":0.2,"rejected":true,"rejection_gate":"scope_rule_7"}
        ]'::JSONB;
BEGIN
  PERFORM set_config('aimee.principal','internal:trace-benchmark',true);
  SELECT pg_total_relation_size('recall_traces')+
         pg_total_relation_size('recall_trace_results') INTO before_bytes;
  started:=clock_timestamp();
  FOR i IN 1..1000 LOOP
    PERFORM recall_trace_record('benchmark-retrieval-'||i,'benchmark-turn-'||i,md5(i::TEXT),
      'project','benchmark','normal',rows,true);
  END LOOP;
  SELECT pg_total_relation_size('recall_traces')+
         pg_total_relation_size('recall_trace_results') INTO after_bytes;
  INSERT INTO evidence_benchmark_result VALUES
    ('trace_1000_elapsed_ms',extract(epoch FROM clock_timestamp()-started)*1000,'ms'),
    ('trace_mean_latency_ms',extract(epoch FROM clock_timestamp()-started)*1000/1000.0,'ms/trace'),
    ('trace_growth_bytes',after_bytes-before_bytes,'bytes'),
    ('trace_result_rows',(SELECT count(*) FROM recall_trace_results),'rows');
END $$;

SELECT jsonb_pretty(jsonb_object_agg(metric,jsonb_build_object('value',round(value::NUMERIC,3),
 'unit',unit) ORDER BY metric)) AS evidence_lifecycle_benchmark
FROM evidence_benchmark_result;
