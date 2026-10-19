package families

import (
	"context"
	"errors"
	"fmt"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
)

func TestSessionHarnessCompareSwapAdmission(t *testing.T) {
	for _, tc := range []struct {
		owner, expected string
		rows            int
		want            uint32
	}{
		{"owner", "", 1, store.StatusOK}, {"owner", "claimed", 1, store.StatusOK},
		{"owner", "stale", 0, store.StatusMissing}, {"other", "", 1, store.StatusInvalid},
	} {
		db := newSessDB()
		db.row = []any{tc.owner}
		db.execRows = tc.rows
		status, _ := sessCall(t, db, opSessionHarnessCompareSwap, []string{"s", "codex-ir", tc.expected, "replacement", "owner"})
		if status != tc.want {
			t.Fatalf("%+v: got %d", tc, status)
		}
		if len(db.executed) < 1 || db.executed[0] != sessionIRLockSQL {
			t.Fatal("no owner lock", db.executed)
		}
		if tc.owner != "owner" && len(db.executed) != 1 {
			t.Fatal("foreign actor reached mutation")
		}
	}
	for _, fields := range [][]string{
		{"", "h", "", "v", "owner"}, {"s", "", "", "v", "owner"},
		{"s", "h", "", "", "owner"}, {"s", "h", "", "v", ""},
		{"s", strings.Repeat("x", 129), "", "v", "owner"},
		{"s", "h", strings.Repeat("x", 1025), "v", "owner"},
	} {
		db := newSessDB()
		status, _ := sessCall(t, db, opSessionHarnessCompareSwap, fields)
		if status != store.StatusInvalid || len(db.executed) != 0 {
			t.Fatal("invalid claim reached store", fields, status)
		}
	}
	db := newSessDB()
	db.scanErr = errors.New("unavailable")
	if _, _, err := sessionHarnessCompareSwap(context.Background(), db, []string{"s", "h", "", "v", "owner"}); err == nil {
		t.Fatal("store outage became conflict")
	}
}

func TestSessionHarnessCompareSwapLiveConcurrentClaims(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	schema := fmt.Sprintf("native_checkpoint_%d", time.Now().UnixNano())
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
	for _, name := range []string{"schema_sessions.sql", "schema_session_harness.sql"} {
		body, err := schemaFS.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(ctx, string(body)); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES('s','owner')`); err != nil {
		t.Fatal(err)
	}
	exchange := func(expected, replacement, actor string) (uint32, error) {
		tx, err := pool.Begin(ctx)
		if err != nil {
			return 0, err
		}
		defer tx.Rollback(ctx)
		status, _, err := sessionHarnessCompareSwap(ctx, sessionIRLiveTx{tx}, []string{"s", "codex-ir", expected, replacement, actor})
		if err != nil {
			return status, err
		}
		return status, tx.Commit(ctx)
	}
	const count = 24
	winners := make(chan string, count)
	failures := make(chan error, count)
	var group sync.WaitGroup
	for i := 0; i < count; i++ {
		group.Add(1)
		go func(i int) {
			defer group.Done()
			claim := fmt.Sprintf("inflight-%d", i)
			status, err := exchange("", claim, "owner")
			if err != nil {
				failures <- err
				return
			}
			if status == store.StatusOK {
				winners <- claim
			} else if status != store.StatusMissing {
				failures <- fmt.Errorf("claim status %d", status)
			}
		}(i)
	}
	group.Wait()
	close(winners)
	close(failures)
	for err := range failures {
		t.Error(err)
	}
	var winner string
	wins := 0
	for claim := range winners {
		winner = claim
		wins++
	}
	if wins != 1 {
		t.Fatalf("expected one durable winner, got %d", wins)
	}
	if status, err := exchange("stale-claim", "completed-stale", "owner"); err != nil || status != store.StatusMissing {
		t.Fatal("stale completion overwrote claim", status, err)
	}
	if status, err := exchange(winner, "completed-foreign", "other"); err != nil || status != store.StatusInvalid {
		t.Fatal("foreign completion", status, err)
	}
	if status, err := exchange(winner, "completed", "owner"); err != nil || status != store.StatusOK {
		t.Fatal("winner cannot complete", status, err)
	}
	if status, err := exchange(winner, "completed-late", "owner"); err != nil || status != store.StatusMissing {
		t.Fatal("late completion overwrote new state", status, err)
	}
	var stored string
	if err := pool.QueryRow(ctx, `SELECT native_id FROM session_harness_handles WHERE session_id='s' AND harness='codex-ir'`).Scan(&stored); err != nil || stored != "completed" {
		t.Fatal("wrong final checkpoint", stored, err)
	}
}

func TestNativeCheckpointIsNeverModelHistory(t *testing.T) {
	db := newSessDB()
	checkpoint := fmt.Sprintf(`{"kind":"native_checkpoint","payload":{"scope":"%s","context_digest":"%s","prefix_digest":"%s","claim":"%s","harness":"codex","native_id":"private-native-handle","message_count":1}}`, strings.Repeat("a", 64), strings.Repeat("b", 64), strings.Repeat("c", 64), strings.Repeat("d", 32))
	if !validSessionIREvent([]byte(checkpoint)) {
		t.Fatal("invalid fixture checkpoint")
	}
	db.rows = [][]any{
		{"t", `{"kind":"execution","payload":{"status":"running"}}`},
		{"t", `{"kind":"message","message_id":"u","payload":{"role":"user","blocks":[{"type":"text","text":"canonical input"}]}}`},
		{"t", `{"kind":"execution","payload":{"status":"completed"}}`},
		{"t", checkpoint},
	}
	status, cells, err := sessionIRHistoryLoad(context.Background(), db, []string{"s", "primary", "owner", "next"})
	if err != nil || status != store.StatusOK || len(cells) != 1 || strings.Contains(cells[0], "native") || !strings.Contains(cells[0], "canonical input") {
		t.Fatal("checkpoint became model content", status, cells, err)
	}
}
