package db2

import (
	"context"
	"os"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

// The fakes prove an operation handles the rows it is given. They cannot prove
// its statement is a statement: a column that does not exist, a function never
// created, a placeholder in the wrong dialect all pass a fake and fail a
// database. That is what this covers, and it is the reason the C side keeps a
// replay against real PostgreSQL -- the unit suites there drive stub backends
// for the same reason and miss the same things.
//
// Skipped without AIMEE_DB2_URL, so an ordinary `go test ./...` stays hermetic.
// Point it at the replay database, which scripts/db2_replay_env.sh creates:
//
//	bash scripts/db2_replay_env.sh
//	AIMEE_DB2_URL="postgres://aimee:aimee@$(ssh root@192.168.1.252 \
//	  'pct exec 9001 -- hostname -I' | tr -d ' \n')/aimee_db2_process_ci" \
//	  go test ./modules/db2/ -run Live
//
// The address is read rather than written down: the container takes a DHCP
// lease, so it changes whenever the container is recreated -- which it has
// been, repeatedly.

func liveStore(t *testing.T) (Store, func()) {
	t.Helper()
	dsn := os.Getenv("AIMEE_DB2_URL")
	if dsn == "" {
		t.Skip("AIMEE_DB2_URL is unset; this test needs a real database")
	}
	config, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		t.Fatalf("parse AIMEE_DB2_URL: %v", err)
	}
	config.MaxConns = 2
	pool, err := pgxpool.NewWithConfig(context.Background(), config)
	if err != nil {
		t.Fatalf("open pool: %v", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := pool.Ping(ctx); err != nil {
		pool.Close()
		t.Fatalf("ping: %v", err)
	}
	return &PoolStore{pool: pool}, pool.Close
}

// liveRequest is one ported operation and a request that must be answerable
// against a schema with no rows in it. Every read entry is a READ: it runs
// against a shared database and must not leave anything in it. Write entries
// run inside a transaction that always rolls back.
type liveRequest struct {
	name    string
	stage   uint32
	encode  func() ([]byte, error)
	decoded func(t *testing.T, body []byte)
	// repeat runs a write probe more than once inside its transaction, for an
	// operation whose second call is the one that used to fail.
	repeat int
}

func liveReads() []liveRequest {
	return []liveRequest{
		{
			name:  "curator_invalidations_since",
			stage: db2contract.StageCuratorInvalidationsSince,
			encode: func() ([]byte, error) {
				return db2contract.EncodeCuratorInvalidationsSinceRequest(0)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeCuratorInvalidationsSinceReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "prospective_get",
			stage: db2contract.StageProspectiveGet,
			encode: func() ([]byte, error) {
				// An identifier nothing holds: the answer is "not found", which
				// is what proves the statement ran rather than that a row exists.
				return db2contract.EncodeProspectiveGetRequest(2147483000)
			},
			decoded: func(t *testing.T, body []byte) {
				found, _, _, _, _, _, _, _, _, _, _, _, _, err :=
					db2contract.DecodeProspectiveGetReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if found != 0 {
					t.Fatalf("found = %d for an identifier nothing holds", found)
				}
			},
		},
		{
			name:  "prospective_record_trigger",
			stage: db2contract.StageProspectiveRecordTrigger,
			encode: func() ([]byte, error) {
				// An UPDATE whose WHERE matches nothing: it runs, changes no row,
				// and leaves the database as it found it.
				return db2contract.EncodeProspectiveRecordTriggerRequest(2147483000, 0)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, err := db2contract.DecodeProspectiveRecordTriggerReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 {
					t.Fatal("a statement that matched no row did not acknowledge")
				}
			},
		},
		{
			name:  "entity_list_active",
			stage: db2contract.StageEntityListActive,
			encode: func() ([]byte, error) {
				return db2contract.EncodeEntityListActiveRequest(1)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEntityListActiveReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "entity_edge_co_targets",
			stage: db2contract.StageEntityEdgeCoTargets,
			encode: func() ([]byte, error) {
				// The union, the visibility projection and its join to
				// code_projection_generations all have to resolve. A fake
				// proves none of that.
				return db2contract.EncodeEntityEdgeCoTargetsRequest("replay-src", "mentions", 0)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEntityEdgeCoTargetsReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:   "code_index_project_list",
			stage:  db2contract.StageCodeIndexProjectList,
			encode: db2contract.EncodeCodeIndexProjectListRequest,
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeCodeIndexProjectListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "audit_event_list",
			stage: db2contract.StageAuditEventList,
			encode: func() ([]byte, error) {
				// All three predicates, so the assembled statement is the widest
				// shape rather than the simplest one.
				return db2contract.EncodeAuditEventListRequest(
					"2000-01-01T00:00:00Z", "2099-01-01T00:00:00Z", "user", 8)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeAuditEventListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "demotion_candidates",
			stage: db2contract.StageDemotionCandidates,
			encode: func() ([]byte, error) {
				return db2contract.EncodeDemotionCandidatesRequest(1)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeDemotionCandidatesReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
		{
			name:  "enrollment_list",
			stage: db2contract.StageEnrollmentList,
			encode: func() ([]byte, error) {
				return db2contract.EncodeEnrollmentListRequest(8)
			},
			decoded: func(t *testing.T, body []byte) {
				if _, err := db2contract.DecodeEnrollmentListReply(body); err != nil {
					t.Fatalf("decode reply: %v", err)
				}
			},
		},
	}
}

// rollbackStore runs an operation inside a transaction the test always rolls
// back, so a write can be proven to parse and run without leaving anything in a
// database other tests share.
//
// Its InTx runs fn directly rather than opening a nested one. The operation's
// own boundary is subsumed by the outer transaction, which is sound here only
// because the outer one never commits: what is proven is that the statements
// run, not that the operation's commit semantics hold. Those are the fakes'
// job, and TestDecisionLogRecordRollsBackWhenTheSupersedeMissed is where they
// are checked.
type rollbackStore struct {
	tx pgx.Tx
}

func (s *rollbackStore) Query(ctx context.Context, sql string, args ...any) (pgx.Rows, error) {
	return s.tx.Query(ctx, sql, args...)
}

func (s *rollbackStore) QueryRow(ctx context.Context, sql string, args ...any) pgx.Row {
	return s.tx.QueryRow(ctx, sql, args...)
}

func (s *rollbackStore) Exec(ctx context.Context, sql string, args ...any) (int64, error) {
	tag, err := s.tx.Exec(ctx, sql, args...)
	if err != nil {
		return 0, err
	}
	return tag.RowsAffected(), nil
}

func (s *rollbackStore) InTx(ctx context.Context, fn func(Store) error) error {
	return fn(s)
}

func liveWrites() []liveRequest {
	return []liveRequest{
		{
			name:  "prospective_insert",
			stage: db2contract.StageProspectiveInsert,
			encode: func() ([]byte, error) {
				return db2contract.EncodeProspectiveInsertRequest(
					"live trigger", "live action", "", "", "once", "", "live-probe")
			},
			decoded: func(t *testing.T, body []byte) {
				id, err := db2contract.DecodeProspectiveInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if id == 0 {
					t.Fatal("the insert returned no identifier")
				}
			},
		},
		{
			name:  "decision_log_insert",
			stage: db2contract.StageDecisionLogInsert,
			// This probe used to have to retire the occupied ('', 0) slot
			// before it could insert, because idx_dl_active_scope covered
			// unscoped rows and admitted exactly one. That was a defect in the
			// index rather than in the probe -- the invariant belongs to the
			// governance write, which cannot produce an unscoped row -- and the
			// predicate now says `subject <> ''`. Two plain decisions logged
			// against the same database is the case that was broken; the second
			// insert below is that case.
			encode: func() ([]byte, error) {
				return db2contract.EncodeDecisionLogInsertRequest(
					0, "live options", "live chosen", "", "", "")
			},
			// Run twice, because once proves nothing here: the defect this
			// covers let the first unscoped insert through and refused every
			// one after it.
			repeat: 2,
			decoded: func(t *testing.T, body []byte) {
				acknowledged, id, _, _, _, _, _, _, createdAt, status, _, _, _, _, _, err :=
					db2contract.DecodeDecisionLogInsertReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 || id == 0 {
					t.Fatalf("acknowledged = %d, id = %d", acknowledged, id)
				}
				// The database stamped it and defaulted the status: both are
				// columns the caller never sent, which is what proves the row
				// came back rather than the request.
				if createdAt == "" || status != "active" {
					t.Fatalf("createdAt = %q, status = %q", createdAt, status)
				}
			},
		},
		{
			name:  "decision_log_record",
			stage: db2contract.StageDecisionLogRecord,
			encode: func() ([]byte, error) {
				// Superseding nothing, so the insert stands alone: the supersede
				// path needs an active decision to aim at, which a probe leaving
				// nothing behind cannot arrange.
				return db2contract.EncodeDecisionLogRecordRequest(
					"live-subject", "live options", "live chosen", "", "live-probe", 0, "", 0)
			},
			decoded: func(t *testing.T, body []byte) {
				acknowledged, id, _, _, _, _, _, _, _, status, _, _, subject, _, _, err :=
					db2contract.DecodeDecisionLogRecordReply(body)
				if err != nil {
					t.Fatalf("decode reply: %v", err)
				}
				if acknowledged != 1 || id == 0 || status != "active" ||
					subject != "live-subject" {
					t.Fatalf("acknowledged=%d id=%d status=%q subject=%q",
						acknowledged, id, status, subject)
				}
			},
		},
	}
}

func TestLiveWritesRunAndLeaveNothingBehind(t *testing.T) {
	store, closeStore := liveStore(t)
	defer closeStore()
	pool := store.(*PoolStore).pool

	for _, testCase := range liveWrites() {
		t.Run(testCase.name, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			tx, err := pool.Begin(ctx)
			if err != nil {
				t.Fatalf("begin: %v", err)
			}
			// Always. The probe proves the statements run; it must not decide
			// what the database holds afterwards.
			defer func() { _ = tx.Rollback(ctx) }()

			handler := NewDispatchHandler(&rollbackStore{tx: tx})
			request, err := testCase.encode()
			if err != nil {
				t.Fatalf("encode request: %v", err)
			}
			attempts := testCase.repeat
			if attempts < 1 {
				attempts = 1
			}
			for attempt := range attempts {
				body, status := handler(invocation(testCase.stage), request)
				if status != bus.ModuleStatusOK {
					t.Fatalf("attempt %d: status = %v -- the statement did not run",
						attempt, status)
				}
				testCase.decoded(t, body)
			}
		})
	}
}

func TestLiveOperationsRunAgainstARealSchema(t *testing.T) {
	store, close := liveStore(t)
	defer close()
	handler := NewDispatchHandler(store)

	for _, testCase := range liveReads() {
		t.Run(testCase.name, func(t *testing.T) {
			request, err := testCase.encode()
			if err != nil {
				t.Fatalf("encode request: %v", err)
			}
			body, status := handler(invocation(testCase.stage), request)
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v -- the statement did not run", status)
			}
			testCase.decoded(t, body)
		})
	}
}

// liveExcluded names an operation that cannot be probed read-only, and why.
//
// An exclusion is a debt, not a dispensation: it says this implementation has
// never been run against a real schema. Naming it keeps that visible, where an
// unexplained gap in the coverage count would not be.
var liveExcluded = map[string]string{}

// Every registered operation is either probed above or named as excluded. A
// port that adds an implementation and neither gets one nothing has ever run
// against a database -- the state the C side spent a whole replay harness to
// get out of, and the reason that harness's own docstring calls it "the only
// test that proves a DB2 statement parses and runs".
func TestLiveCoversEveryPortedOperation(t *testing.T) {
	covered := len(liveReads()) + len(liveWrites()) + len(liveExcluded)
	if covered != Implemented() {
		t.Fatalf("%d operation(s) ported; %d read-probed, %d write-probed, %d excluded",
			Implemented(), len(liveReads()), len(liveWrites()), len(liveExcluded))
	}
}
