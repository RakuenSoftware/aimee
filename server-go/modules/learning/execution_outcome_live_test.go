package learning

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"os"
	"strconv"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
)

// The native integration runner supplies a disposable source session after four
// completed browser tasks. Verification reads the actual C -> Go Learning ->
// DB1 path, with no KB process or fabricated rows. The optional fault injection
// removes one disposable derived row before real HTTP replay reconstructs it.
func TestExecutionObservationLiveCanonicalCapture(t *testing.T) {
	session := os.Getenv("AIMEE_LEARNING_LIVE_SESSION")
	if session == "" {
		t.Skip("disposable native integration session is not configured")
	}
	dsn := os.Getenv("AIMEE_STORE_URL")
	if dsn == "" {
		t.Fatal("native integration store is not configured")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal("native integration store is unavailable")
	}
	defer conn.Close(ctx)
	rows, err := conn.Query(ctx, `SELECT payload FROM interaction_events WHERE session_id=$1 AND event_type='task_outcome' ORDER BY id`, session)
	if err != nil {
		t.Fatal(err)
	}
	var observations []ExecutionObservation
	for rows.Next() {
		var raw string
		if err := rows.Scan(&raw); err != nil {
			t.Fatal(err)
		}
		var observation ExecutionObservation
		if json.Unmarshal([]byte(raw), &observation) != nil {
			t.Fatal("invalid persisted observation")
		}
		observations = append(observations, observation)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		t.Fatal(err)
	}
	if len(observations) != 4 {
		t.Fatalf("got %d local task observations, want four actual completed executions", len(observations))
	}
	seen := map[string]bool{}
	for _, observation := range observations {
		source := observation.Source
		if observation.SchemaVersion != 1 || observation.Verification != "ungraded" || observation.ObservedStatus != "completed" || source.Kind != "canonical_session_log" || source.Session != session || source.Actor != os.Getenv("AIMEE_LEARNING_LIVE_ACTOR") || seen[source.EventID] {
			t.Fatal("observation source, authority or uniqueness changed")
		}
		seen[source.EventID] = true
		sequence, err := strconv.ParseInt(source.Sequence, 10, 64)
		if err != nil {
			t.Fatal("invalid exact source sequence")
		}
		var eventID, turn, actor, payload string
		if err := conn.QueryRow(ctx, `SELECT event_id,turn_id,actor,payload FROM session_ir_events WHERE session_id=$1 AND sequence=$2`, session, sequence).Scan(&eventID, &turn, &actor, &payload); err != nil {
			t.Fatal(err)
		}
		if eventID != source.EventID || turn != source.Turn || actor != source.Actor {
			t.Fatal("observation does not refer to its immutable source")
		}
		var event struct {
			Kind    string `json:"kind"`
			Payload struct {
				Status string `json:"status"`
			} `json:"payload"`
		}
		if json.Unmarshal([]byte(payload), &event) != nil || event.Kind != "execution" || event.Payload.Status != observation.ObservedStatus {
			t.Fatal("observation differs from canonical terminal")
		}
	}
	if os.Getenv("AIMEE_LEARNING_LIVE_DROP_PROJECTION") == "1" {
		tag, err := conn.Exec(ctx, `DELETE FROM interaction_events WHERE id=(SELECT min(id) FROM interaction_events WHERE session_id=$1 AND event_type='task_outcome')`, session)
		if err != nil || tag.RowsAffected() != 1 {
			t.Fatal("disposable projection fault injection failed")
		}
	}

}

