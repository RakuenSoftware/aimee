-- Generic delivery obligations for approved original-input canonical commits.
-- Opaque intents/results are interpreted only by the trusted installed owner.
CREATE TABLE session_input_deliveries (
  id BIGSERIAL PRIMARY KEY,
  session_id TEXT NOT NULL REFERENCES server_sessions(id) ON DELETE CASCADE,
  operation_uuid TEXT NOT NULL,
  actor TEXT NOT NULL,
  receiver_tag TEXT NOT NULL CHECK(octet_length(receiver_tag) BETWEEN 1 AND 128),
  intent_key TEXT NOT NULL CHECK(intent_key ~ '^[0-9a-f]{64}$'),
  opaque_intent TEXT NOT NULL CHECK(octet_length(opaque_intent) BETWEEN 1 AND 16384),
  begin_sequence BIGINT NOT NULL,
  input_sequence BIGINT NOT NULL CHECK(input_sequence = begin_sequence + 1),
  original_turn TEXT NOT NULL,
  begin_sha256 TEXT NOT NULL,
  input_sha256 TEXT NOT NULL,
  outcome TEXT NOT NULL DEFAULT 'pending' CHECK(outcome IN ('pending','delivered','terminal')),
  result_json TEXT NOT NULL DEFAULT '' CHECK(octet_length(result_json) <= 32768),
  UNIQUE(session_id,operation_uuid),
  FOREIGN KEY(session_id,begin_sequence) REFERENCES session_ir_events(session_id,sequence) ON DELETE CASCADE,
  FOREIGN KEY(session_id,input_sequence) REFERENCES session_ir_events(session_id,sequence) ON DELETE CASCADE
);
CREATE INDEX session_input_deliveries_pending ON session_input_deliveries(receiver_tag,actor,id) WHERE outcome='pending';

-- A resolved obligation can follow ordinary canonical/session retention. An
-- unresolved one cannot disappear, even through a cascading FK or direct delete.
CREATE FUNCTION session_input_delivery_delete_guard() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF OLD.outcome = 'pending' THEN
    RAISE EXCEPTION USING ERRCODE='P0001', MESSAGE='canonical_input_delivery_conflict';
  END IF;
  RETURN OLD;
END;
$$;
CREATE TRIGGER session_input_delivery_retention_guard BEFORE DELETE ON session_input_deliveries
FOR EACH ROW EXECUTE FUNCTION session_input_delivery_delete_guard();
