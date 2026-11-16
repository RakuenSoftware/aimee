-- Existing canonical child bindings are the only durable job-to-history authority.
-- CASE protects malformed/legacy payloads from JSON cast failures.
CREATE INDEX IF NOT EXISTS idx_session_ir_delegate_job_actor
 ON session_ir_events ((CASE WHEN payload IS JSON OBJECT WITH UNIQUE KEYS THEN CASE WHEN payload::jsonb->>'kind'='child_execution' AND payload::jsonb#>>'{payload,status}'='running' THEN payload::jsonb#>>'{payload,learning_binding,job_id}' END END), actor)
 WHERE (CASE WHEN payload IS JSON OBJECT WITH UNIQUE KEYS THEN CASE WHEN payload::jsonb->>'kind'='child_execution' AND payload::jsonb#>>'{payload,status}'='running' THEN payload::jsonb#>>'{payload,learning_binding,job_id}' END END) IS NOT NULL;
