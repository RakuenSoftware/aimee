-- Host-owned disposable state follows session erasure/retention. Legacy working
-- memory operations do not write this column. No canonical write is granted by
-- the host-only task operation; promotion uses the guarded correction queue.
ALTER TABLE session_state ADD COLUMN task_projection_state TEXT NOT NULL DEFAULT ''
 CHECK (octet_length(task_projection_state) <= 131072)
 CHECK (task_projection_state='' OR
   COALESCE((task_projection_state IS JSON OBJECT
    AND task_projection_state::jsonb->>'class'='derived_non_authoritative'
    AND task_projection_state::jsonb->>'policy'='task-projection-v1'
    AND NOT task_projection_state::jsonb ? 'authoritative'),false));
