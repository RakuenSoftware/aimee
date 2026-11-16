-- NULL is an explicit legacy/diagnostic display artifact. Never infer a model
-- snapshot format from output text, including reserved-looking marker prefixes.
ALTER TABLE cron_job_runs ADD COLUMN output_ir TEXT;
ALTER TABLE cron_job_runs ADD CONSTRAINT cron_output_ir_size
  CHECK (output_ir IS NULL OR octet_length(output_ir) BETWEEN 1 AND 1048576);
