-- Subject erasure belongs to the retired SQL memory implementation.
-- Retain every existing payload, history, intent and receipt row.
DROP FUNCTION IF EXISTS user_memory_prepare_subject_erasure(TEXT);
DROP FUNCTION IF EXISTS user_memory_replay_erasure_intents();
DROP FUNCTION IF EXISTS local_procedure_prepare_subject_erasure(TEXT);
DROP FUNCTION IF EXISTS local_procedure_replay_erasure_intents();
