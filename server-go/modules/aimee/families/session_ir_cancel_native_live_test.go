package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"reflect"
	"strings"
	"testing"
	"time"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
)

// These are PostgreSQL custody rows, not native process or MAC attestation.
func cancelNativeLivePool(t *testing.T) (*pgxpool.Pool, context.Context) {
	t.Helper()
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	t.Cleanup(cancel)
	schema := fmt.Sprintf("cancel_native_%d", time.Now().UnixNano())
	if _, err := root.Exec(ctx, "CREATE SCHEMA "+schema); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		_, err := root.Exec(context.Background(), "DROP SCHEMA "+schema+" CASCADE")
		if err != nil {
			t.Error(err)
		}
	})
	cfg, err := pgxpool.ParseConfig(os.Getenv("AIMEE_TEST_PG_URL"))
	if err != nil {
		t.Fatal(err)
	}
	cfg.ConnConfig.RuntimeParams["search_path"] = schema
	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(pool.Close)
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_native_cache_recovery.sql", "schema_native_cache_reservations.sql", "schema_learning_projection_outbox.sql", "schema_governed_actions.sql"} {
		raw, err := schemaFS.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(ctx, string(raw)); err != nil {
			t.Fatal(err)
		}
	}
	return pool, ctx
}

func cancelNativeSeed(t *testing.T, ctx context.Context, pool *pgxpool.Pool, session string) {
	t.Helper()
	if _, err := pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES($1,'actor');`, session); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO session_ir_events(session_id,sequence,event_id,turn_id,actor,payload) VALUES($1,1,'running','turn','actor','{"kind":"execution","payload":{"status":"running"}}')`, session); err != nil {
		t.Fatal(err)
	}
}

