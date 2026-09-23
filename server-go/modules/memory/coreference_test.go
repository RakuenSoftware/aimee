package memory

import (
	"context"
	"encoding/json"
	"errors"
	"reflect"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestCoreferenceHeuristics(t *testing.T) {
	for _, tc := range []struct {
		text string
		want []string
	}{
		{"Alice Smith met Bob Jones.", []string{"alice smith", "bob jon"}},
		{"She visited the office.", []string{}},
		{"Alice met Alice.", []string{"alice"}},
		{"Monday was warm.", []string{}},
	} {
		if got := corefNames(tc.text); !reflect.DeepEqual(got, tc.want) {
			t.Fatal(tc.text, got, tc.want)
		}
	}
	for _, text := range []string{"She visited.", "met her.", "Their office"} {
		if !corefPronoun(text) {
			t.Fatal(text)
		}
	}
	if corefPronoun("shelter is nearby") {
		t.Fatal("substring pronoun")
	}
	if entity, outcome := heuristicCoref([]corefPrior{{content: "Alice met Bob."}, {content: "Carol visited."}}); entity != "" || outcome != "ambiguous" {
		t.Fatal(entity, outcome)
	}
	if entity, outcome := heuristicCoref([]corefPrior{{content: "She visited."}, {content: "Alice visited."}}); entity != "alice" || outcome != "bound" {
		t.Fatal(entity, outcome)
	}
	s := &postgresDataStore{}
	settings, err := s.derivedSettings()
	if err != nil || settings.CorefMode != "off" || settings.CorefWindow != 5 {
		t.Fatal(settings, err)
	}
	t.Setenv("AIMEE_MEMORY_COREF_MODE", "heuristic")
	t.Setenv("AIMEE_MEMORY_COREF_WINDOW", "999")
	settings, err = s.derivedSettings()
	if err != nil || settings.CorefMode != "heuristic" || settings.CorefWindow != 12 {
		t.Fatal(settings, err)
	}
}

func exerciseCoreferenceReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	t.Setenv("AIMEE_MEMORY_COREF_MODE", "")
	t.Setenv("AIMEE_MEMORY_COREF_WINDOW", "")
	if _, err := tx.Exec(ctx, `SELECT set_config('aimee.memory_scope_all','1',true)`); err != nil {
		t.Fatal(err)
	}
	seed := func(content, scope, session string) int64 {
		t.Helper()
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,source_session)
 VALUES('L2','fact','coref-row-'||nextval('memories_id_seq'),$1,'project',$2,$3) RETURNING id`, content, scope, session).Scan(&id); err != nil {
			t.Fatal(err)
		}
		return id
	}
	seed("Alice visited office.", "coref-visible", "coref-session")
	seed("Mallory visited office.", "coref-hidden", "coref-session")
	// These newer rows must not crowd the eligible antecedent out of the bounded
	// context window or reach the optional model resolver.
	for _, state := range []string{"future", "expired", "suppressed", "superseded", "archived", "quarantined", "deleted", "revoked"} {
		id := seed("Nadia ineligible antecedent visited office.", "coref-visible", "coref-session")
		query := `UPDATE memories SET lifecycle_state=$2 WHERE id=$1`
		value := state
		switch state {
		case "future":
			query = `UPDATE memories SET valid_from=(CURRENT_TIMESTAMP+interval '1 hour')::text WHERE id=$1 AND $2<>''`
		case "expired":
			query = `UPDATE memories SET valid_until=CURRENT_TIMESTAMP::text WHERE id=$1 AND $2<>''`
		case "suppressed":
			query = `UPDATE memories SET activation_suppressed=1 WHERE id=$1 AND $2<>''`
		}
		if _, err := tx.Exec(ctx, query, id, value); err != nil {
			t.Fatal(err)
		}
	}

	target := seed("She traveled yesterday.", "coref-visible", "coref-session")
	seed("Alice met Bob.", "coref-visible", "ambiguous-session")
	ambiguous := seed("She called yesterday.", "coref-visible", "ambiguous-session")
	unbound := seed("He traveled.", "coref-visible", "unknown-session")
	suppressedTarget := seed("She target-suppressed-needle traveled.", "coref-visible", "coref-session")
	if _, err := tx.Exec(ctx, `UPDATE memories SET activation_suppressed=1 WHERE id=$1`, suppressedTarget); err != nil {
		t.Fatal(err)
	}
	originalSettings, originalRunner := backend.settings, backend.episodeCommand
	defer func() { backend.settings, backend.episodeCommand = originalSettings, originalRunner }()
	mode := "heuristic"
	backend.settings = func() (map[string]any, error) {
		return map[string]any{"memory_coref_mode": mode, "memory_coref_window": 5, "memory_cognify_command": "fixture-cognifier"}, nil
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	args := `{"scope_context":true,"project":"coref-visible"}`
	call := func() {
		t.Helper()
		r, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "reindex", args)
		if status != bus.ModuleStatusOK || r["status"] != "ok" {
			t.Fatal(r, status)
		}
	}
	check := func(id int64, wantEntity, wantOutcome string) {
		t.Helper()
		var entity, outcome string
		if err := tx.QueryRow(ctx, `SELECT COALESCE((SELECT entity FROM memory_entities WHERE memory_id=$1 AND role='coref' ORDER BY id LIMIT 1),''),
 COALESCE((SELECT outcome FROM memory_coref_audit WHERE memory_id=$1 ORDER BY id DESC LIMIT 1),'')`, id).Scan(&entity, &outcome); err != nil || entity != wantEntity || outcome != wantOutcome {
			t.Fatal(id, entity, outcome, err)
		}
	}
	call()
	check(target, "alice", "bound")
	check(ambiguous, "", "ambiguous")
	check(unbound, "", "unbound")
	check(suppressedTarget, "", "")
	// Disabling the resolver clears old inferred bindings rather than retaining
	// an antecedent from an older version of the note.
	mode = "off"
	call()
	check(target, "", "bound")
	mode = "llm"
	seenTarget := false
	backend.episodeCommand = func(ctx context.Context, command string, input []byte) ([]byte, error) {
		if command != "fixture-cognifier" {
			t.Fatal(command)
		}
		var request struct {
			Task    string   `json:"task"`
			ID      int64    `json:"memory_id"`
			Content string   `json:"content"`
			Context []string `json:"context"`
		}
		if err := json.Unmarshal(input, &request); err != nil || request.Task != "coref" {
			t.Fatal(string(input), err)
		}
		if request.ID == suppressedTarget || strings.Contains(request.Content, "target-suppressed-needle") {
			t.Fatal("suppressed target sent to cognifier")
		}
		for _, text := range request.Context {
			if strings.Contains(text, "Mallory") || strings.Contains(text, "ineligible antecedent") {
				t.Fatal("hidden context sent to cognifier")
			}
		}
		if request.ID == target {
			seenTarget = true
			if len(request.Context) != 1 || !strings.Contains(request.Context[0], "Alice") {
				t.Fatal(request)
			}
			return []byte(`{"coref_bindings":[{"entity":"Ignored","confidence":0.2},{"entity":"Alice","confidence":0.7},{"entity":"Invalid","confidence":2}]}`), nil
		}
		return []byte(`{"coref_bindings":[]}`), nil
	}
	call()
	if !seenTarget {
		t.Fatal("configured cognifier was not invoked")
	}
	check(target, "alice", "bound")
	var confidence float64
	if err := tx.QueryRow(ctx, `SELECT confidence FROM memory_coref_audit WHERE memory_id=$1 ORDER BY id DESC LIMIT 1`, target).Scan(&confidence); err != nil || confidence != .7 {
		t.Fatal(confidence, err)
	}
	backend.episodeCommand = func(context.Context, string, []byte) ([]byte, error) {
		return nil, errors.New("fixture cognifier failed")
	}
	call()
	check(target, "", "unbound")
	// The source/index transaction must roll back if its audit cannot persist.
	mode = "heuristic"
	call()
	check(target, "alice", "bound")
	if _, err := tx.Exec(ctx, `RESET ROLE; REVOKE INSERT ON memory_coref_audit FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`); err != nil {
		t.Fatal(err)
	}
	if _, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "reindex", args); status == bus.ModuleStatusOK {
		t.Fatal("accepted denied coreference audit")
	}
	check(target, "alice", "bound")
	if _, err := tx.Exec(ctx, `RESET ROLE; GRANT INSERT ON memory_coref_audit TO aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`); err != nil {
		t.Fatal(err)
	}
}
