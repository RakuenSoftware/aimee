package families

import (
	"context"
	"encoding/json"
	"fmt"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
	"os"
	"strings"
	"testing"
	"time"
)

func TestSessionPrimaryLiveDurableOwnerSelection(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	schema := fmt.Sprintf("primary_selection_%d", time.Now().UnixNano())
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
	defer func() { pool.Close() }()
	sql, e := schemaFS.ReadFile("schema_sessions.sql")
	if e != nil {
		t.Fatal(e)
	}
	if _, e = pool.Exec(ctx, string(sql)); e != nil {
		t.Fatal(e)
	}
	if _, e = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal,metadata) VALUES ('session','actor','{"other":{"keep":true}}'),('foreign','other','{}'),('unowned','','{}')`); e != nil {
		t.Fatal(e)
	}
	run := func(set bool, f ...string) (uint32, []string, error) {
		tx, e := pool.Begin(ctx)
		if e != nil {
			return 0, nil, e
		}
		defer tx.Rollback(ctx)
		fn := sessionPrimaryGet
		if set {
			fn = sessionPrimarySet
		}
		s, c, e := fn(ctx, sessionIRLiveTx{tx}, f)
		if e == nil && s == store.StatusOK {
			e = tx.Commit(ctx)
		}
		return s, c, e
	}
	get := func(w string) {
		t.Helper()
		s, c, e := run(false, "actor", "session")
		if e != nil || s != store.StatusOK || len(c) != 1 || c[0] != w {
			t.Fatal(s, c, e, w)
		}
	}
	get("")
	for _, a := range []string{"codex-pinned", "claude-pinned", "codex-pinned"} {
		s, _, e := run(true, "actor", "session", a)
		if e != nil || s != store.StatusOK {
			t.Fatal(s, e)
		}
		get(a)
	}
	pool.Close()
	pool, e = pgxpool.NewWithConfig(ctx, cfg)
	if e != nil {
		t.Fatal(e)
	}
	get("codex-pinned")
	longActor := strings.Repeat("p", 159)
	if _, err := pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES ('long-owner',$1)`, longActor); err != nil {
		t.Fatal(err)
	}
	if st, _, err := run(true, longActor, "long-owner", "codex"); err != nil || st != store.StatusOK {
		t.Fatal(st, err)
	}
	if st, cells, err := run(false, longActor, "long-owner"); err != nil || st != store.StatusOK || len(cells) != 1 || cells[0] != "codex" {
		t.Fatal(st, cells, err)
	}
	if st, _, err := run(false, "foreign", "long-owner"); err != nil || st != store.StatusInvalid {
		t.Fatal(st, err)
	}

	for _, sid := range []string{"foreign", "missing", "unowned"} {
		for _, set := range []bool{false, true} {
			f := []string{"actor", sid}
			if set {
				f = append(f, "claude")
			}
			s, _, e := run(set, f...)
			if e != nil || s != store.StatusInvalid {
				t.Fatal(sid, set, s, e)
			}
		}
	}
	s, _, e := run(true, "actor", "session", "")
	if e != nil || s != store.StatusOK {
		t.Fatal(s, e)
	}
	get("")
	var raw string
	if e = pool.QueryRow(ctx, `SELECT metadata FROM server_sessions WHERE id='session'`).Scan(&raw); e != nil {
		t.Fatal(e)
	}
	var m map[string]any
	if json.Unmarshal([]byte(raw), &m) != nil || m["other"] == nil || m["primary_agent"] != nil {
		t.Fatal(raw)
	}
	tx, e := pool.Begin(ctx)
	if e != nil {
		t.Fatal(e)
	}
	s, _, e = sessionPrimarySet(ctx, sessionIRLiveTx{tx}, []string{"actor", "session", "rolled-back"})
	if e != nil || s != store.StatusOK {
		t.Fatal(s, e)
	}
	if e = tx.Rollback(ctx); e != nil {
		t.Fatal(e)
	}
	get("")
	if _, e = pool.Exec(ctx, `UPDATE server_sessions SET metadata='not-json' WHERE id='session'`); e != nil {
		t.Fatal(e)
	}
	for _, set := range []bool{false, true} {
		f := []string{"actor", "session"}
		if set {
			f = append(f, "")
		}
		s, _, e := run(set, f...)
		if e != nil || s != store.StatusInvalid {
			t.Fatal(set, s, e)
		}
	}
}