func TestSessionIRCancelNativeLiveExactClassification(t *testing.T) {
	pool, ctx := cancelNativeLivePool(t)
	cases := []struct {
		name, reservationActor, reservationTurn, cacheActor, cacheTurn, requestActor, requestTurn string
		closed, storage, native                                                                   bool
	}{
		{name: "exact reservation", reservationActor: "actor", reservationTurn: "turn", native: true},
		{name: "exact unretired cache", cacheActor: "actor", cacheTurn: "turn", native: true},
		{name: "both exact", reservationActor: "actor", reservationTurn: "turn", cacheActor: "actor", cacheTurn: "turn", native: true},
		{name: "other turn", reservationActor: "actor", reservationTurn: "old"},
		{name: "foreign reservation", reservationActor: "foreign", reservationTurn: "turn"},
		{name: "mixed cache", reservationActor: "actor", reservationTurn: "turn", cacheActor: "actor", cacheTurn: "old"},
		{name: "mixed reservation", reservationActor: "actor", reservationTurn: "old", cacheActor: "actor", cacheTurn: "turn"},
		{name: "foreign cache", cacheActor: "foreign", cacheTurn: "turn"},
		{name: "foreign caller", reservationActor: "actor", reservationTurn: "turn", requestActor: "foreign"},
		{name: "stale requested turn", reservationActor: "actor", reservationTurn: "turn", requestTurn: "old"},
		{name: "unbound reservation", reservationActor: "actor", reservationTurn: ""},
		{name: "closed turn", reservationActor: "actor", reservationTurn: "turn", closed: true},
		{name: "storage unavailable", storage: true},
		{name: "HTTP no native custody"},
	}
	for i, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			session := fmt.Sprint("s", i)
			cancelNativeSeed(t, ctx, pool, session)
			tx, err := pool.Begin(ctx)
			if err != nil {
				t.Fatal(err)
			}
			defer tx.Rollback(ctx)
			if tc.reservationActor != "" {
				_, err = tx.Exec(ctx, `INSERT INTO session_native_reservations(session_id,harness,binding_sha256,owner_id,turn_id,actor,epoch,containment_sha256,containment_json) VALUES($1,'claude','binding','owner',$2,$3,'0','containment','{}')`, session, tc.reservationTurn, tc.reservationActor)
				if err != nil {
					t.Fatal(err)
				}
			}
			if tc.cacheActor != "" {
				_, err = tx.Exec(ctx, `INSERT INTO session_native_cache_owners(session_id,cache_key,actor,turn_id,harness,scope_sha256,context_sha256,binding_sha256,owner_id,claim_id,containment_sha256,cache_epoch) VALUES($1,'cache',$2,$3,'claude','scope','context','binding','owner','claim','containment','0')`, session, tc.cacheActor, tc.cacheTurn)
				if err != nil {
					t.Fatal(err)
				}
			}
			if tc.closed {
				_, err = tx.Exec(ctx, `INSERT INTO session_ir_events(session_id,sequence,event_id,turn_id,actor,payload) VALUES($1,2,'done','turn','actor','{"kind":"execution","payload":{"status":"completed"}}')`, session)
				if err != nil {
					t.Fatal(err)
				}
			}
			if tc.storage {
				if _, err = tx.Exec(ctx, `DROP TABLE session_native_reservations`); err != nil {
					t.Fatal(err)
				}
			}
			actor, turn := tc.requestActor, tc.requestTurn
			if actor == "" {
				actor = "actor"
			}
			if turn == "" {
				turn = "turn"
			}
			status, cells, err := sessionIRCancel(ctx, sessionIRLiveTx{tx}, []string{actor, session, turn})
			if tc.storage {
				if err == nil || len(cells) != 0 {
					t.Fatalf("storage failure classified: %d %v %v", status, cells, err)
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			if tc.native {
				if status != store.StatusInvalid || !reflect.DeepEqual(cells, []string{"native-active"}) {
					t.Fatalf("exact native classification: %d %v", status, cells)
				}
			} else if tc.name == "HTTP no native custody" {
				if status != store.StatusOK || len(cells) != 1 || cells[0] != "2" {
					t.Fatalf("HTTP terminal: %d %v", status, cells)
				}
			} else if status != store.StatusInvalid || len(cells) != 0 {
				t.Fatalf("unrelated custody became native-active: %d %v", status, cells)
			}
			var n int
			if err = tx.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id=$1 AND event_id LIKE 'operator-cancel:%'`, session).Scan(&n); err != nil {
				t.Fatal(err)
			}
			want := 0
			if tc.name == "HTTP no native custody" {
				want = 1
			}
			if n != want {
				t.Fatalf("unexpected durable terminal writes: %d", n)
			}
		})
	}
}

func TestSessionIRCancelNativeLiveReserveOrdering(t *testing.T) {
	pool, ctx := cancelNativeLivePool(t)
	for _, reserveFirst := range []bool{true, false} {
		t.Run(fmt.Sprint("reserve-first=", reserveFirst), func(t *testing.T) {
			session := fmt.Sprint("ordered-", reserveFirst)
			cancelNativeSeed(t, ctx, pool, session)
			descriptor := `{"kind":"metadata-fixture-not-execution-attestation"}`
			sum := sha256.Sum256([]byte(descriptor))
			raw, _ := json.Marshal(nativeReservationRequest{Version: 1, Session: session, Turn: "turn", Harness: "claude", Binding: strings.Repeat("b", 64), Owner: strings.Repeat("a", 32), Containment: hex.EncodeToString(sum[:]), Descriptor: descriptor})
			reserve := func(q store.Queryer) (uint32, []string, error) {
				return sessionNativeReserve(ctx, q, []string{"actor", string(raw)})
			}
			cancel := func(q store.Queryer) (uint32, []string, error) {
				return sessionIRCancel(ctx, q, []string{"actor", session, "turn"})
			}
			first, second := reserve, cancel
			if !reserveFirst {
				first, second = cancel, reserve
			}
			held, err := pool.Begin(ctx)
			if err != nil {
				t.Fatal(err)
			}
			defer held.Rollback(ctx)
			status, _, err := first(sessionIRLiveTx{held})
			if err != nil || status != store.StatusOK {
				t.Fatalf("first: %d %v", status, err)
			}
			type result struct {
				status uint32
				cells  []string
				err    error
			}
			done := make(chan result, 1)
			ready := make(chan uint32, 1)
			go func() {
				waiting, err := pool.Begin(ctx)
				if err != nil {
					done <- result{err: err}
					ready <- 0
					return
				}
				defer waiting.Rollback(ctx)
				ready <- waiting.Conn().PgConn().PID()
				s, c, e := second(sessionIRLiveTx{waiting})
				if e == nil {
					e = waiting.Commit(ctx)
				}
				done <- result{s, c, e}
			}()
			pid := <-ready
			// Observe PostgreSQL's lock wait, not merely an elapsed goroutine.
			deadline := time.Now().Add(2 * time.Second)
			blocked := false
			for time.Now().Before(deadline) {
				select {
				case got := <-done:
					t.Fatalf("second operation bypassed shared row lock: %+v", got)
				default:
				}
				if err = pool.QueryRow(ctx, `SELECT coalesce(wait_event_type='Lock',false) FROM pg_stat_activity WHERE pid=$1`, pid).Scan(&blocked); err != nil {
					t.Fatal(err)
				}
				if blocked {
					break
				}
				time.Sleep(5 * time.Millisecond)
			}
			if !blocked {
				t.Fatal("second operation never reached a PostgreSQL lock wait")
			}
			if err = held.Commit(ctx); err != nil {
				t.Fatal(err)
			}
			got := <-done
			if got.err != nil || got.status != store.StatusInvalid {
				t.Fatalf("second: %+v", got)
			}
			if reserveFirst && !reflect.DeepEqual(got.cells, []string{"native-active"}) {
				t.Fatalf("reservation lost exact native classification: %v", got.cells)
			}
			if !reserveFirst && len(got.cells) != 0 {
				t.Fatalf("cancelled reservation acquired native authority: %v", got.cells)
			}
			var reservations, terminals int
			if err = pool.QueryRow(ctx, `SELECT count(*) FROM session_native_reservations WHERE session_id=$1`, session).Scan(&reservations); err != nil {
				t.Fatal(err)
			}
			if err = pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id=$1 AND event_id='operator-cancel:turn'`, session).Scan(&terminals); err != nil {
				t.Fatal(err)
			}
			if reserveFirst {
				if reservations != 1 || terminals != 0 {
					t.Fatal(reservations, terminals)
				}
			} else if reservations != 0 || terminals != 1 {
				t.Fatal(reservations, terminals)
			}
		})
	}
}
