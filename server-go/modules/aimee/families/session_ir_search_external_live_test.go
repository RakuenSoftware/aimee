package families

import (
	"context"
	"encoding/json"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5/pgxpool"
	"os"
	"strings"
	"testing"
	"time"
)

func TestSessionIRLiveExternalSDKArchiveView(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	schema := fmt.Sprintf("sdk_archive_%d", time.Now().UnixNano())
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
	for _, name := range []string{"schema_sessions.sql", "schema_session_ir_events.sql", "schema_native_cache_recovery.sql", "schema_learning_projection_outbox.sql"} {
		sql, err := schemaFS.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = pool.Exec(ctx, string(sql)); err != nil {
			t.Fatal(err)
		}
	}
	if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal,client_type) VALUES('sdk','alice','claude-code'),('foreign','bob','claude-code'),('legacy','alice','claude-code')`); err != nil {
		t.Fatal(err)
	}
	if _, err = pool.Exec(ctx, `INSERT INTO primary_sessions(session_id,messages_json) VALUES('legacy','[{"role":"user","content":"SDK evidence"}]')`); err != nil {
		t.Fatal(err)
	}
	save := func(session, actor, checkpoint, scope, body string) uint32 {
		t.Helper()
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, _, err := sessionIRHistorySave(ctx, sessionIRLiveTx{tx}, []string{session, checkpoint, scope, actor, body})
		if err != nil {
			t.Fatal(err)
		}
		if status == store.StatusOK {
			if err = tx.Commit(ctx); err != nil {
				t.Fatal(err)
			}
		}
		return status
	}
	expectRevisionError := false
	expectedSourceError := ""
	lookup := func(actor string, request map[string]any) (uint32, map[string]any) {
		t.Helper()
		request["_principal"] = actor
		body, _ := json.Marshal(request)
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, cells, err := sessionIRSearch(ctx, sessionIRLiveTx{tx}, []string{actor, string(body)})
		if err != nil {
			if expectedSourceError != "" && err.Error() == expectedSourceError {
				return store.StatusInvalid, nil
			}
			if expectRevisionError && strings.Contains(err.Error(), "source revision changed") {
				return store.StatusInvalid, nil
			}
			t.Fatal(err)
		}
		if expectedSourceError != "" {
			t.Fatal("expected exact source-continuity refusal")
		}
		if expectRevisionError {
			t.Fatal("expected exact source-revision refusal")
		}
		result := map[string]any{}
		if len(cells) == 1 {
			if err = json.Unmarshal([]byte(cells[0]), &result); err != nil {
				t.Fatal(err)
			}
		}
		return status, result
	}
	archiveBytes, err := json.Marshal([]map[string]any{{"role": "assistant", "blocks": []map[string]any{
		{"type": "text", "text": "SDK evidence"},
		{"type": "thinking", "text": "secret reasoning"},
		{"type": "tool_use", "tool_id": "sdk-call", "name": "exec", "input": map[string]string{"secret": "private argument"}},
		{"type": "text", "text": "classified data", "context": &canonical.Context{ModelVisible: true, Sensitivity: 2}},
		{"type": "text", "text": "hidden data", "context": &canonical.Context{ModelVisible: false}},
	}}})
	if err != nil {
		t.Fatal(err)
	}
	archive := string(archiveBytes)
	if save("sdk", "alice", "sdk-source-1", externalSDKHistoryScope, archive) != store.StatusOK || save("foreign", "bob", "foreign-source", externalSDKHistoryScope, archive) != store.StatusOK {
		t.Fatal("archive rejected")
	}
	if save("sdk", "bob", "forged", externalSDKHistoryScope, archive) == store.StatusOK {
		t.Fatal("foreign archive writer")
	}
	if save("sdk", "alice", "host-source", "primary", `[{"role":"user","blocks":[{"type":"text","text":"host current"}]}]`) != store.StatusOK {
		t.Fatal("host history")
	}
	var before, after int
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id='sdk'`).Scan(&before); err != nil {
		t.Fatal(err)
	}
	if save("sdk", "alice", "sdk-source-1", externalSDKHistoryScope, archive) != store.StatusOK {
		t.Fatal("replay")
	}
	if err = pool.QueryRow(ctx, `SELECT count(*) FROM session_ir_events WHERE session_id='sdk'`).Scan(&after); err != nil || before != after {
		t.Fatal("duplicate archive events", before, after, err)
	}
	status, result := lookup("alice", map[string]any{"view": "external_sdk", "query": "SDK evidence"})
	if status != store.StatusOK || result["count"] != float64(1) {
		t.Fatal("owned archive absent or foreign/legacy source included", status, result)
	}
	sessions := result["sessions"].([]any)
	entry := sessions[0].(map[string]any)
	revision := entry["source_revision"].(string)
	if entry["view"] != "external_sdk" {
		t.Fatal(entry)
	}
	if wire, _ := json.Marshal(result); strings.Contains(string(wire), "secret reasoning") || strings.Contains(string(wire), "private argument") || strings.Contains(string(wire), "classified data") || strings.Contains(string(wire), "hidden data") {
		t.Fatal("private archive snippet", string(wire))
	}
	for _, view := range []string{"current", "observations"} {
		status, r := lookup("alice", map[string]any{"view": view, "query": "SDK evidence"})
		if status != store.StatusOK || r["count"] != float64(0) {
			t.Fatal("archive promoted to host view", view, status, r)
		}
	}
	for _, query := range []string{"secret reasoning", "private argument", "classified data", "hidden data", "host current"} {
		_, r := lookup("alice", map[string]any{"view": "external_sdk", "query": query})
		if r["count"] != float64(0) {
			t.Fatal("wrong source/privacy selection", query, r)
		}
	}
	expectRevisionError = true
	if status, _ = lookup("alice", map[string]any{"view": "current", "session_id": "sdk", "around_message_id": 0, "source_revision": revision}); status == store.StatusOK {
		t.Fatal("cross-view revision accepted")
	}
	expectRevisionError = false
	if save("sdk", "alice", "same-text-distinct-sdk-source", externalSDKHistoryScope, archive) != store.StatusOK || save("sdk", "alice", "newer-host-watermark", "primary", `[{"role":"user","blocks":[{"type":"text","text":"host current"}]}]`) != store.StatusOK {
		t.Fatal("independent equal-text source")
	}
	if save("sdk", "alice", "sdk-source-2", externalSDKHistoryScope, `[{"role":"user","blocks":[{"type":"text","text":"new SDK snapshot"}]}]`) != store.StatusOK {
		t.Fatal("new archive")
	}
	expectRevisionError = true
	if status, _ = lookup("alice", map[string]any{"view": "external_sdk", "session_id": "sdk", "around_message_id": 0, "source_revision": revision}); status == store.StatusOK {
		t.Fatal("stale revision accepted")
	}
	expectRevisionError = false
	_, result = lookup("alice", map[string]any{"view": "external_sdk", "query": "SDK evidence"})
	if result["count"] != float64(0) {
		t.Fatal("old snapshot merged", result)
	}
	if _, err = pool.Exec(ctx, `UPDATE server_sessions SET principal='bob' WHERE id='sdk'`); err != nil {
		t.Fatal(err)
	}
	_, result = lookup("alice", map[string]any{"view": "current", "query": "host current"})
	if result["count"] != float64(0) {
		t.Fatal("changed owner retained access", result)
	}
	// Source erasure is corruption, not permission to guess a new history.
	// Keep the successful snapshot/owner journey above ungapped; this distinct
	// session tests equal-text commit erasure and persistent fail-closed reads.
	if _, err = pool.Exec(ctx, `INSERT INTO server_sessions(id,principal,client_type) VALUES('gapped','alice','claude-code')`); err != nil {
		t.Fatal(err)
	}
	if save("gapped", "alice", "original", externalSDKHistoryScope, archive) != store.StatusOK ||
		save("gapped", "alice", "equal-text", externalSDKHistoryScope, archive) != store.StatusOK ||
		save("gapped", "alice", "host-after", "primary", `[{"role":"user","blocks":[{"type":"text","text":"host survives only with intact source"}]}]`) != store.StatusOK {
		t.Fatal("gapped source setup")
	}
	_, sameText := lookup("alice", map[string]any{"view": "external_sdk", "session_id": "gapped", "query": "SDK evidence"})
	sameTextRevision := sameText["sessions"].([]any)[0].(map[string]any)["source_revision"].(string)
	if _, err = pool.Exec(ctx, `DELETE FROM session_ir_events WHERE session_id='gapped' AND turn_id='equal-text'`); err != nil {
		t.Fatal(err)
	}
	var firstAfterGap string
	if err = pool.QueryRow(ctx, `SELECT sequence::text FROM (SELECT sequence,lag(sequence,1,0::bigint) OVER (ORDER BY sequence) previous FROM session_ir_events WHERE session_id='gapped') source WHERE sequence<>previous+1 ORDER BY sequence LIMIT 1`).Scan(&firstAfterGap); err != nil {
		t.Fatal(err)
	}
	expectedSourceError = "invalid canonical history source at sequence " + firstAfterGap
	for _, view := range []string{"external_sdk", "current"} {
		if status, _ = lookup("alice", map[string]any{"view": view, "session_id": "gapped"}); status == store.StatusOK {
			t.Fatal("gapped source admitted", view)
		}
	}
	if status, _ = lookup("alice", map[string]any{"view": "external_sdk", "session_id": "gapped", "around_message_id": 0, "source_revision": sameTextRevision}); status == store.StatusOK {
		t.Fatal("erased equal-text source retained revision")
	}
	if save("gapped", "alice", "later-snapshot", externalSDKHistoryScope, `[{"role":"user","blocks":[{"type":"text","text":"new bytes do not repair erased source"}]}]`) != store.StatusOK {
		t.Fatal("append after erased source")
	}
	if status, _ = lookup("alice", map[string]any{"view": "external_sdk", "session_id": "gapped"}); status == store.StatusOK {
		t.Fatal("later snapshot laundered source erasure")
	}

	// Removing every external archive row leaves no external search result,
	// but cannot silently repair the remaining primary source's sequence gap.
	if _, err = pool.Exec(ctx, `DELETE FROM session_ir_events WHERE session_id='gapped' AND payload::jsonb->'payload'->>'scope'=$1`, externalSDKHistoryScope); err != nil {
		t.Fatal(err)
	}
	expectedSourceError = ""
	_, result = lookup("alice", map[string]any{"view": "external_sdk", "session_id": "gapped"})
	if result["count"] != float64(0) {
		t.Fatal("erased external archive retained", result)
	}
	if err = pool.QueryRow(ctx, `SELECT sequence::text FROM session_ir_events WHERE session_id='gapped' ORDER BY sequence LIMIT 1`).Scan(&firstAfterGap); err != nil {
		t.Fatal(err)
	}
	expectedSourceError = "invalid canonical history source at sequence " + firstAfterGap
	if status, _ = lookup("alice", map[string]any{"view": "current", "session_id": "gapped"}); status == store.StatusOK {
		t.Fatal("archive erasure laundered primary sequence gap")
	}

}
