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
			if expectRevisionError && strings.Contains(err.Error(), "source revision changed") {
				return store.StatusInvalid, nil
			}
			t.Fatal(err)
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
	archive := `[{"role":"assistant","blocks":[{"type":"text","text":"SDK evidence"},{"type":"thinking","text":"secret reasoning"},{"type":"tool_use","tool_id":"sdk-call","name":"exec","input":{"secret":"private argument"}},{"type":"text","text":"classified data","context":{"model_visible":true,"sensitivity":2}},{"type":"text","text":"hidden data","context":{"model_visible":false}}]}]`
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
	_, sameText := lookup("alice", map[string]any{"view": "external_sdk", "query": "SDK evidence"})
	sameTextRevision := sameText["sessions"].([]any)[0].(map[string]any)["source_revision"].(string)
	if _, err = pool.Exec(ctx, `DELETE FROM session_ir_events WHERE session_id='sdk' AND turn_id='same-text-distinct-sdk-source'`); err != nil {
		t.Fatal(err)
	}
	// Display bytes and the unrelated global host watermark are unchanged;
	// loss of the selected external commit must still revoke its revision.
	expectRevisionError = true
	if status, _ = lookup("alice", map[string]any{"view": "external_sdk", "session_id": "sdk", "around_message_id": 0, "source_revision": sameTextRevision}); status == store.StatusOK {
		t.Fatal("equal-text erased external source retained revision")
	}
	expectRevisionError = false
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
	// Erase the complete derived archive while retaining independent host history.
	if _, err = pool.Exec(ctx, `DELETE FROM session_ir_events WHERE session_id='sdk' AND payload::jsonb->'payload'->>'scope'=$1`, externalSDKHistoryScope); err != nil {
		t.Fatal(err)
	}
	_, result = lookup("alice", map[string]any{"view": "external_sdk", "query": "new SDK snapshot"})
	if result["count"] != float64(0) {
		t.Fatal("erased source retained", result)
	}
	_, result = lookup("alice", map[string]any{"view": "current", "query": "host current"})
	if result["count"] != float64(1) {
		t.Fatal("archive touched host primary", result)
	}
	if _, err = pool.Exec(ctx, `UPDATE server_sessions SET principal='bob' WHERE id='sdk'`); err != nil {
		t.Fatal(err)
	}
	_, result = lookup("alice", map[string]any{"view": "current", "query": "host current"})
	if result["count"] != float64(0) {
		t.Fatal("changed owner retained access", result)
	}
}
