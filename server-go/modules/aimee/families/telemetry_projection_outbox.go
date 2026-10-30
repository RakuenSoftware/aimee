package families

// Delivery acknowledgments survive projection retention. This queue contains
// source pointers only; Learning and the existing private publishers own evidence.
import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"sort"
	"strings"

	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

func projectionEligible(raw string) bool {
	var event struct {
		Kind    string          `json:"kind"`
		Payload json.RawMessage `json:"payload"`
	}
	if !canonical.ValidSessionEvent([]byte(raw)) || json.Unmarshal([]byte(raw), &event) != nil {
		return false
	}
	if event.Kind == "execution" {
		var payload canonical.ExecutionPayload
		return canonical.Decode(event.Payload, &payload) && payload.Status != "running"
	}
	_, ok := decodeExperienceChild(raw)
	return ok
}
func enqueueLearningProjection(ctx context.Context, q store.Queryer, f []string, sequence int64) error {
	if !projectionEligible(f[4]) {
		return nil
	}
	tag, err := q.Exec(ctx, `INSERT INTO learning_projection_outbox(session_id,sequence,event_id,turn_id,actor) VALUES($1,$2,$3,$4,$5) ON CONFLICT DO NOTHING`, f[0], sequence, f[1], f[2], f[3])
	if err == nil && tag != nil && tag.RowsAffected() > 0 {
		store.Note(ctx, NoteLearningProjectionQueued)
	}
	return err
}

// NoteLearningProjectionQueued is noted when an operation queued a learning
// projection, so the delivery worker is told instead of polling the queue.
const NoteLearningProjectionQueued = "learning_projection_queued"

type projectionTicket struct {
	Lease  string          `json:"lease"`
	Record json.RawMessage `json:"record"`
}

