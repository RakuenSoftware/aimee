package families

import (
	"context"
	"errors"
	"fmt"
	"os"
	"testing"
	"time"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
)

func TestSubjectErasurePrivateCopiesPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB1_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_DB1_REPLAY_URL")
	}
	ctx := context.Background()
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
	exec := func(q string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, q, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`CREATE SCHEMA mr04_private_erasure; SET LOCAL search_path=mr04_private_erasure,pg_catalog`)
	migrations, err := Migrations()
	if err != nil {
		t.Fatal(err)
	}
	for _, m := range migrations {
		for _, sql := range m.Statements {
			exec(sql)
		}
	}
	exec(`INSERT INTO server_sessions(id,principal) VALUES('erased-session','erased-principal'),('retained-session','retained-principal')`)
	exec(`SELECT set_config('aimee.private_authority','user',true),set_config('aimee.private_principal','erased-principal',true)`)
	var erasedID int64
	if err := tx.QueryRow(ctx, `INSERT INTO user_memories(key,content,source_session) VALUES('private copy','private secret','erased-session') RETURNING id`).Scan(&erasedID); err != nil {
		t.Fatal(err)
	}
	exec(`UPDATE user_memories SET content='updated private secret' WHERE id=$1`, erasedID)
	exec(`SELECT set_config('aimee.private_principal','retained-principal',true)`)
	exec(`INSERT INTO user_memories(key,content,source_session) VALUES('unrelated private copy','retained secret','retained-session')`)
	exec(`INSERT INTO context_cache(hash,output,session_id) VALUES('mr04-secret-cache','private secret','erased-session')`)
	erase := func() {
		t.Helper()
		status, cells, err := serverSessionEraseSubject(ctx, erasureReplayQueryer{traceReplayQueryer{tx}}, []string{"mr04-private-erase-0001", "erased-principal"})
		if err != nil || status != store.StatusOK || len(cells) != 1 || cells[0] != "1" {
			t.Fatal(status, cells, err)
		}
	}
	erase()
	for _, table := range []string{"user_memories", "user_memory_versions"} {
		column := "id"
		if table == "user_memory_versions" {
			column = "memory_id"
		}
		var n int
		if err := tx.QueryRow(ctx, "SELECT count(*) FROM "+table+" WHERE "+column+"=$1", erasedID).Scan(&n); err != nil || n != 0 {
			t.Fatal("private retained copy", table, n, err)
		}
	}
	var n int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM context_cache WHERE session_id='erased-session'`).Scan(&n); err != nil || n != 0 {
		t.Fatal("erased prompt cache retained", n, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM user_memories WHERE key='unrelated private copy'`).Scan(&n); err != nil || n != 1 {
		t.Fatal("unrelated private memory erased", n, err)
	}
	erase()
	for _, q := range []string{
		`INSERT INTO user_memories(key,content) VALUES('restored snapshot','updated private secret')`,
		`INSERT INTO user_memories(key,content) VALUES('old revision snapshot','private secret')`,
		`INSERT INTO user_memories(key,content,source_session) VALUES('late derived copy','late copied payload','erased-session')`,
	} {
		exec(`SAVEPOINT restore_attempt`)
		_, err := tx.Exec(ctx, q)
		var pgerr *pgconn.PgError
		if !errors.As(err, &pgerr) || pgerr.Code != "23514" {
			t.Fatal("private restored copy admitted", err)
		}
		exec(`ROLLBACK TO restore_attempt; RELEASE restore_attempt`)
	}
}

type erasureReplayQueryer struct{ traceReplayQueryer }

func (q erasureReplayQueryer) QueryRow(ctx context.Context, sql string, args ...any) store.Row {
	return erasureReplayRow{q.Tx.QueryRow(ctx, sql, args...)}
}

type erasureReplayRow struct{ pgx.Row }

func (r erasureReplayRow) Scan(dest ...any) error {
	err := r.Row.Scan(dest...)
	if errors.Is(err, pgx.ErrNoRows) {
		return store.ErrNoRows
	}
	return err
}

func TestSubjectErasureRejectsOldSnapshotPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB1_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_DB1_REPLAY_URL")
	}
	ctx := context.Background()
	owner, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer owner.Close(ctx)
	schema := fmt.Sprintf("mr04_erasure_race_%d", time.Now().UnixNano())
	if _, err = owner.Exec(ctx, "CREATE SCHEMA "+schema); err != nil {
		t.Fatal(err)
	}
	defer func() {
		if _, err := owner.Exec(ctx, "DROP SCHEMA "+schema+" CASCADE"); err != nil {
			t.Error("cleanup", err)
		}
	}()
	setup, err := owner.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer setup.Rollback(ctx)
	if _, err = setup.Exec(ctx, "SET LOCAL search_path="+schema+",pg_catalog"); err != nil {
		t.Fatal(err)
	}
	migrations, err := Migrations()
	if err != nil {
		t.Fatal(err)
	}
	for _, m := range migrations {
		for _, sql := range m.Statements {
			if _, err = setup.Exec(ctx, sql); err != nil {
				t.Fatal(err)
			}
		}
	}
	if _, err = setup.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES('old-session','erased-principal'); INSERT INTO user_memories(key,content,source_session) VALUES('old-copy','old-content','old-session')`); err != nil {
		t.Fatal(err)
	}
	if err = setup.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	producer, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer producer.Close(ctx)
	old, err := producer.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.RepeatableRead})
	if err != nil {
		t.Fatal(err)
	}
	defer old.Rollback(ctx)
	if _, err = old.Exec(ctx, "SET LOCAL search_path="+schema+",pg_catalog; SELECT count(*) FROM user_memories"); err != nil {
		t.Fatal(err)
	}
	erase, err := owner.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer erase.Rollback(ctx)
	if _, err = erase.Exec(ctx, "SET LOCAL search_path="+schema+",pg_catalog"); err != nil {
		t.Fatal(err)
	}
	status, cells, err := serverSessionEraseSubject(ctx, erasureReplayQueryer{traceReplayQueryer{erase}}, []string{"mr04-durable-erase-0001", "erased-principal"})
	if err != nil || status != store.StatusOK || len(cells) != 1 || cells[0] != "1" {
		t.Fatal(status, cells, err)
	}
	if err = erase.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	// The old producer snapshot cannot see the newly committed intent. Locking
	// the advanced epoch must refuse it instead of letting it publish a copy.
	_, err = old.Exec(ctx, `INSERT INTO user_memories(key,content,source_session) VALUES('late-copy','different-derived-text','old-session')`)
	var pgerr *pgconn.PgError
	if !errors.As(err, &pgerr) || pgerr.Code != "40001" {
		t.Fatal("old snapshot bypassed erasure epoch", err)
	}
	if err = old.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	// A new connection observes the original journaled result after coordinator loss.
	retry, err := producer.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer retry.Rollback(ctx)
	if _, err = retry.Exec(ctx, "SET LOCAL search_path="+schema+",pg_catalog"); err != nil {
		t.Fatal(err)
	}
	status, cells, err = serverSessionEraseSubject(ctx, erasureReplayQueryer{traceReplayQueryer{retry}}, []string{"mr04-durable-erase-0001", "erased-principal"})
	if err != nil || status != store.StatusOK || len(cells) != 1 || cells[0] != "1" {
		t.Fatal("durable replay", status, cells, err)
	}
}
