package db2

import (
	"context"
	"os"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
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
// against a schema with no rows in it. Every entry is a READ or an idempotent
// probe: this runs against a shared database and must not leave anything in it.
type liveRequest struct {
	name    string
	stage   uint32
	encode  func() ([]byte, error)
	decoded func(t *testing.T, body []byte)
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
var liveExcluded = map[string]string{
	"prospective_insert": "writes a row, and this may run against a shared " +
		"database; it needs the replay harness, which drops and rebuilds",
}

// Every registered operation is either probed above or named as excluded. A
// port that adds an implementation and neither gets one nothing has ever run
// against a database -- the state the C side spent a whole replay harness to
// get out of, and the reason that harness's own docstring calls it "the only
// test that proves a DB2 statement parses and runs".
func TestLiveCoversEveryPortedOperation(t *testing.T) {
	covered := len(liveReads()) + len(liveExcluded)
	if covered != Implemented() {
		t.Fatalf("%d operation(s) ported; %d probed, %d excluded",
			Implemented(), len(liveReads()), len(liveExcluded))
	}
}
