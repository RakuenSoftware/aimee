package families

import (
	"context"
	"crypto/sha256"
	"encoding/json"
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
	exec(`CREATE SCHEMA mr04_private_erasure; CREATE ROLE mr04_private_runtime NOINHERIT NOBYPASSRLS;
 GRANT USAGE ON SCHEMA mr04_private_erasure TO mr04_private_runtime;
 ALTER DEFAULT PRIVILEGES IN SCHEMA mr04_private_erasure GRANT SELECT,INSERT,UPDATE,DELETE ON TABLES TO mr04_private_runtime;
 ALTER DEFAULT PRIVILEGES IN SCHEMA mr04_private_erasure GRANT USAGE,SELECT ON SEQUENCES TO mr04_private_runtime;
 ALTER DEFAULT PRIVILEGES IN SCHEMA mr04_private_erasure GRANT EXECUTE ON FUNCTIONS TO mr04_private_runtime;
 SET LOCAL search_path=mr04_private_erasure,pg_catalog`)
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
	exec(`INSERT INTO delegation_spawns(delegation_id,session_id) VALUES('erased-delegation','erased-session');
 INSERT INTO delegation_messages(delegation_id,content) VALUES('erased-delegation','private delegated text');
 INSERT INTO agent_cache(role,prompt,result) VALUES('worker','private prompt','private unowned answer')`)
	exec(`SET LOCAL ROLE mr04_private_runtime`)
	erase := func() {
		t.Helper()
		status, cells, err := serverSessionEraseSubject(ctx, erasureReplayQueryer{traceReplayQueryer{tx}}, []string{"mr04-private-erase-0001", "erased-principal"})
		if err != nil || status != store.StatusOK || len(cells) != 1 || cells[0] != "1" {
			t.Fatal(status, cells, err)
		}
	}
	erase()
	status, cells, receiptErr := serverSessionErasureReceipt(ctx, erasureReplayQueryer{traceReplayQueryer{tx}}, []string{"mr04-private-erase-0001", "erased-principal"})
	if receiptErr != nil || status != store.StatusOK || len(cells) != 1 {
		t.Fatal("private completion receipt unavailable", status, cells, receiptErr)
	}
	var digests []string
	if json.Unmarshal([]byte(cells[0]), &digests) != nil || len(digests) != 1 || digests[0] != fmt.Sprintf("sha256:%x", sha256.Sum256([]byte("erased-session"))) {
		t.Fatal("receipt did not bind erased session set", cells)
	}
	wrong, _, receiptErr := serverSessionErasureReceipt(ctx, erasureReplayQueryer{traceReplayQueryer{tx}}, []string{"mr04-private-erase-0001", "retained-principal"})
	if receiptErr != nil || wrong != store.StatusInvalid {
		t.Fatal("receipt crossed subject boundary", wrong, receiptErr)
	}

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
		`INSERT INTO context_cache(hash,output,session_id) VALUES('late-cache','private cached text','erased-session')`,
		`INSERT INTO working_memory(session_id,key,value) VALUES('erased-session','late','private scratch')`,
		`INSERT INTO delegation_messages(delegation_id,content) VALUES('erased-delegation','late private payload')`,
		`INSERT INTO server_sessions(id,principal) VALUES('erased-session','erased-principal')`,
	} {
		exec(`SAVEPOINT restore_attempt`)
		_, err := tx.Exec(ctx, q)
		var pgerr *pgconn.PgError
		if !errors.As(err, &pgerr) || pgerr.Code != "23514" {
			t.Fatal("private restored copy admitted", err)
		}
		exec(`ROLLBACK TO restore_attempt; RELEASE restore_attempt`)
	}
	// A cache with no source/session observations cannot prove that a late fill
	// excludes erased text. Its documented privacy policy falls back to misses.
	exec(`INSERT INTO agent_cache(role,prompt,result) VALUES('worker','late private prompt','late private answer')`)
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM agent_cache`).Scan(&n); err != nil || n != 0 {
		t.Fatal("unowned cache retained erased data", n, err)
	}
	for _, table := range []string{"user_memory_erasure_intents", "user_memory_erasure_delegations", "db1_subject_erasure_request"} {
		exec(`SAVEPOINT control_acl`)
		_, err := tx.Exec(ctx, "DELETE FROM "+table)
		exec(`ROLLBACK TO control_acl; RELEASE control_acl`)
		var pgerr *pgconn.PgError
		if !errors.As(err, &pgerr) || pgerr.Code != "42501" {
			t.Fatal("runtime could erase control metadata", table, err)
		}
	}
	// Restore old content with triggers disabled, exactly as a content snapshot
	// loader can. Startup replay must run even with no pending migration version.
	exec(`RESET ROLE; ALTER TABLE user_memories DISABLE TRIGGER user_memory_erasure_restore_guard;
 ALTER TABLE context_cache DISABLE TRIGGER user_memory_erasure_payload_guard;
 ALTER TABLE delegation_messages DISABLE TRIGGER user_memory_erasure_payload_guard`)
	exec(`INSERT INTO user_memories(id,key,content,source_session) VALUES($1,'restored private snapshot','updated private secret','erased-session')`, erasedID)
	exec(`INSERT INTO context_cache(hash,output,session_id) VALUES('restored-cache','private secret','erased-session');
 INSERT INTO delegation_messages(delegation_id,content) VALUES('erased-delegation','restored delegated secret');
 ALTER TABLE user_memories ENABLE TRIGGER user_memory_erasure_restore_guard;
 ALTER TABLE context_cache ENABLE TRIGGER user_memory_erasure_payload_guard;
 ALTER TABLE delegation_messages ENABLE TRIGGER user_memory_erasure_payload_guard;
 SET LOCAL ROLE mr04_private_runtime`)
	removed, err := ReplayErasureIntents(ctx, erasureReplayQueryer{traceReplayQueryer{tx}})
	if err != nil || removed < 3 {
		t.Fatal("startup did not replay surviving intent", removed, err)
	}
	removed, err = ReplayErasureIntents(ctx, erasureReplayQueryer{traceReplayQueryer{tx}})
	if err != nil || removed != 0 {
		t.Fatal("startup replay is not idempotent", removed, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM user_memories WHERE key='unrelated private copy'`).Scan(&n); err != nil || n != 1 {
		t.Fatal("startup replay erased unrelated content", n, err)
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
