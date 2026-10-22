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

	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
)

func TestExecutionGradePublisherRequiresHostPrincipal(t *testing.T) {
	db := newDelDB()
	frame, err := wire.EncodeFields(opExecutionTestGradeRecord, []string{"operator", "{}"})
	if err != nil {
		t.Fatal(err)
	}
	_, status := Telemetry.Handler(db)(bus.ModuleInvocation{StageID: StageTelemetry, PrincipalRef: 7}, frame)
	if status != bus.ModuleStatusInvalidRequest || len(db.executed) != 0 {
		t.Fatal("non-host grade publication reached storage", status, db.executed)
	}
}

func TestExecutionGradesPostgresImmutableSourceAndReplay(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	schema := fmt.Sprintf("execution_grades_%d", time.Now().UnixNano())
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
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_native_cache_recovery.sql", "schema_learning_projection_outbox.sql", "schema_telemetry.sql"} {
		body, err := schemaFS.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(ctx, string(body)); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES ('s','operator')`); err != nil {
		t.Fatal(err)
	}
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	proof := sessionIRVerificationFixture()
	proof.Session = "s"
	proof.Turn = "turn"
	proof.Actor = "operator"
	proofRaw, _ := json.Marshal(proof)
	status, _, err := sessionIRAppendVerified(ctx, sessionIRLiveTx{tx}, []string{"s", "terminal", "turn", "operator", sessionIRVerificationEvent(t, proof)})
	if err != nil || status != store.StatusOK {
		tx.Rollback(ctx)
		t.Fatal("canonical source append", status, err)
	}
	if err = tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	// The private publisher accepts an already admitted owner result. Policy is
	// exercised in Learning; this fixture tests source integrity and transactions.
	grade := localExecutionTestGrade{SchemaVersion: 1,
		Source:         executionGradeSource{Kind: "canonical_session_log", Session: "s", Turn: "turn", Sequence: "1", Event: "terminal", Actor: "operator"},
		ObservedStatus: "completed", Verification: "tests_bound_revision", Qualification: "configured_tests_only",
		Verdict: "passed", VerificationID: "sha256:" + strings.Repeat("a", 64),
		Proof: proofRaw}
	raw, _ := json.Marshal(grade)
	write := func(principal, body string, generic bool) (uint32, error) {
		tx, err := pool.Begin(ctx)
		if err != nil {
			return 0, err
		}
		defer tx.Rollback(ctx)
		var status uint32
		if generic {
			status, _, err = interactionEventRecord(ctx, sessionIRLiveTx{tx}, []string{"s", "task_outcome", "system", body, "observed"})
		} else {
			status, _, err = executionTestGradeRecord(ctx, sessionIRLiveTx{tx}, []string{principal, body})
		}
		if err == nil && status == store.StatusOK {
			err = tx.Commit(ctx)
		}
		return status, err
	}
	if status, err = write("operator", string(raw), true); err != nil || status != store.StatusInvalid {
		t.Fatal("generic event writer accepted a grade", status, err)
	}
	if status, err = write("foreign", string(raw), false); err != nil || status != store.StatusInvalid {
		t.Fatal("foreign principal published a grade", status, err)
	}
	errors := make(chan error, 12)
	var workers sync.WaitGroup
	for i := 0; i < 12; i++ {
		workers.Add(1)
		go func() {
			defer workers.Done()
			status, err := write("operator", string(raw), false)
			if err != nil || status != store.StatusOK {
				errors <- fmt.Errorf("concurrent publication: %d %v", status, err)
			}
		}()
	}
	workers.Wait()
	close(errors)
	for err := range errors {
		t.Error(err)
	}
	var count int
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM interaction_events WHERE outcome='tests_passed'`).Scan(&count); err != nil || count != 1 {
		t.Fatal("retried immutable proof duplicated", count, err)
	}
	for _, mutate := range []func(*localExecutionTestGrade){
		func(g *localExecutionTestGrade) { g.Source.Event = "foreign" },
		func(g *localExecutionTestGrade) { g.Source.Turn = "foreign" },
		func(g *localExecutionTestGrade) { g.Source.Sequence = "2" },
		func(g *localExecutionTestGrade) { g.Verdict = "failed" },
		func(g *localExecutionTestGrade) {
			foreign := proof
			foreign.ProfileDigest = strings.Repeat("f", 64)
			g.Proof, _ = json.Marshal(foreign)
		},
	} {
		changed := grade
		mutate(&changed)
		body, _ := json.Marshal(changed)
		status, err = write("operator", string(body), false)
		if err != nil || status == store.StatusOK {
			t.Fatal("changed source or conflicting immutable proof accepted", status, err)
		}
	}
	observation := `{"schema_version":1,"source":{"kind":"canonical_session_log","session_id":"s","sequence":"1","event_id":"terminal","turn_id":"turn","actor":"operator"},"observed_status":"completed","verification":"ungraded"}`
	for i := 0; i < 2; i++ {
		if status, err = write("operator", observation, true); err != nil || status != store.StatusOK {
			t.Fatal("grade prevented canonical ungraded observation recovery", status, err)
		}
	}
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM interaction_events WHERE event_type='task_outcome'`).Scan(&count); err != nil || count != 2 {
		t.Fatal("source projections lost or duplicated", count, err)
	}
	if _, err = pool.Exec(ctx, `DELETE FROM server_sessions WHERE id='s'`); err != nil {
		t.Fatal(err)
	}
	if status, err = write("operator", string(raw), false); err != nil || status != store.StatusMissing {
		t.Fatal("erased source still grants grade publication", status, err)
	}
}
