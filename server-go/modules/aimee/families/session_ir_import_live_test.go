package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	wire "github.com/JBailes/aimee/server-go/aimee"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
	"github.com/jackc/pgx/v5/pgxpool"
	"os"
	"strings"
	"testing"
	"time"
	"unicode/utf8"
)

func TestSessionIRImportHostAuthority(t *testing.T) {
	db := newSessDB()
	f, _ := wire.EncodeFields(50, []string{"session", "primary", "actor", "session", "agent", strings.Repeat("a", 64), "[]", "{}", "model"})
	_, status := Sessions.Handler(db)(ring.ModuleInvocation{StageID: StageSessions, PrincipalRef: 7}, f)
	if status != ring.ModuleStatusInvalidRequest || len(db.executed) != 0 {
		t.Fatal(status)
	}
}
func TestSessionIRImportLiveBoundDurableFidelity(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
	defer cancel()
	schema := fmt.Sprintf("ir_import_%d", time.Now().UnixNano())
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
		sql, e := schemaFS.ReadFile(name)
		if e != nil {
			t.Fatal(e)
		}
		if _, e = pool.Exec(ctx, string(sql)); e != nil {
			t.Fatal(name, e)
		}
	}
	if _, e = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES ('target','actor'),('source','actor'),('foreign','other'),('archive','actor'),('refused','actor')`); e != nil {
		t.Fatal(e)
	}
	canonicalText := `[{"role":"user","blocks":[{"type":"text","text":"retained original"}]}]`
	archive := strings.Repeat("火\t\n", 260000) // >1MiB source, exact UTF8/control bytes.
	rejected := `not a native history`
	for _, r := range []struct{ sid, provider, raw string }{{"source", "anthropic", canonicalText}, {"foreign", "anthropic", canonicalText}, {"archive", "tmux-cli", archive}, {"refused", "anthropic", rejected}} {
		if _, e = pool.Exec(ctx, `INSERT INTO primary_sessions(session_id,agent_name,provider,messages_json) VALUES ($1,'agent',$2,$3)`, r.sid, r.provider, r.raw); e != nil {
			t.Fatal(e)
		}
	}
	makeReport := func(sid, provider, raw, format, outcome string) canonical.HistoryImportReport {
		sum := sha256.Sum256([]byte(raw))
		r := canonical.HistoryImportReport{Version: 1, SourceSession: sid, SourceAgent: "agent", SourceProvider: provider, SourceSHA256: hex.EncodeToString(sum[:]), SourceFormat: format, Outcome: outcome, ToolStructure: "unrecoverable"}
		if outcome == "exact" {
			r.SourceMessages = 1
			r.ImportedMessages = 1
			r.ToolStructure = "preserved"
		}
		if outcome == "archived" {
			for len(raw) > 0 {
				n := len(raw)
				if n > 65536 {
					n = 65536
					for !utf8.RuneStart(raw[n]) {
						n--
					}
				}
				r.ArchiveChunks++
				raw = raw[n:]
			}
		}
		return r
	}
	runMode := func(r canonical.HistoryImportReport, messages, mode string) (uint32, []string, error) {
		tx, e := pool.Begin(ctx)
		if e != nil {
			return 0, nil, e
		}
		defer tx.Rollback(ctx)
		encoded, _ := json.Marshal(r)
		s, disposition, e := sessionIRHistoryImport(ctx, sessionIRLiveTx{tx}, []string{"target", "primary", "actor", r.SourceSession, r.SourceAgent, r.SourceSHA256, messages, string(encoded), mode})
		if e == nil && s == store.StatusOK {
			e = tx.Commit(ctx)
		}
		return s, disposition, e
	}
	run := func(r canonical.HistoryImportReport, messages string) (uint32, error) {
		s, _, e := runMode(r, messages, "model")
		return s, e
	}

	appendCurrent := func(id, payload string) {
		t.Helper()
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, _, err := sessionIRAppend(ctx, sessionIRLiveTx{tx}, []string{"target", id, "current", "actor", payload})
		if err != nil || status != store.StatusOK {
			t.Fatal(status, err)
		}
		if err = tx.Commit(ctx); err != nil {
			t.Fatal(err)
		}
	}
	appendCurrent("current-running", `{"kind":"execution","payload":{"status":"running"}}`)
	appendCurrent("current-user", `{"kind":"message","message_id":"current-user","payload":{"role":"user","blocks":[{"type":"text","text":"new prompt before legacy base"}]}}`)
	report := makeReport("source", "anthropic", canonicalText, "canonical", "exact")
	if s, d, e := runMode(report, canonicalText, "browse"); e != nil || s != store.StatusOK || len(d) != 1 || d[0] != "imported" {
		t.Fatal("browse during running turn", s, d, e)
	}
	var count int
	if e = pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id='target'`).Scan(&count); e != nil || count != 5 {
		t.Fatal(count, e)
	}
	if s, e := run(report, canonicalText); e != nil || s != store.StatusOK {
		t.Fatal("idempotent lost ack retry", s, e)
	}
	var after int
	pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id='target'`).Scan(&after)
	if after != count {
		t.Fatal("duplicate migration", after, count)
	}

	appendCurrent("current-failed", `{"kind":"execution","payload":{"status":"failed","error":"model unavailable after import"}}`)
	tx, e := pool.Begin(ctx)
	if e != nil {
		t.Fatal(e)
	}
	s, cells, e := sessionIRHistoryLoad(ctx, sessionIRLiveTx{tx}, []string{"target", "primary", "actor", ""})
	tx.Rollback(ctx)
	if e != nil || s != store.StatusOK || len(cells) != 1 || (!strings.Contains(cells[0], "retained original") || !strings.Contains(cells[0], "new prompt before legacy base")) {
		t.Fatal("durable reload", s, cells, e)
	}
	bad := report
	bad.SourceSession = "foreign"
	if s, e := run(bad, canonicalText); e != nil || s != store.StatusInvalid {
		t.Fatal("foreign source", s, e)
	}
	bad = report
	bad.SourceSHA256 = strings.Repeat("0", 64)
	if s, e := run(bad, canonicalText); e != nil || s != store.StatusInvalid {
		t.Fatal("stale source", s, e)
	}
	if s, e := run(report, `[{"role":"user","blocks":[{"type":"text","text":"replacement"}]}]`); e != nil || s != store.StatusInvalid {
		t.Fatal("false exact report", s, e)
	}

	if _, e = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal) VALUES ('next','actor')`); e != nil {
		t.Fatal(e)
	}
	if _, e = pool.Exec(ctx, `INSERT INTO primary_sessions(session_id,agent_name,provider,messages_json) VALUES ('next','agent','anthropic',$1)`, canonicalText); e != nil {
		t.Fatal(e)
	}
	nextReport := makeReport("next", "anthropic", canonicalText, "canonical", "exact")
	var beforeBrowse, afterBrowse int
	pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id='target'`).Scan(&beforeBrowse)
	if s, d, e := runMode(nextReport, canonicalText, "browse"); e != nil || s != store.StatusOK || len(d) != 1 || d[0] != "skipped" {
		t.Fatal("browse must preserve authoritative history", s, d, e)
	}
	pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id='target'`).Scan(&afterBrowse)
	if beforeBrowse != afterBrowse {
		t.Fatal("browse skip appended", beforeBrowse, afterBrowse)
	}
	if s, e := run(nextReport, canonicalText); e != nil || s != store.StatusInvalid {
		t.Fatal("model import must remain strict", s, e)
	}
	invalidBrowse := nextReport
	invalidBrowse.SourceSHA256 = strings.Repeat("0", 64)
	if s, _, e := runMode(invalidBrowse, canonicalText, "browse"); e != nil || s != store.StatusInvalid {
		t.Fatal("browse skipped unvalidated source", s, e)
	}
	invalidBrowse = nextReport
	invalidBrowse.SourceSession = "foreign"
	if s, _, e := runMode(invalidBrowse, canonicalText, "browse"); e != nil || s != store.StatusInvalid {
		t.Fatal("browse skipped foreign source", s, e)
	}
	archived := makeReport("archive", "tmux-cli", archive, "tmux_text", "archived")
	if s, e := run(archived, "[]"); e != nil || s != store.StatusOK {
		t.Fatal("archive", s, e)
	}
	rows, e := pool.Query(ctx, `SELECT payload FROM session_ir_events WHERE session_id='target' AND payload::jsonb->'payload'->>'phase'='archive' ORDER BY session_ir_events.sequence`)
	if e != nil {
		t.Fatal(e)
	}
	var rebuilt strings.Builder
	index := 0
	for rows.Next() {
		var raw string
		rows.Scan(&raw)
		var event struct {
			Kind    string                 `json:"kind"`
			Payload canonical.HistoryEvent `json:"payload"`
		}
		if !canonical.Decode([]byte(raw), &event) || event.Payload.Index != index || event.Payload.ArchiveText == nil || !canonical.ValidHistoryEvent(json.RawMessage(mustPayload(t, raw))) {
			t.Fatal("invalid archival chunk", index)
		}
		rebuilt.WriteString(*event.Payload.ArchiveText)
		index++
	}
	rows.Close()
	if index != archived.ArchiveChunks || rebuilt.String() != archive {
		t.Fatal("lost archive bytes", index, archived.ArchiveChunks)
	}
	refused := makeReport("refused", "anthropic", rejected, "unknown", "refused")
	if s, e := run(refused, "[]"); e != nil || s != store.StatusOK {
		t.Fatal("durable refusal", s, e)
	}
	// Archive/refusal metadata cannot replace the one admitted model checkpoint.
	tx, e = pool.Begin(ctx)
	if e != nil {
		t.Fatal(e)
	}
	s, cells, e = sessionIRHistoryLoad(ctx, sessionIRLiveTx{tx}, []string{"target", "primary", "actor", ""})
	tx.Rollback(ctx)
	if e != nil || s != store.StatusOK || strings.Contains(cells[0], "火") || !strings.Contains(cells[0], "retained original") {
		t.Fatal(s, cells, e)
	}
	var retained string
	pool.QueryRow(ctx, `SELECT messages_json FROM primary_sessions WHERE session_id='archive'`).Scan(&retained)
	if retained != archive {
		t.Fatal("legacy archive erased before/after ack")
	}
	// Ordinary append cannot forge a host migration receipt/archive.
	event, _ := json.Marshal(struct {
		Kind    string                 `json:"kind"`
		Payload canonical.HistoryEvent `json:"payload"`
	}{"history", canonical.HistoryEvent{Scope: "primary", Phase: "import_report", ImportReport: &report}})
	tx, e = pool.Begin(ctx)
	if e != nil {
		t.Fatal(e)
	}
	s, _, e = sessionIRAppend(ctx, sessionIRLiveTx{tx}, []string{"target", "forged", "forged", "actor", string(event)})
	tx.Rollback(ctx)
	if e != nil || s != store.StatusInvalid {
		t.Fatal("forged report", s, e)
	}
}
func mustPayload(t *testing.T, raw string) string {
	t.Helper()
	var e struct {
		Payload json.RawMessage `json:"payload"`
	}
	if json.Unmarshal([]byte(raw), &e) != nil {
		t.Fatal("payload")
	}
	return string(e.Payload)
}