// Two real sandboxed delegates must retain canonical child observations. Legacy
// record compatibility is tested separately; it is not a second live publisher.
func TestDelegateExitLiveUngradedRetention(t *testing.T) {
	session := os.Getenv("AIMEE_LEARNING_LIVE_SESSION")
	if session == "" {
		t.Skip("disposable delegate integration session is not configured")
	}
	var jobs []string
	if json.Unmarshal([]byte(os.Getenv("AIMEE_LEARNING_LIVE_JOBS")), &jobs) != nil || len(jobs) != 2 || jobs[0] == jobs[1] {
		t.Fatal("two distinct actual launched delegate jobs are required")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	conn, err := pgx.Connect(ctx, os.Getenv("AIMEE_STORE_URL"))
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	actor := os.Getenv("AIMEE_LEARNING_LIVE_ACTOR")
	var owner string
	if err = conn.QueryRow(ctx, `SELECT principal FROM server_sessions WHERE id=$1`, session).Scan(&owner); err != nil || owner != actor || actor == "" {
		t.Fatal("parent source owner mismatch", err)
	}
	for _, job := range jobs {
		id, err := strconv.ParseInt(job, 10, 32)
		if err != nil || id <= 0 || strconv.FormatInt(id, 10) != job {
			t.Fatal("invalid actual job ID")
		}
		child := fmt.Sprintf("delegate-%x", sha256.Sum256([]byte(session+":"+job+":durable")))
		// Every join binds the complete immutable source tuple. Counts remain
		// positive: absent bindings/projections and duplicate writers both fail.
		var count, invalid int
		err = conn.QueryRow(ctx, `WITH actual AS (
   SELECT p.*,p.payload::jsonb->'payload' AS c FROM session_ir_events p
   WHERE p.session_id=$1 AND p.actor=$2 AND p.payload::jsonb->>'kind'='child_execution'
   AND p.payload::jsonb->'payload'->>'status'='running'
   AND p.payload::jsonb->'payload'->'learning_binding'->>'job_id'=$3
  ), bound AS (
   SELECT p.*,e.sequence AS child_sequence,e.event_id AS child_event,e.payload::jsonb AS terminal,
    jsonb_build_object('kind','canonical_session_log','session_id',e.session_id,'turn_id',e.turn_id,'sequence',e.sequence::text,'event_id',e.event_id,'actor',e.actor) AS source,
    jsonb_build_object('kind','canonical_session_log','session_id',p.session_id,'turn_id',p.turn_id,'sequence',p.sequence::text,'event_id',p.event_id,'actor',p.actor) AS parent_source,
    j.status AS job_status,j.role AS job_role,j.agent_name AS job_agent,s.principal AS child_actor
   FROM actual p JOIN server_sessions s ON s.id=p.c->>'session_id'
   JOIN agent_jobs j ON j.id=$3::bigint
   JOIN session_ir_events e ON e.session_id=s.id AND e.turn_id=p.c->>'turn_id' AND e.actor=$2
    AND e.payload::jsonb->>'kind'='execution' AND e.payload::jsonb->'payload'->>'status'<>'running'
  ) SELECT count(*),count(*) FILTER(WHERE
   b.c->>'session_id' IS DISTINCT FROM $4 OR b.c->>'turn_id' IS DISTINCT FROM b.turn_id OR b.c->>'role' IS DISTINCT FROM 'format'
   OR b.c->'learning_binding'->>'agent' IS DISTINCT FROM 'ir-live-a' OR b.c->'learning_binding'->>'scope_kind' IS DISTINCT FROM 'project'
   OR b.child_actor<>$2 OR b.job_status<>'done' OR b.job_role<>'format' OR b.job_agent<>'ir-live-a'
   OR b.terminal IS DISTINCT FROM '{"kind":"execution","payload":{"status":"completed"}}'::jsonb
   OR o.actor<>'system' OR o.outcome<>'observed' OR o.payload::jsonb IS DISTINCT FROM jsonb_build_object('schema_version',1,'source',b.source,'observed_status','completed','verification','ungraded')
   OR d.actor<>'system' OR d.outcome<>'observed' OR d.payload::jsonb->>'schema_version' IS DISTINCT FROM '1'
   OR d.payload::jsonb->>'kind' IS DISTINCT FROM 'delegate_execution_experience'
   OR d.payload::jsonb->>'principal' IS DISTINCT FROM $2
   OR d.payload::jsonb->>'scope_kind' IS DISTINCT FROM 'project'
   OR d.payload::jsonb->>'scope_id' IS DISTINCT FROM b.c->'learning_binding'->>'scope_id'
   OR d.payload::jsonb->'source' IS DISTINCT FROM b.source
   OR d.payload::jsonb->'parent_source' IS DISTINCT FROM b.parent_source
   OR d.payload::jsonb->'child' IS DISTINCT FROM b.c
   OR d.payload::jsonb->>'qualification' IS DISTINCT FROM 'ungraded'
   OR d.payload::jsonb->>'observed_status' IS DISTINCT FROM 'completed'
   OR d.payload::jsonb ? 'verification_ref' OR d.payload::jsonb ? 'verdict'
   OR d.payload::jsonb->>'action_coverage' IS DISTINCT FROM 'bounded_canonical_tool_calls'
   OR d.payload::jsonb->'actions' IS DISTINCT FROM '[]'::jsonb
   OR l.role<>'format' OR l.failure_mode<>'observed' OR l.auto_applied<>0 OR l.confidence<>0 OR l.review_status<>'pending'
   OR l.evidence_json::jsonb IS DISTINCT FROM d.payload::jsonb)
  FROM bound b JOIN interaction_events o ON o.session_id=$4 AND o.event_type='task_outcome'
  JOIN interaction_events d ON d.session_id=$4 AND d.event_type='delegate_experience'
  JOIN delegate_learnings l ON l.session_id=$4`, session, actor, job, child).Scan(&count, &invalid)
		if err != nil || count != 1 || invalid != 0 {
			t.Fatalf("actual job %s canonical retention: count=%d invalid=%d error=%v", job, count, invalid, err)
		}
	}
	var duplicates int
	if err = conn.QueryRow(ctx, `SELECT count(*) FROM delegate_learnings WHERE session_id=$1 AND role='format' AND failure_mode='success'`, session).Scan(&duplicates); err != nil || duplicates != 0 {
		t.Fatal("retired raw exit writer produced duplicate parent learning", duplicates, err)
	}
}