func learningProjectionClaim(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	rows, err := q.Query(ctx, `SELECT o.session_id,o.sequence::text,o.event_id,o.turn_id,o.actor,e.payload,CASE WHEN e.actor=o.actor AND e.event_id=o.event_id AND e.turn_id=o.turn_id THEN COALESCE(s.principal,'') ELSE '' END
 FROM learning_projection_outbox o JOIN session_ir_events e ON e.session_id=o.session_id AND e.sequence=o.sequence
 LEFT JOIN server_sessions s ON s.id=o.session_id
 WHERE o.state='pending' AND o.next_attempt<=now() AND (o.lease_until IS NULL OR o.lease_until<now())
 ORDER BY o.next_attempt,o.session_id,o.sequence FOR UPDATE OF o SKIP LOCKED LIMIT 4`)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	type candidate struct {
		source     executionGradeSource
		raw, owner string
	}
	candidates := []candidate{}
	for rows.Next() {
		c := candidate{}
		c.source.Kind = "canonical_session_log"
		if err = rows.Scan(&c.source.Session, &c.source.Sequence, &c.source.Event, &c.source.Turn, &c.source.Actor, &c.raw, &c.owner); err != nil {
			break
		}
		candidates = append(candidates, c)
	}
	rowErr := rows.Err()
	rows.Close()
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if rowErr != nil {
		return store.StatusFailed, nil, rowErr
	}
	tickets := []projectionTicket{}
	total := 0
	for _, c := range candidates {
		source := c.source
		invalidChild := false
		if child, ok := decodeExperienceChild(c.raw); ok {
			invalidChild = child.Turn != source.Turn || !validDelegateChildIdentity(source.Session, child)
		}
		if invalidChild || c.owner != source.Actor || !validExperienceSource(source, c.owner) || !projectionEligible(c.raw) {
			if _, err = q.Exec(ctx, `UPDATE learning_projection_outbox SET state='revoked',lease_token='',lease_until=NULL WHERE session_id=$1 AND sequence=$2::bigint`, source.Session, source.Sequence); err != nil {
				return store.StatusFailed, nil, err
			}
			continue
		}
		record, encodeErr := encodeSessionIR(map[string]any{"version": 1, "session_id": source.Session, "sequence": source.Sequence, "event_id": source.Event, "turn_id": source.Turn, "actor": source.Actor, "event": json.RawMessage(c.raw)})
		if encodeErr != nil {
			return store.StatusFailed, nil, encodeErr
		}
		if total+len(record) > 1536*1024 {
			continue
		}
		random := make([]byte, 16)
		if _, err = rand.Read(random); err != nil {
			return store.StatusFailed, nil, err
		}
		token := hex.EncodeToString(random)
		if _, err = q.Exec(ctx, `UPDATE learning_projection_outbox SET lease_token=$3,lease_until=now()+interval '180 seconds',attempts=LEAST(attempts+1,30) WHERE session_id=$1 AND sequence=$2::bigint`, source.Session, source.Sequence, token); err != nil {
			return store.StatusFailed, nil, err
		}
		tickets = append(tickets, projectionTicket{token, json.RawMessage(record)})
		total += len(record)
	}
	var cutoff string
	if err = q.QueryRow(ctx, `SELECT to_char(cutoff AT TIME ZONE 'utc','YYYY-MM-DD"T"HH24:MI:SS"Z"') FROM learning_projection_coverage WHERE version=1`).Scan(&cutoff); err != nil {
		return store.StatusFailed, nil, err
	}
	// When the worker has to look again with nothing announced: the next retry
	// or lease expiry still pending, 0 when one is already due, -1 for none.
	var nextDue int64
	if err = q.QueryRow(ctx, `SELECT COALESCE(GREATEST(0,CEIL(EXTRACT(EPOCH FROM MIN(GREATEST(next_attempt,COALESCE(lease_until,next_attempt)))-now())*1000))::bigint,-1) FROM learning_projection_outbox WHERE state='pending'`).Scan(&nextDue); err != nil {
		return store.StatusFailed, nil, err
	}
	body, err := encodeSessionIR(map[string]any{"schema_version": 1, "coverage": "canonical_appends_since_cutover", "cutoff": cutoff, "items": tickets, "next_due_ms": nextDue})
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if len(body) >= 2097152 {
		return store.StatusInvalid, nil, nil
	}
	return store.StatusOK, []string{string(body)}, nil
}

