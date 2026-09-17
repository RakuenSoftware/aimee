package memory

import (
	"context"
	"encoding/json"
	"os"
	"reflect"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestActivationSnapshotValidation(t *testing.T) {
	for _, raw := range []string{"", `null`, `[]`, `{"current_turn":0}`, `{"current_turn":-1}`, `{"current_turn":1.5}`, `{"current_turn":9007199254740992}`} {
		if got := parseActivation(json.RawMessage(raw)); got != nil {
			t.Fatalf("malformed state %s loaded: %+v", raw, got)
		}
	}
	got := parseActivation(json.RawMessage(`{"current_turn":3,"rows":[
 null,false,{"memory_id":1,"last_turn":0},{"memory_id":2,"last_turn":4},
 {"memory_id":3,"last_turn":1},{"memory_id":3,"last_turn":2},{"memory_id":4,"last_turn":2}]}`))
	if got == nil || !reflect.DeepEqual(got.Rows, []ActivationRow{{3, 1}, {4, 2}}) {
		t.Fatalf("rows=%+v", got)
	}
	rows := make([]ActivationRow, activationMaxRows+10)
	for i := range rows {
		rows[i] = ActivationRow{int64(i + 1), 1}
	}
	raw, _ := json.Marshal(ActivationSnapshot{CurrentTurn: 2, Rows: rows})
	if n := len(parseActivation(raw).Rows); n != activationMaxRows {
		t.Fatal(n)
	}
}

// These fixtures run the production query on PostgreSQL. The high-ranked held
// rows deliberately exceed the result cap: filtering after LIMIT cannot pass.
func TestActivationPostgresSelectionAndRecall(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("set AIMEE_MEMORY_EVAL_URL for the PostgreSQL activation regression")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	_, err = tx.Exec(ctx, `CREATE TEMP TABLE memories (
 id bigint PRIMARY KEY,scope_type text DEFAULT 'global',scope_value text DEFAULT '_global',
 tier text DEFAULT 'L2',kind text DEFAULT 'preference',key text DEFAULT 'editor',content text DEFAULT 'fixture',
 confidence double precision DEFAULT 0.5,lifecycle_state text DEFAULT 'active',
 use_count bigint DEFAULT 0,updated_at timestamptz DEFAULT now(),
 activation_sticky_turns bigint DEFAULT 2,activation_cooldown_turns bigint DEFAULT 1,
 activation_delay_turns bigint DEFAULT 0,activation_suppressed bigint DEFAULT 0);
 INSERT INTO memories(id,confidence,activation_cooldown_turns,activation_delay_turns,activation_suppressed) VALUES
 (1,1,3,0,0),(2,0.99,1,3,0),(3,0.98,1,0,1),(4,0.8,1,0,0),(5,0.82,1,0,0);
 INSERT INTO memories(id,kind,key,confidence) VALUES(6,'fact','unrelated',0.7);
 INSERT INTO memories(id,lifecycle_state,activation_delay_turns) VALUES(7,'pending',3),(8,'pending',0);
 INSERT INTO memories(id,lifecycle_state,activation_suppressed) VALUES(9,'pending',1);
 CREATE TEMP TABLE prospective_memories(id bigint,trigger_text text,action_text text,anchor_entity text,
 anchor_file text,recurrence text,state text,valid_until text,source_session text,trigger_count bigint,
 last_triggered_at text,created_at text,updated_at text);
 CREATE TEMP TABLE epistemic_directives(id bigint,question text,topic text,anchor_entity text,anchor_file text,
 cause text,priority bigint,state text,memory_a_id bigint,memory_b_id bigint,resolution_memory_id bigint,
 evidence text,source_session text,surfaced_count bigint,last_surfaced_at text,resolved_at text,
 valid_until text,created_at text,updated_at text);
 CREATE SCHEMA activation_test;
 CREATE FUNCTION activation_test.pg_now_text() RETURNS text LANGUAGE sql AS $$ SELECT now()::text $$;
 SET LOCAL search_path TO pg_temp,activation_test,public;`)
	if err != nil {
		t.Fatal(err)
	}
	s := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB}
	snapshot := &ActivationSnapshot{CurrentTurn: 3, Rows: []ActivationRow{{1, 2}, {4, 1}, {6, 1}}}
	ids := func(records []Record) []int64 {
		out := make([]int64, 0, len(records))
		for _, record := range records {
			out = append(out, record.ID)
		}
		return out
	}
	records, why, held, err := s.recallActivated(ctx, snapshot, "kind='preference'", 2, false, false)
	if err != nil || !reflect.DeepEqual(ids(records), []int64{4, 5}) || held != 3 || why[4] != "sticky activation" {
		t.Fatalf("selection=%v why=%v held=%d err=%v", ids(records), why, held, err)
	}
	records, _, held, err = s.recallActivated(ctx, snapshot, "id=1", 2, false, false)
	if err != nil || len(records) != 0 || held != 1 {
		t.Fatalf("all held=%v %d %v", records, held, err)
	}
	records, _, _, err = s.recallActivated(ctx, snapshot, "key='no-match'", 10, true, false)
	if err != nil || !reflect.DeepEqual(ids(records), []int64{4, 6}) {
		t.Fatalf("sticky relevance=%v %v", ids(records), err)
	}
	records, _, held, err = s.activationAfterFusion(ctx, snapshot, []Record{{ID: 1}}, []Record{{ID: 4}, {ID: 5}}, 2)
	if err != nil || !reflect.DeepEqual(ids(records), []int64{4, 5}) || held != 1 {
		t.Fatalf("graph bypass/backfill=%v %d %v", ids(records), held, err)
	}
	records, _, held, err = s.recallActivated(ctx, snapshot, "true", 5, false, true)
	if err != nil || !reflect.DeepEqual(ids(records), []int64{8}) || held != 2 {
		t.Fatalf("commitments=%v %d %v", ids(records), held, err)
	}
	// Current turn comes from the persisted conversation, even if no memory
	// fired on intervening turns. Cooldown wins while it overlaps sticky.
	for _, turn := range []int64{2, 3, 4} {
		snap := &ActivationSnapshot{CurrentTurn: turn, Rows: []ActivationRow{{4, 1}}}
		records, why, _, err = s.recallActivated(ctx, snap, "id=4", 2, false, false)
		if err != nil {
			t.Fatal(err)
		}
		if turn == 2 && len(records) != 0 {
			t.Fatal("cooldown lost to sticky")
		}
		if turn == 3 && (len(records) != 1 || why[4] != "sticky activation") {
			t.Fatal("sticky band lost")
		}
		if turn == 4 && (len(records) != 1 || why[4] != "") {
			t.Fatal("sticky did not expire")
		}
	}
	raw, _ := json.Marshal(snapshot)
	if _, err := s.RecallBundleWithActivation(ctx, "", 0, false, raw); err != nil {
		t.Fatal(err)
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, s))
	beforeMetrics := recallMetrics()
	dashboard := runHostRuntime(t, handler, `{"operation":"recall-dashboard"}`)
	metrics := dashboard["metrics"].(map[string]any)
	if dashboard["session_start"] != true || metrics["assemblies_total"].(float64) != float64(beforeMetrics.Assemblies+1) || metrics["session_start_assemblies"].(float64) != float64(beforeMetrics.Starts+1) || metrics["ms_max"].(float64) < 0 {
		t.Fatal(dashboard)
	}
	client := clientForHandler(t, handler)
	reply, err := client.Data(ctx, 73, DataRequest{Operation: "recall-bundle", Activation: raw})
	if err != nil {
		t.Fatal(err)
	}
	var bundle recallBundle
	if err = json.Unmarshal(reply.Payload, &bundle); err != nil {
		t.Fatal(err)
	}
	if bundle.ActivationHeld < 3 || len(bundle.Preferences) != 2 || bundle.Preferences[0].ID != 4 ||
		!bundle.Preferences[0].ActivationManaged || bundle.Preferences[0].Why != "sticky activation" {
		t.Fatalf("bundle=%+v", bundle)
	}
	// The public KB command must retain the snapshot and the established scope
	// envelope. An injected data operation/scope cannot replace recall.
	for _, scoped := range []bool{false, true} {
		args, _ := json.Marshal(map[string]any{"activation": snapshot, "scope_context": scoped,
			"operation": "delete", "scope": map[string]string{"type": "user"}})
		body, err := client.Command(ctx, 73, "recall", args)
		if err != nil {
			t.Fatal(err)
		}
		var envelope struct {
			Status  string
			Recall  recallBundle
			Missing *bool `json:"active_context_missing"`
		}
		if json.Unmarshal(body, &envelope) != nil || envelope.Status != "ok" ||
			len(envelope.Recall.Preferences) != 2 || envelope.Recall.Preferences[0].ID != 4 ||
			!envelope.Recall.Preferences[0].ActivationManaged {
			t.Fatalf("public recall=%s", body)
		}
		if scoped && (envelope.Missing == nil || !*envelope.Missing) || !scoped && envelope.Missing != nil {
			t.Fatalf("scope envelope=%s", body)
		}
	}
	// The recall operation never advances or writes activation state. No
	// activation-event/turn or reinforcement tables exist in this fixture.
	for _, bad := range []json.RawMessage{nil, json.RawMessage(`false`)} {
		reply, err = client.Data(ctx, 73, DataRequest{Operation: "recall-bundle", Activation: bad})
		if err != nil {
			t.Fatal(err)
		}
		bundle = recallBundle{}
		if json.Unmarshal(reply.Payload, &bundle) != nil || bundle.Preferences[0].ID != 1 || bundle.Preferences[0].ActivationManaged {
			t.Fatal("missing state did not fail open")
		}
		for _, item := range bundle.OpenCommitments {
			if item.ID == 9 {
				t.Fatal("missing state bypassed stored suppression")
			}
		}
	}
}
