package families

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"testing"
	"time"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
)

// This fixture proves atomic storage/reload and actor binding of host metadata.
// It does not establish independent execution from a crafted schema fixture.
func TestSessionIRVerifiedTerminalProofPostgresAtomicReload(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	schema := fmt.Sprintf("session_proof_%d", time.Now().UnixNano())
	if _, err := root.Exec(ctx, "CREATE SCHEMA "+schema); err != nil {
		t.Fatal(err)
	}
	defer root.Exec(context.Background(), "DROP SCHEMA "+schema+" CASCADE")
	config, err := pgxpool.ParseConfig(os.Getenv("AIMEE_TEST_PG_URL"))
	if err != nil {
		t.Fatal(err)
	}
	config.ConnConfig.RuntimeParams["search_path"] = schema
	pool, err := pgxpool.NewWithConfig(ctx, config)
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if pool != nil {
			pool.Close()
		}
	}()
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_native_cache_recovery.sql", "schema_learning_projection_outbox.sql"} {
		body, err := schemaFS.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(ctx, string(body)); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES ('session-1','actor')`); err != nil {
		t.Fatal(err)
	}
	body := sessionIRVerificationEvent(t, sessionIRVerificationFixture())
	fields := []string{"session-1", "terminal", "turn-1", "actor", body}
	appendProof := func(commit bool) string {
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, cells, err := sessionIRAppendVerified(ctx, sessionIRLiveTx{tx}, fields)
		if err != nil || status != store.StatusOK || len(cells) != 1 {
			t.Fatal("host terminal append", status, cells, err)
		}
		if commit {
			if err := tx.Commit(ctx); err != nil {
				t.Fatal(err)
			}
		}
		return cells[0]
	}
	appendProof(false)
	var count int
	if err = pool.QueryRow(ctx, "SELECT count(*) FROM session_ir_events").Scan(&count); err != nil || count != 0 {
		t.Fatal("rolled-back terminal left an orphan proof", count, err)
	}
	if appendProof(true) != "1" {
		t.Fatal("terminal/proof did not occupy one durable event")
	}
	pool.Close()
	pool = nil
	pool, err = pgxpool.NewWithConfig(ctx, config)
	if err != nil {
		t.Fatal(err)
	}
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	status, cells, err := sessionIRPage(ctx, sessionIRLiveTx{tx}, []string{"session-1", "0", "128", "actor"})
	if err != nil || status != store.StatusOK || len(cells) != 1 {
		t.Fatal("durable reload", status, cells, err)
	}
	var events []struct {
		Session  string          `json:"session_id"`
		Turn     string          `json:"turn_id"`
		Actor    string          `json:"actor"`
		Sequence string          `json:"sequence"`
		Event    json.RawMessage `json:"event"`
	}
	if err = json.Unmarshal([]byte(cells[0]), &events); err != nil || len(events) != 1 {
		t.Fatal("terminal/proof did not reload atomically", cells, err)
	}
	event := events[0]
	var restored, original any
	json.Unmarshal(event.Event, &restored)
	json.Unmarshal([]byte(body), &original)
	restoredJSON, _ := json.Marshal(restored)
	originalJSON, _ := json.Marshal(original)
	if event.Session != "session-1" || event.Turn != "turn-1" || event.Actor != "actor" || event.Sequence != "1" || string(restoredJSON) != string(originalJSON) {
		t.Fatal("reload changed bound proof", string(event.Event))
	}
	tx.Rollback(ctx)
	if appendProof(true) != "1" {
		t.Fatal("retry duplicated proof-bearing terminal")
	}
	// Caller metadata cannot claim another actor even through the host-only API.
	foreign := sessionIRVerificationFixture()
	foreign.Actor = "other"
	fields[1] = "foreign"
	fields[4] = sessionIRVerificationEvent(t, foreign)
	tx, err = pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	status, _, err = sessionIRAppendVerified(ctx, sessionIRLiveTx{tx}, fields)
	if err != nil || status != store.StatusInvalid {
		t.Fatal("foreign proof survived append binding", status, err)
	}
}
