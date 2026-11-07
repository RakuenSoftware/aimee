package families

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
)

func TestLocalProcedureEventsPostgres(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	schema := fmt.Sprintf("procedure_events_%d", time.Now().UnixNano())
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
	defer pool.Close()
	migrations, err := Migrations()
	if err != nil {
		t.Fatal(err)
	}
	for _, migration := range migrations {
		for _, statement := range migration.Statements {
			if _, err = pool.Exec(ctx, statement); err != nil {
				t.Fatalf("migration %d: %v", migration.Version, err)
			}
		}
	}
	procedure := `{"owner_id":"local-owner","procedure_id":"route","revision":"r1"}`
	exposure := `{"procedure":` + procedure + `,"producer":"local_host_ledger","request_id":"request-1","receipt_ref":"receipt-1","attempt_id":"attempt-1","state":"delivered"}`
	event := func(id, state string) string {
		return `{"schema_version":1,"event_id":"` + id + `","actor":"alice","procedure":` + procedure + `,"receipt_ref":"receipt-1","attempt_id":"attempt-1","state":"` + state + `","authority":"user_feedback"}`
	}
	load := func(actor string) (string, []json.RawMessage) {
		t.Helper()
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, cells, err := procedureEventLoad(ctx, sessionIRLiveTx{tx}, []string{actor, procedure})
		if err != nil || status != store.StatusOK || len(cells) != 1 {
			t.Fatalf("load: %d %v %v", status, cells, err)
		}
		var snapshot struct {
			Revision string
			Events   []json.RawMessage
		}
		if err = json.Unmarshal([]byte(cells[0]), &snapshot); err != nil {
			t.Fatal(err)
		}
		return snapshot.Revision, snapshot.Events
	}
	appendEvent := func(actor, revision, raw string) (uint32, string, error) {
		tx, err := pool.Begin(ctx)
		if err != nil {
			return 0, "", err
		}
		defer tx.Rollback(ctx)
		status, cells, err := procedureEventAppend(ctx, sessionIRLiveTx{tx}, []string{actor, procedure, revision, raw, exposure, "global", ""})
		if err != nil {
			return status, "", err
		}
		if err = tx.Commit(ctx); err != nil {
			return status, "", err
		}
		if len(cells) == 1 {
			return status, cells[0], nil
		}
		return status, "", nil
	}
	revision, events := load("alice")
	if len(events) != 0 || len(revision) != 64 {
		t.Fatal("empty snapshot")
	}
	if status, result, err := appendEvent("bob", revision, event("one", "applied")); err != nil || status != store.StatusInvalid || result != "" {
		t.Fatalf("actor mismatch: %d %s %v", status, result, err)
	}
	if status, result, err := appendEvent("alice", revision, event("one", "applied")); err != nil || status != store.StatusOK || result != "inserted" {
		t.Fatalf("insert: %d %s %v", status, result, err)
	}
	if status, result, err := appendEvent("alice", revision, event("one", "applied")); err != nil || status != store.StatusOK || result != "retry" {
		t.Fatalf("stale CAS: %d %s %v", status, result, err)
	}
	revision, events = load("alice")
	if len(events) != 1 {
		t.Fatal("missing event")
	}
	if status, result, err := appendEvent("alice", revision, event("one", "applied")); err != nil || status != store.StatusOK || result != "duplicate" {
		t.Fatalf("retry: %d %s %v", status, result, err)
	}
	if status, _, err := appendEvent("alice", revision, event("one", "verified_success")); err != nil || status != store.StatusInvalid {
		t.Fatalf("immutable replacement: %d %v", status, err)
	}
	if _, events := load("bob"); len(events) != 0 {
		t.Fatal("cross-principal history")
	}
	// Two projectors can read the same snapshot. Exactly one append may commit;
	// the loser must re-project the newer snapshot rather than overwrite it.
	var wg sync.WaitGroup
	outcomes := make(chan string, 2)
	for _, id := range []string{"two", "three"} {
		wg.Add(1)
		go func(id string) {
			defer wg.Done()
			status, result, err := appendEvent("alice", revision, event(id, "outcome_unknown"))
			if err != nil || status != store.StatusOK {
				outcomes <- fmt.Sprintf("error:%d:%v", status, err)
				return
			}
			outcomes <- result
		}(id)
	}
	wg.Wait()
	close(outcomes)
	counts := map[string]int{}
	for result := range outcomes {
		counts[result]++
	}
	if counts["inserted"] != 1 || counts["retry"] != 1 {
		t.Fatalf("CAS race: %v", counts)
	}
	revision, events = load("alice")
	if len(events) != 2 {
		t.Fatal("concurrent snapshot")
	}
	// No 4096-byte DTO truncation: an admitted observation can carry long refs.
	var large map[string]any
	if err = json.Unmarshal([]byte(event("large", "outcome_unknown")), &large); err != nil {
		t.Fatal(err)
	}
	refs := make([]string, 64)
	for i := range refs {
		refs[i] = strings.Repeat("a", 1024)
	}
	gaps := make([]string, 32)
	for i := range gaps {
		gaps[i] = strings.Repeat("b", 1024)
	}
	large["action_refs"] = refs
	large["applicability_gaps"] = gaps

	raw, _ := json.Marshal(large)
	if status, result, err := appendEvent("alice", revision, string(raw)); err != nil || status != store.StatusOK || result != "inserted" {
		t.Fatalf("large event: %d %s %v", status, result, err)
	}
	_, events = load("alice")
	if len(events) != 3 {
		t.Fatal("large event lost")
	}
	for _, raw := range events {
		var item map[string]any
		_ = json.Unmarshal(raw, &item)
		if item["event_id"] == "large" && (len(item["action_refs"].([]any)) != 64 || len(item["applicability_gaps"].([]any)) != 32) {
			t.Fatal("truncated event")
		}
	}
}
