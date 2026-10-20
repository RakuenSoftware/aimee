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

func TestSessionIRLiveSearchCanonicalOnlyAndOwnerScoped(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	schema := fmt.Sprintf("session_search_%d", time.Now().UnixNano())
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
	for _, row := range [][3]string{{"alice-session", "alice", "browser"}, {"bob-session", "bob", "browser"}, {"unowned", "", "browser"}, {"delegate-session", "alice", "delegate"}, {"legacy-only", "alice", "browser"}} {
		if _, err := pool.Exec(ctx, `INSERT INTO server_sessions(id,principal,client_type) VALUES($1,$2,$3)`, row[0], row[1], row[2]); err != nil {
			t.Fatal(err)
		}
	}
	appendMessage := func(session, actor, id, text string) {
		t.Helper()
		encoded, _ := json.Marshal(map[string]any{"kind": "message", "message_id": "same", "payload": map[string]any{"role": "assistant", "blocks": []map[string]any{{"type": "text", "text": text}, {"type": "text", "text": "second block"}, {"type": "thinking", "text": "opaque-secret"}, {"type": "tool_use", "tool_id": "tool", "name": "exec", "input": map[string]string{"value": "tool-secret"}}}}})
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, _, err := sessionIRAppend(ctx, sessionIRLiveTx{tx}, []string{session, id, "turn", actor, string(encoded)})
		if err != nil || status != store.StatusOK {
			t.Fatal(status, err)
		}
		if err = tx.Commit(ctx); err != nil {
			t.Fatal(err)
		}
	}
	appendMessage("alice-session", "alice", "first", "obsolete")
	appendMessage("alice-session", "alice", "replacement", "needle 10%")
	appendMessage("bob-session", "bob", "bob", "needle foreign")
	appendMessage("delegate-session", "alice", "delegate", "needle delegate")
	if _, err := pool.Exec(ctx, `INSERT INTO primary_sessions(session_id,messages_json) VALUES('unowned','[{"role":"user","content":"needle"}]'),('legacy-only','[{"role":"user","content":"needle"}]')`); err != nil {
		t.Fatal(err)
	}
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(context.Background())
	status, _, err := sessionIRHistorySave(ctx, sessionIRLiveTx{tx}, []string{"alice-session", "compaction", "primary", "alice", `[{"role":"user","blocks":[{"type":"text","text":"compact summary"}]}]`})
	if err != nil || status != store.StatusOK {
		t.Fatal(status, err)
	}
	if err = tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	search := func(actor, query string, include []string) map[string]any {
		t.Helper()
		request, _ := json.Marshal(map[string]any{"_principal": actor, "query": query, "include_sources": include, "view": "observations"})
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, cells, err := sessionIRSearch(ctx, sessionIRLiveTx{tx}, []string{actor, string(request)})
		if err != nil || status != store.StatusOK || len(cells) != 1 {
			t.Fatal(status, cells, err)
		}
		var result map[string]any
		if err = json.Unmarshal([]byte(cells[0]), &result); err != nil {
			t.Fatal(err)
		}
		return result
	}
	toolEvent := `{"kind":"message","message_id":"actual-tool-result","payload":{"role":"tool","blocks":[{"type":"text","text":"public-tool-prefix","context":null},{"type":"text","text":"hidden-between","context":{"model_visible":false}},{"type":"tool_result","tool_id":"owned-call","content":"canonical-tool-output"},{"type":"text","text":"hidden-search-secret","context":{"model_visible":false}},{"type":"text","text":"classified-search-secret","context":{"model_visible":true,"sensitivity":2}}]}}`
	toolTx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer toolTx.Rollback(context.Background())
	toolStatus, _, err := sessionIRAppend(ctx, sessionIRLiveTx{toolTx}, []string{"alice-session", "tool-result", "turn", "alice", toolEvent})
	if err != nil || toolStatus != store.StatusOK {
		t.Fatal(toolStatus, err)
	}
	if err = toolTx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	if search("alice", "needle 10%\nsecond block", nil)["count"] != float64(1) {
		t.Fatal("visible message blocks were not searched as ordered joined text")
	}
	if search("alice", "public-tool-prefix\ncanonical-tool-output", nil)["count"] != float64(1) {
		t.Fatal("plain tool result and visible text did not join across hidden block")
	}
	if search("alice", "canonical-tool-output", nil)["count"] != float64(1) {
		t.Fatal("owned canonical tool result was not searched")
	}
	for _, query := range []string{"hidden-search-secret", "classified-search-secret"} {
		if search("alice", query, nil)["count"] != float64(0) {
			t.Fatal("classified content leaked", query)
		}
	}
	current := func(actor, session, query, view, revision string) (uint32, map[string]any, error) {
		request := map[string]any{"_principal": actor, "query": query, "session_id": session}
		if view != "" {
			request["view"] = view
		}
		if revision != "" {
			request["source_revision"] = revision
			request["around_message_id"] = 0
		}
		encoded, _ := json.Marshal(request)
		tx, err := pool.Begin(ctx)
		if err != nil {
			return 0, nil, err
		}
		defer tx.Rollback(ctx)
		status, cells, err := sessionIRSearch(ctx, sessionIRLiveTx{tx}, []string{actor, string(encoded)})
		var result map[string]any
		if len(cells) == 1 {
			json.Unmarshal([]byte(cells[0]), &result)
		}
		return status, result, err
	}
	// Explicit-session windows are not candidate pages. A nonzero candidate
	// offset must not turn an owned unchanged source into found:false.
	offsetRequest := `{"_principal":"alice","session_id":"alice-session","around_message_id":0,"offset":1}`
	offsetTx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer offsetTx.Rollback(context.Background())
	offsetStatus, _, offsetErr := sessionIRSearch(ctx, sessionIRLiveTx{offsetTx}, []string{"alice", offsetRequest})
	offsetTx.Rollback(ctx)
	if offsetErr != nil || offsetStatus != store.StatusInvalid {
		t.Fatal(offsetStatus, offsetErr)
	}
	// Current projection includes checkpoint-imported messages even alongside
	// later original observations; it never merges messages by text equality.
	endTx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer endTx.Rollback(context.Background())
	endStatus, _, err := sessionIRAppend(ctx, sessionIRLiveTx{endTx}, []string{"alice-session", "terminal", "turn", "alice", runningDone})
	if err != nil || endStatus != store.StatusOK {
		t.Fatal(endStatus, err)
	}
	if err = endTx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	statusCurrent, currentResult, err := current("alice", "", "compact summary", "", "")
	if err != nil || statusCurrent != store.StatusOK || currentResult["count"] != float64(1) {
		t.Fatal("checkpoint-only message missing from mixed current projection", statusCurrent, currentResult, err)
	}
	currentSession := currentResult["sessions"].([]any)[0].(map[string]any)
	revision := currentSession["source_revision"].(string)
	revisionOffset, _ := json.Marshal(map[string]any{"_principal": "alice", "session_id": "alice-session", "around_message_id": 0, "source_revision": revision, "offset": 1})
	revisionTx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer revisionTx.Rollback(context.Background())
	revisionStatus, _, revisionErr := sessionIRSearch(ctx, sessionIRLiveTx{revisionTx}, []string{"alice", string(revisionOffset)})
	revisionTx.Rollback(ctx)
	if revisionErr != nil || revisionStatus != store.StatusInvalid {
		t.Fatal("revision-bound offset accepted", revisionStatus, revisionErr)
	}
	statusCurrent, currentResult, err = current("alice", "alice-session", "", "current", revision)
	if err != nil || statusCurrent != store.StatusOK || currentResult["found"] != true || currentResult["view"] != "current" {
		t.Fatal("revision-bound scroll", statusCurrent, currentResult, err)
	}
	statusCurrent, currentResult, err = current("bob", "alice-session", "", "current", revision)
	if err != nil || statusCurrent != store.StatusOK || currentResult["found"] != false {
		t.Fatal("foreign revision became source authority", statusCurrent, currentResult, err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO server_sessions(id,principal,client_type) VALUES('checkpoint-only','alice','primary')`); err != nil {
		t.Fatal(err)
	}
	checkpointTx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer checkpointTx.Rollback(context.Background())
	checkpointStatus, _, err := sessionIRHistorySave(ctx, sessionIRLiveTx{checkpointTx}, []string{"checkpoint-only", "imported", "primary", "alice", `[{"role":"user","blocks":[{"type":"text","text":"imported-alone"}]},{"role":"user","blocks":[{"type":"text","text":"imported-alone"}]}]`})
	if err != nil || checkpointStatus != store.StatusOK {
		t.Fatal(checkpointStatus, err)
	}
	if err = checkpointTx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	_, checkpointResult, err := current("alice", "", "imported-alone", "", "")
	if err != nil || checkpointResult["count"] != float64(1) {
		t.Fatal("checkpoint-only source missing", checkpointResult, err)
	}
	imported := checkpointResult["sessions"].([]any)[0].(map[string]any)
	if imported["message_count"] != float64(2) {
		t.Fatal("identical checkpoint messages were merged", imported)
	}
	if search("alice", "imported-alone", nil)["count"] != float64(0) {
		t.Fatal("checkpoint chunks invented original message observations")
	}
	otherSessionStatus, _, otherSessionErr := current("alice", "checkpoint-only", "", "current", revision)
	if otherSessionErr == nil && otherSessionStatus == store.StatusOK {
		t.Fatal("source revision crossed sessions")
	}
	sameTx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer sameTx.Rollback(context.Background())
	sameStatus, sameCells, err := sessionIRHistoryLoad(ctx, sessionIRLiveTx{sameTx}, []string{"alice-session", "primary", "alice", ""})
	if err != nil || sameStatus != store.StatusOK {
		t.Fatal(sameStatus, err)
	}
	sameStatus, _, err = sessionIRHistorySave(ctx, sessionIRLiveTx{sameTx}, []string{"alice-session", "same-content-checkpoint", "primary", "alice", sameCells[0]})
	if err != nil || sameStatus != store.StatusOK {
		t.Fatal(sameStatus, err)
	}
	if err = sameTx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	identicalStatus, _, identicalErr := current("alice", "alice-session", "", "current", revision)
	if identicalErr == nil && identicalStatus == store.StatusOK {
		t.Fatal("identical content at a different durable revision accepted stale selector")
	}
	// A source change invalidates the old window selector instead of silently
	// scrolling another revision. Original observations remain a separate view.
	changedTx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer changedTx.Rollback(context.Background())
	changedStatus, _, err := sessionIRHistorySave(ctx, sessionIRLiveTx{changedTx}, []string{"alice-session", "new-checkpoint", "primary", "alice", `[{"role":"user","blocks":[{"type":"text","text":"new current view"}]}]`})
	if err != nil || changedStatus != store.StatusOK {
		t.Fatal(changedStatus, err)
	}
	if err = changedTx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	statusCurrent, _, err = current("alice", "alice-session", "", "current", revision)
	if err == nil && statusCurrent == store.StatusOK {
		t.Fatal("stale source revision accepted")
	}
	_, currentResult, err = current("alice", "", "needle", "current", "")
	if err != nil || currentResult["count"] != float64(0) {
		t.Fatal("historical observation guessed into current projection", currentResult, err)
	}
	result := search("alice", "needle", nil)
	sessions := result["sessions"].([]any)
	if len(sessions) != 1 {
		t.Fatalf("canonical/owner/source filter: %#v", result)
	}
	session := sessions[0].(map[string]any)
	if session["session_id"] != "alice-session" || session["message_count"] != float64(2) {
		t.Fatal(session)
	}
	match := session["match"].(map[string]any)
	if !strings.Contains(match["snippet"].(string), "needle 10%\nsecond block") || strings.Contains(match["snippet"].(string), "opaque-secret") {
		t.Fatal(match)
	}
	for _, query := range []string{"opaque-secret", "tool-secret", "obsolete"} {
		if search("alice", query, nil)["count"] != float64(0) {
			t.Fatal("non-text or superseded snapshot searched", query)
		}
	}
	if search("alice", "%", nil)["count"] != float64(1) {
		t.Fatal("literal wildcard semantics changed")
	}
	if search("alice", "needle", []string{"delegate"})["count"] != float64(2) {
		t.Fatal("explicit delegate filter lost")
	}
	if search("bob", "needle", nil)["count"] != float64(1) || search("missing", "needle", nil)["count"] != float64(0) {
		t.Fatal("cross-principal search")
	}
	page := func(offset int) map[string]any {
		request, _ := json.Marshal(map[string]any{"_principal": "alice", "query": "needle", "view": "observations", "include_sources": []string{"delegate"}, "limit": 1, "offset": offset})
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(ctx)
		status, cells, err := sessionIRSearch(ctx, sessionIRLiveTx{tx}, []string{"alice", string(request)})
		if err != nil || status != store.StatusOK {
			t.Fatal(status, err)
		}
		var result map[string]any
		if err = json.Unmarshal([]byte(cells[0]), &result); err != nil {
			t.Fatal(err)
		}
		return result
	}
	firstPage := page(0)
	if firstPage["has_more"] != true || firstPage["next_offset"] != float64(1) {
		t.Fatal("bounded discovery has no continuation", firstPage)
	}
	secondPage := page(1)
	if firstPage["sessions"].([]any)[0].(map[string]any)["session_id"] == secondPage["sessions"].([]any)[0].(map[string]any)["session_id"] {
		t.Fatal("pagination replayed prior source")
	}
	if _, err := pool.Exec(ctx, `INSERT INTO server_sessions(id,principal,client_type) VALUES('streaming-session','alice','native')`); err != nil {
		t.Fatal(err)
	}
	streamEvents := []string{
		`{"kind":"delta","message_id":"stream-model","role":"assistant","payload":{"type":"TURN_START"}}`,
		`{"kind":"message","message_id":"stream-tool","payload":{"role":"tool","blocks":[{"type":"tool_result","tool_id":"call","content":"stream unique tool"}]}}`,
		`{"kind":"message","message_id":"stream-model","payload":{"role":"assistant","blocks":[{"type":"text","text":"stream unique model"}]}}`,
		runningDone,
	}
	streamTx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer streamTx.Rollback(context.Background())
	for i, event := range streamEvents {
		status, _, err := sessionIRAppend(ctx, sessionIRLiveTx{streamTx}, []string{"streaming-session", fmt.Sprint(i), "stream-turn", "alice", event})
		if err != nil || status != store.StatusOK {
			t.Fatal(status, err)
		}
	}
	if err = streamTx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	streamResult := search("alice", "stream unique", nil)
	streamWindow := streamResult["sessions"].([]any)[0].(map[string]any)["window"].([]any)
	if streamWindow[0].(map[string]any)["source_message_id"] != "stream-model" || streamWindow[1].(map[string]any)["source_message_id"] != "stream-tool" {
		t.Fatal("late snapshot reordered original stream observation", streamWindow)
	}
	// Explicit view selectors cannot cross a session or source revision.
	wrongViewStatus, _, wrongViewErr := current("alice", "alice-session", "", "observations", revision)
	if wrongViewErr == nil && wrongViewStatus == store.StatusOK {
		t.Fatal("current revision accepted for observations view")
	}
	// This is a store-integrity fixture, not an authorized append: actor-less or
	// transferred source content must not become visible through current history.
	if _, err := pool.Exec(ctx, `UPDATE session_ir_events SET actor='bob' WHERE session_id='alice-session' AND event_id='new-checkpoint:0'`); err != nil {
		t.Fatal(err)
	}
	_, changedOwnerResult, err := current("alice", "", "new current view", "current", "")
	if err != nil || changedOwnerResult["count"] != float64(0) {
		t.Fatal("foreign source actor admitted by current owner metadata", changedOwnerResult, err)
	}
	// A current search invoked inside its own captured turn may match that
	// unfinished turn. It cannot poison other completed source matches, and
	// an explicitly selected busy source must not look nonexistent.
	for _, id := range []string{"busy-search", "completed-search"} {
		if _, err := pool.Exec(ctx, `INSERT INTO server_sessions(id,principal,client_type) VALUES($1,'alice','browser')`, id); err != nil {
			t.Fatal(err)
		}
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(context.Background())
		events := []string{
			`{"kind":"execution","payload":{"status":"running"}}`,
			`{"kind":"message","message_id":"availability","payload":{"role":"user","blocks":[{"type":"text","text":"availability-needle"}]}}`,
		}
		if id == "completed-search" {
			events = append(events, runningDone)
		}
		for i, event := range events {
			status, _, err := sessionIRAppend(ctx, sessionIRLiveTx{tx}, []string{id, fmt.Sprint(i), "availability-turn", "alice", event})
			if err != nil || status != store.StatusOK {
				tx.Rollback(ctx)
				t.Fatal(status, err)
			}
		}
		if err := tx.Commit(ctx); err != nil {
			t.Fatal(err)
		}
	}
	availableStatus, available, err := current("alice", "", "availability-needle", "current", "")
	if err != nil || availableStatus != store.StatusOK || available["count"] != float64(1) || available["sessions"].([]any)[0].(map[string]any)["session_id"] != "completed-search" {
		t.Fatal("busy source poisoned completed search", availableStatus, available, err)
	}
	_, _, busyErr := current("alice", "busy-search", "", "current", "")
	if busyErr == nil || !strings.Contains(busyErr.Error(), "view observations") {
		t.Fatal("explicit busy current view did not explain unavailability", busyErr)
	}
	_, busyObservations, err := current("alice", "busy-search", "", "observations", "")
	if err != nil || busyObservations["count"] != float64(1) {
		t.Fatal("busy original observations lost", busyObservations, err)
	}
	// Corruption is a separate store-integrity fixture, not an admitted append.
	// It must retain its strict error even when the same turn is unfinished.
	if _, err := pool.Exec(ctx, `UPDATE session_ir_events SET payload='{"kind":"message","message_id":"availability","payload":{"role":"user","forged":true,"blocks":[{"type":"text","text":"availability-needle"}]}}' WHERE session_id='busy-search' AND event_id='1'`); err != nil {
		t.Fatal(err)
	}
	_, _, corruptErr := current("alice", "", "availability-needle", "current", "")
	if corruptErr == nil {
		t.Fatal("corrupt unfinished source was silently skipped")
	}
	if _, err := pool.Exec(ctx, `DELETE FROM server_sessions WHERE id='busy-search'`); err != nil {
		t.Fatal(err)
	}
	// Pathological unbounded display metadata can make even one source exceed
	// the page budget. Refuse explicitly without truncating identity or hiding
	// that source behind an advancing continuation token.
	if _, err := pool.Exec(ctx, `INSERT INTO server_sessions(id,principal,client_type,title) VALUES('oversized-search','alice','browser',$1)`, strings.Repeat("\x01", 400000)); err != nil {
		t.Fatal(err)
	}
	appendMessage("oversized-search", "alice", "oversized-message", "oversized-needle")
	oversizedTx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer oversizedTx.Rollback(context.Background())
	oversizedStatus, _, err := sessionIRAppend(ctx, sessionIRLiveTx{oversizedTx}, []string{"oversized-search", "terminal", "turn", "alice", runningDone})
	if err != nil || oversizedStatus != store.StatusOK {
		oversizedTx.Rollback(ctx)
		t.Fatal(oversizedStatus, err)
	}
	if err := oversizedTx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	_, _, oversizedErr := current("alice", "", "oversized-needle", "current", "")
	if oversizedErr == nil || !strings.Contains(oversizedErr.Error(), "display exceeds 2 MiB") {
		t.Fatal("oversized first source lacked explicit error", oversizedErr)
	}
	if _, err := pool.Exec(ctx, `DELETE FROM server_sessions WHERE id='alice-session'`); err != nil {
		t.Fatal(err)
	}
	if search("alice", "needle 10%", nil)["count"] != float64(0) {
		t.Fatal("deleted source remained visible")
	}
}

// Maximum-window pagination is an independent canonical-history journey, not
// part of the deadline shared by ownership, revisions, privacy and corruption.
func TestSessionIRLiveSearchLargeCanonicalWindowsPageWithoutSkipping(t *testing.T) {
	root := livePool(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	schema := fmt.Sprintf("session_search_%d", time.Now().UnixNano())
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
	// Maximum accepted limits/windows must produce bounded whole-session pages,
	// including worst-case JSON escaping, valid UTF-8 and private blocks. Use
	// real checkpoint imports so candidate selection and the current reducer
	// both exercise the canonical durable history path.
	const largeSessions = 8
	for n := 0; n < largeSessions; n++ {
		id := fmt.Sprintf("large-search-%02d", n)
		if !t.Run(id, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			defer cancel()
			if _, err := pool.Exec(ctx, `INSERT INTO server_sessions(id,principal,client_type) VALUES($1,'alice','browser')`, id); err != nil {
				t.Fatal(err)
			}
			messages := make([]map[string]any, 43)
			for i := range messages {
				text := "界<&>" + strings.Repeat("\x01", 4090)
				if i == 21 {
					text = "page-needle" + text
				}
				messages[i] = map[string]any{"role": "assistant", "blocks": []map[string]any{
					{"type": "text", "text": text},
					{"type": "text", "text": "private-pagination-secret", "context": map[string]any{"model_visible": false}},
				}}
			}
			encoded, err := json.Marshal(messages)
			if err != nil {
				t.Fatal(err)
			}
			tx, err := pool.Begin(ctx)
			if err != nil {
				t.Fatal(err)
			}
			defer tx.Rollback(context.Background())
			status, _, err := sessionIRHistorySave(ctx, sessionIRLiveTx{tx}, []string{id, "large-import", "primary", "alice", string(encoded)})
			if err != nil || status != store.StatusOK {
				tx.Rollback(ctx)
				t.Fatal(status, err)
			}
			if err := tx.Commit(ctx); err != nil {
				t.Fatal(err)
			}
		}) {
			return
		}
	}
	seen := map[string]bool{}
	offset := 0
	pages := 0
	readPage := func(offset int) string {
		t.Helper()
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		request, _ := json.Marshal(map[string]any{"_principal": "alice", "query": "page-needle", "window": 20, "limit": 20, "offset": offset})
		tx, err := pool.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(context.Background())
		status, cells, err := sessionIRSearch(ctx, sessionIRLiveTx{tx}, []string{"alice", string(request)})
		tx.Rollback(ctx)
		if err != nil || status != store.StatusOK || len(cells) != 1 {
			t.Fatal(status, err)
		}
		return cells[0]
	}
	for {
		encodedPage := readPage(offset)
		if len(encodedPage) > sessionSearchResponseMax || strings.Contains(encodedPage, "private-pagination-secret") {
			t.Fatal("page exceeded bound or leaked hidden text")
		}
		var page map[string]any
		if err := json.Unmarshal([]byte(encodedPage), &page); err != nil {
			t.Fatal(err)
		}
		entries := page["sessions"].([]any)
		if len(entries) == 0 {
			t.Fatal("valid matching session disappeared", page)
		}
		for _, entry := range entries {
			item := entry.(map[string]any)
			id := item["session_id"].(string)
			if seen[id] {
				t.Fatal("page replayed source", id)
			}
			seen[id] = true
			window := item["window"].([]any)
			if len(window) != 41 || item["message_count"] != float64(43) {
				t.Fatal("session window was partially emitted", len(window), item["message_count"])
			}
			text := window[0].(map[string]any)["text"].(string)
			if !strings.HasPrefix(text, "界<&>") || !strings.Contains(text, "\x01") {
				t.Fatal("escaped UTF-8/control text changed")
			}
		}
		pages++
		if page["has_more"] != true {
			break
		}
		next := int(page["next_offset"].(float64))
		if next != offset+len(entries) {
			t.Fatal("response budget skipped unconsumed source", offset, next, len(entries))
		}
		offset = next
		if pages > largeSessions {
			t.Fatal("pagination did not progress")
		}
	}
	if len(seen) != largeSessions || pages < 2 {
		t.Fatal("large allowed request did not page completely", len(seen), pages)
	}
}
