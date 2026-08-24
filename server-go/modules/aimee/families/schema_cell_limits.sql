-- Refuse, at write time, a value the read path can never return.
--
-- THE DEFECT THIS CLOSES. Twelve columns are unbounded TEXT while the catalog
-- declares the reply fields that carry them at 1 MiB, and the store wire caps a
-- cell at MaxCellBytes (1 MiB) -- refusing an over-large cell rather than
-- truncating it, which is the right behaviour and the reason this matters. A
-- caller could write a 2 MiB prompt, PostgreSQL would store it happily, and
-- every subsequent read of that row would fail. Not corrupted: unreachable. The
-- job, page or session would be permanently unreadable, and nothing at write
-- time said so.
--
-- Nothing enforced a size anywhere on the way in. The wire's ValidField only
-- rejects NUL, and none of these operations declare a request allocation.
--
-- This does not remove working capability. The C module read these columns into
-- fixed 1 MiB buffers, so a value above the ceiling was already unreadable --
-- the limit has always been there. What changes is when you find out: a
-- classified CHECK violation at write time, which the client already recognises
-- as a constraint refusal, instead of a row that silently becomes unreadable.
--
-- OCTET_LENGTH, NOT LENGTH. length() counts characters and the wire counts
-- bytes. A string of 1,048,576 multi-byte characters passes a length() check
-- and is up to four times over the byte ceiling, so length() here would enforce
-- a limit that is not the one the wire applies -- which is worse than no check,
-- because it would look like the boundary was covered.
--
-- The bound is stated once as a literal rather than derived, because a
-- migration is a historical record: it has to keep meaning what it meant when
-- it ran, even if MaxCellBytes is raised later. Raising the wire's cap is a new
-- migration, not an edit to this one.

ALTER TABLE coord_job_tasks
  ADD CONSTRAINT coord_job_tasks_prompt_fits_wire
  CHECK (octet_length(prompt) <= 1048576);

ALTER TABLE coord_job_tasks
  ADD CONSTRAINT coord_job_tasks_result_fits_wire
  CHECK (octet_length(result) <= 1048576);

ALTER TABLE cron_jobs
  ADD CONSTRAINT cron_jobs_prompt_fits_wire
  CHECK (octet_length(prompt) <= 1048576);

ALTER TABLE agent_jobs
  ADD CONSTRAINT agent_jobs_prompt_fits_wire
  CHECK (octet_length(prompt) <= 1048576);

ALTER TABLE agent_jobs
  ADD CONSTRAINT agent_jobs_result_fits_wire
  CHECK (octet_length(result) <= 1048576);

ALTER TABLE ensembles
  ADD CONSTRAINT ensembles_context_json_fits_wire
  CHECK (octet_length(context_json) <= 1048576);

ALTER TABLE agent_cache
  ADD CONSTRAINT agent_cache_prompt_fits_wire
  CHECK (octet_length(prompt) <= 1048576);

ALTER TABLE agent_cache
  ADD CONSTRAINT agent_cache_result_fits_wire
  CHECK (octet_length(result) <= 1048576);

ALTER TABLE web_page_cache
  ADD CONSTRAINT web_page_cache_body_fits_wire
  CHECK (octet_length(body) <= 1048576);

ALTER TABLE primary_sessions
  ADD CONSTRAINT primary_sessions_messages_json_fits_wire
  CHECK (octet_length(messages_json) <= 1048576);

ALTER TABLE webchat_live
  ADD CONSTRAINT webchat_live_text_fits_wire
  CHECK (octet_length(text) <= 1048576);

ALTER TABLE roadmap_unit_dispatch
  ADD CONSTRAINT roadmap_unit_dispatch_result_fits_wire
  CHECK (octet_length(result) <= 1048576);
