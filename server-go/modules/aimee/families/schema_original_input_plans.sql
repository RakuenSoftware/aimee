-- Optional operational reservations, never a memory-source role or raw-value ledger.
CREATE TABLE session_input_plans (
 id BIGSERIAL PRIMARY KEY,
 session_id TEXT NOT NULL REFERENCES server_sessions(id) ON DELETE CASCADE,
 operation_uuid TEXT NOT NULL,
 actor TEXT NOT NULL,
 receiver_tag TEXT NOT NULL CHECK(octet_length(receiver_tag) BETWEEN 1 AND 128),
 safe_text_sha256 TEXT NOT NULL CHECK(safe_text_sha256 ~ '^[0-9a-f]{64}$'),
 plan_sha256 TEXT NOT NULL CHECK(plan_sha256 ~ '^[0-9a-f]{64}$'),
 opaque_plan TEXT NOT NULL CHECK(octet_length(opaque_plan) BETWEEN 1 AND 16384),
 state TEXT NOT NULL DEFAULT 'reserved' CHECK(state IN ('reserved','committed','terminal')),
 terminal_result TEXT NOT NULL DEFAULT '' CHECK(octet_length(terminal_result)<=32768),
 begin_sequence BIGINT,
 input_sequence BIGINT,
 original_turn TEXT,
 delivery_id BIGINT REFERENCES session_input_deliveries(id) ON DELETE CASCADE,
 UNIQUE(session_id,operation_uuid),
 FOREIGN KEY(session_id,begin_sequence) REFERENCES session_ir_events(session_id,sequence) ON DELETE CASCADE,
 FOREIGN KEY(session_id,input_sequence) REFERENCES session_ir_events(session_id,sequence) ON DELETE CASCADE,
 CHECK((state='reserved' AND terminal_result='') OR (state='terminal' AND terminal_result<>'') OR state='committed'),
 CHECK((state IN ('reserved','terminal') AND begin_sequence IS NULL AND input_sequence IS NULL AND original_turn IS NULL AND delivery_id IS NULL)
    OR (state='committed' AND begin_sequence IS NOT NULL AND input_sequence IS NOT NULL AND begin_sequence>0 AND input_sequence=begin_sequence+1 AND original_turn IS NOT NULL AND delivery_id IS NOT NULL))
);
CREATE FUNCTION session_input_plan_delete_guard() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
 IF OLD.state='reserved' OR EXISTS(SELECT 1 FROM session_input_deliveries d WHERE d.id=OLD.delivery_id AND d.outcome='pending') THEN
  RAISE EXCEPTION USING ERRCODE='P0001', MESSAGE='canonical_input_delivery_conflict';
 END IF;
 RETURN OLD;
END;
$$;
CREATE TRIGGER session_input_plan_retention_guard BEFORE DELETE ON session_input_plans
 FOR EACH ROW EXECUTE FUNCTION session_input_plan_delete_guard();
-- The legacy/no-reservation path remains unchanged. Once an operational plan
-- exists, even an unplanned56/58 path cannot commit a different event pair.
CREATE FUNCTION session_input_plan_canonical_guard() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
 prefix CONSTANT TEXT := 'host-original-input-batch-v1:';
 operation TEXT;
 p session_input_plans%ROWTYPE;
 is_input BOOLEAN;
BEGIN
 IF left(NEW.event_id,length(prefix))<>prefix THEN RETURN NULL; END IF;
 operation:=substring(NEW.event_id FROM length(prefix)+1 FOR 36);
 IF NEW.event_id=prefix||operation||':begin' THEN is_input:=false;
 ELSIF NEW.event_id=prefix||operation||':input' THEN is_input:=true;
 ELSE RETURN NULL;
 END IF;
 SELECT * INTO p FROM session_input_plans WHERE session_id=NEW.session_id AND operation_uuid=operation FOR UPDATE;
 IF NOT FOUND THEN RETURN NULL; END IF;
 IF p.state<>'committed' OR p.actor<>NEW.actor OR p.original_turn<>NEW.turn_id
    OR (NOT is_input AND NEW.sequence<>p.begin_sequence)
    OR (is_input AND (NEW.sequence<>p.input_sequence OR NEW.payload::jsonb#>>'{payload,blocks,0,input_attachment,text_sha256}' IS DISTINCT FROM p.safe_text_sha256)) THEN
  RAISE EXCEPTION USING ERRCODE='P0001', MESSAGE='canonical_input_plan_conflict';
 END IF;
 RETURN NULL;
END;
$$;
CREATE CONSTRAINT TRIGGER session_input_plan_canonical_fence AFTER INSERT ON session_ir_events
 DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION session_input_plan_canonical_guard();
