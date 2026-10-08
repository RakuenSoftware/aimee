package memory

import (
	"context"
	"errors"
	"fmt"
	store "github.com/JBailes/aimee/server-go/db"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

type recoverySlowReader struct{ store.Queryer }

func (s recoverySlowReader) Query(ctx context.Context, _ string, _ ...any) (store.Rows, error) {
	<-ctx.Done()
	return nil, ctx.Err()
}

func TestEvidenceRecoveryDurableRoundPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_KB_STORE_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_KB_STORE_REPLAY_URL")
	}
	ctx := context.Background()
	pool, err := pgxpool.New(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer pool.Close()
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
	actor := fmt.Sprintf("test:mr05:%d", time.Now().UnixNano())
	defer pool.Exec(ctx, `DELETE FROM memory_evidence_recovery WHERE actor_principal=$1`, actor)
	backend := &postgresDataStore{db: runtimeRoleTx{evalQueryer{tx}, t}, recoveryDB: scopePoolStore{pool: pool}, placement: PlacementKB}
	if _, err = tx.Exec(ctx, `SELECT set_config('jit','off',true),set_config('aimee.memory_scope_all','1',true)`); err != nil {
		t.Fatal(err)
	}
	_, err = backend.assertFact(ctx, factAssertion{FactCandidate: FactCandidate{Subject: actor, Relation: "uses", Object: "canonical recovered value", AssertionKind: "world_fact", Actor: FactActor{Principal: actor, Role: "user", Rank: 30, Authenticated: 1, TransportIdentity: "test"}, Evidence: FactEvidence{SourceKind: "observation", SourceID: actor, Stance: "supports"}}})
	if err != nil {
		t.Fatal(err)
	}
	build := func(task string, tokens int) (DataRequest, *typedContextResult) {
		r := recoveryFixture(t)
		r.Requirements.TaskRevision = task
		r.Requirements.Obligations[0].Subject = actor
		r.Requirements.Recovery.MaxTokens = tokens
		r.Requirements.Recovery.MaxElapsedMS = 2000
		if err := r.finish(); err != nil {
			t.Fatal(err)
		}
		request := DataRequest{recoveryActor: actor, TypedContext: &typedContextOptions{Requirements: r.Requirements}, Assertions: &assertionSearchRequest{}}
		return request, r
	}
	request, r := build("round", 4096)
	if err = backend.executeEvidenceRecovery(ctx, request, Scope{}, r); err != nil {
		t.Fatal(err)
	}
	if r.Sufficiency != "complete" || r.Recovery.Execution.NewItems != 1 || r.Recovery.Execution.Rounds != 1 {
		t.Fatalf("recovery did not retain canonical evidence: %+v / %+v", r.Coverage, r.Recovery.Execution)
	}
	// Repeated queries/packing or an altered requirement cannot reset this revision.
	request, retry := build("round", 4096)
	if err = backend.executeEvidenceRecovery(ctx, request, Scope{}, retry); err != nil {
		t.Fatal(err)
	}
	if retry.Recovery.Execution.State != "duplicate_blocked" || retry.Recovery.Execution.NewItems != 0 || retry.Sufficiency == "complete" {
		t.Fatal(retry.Recovery)
	}
	// A one-token ceiling cannot admit a serialized source and is not rounded up.
	request, small := build("token-cap", 1)
	if err = backend.executeEvidenceRecovery(ctx, request, Scope{}, small); err != nil {
		t.Fatal(err)
	}
	if small.Recovery.Execution.State != "work_exhausted" || small.Recovery.Execution.Tokens != 0 || small.Sufficiency == "complete" {
		t.Fatal(small.Recovery)
	}
	request, timed := build("latency-cap", 4096)
	timed.Requirements.Recovery.MaxElapsedMS = 25
	if err := timed.finish(); err != nil {
		t.Fatal(err)
	}
	original := backend.db
	backend.db = recoverySlowReader{original}
	began := time.Now()
	err = backend.executeEvidenceRecovery(ctx, request, Scope{}, timed)
	backend.db = original
	if err != nil || timed.Recovery.Execution.State != "time_exhausted" || timed.Recovery.Execution.NewItems != 0 || time.Since(began) > time.Second {
		t.Fatal("latency bound failed", err, timed.Recovery)
	}
	request, limited := build("item-cap", 4096)
	limited.Requirements.Recovery.MaxItems = 1
	limited.Requirements.Obligations = append(limited.Requirements.Obligations, evidenceObligation{Subject: "missing second obligation", Relation: "uses"})
	if err := limited.finish(); err != nil {
		t.Fatal(err)
	}
	if err = backend.executeEvidenceRecovery(ctx, request, Scope{}, limited); err != nil || limited.Recovery.Execution.NewItems != 1 || limited.Sufficiency != "partial" {
		t.Fatal(err, limited.Recovery)
	}
	// A reservation survives loss of the owning context transaction and owner
	// reconstruction; pending work is never assumed to have completed.
	if admitted, err := backend.recoveryLedger(ctx, actor, "crash", "digest", nil); err != nil || !admitted {
		t.Fatal(admitted, err)
	}
	if err = tx.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	restarted := &postgresDataStore{recoveryDB: scopePoolStore{pool: pool}}
	if admitted, err := restarted.recoveryLedger(ctx, actor, "crash", "digest", nil); err != nil || admitted {
		t.Fatal("crash repeated round", admitted, err)
	}
	var duplicates int
	if err = pool.QueryRow(ctx, `SELECT sum(duplicate_attempts) FROM memory_evidence_recovery WHERE actor_principal=$1`, actor).Scan(&duplicates); err != nil || duplicates != 2 {
		t.Fatal(duplicates, err)
	}
	// Concurrent requests consume exactly one round; all losing attempts persist.
	var wg sync.WaitGroup
	results := make(chan bool, 8)
	errs := make(chan error, 8)
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			ok, err := restarted.recoveryLedger(ctx, actor, "concurrent", "digest", nil)
			results <- ok
			errs <- err
		}()
	}
	wg.Wait()
	close(results)
	close(errs)
	admissions := 0
	for ok := range results {
		if ok {
			admissions++
		}
	}
	for err := range errs {
		if err != nil {
			t.Fatal(err)
		}
	}
	if admissions != 1 {
		t.Fatal("concurrent budget reset", admissions)
	}
}

