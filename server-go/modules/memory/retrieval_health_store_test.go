package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"github.com/JBailes/aimee/server-go/bus"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	store "github.com/JBailes/aimee/server-go/db"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

type healthPoolDB struct{ pool *pgxpool.Pool }

func (d healthPoolDB) Exec(ctx context.Context, q string, args ...any) (store.Tag, error) {
	return d.pool.Exec(ctx, q, args...)
}
func (d healthPoolDB) Query(ctx context.Context, q string, args ...any) (store.Rows, error) {
	return d.pool.Query(ctx, q, args...)
}
func (d healthPoolDB) QueryRow(ctx context.Context, q string, args ...any) store.Row {
	return evalRow{d.pool.QueryRow(ctx, q, args...)}
}
func (d healthPoolDB) Begin(ctx context.Context) (store.Tx, error) {
	tx, err := d.pool.Begin(ctx)
	if err != nil {
		return nil, err
	}
	return evalQueryer{tx}, nil
}

func TestHealthPostgresConcurrentOwnerAndRestart(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("requires isolated PostgreSQL")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	defer cancel()
	admin, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer admin.Close(context.Background())
	name := fmt.Sprintf("health_owner_%d", time.Now().UnixNano())
	quoted := pgx.Identifier{name}.Sanitize()
	_, err = admin.Exec(ctx, `CREATE ROLE `+quoted+` NOINHERIT NOBYPASSRLS; CREATE SCHEMA `+quoted+`;
 SET search_path=`+quoted+`,public;
 CREATE TABLE user_memory_collection_generation(id integer PRIMARY KEY,owner_id uuid NOT NULL);
 INSERT INTO user_memory_collection_generation VALUES(1,'00000000-0000-0000-0000-000000000008');`+healthStoreSchema+`;
 GRANT USAGE ON SCHEMA `+quoted+` TO `+quoted+`;
 GRANT SELECT,INSERT,UPDATE,DELETE ON ALL TABLES IN SCHEMA `+quoted+` TO `+quoted)
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if _, err := admin.Exec(context.Background(), "DROP SCHEMA "+quoted+" CASCADE; DROP ROLE "+quoted); err != nil {
			t.Error(err)
		}
	}()
	cfg, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		t.Fatal("invalid test pool configuration")
	}
	cfg.MaxConns = 4
	cfg.AfterConnect = func(ctx context.Context, c *pgx.Conn) error {
		_, err := c.Exec(ctx, "SET ROLE "+quoted+"; SET search_path="+quoted+",public")
		return err
	}
	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer pool.Close()
	var restricted bool
	err = pool.QueryRow(ctx, `SELECT NOT rolsuper AND NOT rolbypassrls AND current_user<>pg_get_userbyid(
 (SELECT relowner FROM pg_class WHERE oid='memory_retrieval_health_namespaces'::regclass)) FROM pg_roles WHERE rolname=current_user`).Scan(&restricted)
	if err != nil || !restricted {
		t.Fatal("fixture must use a real non-owner runtime role", err)
	}
	owner := func() *postgresDataStore {
		d, err := NewPostgresDataStore(healthPoolDB{pool}, PlacementServer)
		if err != nil {
			t.Fatal(err)
		}
		s := d.(*postgresDataStore)
		s.health.ready = true
		return s
	}
	s := owner()
	p, events := healthFixture()
	now := time.Now().UTC()
	p.From, p.Until = now, now.Add(time.Second)
	p.Namespace = "00000000-0000-0000-0000-000000000008"
	event := events[0]
	event.Namespace, event.At = p.Namespace, now
	const count = 24
	errs := make(chan error, count)
	var workers sync.WaitGroup
	for i := 0; i < count; i++ {
		workers.Add(1)
		go func(i int) {
			defer workers.Done()
			e := event
			e.Attempt = fmt.Sprint(i)
			for _, ack := range []bool{false, true, true} {
				_, err := s.updateHealthStore(ctx, p.Principal, p.Project, p.Workspace, now, func(j *healthJournal) error {
					return j.apply(healthSnapshot{Invocation: e, PreparedSequence: uint64(i + 1), Admitted: true, Acknowledged: ack}, now)
				})
				if err != nil {
					errs <- err
					return
				}
			}
		}(i)
	}
	workers.Wait()
	close(errs)
	for err := range errs {
		t.Fatal(err)
	}
	// A new owner object has no process-local event state or private key cache.
	restarted := owner()
	j, err := restarted.readHealthStore(ctx, p.Principal, p.Project, p.Workspace)
	if err != nil {
		t.Fatal(err)
	}
	r, err := j.report(p, p.Until)
	if err != nil || r.Attempts["dispatched"] != count || r.Metrics.Invocations != count || r.Records["dispatched"] != 2*count {
		t.Fatal(r, err)
	}
	key := healthQueryFingerprint(j.Key, j.Namespace, "yes")
	if _, err = s.updateHealthStore(ctx, p.Principal, p.Project, p.Workspace, now, func(j *healthJournal) error {
		j.Attempts = map[string]healthStoredAttempt{}
		return errors.New("rollback fixture")
	}); err == nil {
		t.Fatal("failed update committed")
	}
	j, err = restarted.readHealthStore(ctx, p.Principal, p.Project, p.Workspace)
	if err != nil || len(j.Attempts) != count || healthQueryFingerprint(j.Key, j.Namespace, "yes") != key {
		t.Fatal("rollback or restart changed journal/key", err)
	}
	if _, err = restarted.readHealthStore(ctx, "bob", p.Project, p.Workspace); !store.IsNoRows(err) {
		t.Fatal("another principal discovered the journal", err)
	}
	if _, err = restarted.readHealthStore(ctx, p.Principal, "other", p.Workspace); !store.IsNoRows(err) {
		t.Fatal("another project discovered the journal", err)
	}

	// Exercise the actual private receipt importer and report against the same
	// non-owner PostgreSQL capability, including an owner object restart.
	t.Setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1")
	release := &sourceReleaseState{}
	receiptArgs := receiptTestAdmission(t, release)
	receiptArgs["dispatch_owner"], _ = json.Marshal(strings.Repeat("a", 32))
	plan := sourceReleaseCall(t, release, receiptArgs)
	var prepared providerReceiptEvent
	if err := json.Unmarshal([]byte(plan["prepared_detail"].(string)), &prepared); err != nil {
		t.Fatal(err)
	}
	rows := []map[string]any{}
	appendRow := func(detail string) {
		var event providerReceiptEvent
		if json.Unmarshal([]byte(detail), &event) != nil {
			t.Fatal("invalid fixture event")
		}
		rows = append(rows, map[string]any{"sequence": fmt.Sprint(len(rows) + 1), "action": "memory.provider." + event.Stage, "attempt_id": event.AttemptID, "detail": detail, "row_hash": strings.Repeat("f", 64)})
	}
	appendRow(plan["prepared_detail"].(string))
	appendRow(plan["admitted_detail"].(string))
	call := func(owner *postgresDataStore, args commandArgs) map[string]any {
		raw, status := handleRetrievalHealth(handlerOptions{placement: PlacementServer, data: owner}, bus.ModuleInvocation{}, args)
		body, err := bus.DecodeCommandResult(raw)
		var result map[string]any
		if status != bus.ModuleStatusOK || err != nil || json.Unmarshal(body, &result) != nil || result["status"] != "ok" {
			t.Fatalf("health command: %s (%v, %v)", body, status, err)
		}
		return result
	}
	importArgs := sourceReleaseArgs(map[string]any{"operation": "health-import", "health_principal": "receipt-owner", "receipt_request_id": "request", "dispatch_owner": strings.Repeat("b", 32), "chain_intact": true, "ledger_events": rows})
	call(owner(), importArgs)
	reportArgs := sourceReleaseArgs(map[string]any{"operation": "health-report", "health_principal": "receipt-owner", "project": "private", "window": "1h"})
	result := call(owner(), reportArgs)
	health := result["health"].(map[string]any)
	if health["exact_retained_attempts_by_stage"].(map[string]any)["unknown_dispatch"] != float64(1) {
		t.Fatal(result)
	}
	ack, _ := json.Marshal(providerReceiptEvent{SchemaVersion: 1, Stage: "acknowledged", AttemptID: prepared.AttemptID, BindingDigest: prepared.BindingDigest, HTTPStatus: 200})
	appendRow(string(ack))
	importArgs["ledger_events"], _ = json.Marshal(rows)
	call(owner(), importArgs)
	call(owner(), importArgs)
	result = call(owner(), reportArgs)
	health = result["health"].(map[string]any)
	if health["exact_retained_attempts_by_stage"].(map[string]any)["dispatched"] != float64(1) || health["window_complete"] != false {
		t.Fatal(result)
	}
	public, _ := json.Marshal(result)
	if strings.Contains(string(public), prepared.AttemptID) || strings.Contains(string(public), releaseTestRef().ID) {
		t.Fatal("private receipt identity in aggregate")
	}
	foreign := sourceReleaseArgs(map[string]any{"operation": "health-report", "health_principal": "foreign", "project": "private", "window": "1h"})
	if absent := call(owner(), foreign); absent["collection_state"] != "not_collected" {
		t.Fatal(absent)
	}

	// The per-principal quota is checked under the same transaction lock.
	for i := 1; i < 128; i++ {
		if _, err = s.updateHealthStore(ctx, p.Principal, fmt.Sprint(i), p.Workspace, now, func(*healthJournal) error { return nil }); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = s.updateHealthStore(ctx, p.Principal, "overflow", p.Workspace, now, func(*healthJournal) error { return nil }); err == nil {
		t.Fatal("unbounded health namespaces")
	}
}
