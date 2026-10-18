package learning

import (
	"context"
	"encoding/json"
	"os"
	"strconv"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
)

// The native integration runner supplies a disposable source session after four
// completed browser tasks. This test performs read-only verification against
// the actual C -> Go Learning -> DB1 path, with no KB process or fabricated rows.
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
}
