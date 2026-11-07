-- NULL explicitly identifies a legacy prompt row. Preserve its literal text;
-- never infer the storage format from JSON-looking prompt contents. TEXT keeps
-- canonical message bytes exact, including opaque field and number spelling.
ALTER TABLE coord_job_tasks ADD COLUMN message_ir TEXT;
ALTER TABLE coord_job_tasks ADD CONSTRAINT coord_task_message_ir_size
  CHECK (message_ir IS NULL OR octet_length(message_ir) BETWEEN 1 AND 1048576);