// Recheck complete admitted receipts; no exit-code interpretation or new verdict.
func projectionTerminalDelivered(ctx context.Context, q store.Queryer, source executionGradeSource, raw string) (bool, error) {
	var event struct {
		Kind    string                     `json:"kind"`
		Payload canonical.ExecutionPayload `json:"payload"`
	}
	if !canonical.ValidSessionEvent([]byte(raw)) || !canonical.Decode([]byte(raw), &event) || event.Kind != "execution" || event.Payload.Status == "running" {
		return false, nil
	}
	var observed bool
	if err := q.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM interaction_events WHERE session_id=$1 AND actor='system' AND event_type='task_outcome' AND outcome='observed' AND payload::jsonb->'source'=$2::jsonb AND payload::jsonb->>'observed_status'=$3 AND payload::jsonb->>'verification'='ungraded' AND COALESCE(payload::jsonb->>'error','')=$4)`, source.Session, mustJSON(source), event.Payload.Status, event.Payload.Error).Scan(&observed); err != nil {
		return false, err
	}
	if !observed {
		return false, nil
	}
	for _, proof := range event.Payload.VerificationProofs {
		var present bool
		if err := q.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM interaction_events WHERE session_id=$1 AND actor='system' AND event_type='task_outcome' AND outcome IN ('tests_passed','tests_failed') AND payload::jsonb->'source'=$2::jsonb AND payload::jsonb->'proof'=$3::jsonb AND payload::jsonb->>'qualification'='configured_tests_only' AND payload::jsonb->>'verification'='tests_bound_revision')`, source.Session, mustJSON(source), mustJSON(proof)).Scan(&present); err != nil {
			return false, err
		}
		if !present {
			return false, nil
		}
	}
	return true, nil
}
func projectionDelivered(ctx context.Context, q store.Queryer, source executionGradeSource, raw string) (bool, error) {
	_, ok := decodeExperienceChild(raw)
	if !ok {
		return projectionTerminalDelivered(ctx, q, source, raw)
	}
	record, status, err := loadDelegateExecutionSource(ctx, q, []string{source.Actor, mustJSON(source)})
	if err != nil || status != store.StatusOK {
		return false, err
	}
	ready, err := projectionTerminalDelivered(ctx, q, record.Source, record.Terminal)
	if err != nil || !ready {
		return false, err
	}
	// op63 already rechecks job, both owners, exact binding and all grades. Calling
	// it again is idempotent and repairs a missing pending-evidence row atomically.
	status, _, err = delegateExecutionExperienceRecord(ctx, q, []string{source.Actor, mustJSON(source)})
	return status == store.StatusOK, err
}
func learningProjectionAck(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 2 || len(f[0]) != 32 || strings.Trim(f[0], "0123456789abcdef") != "" || (f[1] != "delivered" && f[1] != "retry") {
		return store.StatusInvalid, nil, nil
	}
	var source executionGradeSource
	source.Kind = "canonical_session_log"
	err := q.QueryRow(ctx, `SELECT session_id,sequence::text,event_id,turn_id,actor FROM learning_projection_outbox WHERE lease_token=$1 AND state='pending' AND lease_until>now()`, f[0]).Scan(&source.Session, &source.Sequence, &source.Event, &source.Turn, &source.Actor)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	raw, err := loadExperienceEvent(ctx, q, source)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	sessions := []string{source.Session}
	if child, ok := decodeExperienceChild(raw); ok {
		sessions = append(sessions, child.Session)
	}
	sort.Strings(sessions)
	owner := ""
	for _, session := range sessions {
		var principal string
		if err = q.QueryRow(ctx, sessionIRLockSQL, session).Scan(&principal); err != nil {
			if store.IsNoRows(err) {
				principal = ""
			} else {
				return store.StatusFailed, nil, err
			}
		}
		if session == source.Session {
			owner = principal
		}
	}
	var token string
	if err = q.QueryRow(ctx, `SELECT lease_token FROM learning_projection_outbox WHERE lease_token=$1 AND state='pending' AND lease_until>now() FOR UPDATE`, f[0]).Scan(&token); err != nil {
		if store.IsNoRows(err) {
			return store.StatusMissing, nil, nil
		}
		return store.StatusFailed, nil, err
	}
	current, err := loadExperienceEvent(ctx, q, source)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	if current != raw {
		return store.StatusInvalid, nil, nil
	}
	state := "pending"
	if child, ok := decodeExperienceChild(raw); ok {
		var retained bool
		err = q.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM agent_jobs j JOIN server_sessions s ON s.id=$4 AND s.principal=$5 WHERE j.id=$1::bigint AND j.role=$2 AND j.agent_name=$3)`, child.LearningBinding.JobID, child.Role, child.LearningBinding.Agent, child.Session, source.Actor).Scan(&retained)
		if err != nil {
			return store.StatusFailed, nil, err
		}
		if !retained {
			state = "revoked"
		}
	}
	if owner != source.Actor || !projectionEligible(raw) {
		state = "revoked"
	} else if state != "revoked" && f[1] == "delivered" {
		delivered, checkErr := projectionDelivered(ctx, q, source, raw)
		if checkErr != nil {
			return store.StatusFailed, nil, checkErr
		}
		if !delivered {
			return store.StatusMissing, nil, nil
		}
		state = "delivered"
	}
	_, err = q.Exec(ctx, `UPDATE learning_projection_outbox SET state=$2,lease_token='',lease_until=NULL,next_attempt=now()+make_interval(secs=>LEAST(300,attempts*5)) WHERE lease_token=$1`, f[0], state)
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}
