-- case_artifacts and ruleset_registry views for graph reasoning Phase 1.
-- See docs/proposals/accepted/graph-reasoning-case-based-recall-and-contradiction-logic.md

-- Exposes all artifacts of kind = 'case', enriched with parsed outcome field.
CREATE OR REPLACE VIEW case_artifacts AS
  SELECT id,
         state,
         scope_kind,
         scope_id,
         confidence,
         created_at,
         committed_at,
         payload::jsonb                              AS payload,
         payload::jsonb->>'outcome'                 AS outcome
    FROM artifacts
   WHERE kind = 'case';

-- Exposes all artifacts of kind = 'ruleset', with typed fields extracted
-- from the JSON payload for ruleset_id, version, and content_hash.
CREATE OR REPLACE VIEW ruleset_registry AS
  SELECT id,
         state,
         scope_kind,
         scope_id,
         payload::jsonb->>'ruleset_id'              AS ruleset_id,
         payload::jsonb->>'version'                 AS version,
         payload::jsonb->>'content_hash'             AS content_hash,
         created_at,
         committed_at
    FROM artifacts
   WHERE kind = 'ruleset';
