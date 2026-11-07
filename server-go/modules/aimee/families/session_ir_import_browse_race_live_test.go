package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
	"os"
	"testing"
	"time"
)

func TestSessionIRImportLiveBrowseCompletedCheckpointRace(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	schema := fmt.Sprintf("browse_import_race_%d", time.Now().UnixNano())
	if _, e := root.Exec(ctx, "CREATE SCHEMA "+schema); e != nil {
		t.Fatal(e)
	}
	defer root.Exec(context.Background(), "DROP SCHEMA "+schema+" CASCADE")
	cfg, e := pgxpool.ParseConfig(os.Getenv("AIMEE_TEST_PG_URL"))
	if e != nil {
		t.Fatal(e)
	}
	cfg.ConnConfig.RuntimeParams["search_path"] = schema
	pool, e := pgxpool.NewWithConfig(ctx, cfg)
	if e != nil {
		t.Fatal(e)
	}
	defer pool.Close()
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_session_harness.sql", "schema_learning_projection_outbox.sql", "schema_native_cache_recovery.sql", "schema_native_cache_reservations.sql"} {
		raw, e := schemaFS.ReadFile(name)
		if e != nil {
			t.Fatal(e)
		}
		if _, e = pool.Exec(ctx, string(raw)); e != nil {
			t.Fatal(name, e)
		}
	}
	for _, mode := range []string{"model", "browse"} {
		t.Run(mode, func(t *testing.T) {
			session := "race-" + mode
			if _, e := pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,'actor')`, session); e != nil {
				t.Fatal(e)
			}
			legacy := `[{"role":"user","blocks":[{"type":"text","text":"older legacy source"}]}]`
			if _, e := pool.Exec(ctx, `INSERT INTO primary_sessions(session_id,messages_json) VALUES($1,$2)`, session, legacy); e != nil {
				t.Fatal(e)
			}
			sum := sha256.Sum256([]byte(legacy))
			report := canonical.HistoryImportReport{Version: 1, SourceSession: session, SourceSHA256: hex.EncodeToString(sum[:]), SourceFormat: "canonical", Outcome: "exact", SourceMessages: 1, ImportedMessages: 1, ToolStructure: "preserved"}
			encoded, _ := json.Marshal(report)
			fields := []string{session, "primary", "actor", session, "", report.SourceSHA256, legacy, string(encoded), mode}
			// Model the stale initial read: no canonical source existed when browse
			// began. A different producer now owns the row and commits current history.
			var before int
			if e := pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id=$1`, session).Scan(&before); e != nil || before != 0 {
				t.Fatal(before, e)
			}
			hold, e := pool.Begin(ctx)
			if e != nil {
				t.Fatal(e)
			}
			defer hold.Rollback(ctx)
			current := `[{"role":"assistant","blocks":[{"type":"text","text":"new canonical source wins"}]}]`
			s, _, e := sessionIRHistorySave(ctx, sessionIRLiveTx{hold}, []string{session, "concurrent-completed", "primary", "actor", current})
			if e != nil || s != store.StatusOK {
				t.Fatal(s, e)
			}
			type answer struct {
				status uint32
				cells  []string
				err    error
			}
			result := make(chan answer, 1)
			pid := make(chan uint32, 1)
			go func() {
				tx, e := pool.Begin(ctx)
				if e != nil {
					result <- answer{err: e}
					return
				}
				defer tx.Rollback(ctx)
				pid <- tx.Conn().PgConn().PID()
				s, c, e := sessionIRHistoryImport(ctx, sessionIRLiveTx{tx}, fields)
				if e == nil && s == store.StatusOK {
					e = tx.Commit(ctx)
				}
				result <- answer{s, c, e}
			}()
			var backend uint32
			select {
			case backend = <-pid:
			case <-ctx.Done():
				t.Fatal(ctx.Err())
			}
			// Observe a real PostgreSQL row-lock wait rather than assuming scheduling
			// or using a sleep as evidence that the stale read/import raced.
			tick := time.NewTicker(5 * time.Millisecond)
			defer tick.Stop()
			waitCtx, stop := context.WithTimeout(ctx, 5*time.Second)
			defer stop()
			for {
				var waiting bool
				if e := pool.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM pg_stat_activity WHERE pid=$1 AND wait_event_type='Lock')`, backend).Scan(&waiting); e != nil {
					t.Fatal(e)
				}
				if waiting {
					break
				}
				select {
				case <-tick.C:
				case <-waitCtx.Done():
					t.Fatal("import never reached actual row-lock wait", waitCtx.Err())
				}
			}
			if e := hold.Commit(ctx); e != nil {
				t.Fatal(e)
			}
			var got answer
			select {
			case got = <-result:
			case <-ctx.Done():
				t.Fatal(ctx.Err())
			}
			if got.err != nil {
				t.Fatal(got.err)
			}
			if mode == "model" {
				if got.status != store.StatusInvalid || len(got.cells) != 0 {
					t.Fatal("model replaced concurrent checkpoint", got)
				}
			} else if got.status != store.StatusOK || len(got.cells) != 1 || got.cells[0] != "skipped" {
				t.Fatal("browse did not acknowledge canonical winner", got)
			}
			tx, e := pool.Begin(ctx)
			if e != nil {
				t.Fatal(e)
			}
			defer tx.Rollback(ctx)
			s, c, e := sessionIRHistoryLoad(ctx, sessionIRLiveTx{tx}, []string{session, "primary", "actor", ""})
			if e != nil || s != store.StatusOK || len(c) != 1 || c[0] != current {
				t.Fatal("concurrent source changed", s, c, e)
			}
			var after, reports int
			if e := tx.QueryRow(ctx, `SELECT count(*),count(*) FILTER(WHERE payload::jsonb->'payload'->>'phase'='import_report') FROM session_ir_events WHERE session_id=$1`, session).Scan(&after, &reports); e != nil || after != 2 || reports != 0 {
				t.Fatal("skipped import wrote source", after, reports, e)
			}
		})
	}
}