// Model a saturated admission pool and a committed reservation whose reply is
// lost. Neither case may turn the exhausted work budget into canonical reads.
type recoveryAdmissionDB struct {
	store.DB
	delayBegin bool
	loseCommit bool
	pending    bool
	failure    error
}

func (d *recoveryAdmissionDB) Begin(ctx context.Context) (store.Tx, error) {
	if d.delayBegin {
		<-ctx.Done()
		return nil, ctx.Err()
	}
	if d.failure != nil {
		return nil, d.failure
	}
	return &recoveryAdmissionTx{owner: d}, nil
}

type recoveryAdmissionTx struct {
	store.Queryer
	owner *recoveryAdmissionDB
}

func (t *recoveryAdmissionTx) Exec(_ context.Context, sql string, _ ...any) (store.Tag, error) {
	if strings.HasPrefix(sql, "INSERT INTO memory_evidence_recovery") && t.owner.pending {
		return store.RowsAffected(0), nil
	}
	return store.RowsAffected(1), nil
}
func (t *recoveryAdmissionTx) Commit(ctx context.Context) error {
	t.owner.pending = true
	if t.owner.loseCommit {
		t.owner.loseCommit = false
		<-ctx.Done()
		return ctx.Err()
	}
	return nil
}
func (*recoveryAdmissionTx) Rollback(context.Context) error { return nil }

func TestEvidenceRecoveryAdmissionDeadline(t *testing.T) {
	for _, mode := range []string{"pool_wait", "lost_commit_receipt", "outer_cancellation", "ledger_failure"} {
		t.Run(mode, func(t *testing.T) {
			db := &recoveryAdmissionDB{delayBegin: mode == "pool_wait" || mode == "outer_cancellation", loseCommit: mode == "lost_commit_receipt"}
			failed := errors.New("admission permission refused")
			if mode == "ledger_failure" {
				db.failure = failed
			}
			backend := &postgresDataStore{recoveryDB: db}
			build := func() (DataRequest, *typedContextResult) {
				r := recoveryFixture(t)
				r.Requirements.Recovery.MaxElapsedMS = 25
				if err := r.finish(); err != nil {
					t.Fatal(err)
				}
				return DataRequest{recoveryActor: "test:admission", TypedContext: &typedContextOptions{Requirements: r.Requirements}, Assertions: &assertionSearchRequest{}}, r
			}
			req, r := build()
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			if mode == "outer_cancellation" {
				cancel()
			}
			err := backend.executeEvidenceRecovery(ctx, req, Scope{}, r)
			if mode == "outer_cancellation" {
				if !errors.Is(err, context.Canceled) {
					t.Fatal(err)
				}
				return
			}
			if mode == "ledger_failure" {
				if !errors.Is(err, failed) {
					t.Fatal(err)
				}
				return
			}
			if err != nil || r.Recovery.Execution == nil || r.Recovery.Execution.State != "time_exhausted" || r.Recovery.Execution.NewItems != 0 || r.Recovery.Execution.Rounds != 0 || r.Recovery.State != "exhausted" || r.Sufficiency == "complete" {
				t.Fatal(err, r.Recovery)
			}
			if mode == "lost_commit_receipt" {
				req, retry := build()
				if err := backend.executeEvidenceRecovery(context.Background(), req, Scope{}, retry); err != nil || retry.Recovery.Execution.State != "duplicate_blocked" {
					t.Fatal("unknown commit allowed new reads", err, retry.Recovery)
				}
			}
		})
	}
}

func TestEvidenceRecoveryAdmissionBlockedPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_KB_STORE_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_KB_STORE_REPLAY_URL")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	blocker, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer blocker.Rollback(ctx)
	if _, err = blocker.Exec(ctx, `LOCK TABLE memory_evidence_recovery IN ACCESS EXCLUSIVE MODE`); err != nil {
		t.Fatal(err)
	}
	pool, err := pgxpool.New(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer pool.Close()
	r := recoveryFixture(t)
	r.Requirements.Recovery.MaxElapsedMS = 25
	if err := r.finish(); err != nil {
		t.Fatal(err)
	}
	backend := &postgresDataStore{recoveryDB: scopePoolStore{pool: pool}}
	req := DataRequest{recoveryActor: fmt.Sprintf("test:blocked:%d", time.Now().UnixNano()), TypedContext: &typedContextOptions{Requirements: r.Requirements}, Assertions: &assertionSearchRequest{}}
	if err := backend.executeEvidenceRecovery(ctx, req, Scope{}, r); err != nil || r.Recovery.Execution == nil || r.Recovery.Execution.State != "time_exhausted" || r.Recovery.Execution.NewItems != 0 {
		t.Fatal("blocked admission was not bounded", err, r.Recovery)
	}
}
