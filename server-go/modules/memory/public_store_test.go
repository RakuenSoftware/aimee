package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestStorePublicValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, args := range []string{`{}`, `{"key":"x","content":" "}`, `{"key":"x","content":"y","confidence":null}`, `{"key":"x","content":"y","confidence":2}`, `{"key":"x","content":"y","epistemic_kind":"anything"}`} {
		r := runPublicCommand(t, client, "store", args)
		if r["kind"] != "invalid_argument" {
			t.Fatal(r)
		}
		if strings.Contains(args, "confidence") && !strings.Contains(fmt.Sprint(r["message"]), "confidence must be between 0 and 1") {
			t.Fatal("confidence refusal changed the public contract", r)
		}
	}
	for _, context := range []string{
		`"project":"__aimee_scope_missing__","workspace":"__aimee_scope_missing__"`,
		`"workspace":"__aimee_scope_missing__"`,
		`"project":"  __aimee_scope_missing__  "`,
	} {
		r := runPublicCommand(t, client, "store", `{"key":"x","content":"y","scope_context":true,`+context+`}`)
		if r["kind"] != "invalid_argument" || r["reason"] != "active_context_missing" || r["active_context_missing"] != true {
			t.Fatal("missing scope must be refused before accessing storage", r)
		}
	}
	if r := runPublicCommand(t, client, "store", `{"key":"x","content":"y"}`); r["kind"] != "unavailable" {
		t.Fatal(r)
	}
}

func TestStorePublicPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("set AIMEE_MEMORY_EVAL_URL for PostgreSQL store regression")
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
	_, err = tx.Exec(ctx, `CREATE SCHEMA store_command_test;
CREATE FUNCTION store_command_test.pg_now_text(shift text DEFAULT '0 seconds') RETURNS text LANGUAGE sql AS $$ SELECT (now()+shift::interval)::text $$;
SET LOCAL search_path TO pg_temp,store_command_test,public;
CREATE TEMP TABLE memories(id bigserial PRIMARY KEY,key text,content text,tier text,kind text,epistemic_kind text,
 scope_type text,scope_value text,confidence double precision,confidence_ceiling double precision,use_count int DEFAULT 0,
 lifecycle_state text,activation_suppressed int DEFAULT 0,archive_reason text DEFAULT '',use_cases text DEFAULT '',last_used_at text DEFAULT '',source_session text DEFAULT '',provenance_category text DEFAULT '',
 owner_principal text DEFAULT '',sensitivity text DEFAULT 'normal',valid_from text DEFAULT '',valid_until text DEFAULT '',created_at text DEFAULT pg_now_text(),updated_at text DEFAULT pg_now_text());
CREATE UNIQUE INDEX memory_key_scope ON memories(kind,key,scope_type,scope_value);
CREATE TEMP TABLE memory_scopes(memory_id bigint,scope_type text,scope_value text,UNIQUE(memory_id,scope_type,scope_value));
CREATE TEMP TABLE memory_links(id bigserial PRIMARY KEY,source_id bigint,target_id bigint,relation text);
CREATE TEMP TABLE memory_rejection_tombstones(object_kind text,memory_key text,memory_content text,scope_type text,scope_value text,active int DEFAULT 1);
CREATE TEMP TABLE memory_summaries(id bigserial PRIMARY KEY,memory_id bigint,scope text,summary text);
CREATE TEMP TABLE memory_fact_actors(memory_id bigint PRIMARY KEY REFERENCES memories(id) ON DELETE CASCADE,actor_principal text,actor_role text,authority_rank int,authenticated int,transport_identity text,captured_at text DEFAULT pg_now_text());
CREATE TEMP TABLE kb_async_jobs(id bigserial PRIMARY KEY,kind text,document_id bigint,project text,status text,updated_at text,generation bigint DEFAULT 1,attempts int DEFAULT 0,claimed_by text DEFAULT '',claimed_at text DEFAULT '',last_error text DEFAULT '',next_attempt_at text DEFAULT '',UNIQUE(kind,document_id));`)
	if err != nil {
		t.Fatal(err)
	}
	installProposalFixture(t, ctx, tx)
	handler := NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}))
	client := clientForHandler(t, handler)
	caller := bus.CommandContext{Authenticated: true, Principal: "user:alice", UserAuthority: true, TransportIdentity: "cert:server"}
	put := func(args string, verified bool) map[string]any {
		t.Helper()
		if !verified {
			return runPublicCommand(t, client, "store", args)
		}
		r, status := invokeContextCommand(t, handler, 0, caller, "store", args)
		if status != bus.ModuleStatusOK {
			t.Fatal(status)
		}
		return r
	}
	// Canonical store admission also covers Put, workflows and practice writes.
	backend := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB}
	for _, scope := range []Scope{{Type: ScopeProject, Value: missingScopeValue}, {Type: ScopeWorkspace, Value: missingScopeValue}} {
		_, err := backend.InsertEpistemic(ctx, DataRequest{Scope: scope, Tier: "L2", Kind: "fact", Key: "missing-context", Content: "must not persist"})
		if err == nil || !strings.Contains(err.Error(), "active scope context") {
			t.Fatal("canonical insert accepted missing context", scope, err)
		}
		_, err = backend.Put(ctx, scope, Record{Tier: "L2", Kind: "fact", Key: "missing-context", Content: "must not persist"})
		if err == nil || !strings.Contains(err.Error(), "active scope context") {
			t.Fatal("Put accepted missing context", scope, err)
		}
	}
	var missingRows int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE key='missing-context'`).Scan(&missingRows); err != nil || missingRows != 0 {
		t.Fatal(missingRows, err)
	}
	r := put(`{"key":"model-note","content":"a useful note","authority":"user","actor":"user:forged","tier":"L2","confidence":1,"session_id":"session","use_cases":"answer questions"}`, false)
	if r["status"] != "ok" {
		t.Fatal(r)
	}
	record := r["memory"].(map[string]any)
	if r["id"] != record["id"] || record["confidence"] != 0.8 || record["source_session"] != "session" || record["use_cases"] != "answer questions" || record["provenance_category"] != "agent_message" {
		t.Fatal(r)
	}
	checkActor := func(id any, principal string, rank, authenticated int) {
		t.Helper()
		var p string
		var n, a int
		err := tx.QueryRow(ctx, `SELECT actor_principal,authority_rank,authenticated FROM memory_fact_actors WHERE memory_id=$1`, int64(id.(float64))).Scan(&p, &n, &a)
		if err != nil || p != principal || n != rank || a != authenticated {
			t.Fatalf("actor=%s/%d/%d %v", p, n, a, err)
		}
	}
	checkActor(r["id"], "system:model-inference", 10, 0)
	user := put(`{"key":"user-note","content":"verified note","authority":"user","scope_context":true,"project":"app"}`, true)
	if user["status"] != "ok" || user["memory"].(map[string]any)["confidence"] != float64(1) || user["memory"].(map[string]any)["provenance_category"] != "user_stated" {
		t.Fatal(user)
	}
	checkActor(user["id"], "user:alice", 30, 1)
	// A failed authorized correction must roll back the closed interval, replacement,
	// lineage, and extraction actor together.
	if _, err := tx.Exec(ctx, `ALTER TABLE kb_async_jobs ADD CONSTRAINT update_enqueue_failure CHECK (document_id<0) NOT VALID`); err != nil {
		t.Fatal(err)
	}
	edit := fmt.Sprintf(`{"id":%.0f,"content":"failed replacement","authority":"user"}`, user["id"])
	if r, status := invokeContextCommand(t, handler, 0, caller, "update", edit); status != bus.ModuleStatusOK || r["kind"] != "unavailable" {
		t.Fatal(r)
	}
	var active bool
	if err := tx.QueryRow(ctx, `SELECT lifecycle_state='active' AND valid_until='' AND content='verified note' FROM memories WHERE id=$1`, int64(user["id"].(float64))).Scan(&active); err != nil || !active {
		t.Fatal(active, err)
	}
	checkActor(user["id"], "user:alice", 30, 1)
	var leaked int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE content='failed replacement'`).Scan(&leaked); err != nil || leaked != 0 {
		t.Fatal(leaked, err)
	}
	if _, err := tx.Exec(ctx, `ALTER TABLE kb_async_jobs DROP CONSTRAINT update_enqueue_failure`); err != nil {
		t.Fatal(err)
	}
	// A same-key write, edit, supersede or retirement cannot silently displace
	// user-authored content, even through an authenticated model host.
	for _, attempt := range []struct{ verb, args string }{
		{"store", `{"key":"user-note","content":"model replacement","scope_context":true,"project":"app"}`},
		{"update", fmt.Sprintf(`{"id":%.0f,"content":"model replacement"}`, user["id"])},
		{"supersede", fmt.Sprintf(`{"old_id":%.0f,"new_content":"model replacement"}`, user["id"])},
		{"delete", fmt.Sprintf(`{"id":%.0f}`, user["id"])},
	} {
		got, status := invokeContextCommand(t, handler, 0, caller, attempt.verb, attempt.args)
		if status != bus.ModuleStatusOK || got["kind"] != "review_required" {
			t.Fatal(attempt, got, status)
		}
	}
	checkActor(user["id"], "user:alice", 30, 1)
	replaced := put(`{"key":"user-note","content":"reviewed correction","authority":"user","scope_context":true,"project":"app"}`, true)
	if replaced["status"] != "ok" || replaced["id"] == user["id"] {
		t.Fatal(replaced, user)
	}
	checkActor(replaced["id"], "user:alice", 30, 1)
	// Older data-stage verbs are compatibility adapters to the same admission.
	for _, operation := range []string{"store", "update-content", "delete"} {
		request := DataRequest{Operation: operation, ID: int64(replaced["id"].(float64)), Scope: Scope{Type: ScopeProject, Value: "app"}, Tier: "L2", Kind: "fact", Key: "user-note", Content: "legacy overwrite"}
		body, _ := json.Marshal(request)
		raw, status := handler(bus.ModuleInvocation{StageID: StageData}, body)
		var result DataResponse
		if status != bus.ModuleStatusOK || json.Unmarshal(raw, &result) != nil || result.Code == nil || *result.Code != MutationReviewRequired {
			t.Fatal(operation, string(raw), status)
		}
	}

	var oldContent, oldState string
	if err := tx.QueryRow(ctx, `SELECT content,lifecycle_state FROM memories WHERE id=$1`, int64(user["id"].(float64))).Scan(&oldContent, &oldState); err != nil || oldContent != "verified note" || oldState != "superseded" {
		t.Fatal(oldContent, oldState, err)
	}
	var jobs int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM kb_async_jobs WHERE kind='memory_facts' AND status='pending'`).Scan(&jobs); err != nil || jobs != 3 {
		t.Fatal(jobs, err)
	}
	// Retrying identical bytes does not rewrite the original extraction author.
	other := caller
	other.Principal = "user:bob"
	same, status := invokeContextCommand(t, handler, 0, other, "store", `{"key":"user-note","content":"reviewed correction","authority":"user","scope_context":true,"project":"app"}`)
	if status != bus.ModuleStatusOK || same["id"] != replaced["id"] {
		t.Fatal(same, status)
	}
	checkActor(replaced["id"], "user:alice", 30, 1)
	// Immutable epistemic kinds retain the same protection on upsert as on edit.
	for _, kind := range []string{"episode", "experience", "instruction", "policy"} {
		made := put(fmt.Sprintf(`{"key":"guard-%s","content":"original","epistemic_kind":"%s","authority":"user"}`, kind, kind), true)
		if made["status"] != "ok" {
			t.Fatal(made)
		}
		conflict := put(fmt.Sprintf(`{"key":"guard-%s","content":"overwrite","authority":"user"}`, kind), true)
		if conflict["kind"] != "conflict" {
			t.Fatal(kind, conflict)
		}
	}
	if low := put(`{"key":"hypothesis","content":"tentative","tier":"L5","authority":"user"}`, true); low["status"] != "ok" || low["memory"].(map[string]any)["confidence"] != 0.5 {
		t.Fatal(low)
	}
	// Supersede has the same host-authorized user correction path as update.
	supersedeSource := put(`{"key":"verified-supersede","content":"user original","authority":"user"}`, true)
	supersedeArgs := fmt.Sprintf(`{"old_id":%.0f,"new_content":"user correction","authority":"user"}`, supersedeSource["id"])
	for _, unverified := range []bus.CommandContext{
		{}, {Authenticated: true, Principal: "model:host"},
	} {
		if got, status := invokeContextCommand(t, handler, 0, unverified, "supersede", supersedeArgs); status != bus.ModuleStatusOK || got["kind"] != "review_required" {
			t.Fatal("supersede accepted unverified authority", unverified, got, status)
		}
	}
	correction, status := invokeContextCommand(t, handler, 0, caller, "supersede", supersedeArgs)
	if status != bus.ModuleStatusOK || correction["status"] != "ok" {
		t.Fatal("verified user correction refused", correction, status)
	}
	correctedMemory := correction["memory"].(map[string]any)
	if correctedMemory["id"] == supersedeSource["id"] || correctedMemory["provenance_category"] != "user_stated" {
		t.Fatal("verified correction lost version or authority", correction)
	}
	checkActor(correctedMemory["id"], "user:alice", 30, 1)
	// Both failure positions roll back the memory row, actor capture and enqueue.
	for _, tt := range []struct{ table, check, key string }{
		{"memory_fact_actors", "authority_rank<0", "capture-failure"}, {"kb_async_jobs", "document_id<0", "enqueue-failure"},
	} {
		if _, err := tx.Exec(ctx, `ALTER TABLE `+tt.table+` ADD CONSTRAINT injected_failure CHECK (`+tt.check+`) NOT VALID`); err != nil {
			t.Fatal(err)
		}
		args, _ := json.Marshal(map[string]any{"key": tt.key, "content": "must roll back"})
		if r := put(string(args), false); r["status"] != "error" {
			t.Fatal(r)
		}
		var count int
		if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE key=$1`, tt.key).Scan(&count); err != nil || count != 0 {
			t.Fatal(count, err)
		}
		if _, err := tx.Exec(ctx, `ALTER TABLE `+tt.table+` DROP CONSTRAINT injected_failure`); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := tx.Exec(ctx, `INSERT INTO memory_rejection_tombstones VALUES ('memory','blocked','rejected content','global','_global',1)`); err != nil {
		t.Fatal(err)
	}
	if r := put(`{"key":"blocked","content":"rejected content"}`, true); r["status"] != "error" {
		t.Fatal(r)
	}
	// Store scope is applied by the owner transaction, including import overrides.
	var scope string
	if err := tx.QueryRow(ctx, `SELECT scope_value FROM memories WHERE id=$1`, int64(user["id"].(float64))).Scan(&scope); err != nil || scope != "app" {
		t.Fatal(scope, err)
	}

	// Model replacement preserves its own history, scope and model provenance.
	original := put(`{"key":"version#v123","content":"original","tier":"L2","use_cases":"context","session_id":"before","scope_context":true,"project":"app"}`, true)
	oldID := int64(original["id"].(float64))
	if _, err := tx.Exec(ctx, `INSERT INTO memory_scopes VALUES ($1,'workspace','team')`, oldID); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(ctx, `UPDATE memories SET owner_principal='user:private',sensitivity='sensitive' WHERE id=$1`, oldID); err != nil {
		t.Fatal(err)
	}
	replacementArgs := fmt.Sprintf(`{"old_id":%d,"new_content":"replacement","confidence":1,"session_id":"after","scope_context":true,"project":"app"}`, oldID)
	replacement := runPublicCommand(t, client, "supersede", replacementArgs)
	if replacement["status"] != "ok" {
		t.Fatal(replacement)
	}
	fresh := replacement["memory"].(map[string]any)
	newID := int64(fresh["id"].(float64))
	if newID == oldID || fresh["key"] != "version#v123" || fresh["source_session"] != "after" || fresh["use_cases"] != "context" || fresh["confidence"] != 0.8 || fresh["provenance_category"] != "agent_message" {
		t.Fatal(replacement)
	}
	checkActor(fresh["id"], "system:model-inference", 10, 0)
	var owner, sensitivity string
	if err := tx.QueryRow(ctx, `SELECT owner_principal,sensitivity FROM memories WHERE id=$1`, newID).Scan(&owner, &sensitivity); err != nil || owner != "user:private" || sensitivity != "sensitive" {
		t.Fatal(owner, sensitivity, err)
	}
	var boundary, state, oldKey string
	var equal bool
	err = tx.QueryRow(ctx, `SELECT o.key,o.lifecycle_state,o.valid_until,o.valid_until=n.valid_from FROM memories o,memories n WHERE o.id=$1 AND n.id=$2`, oldID, newID).Scan(&oldKey, &state, &boundary, &equal)
	if err != nil || !equal || boundary == "" || state != "superseded" || oldKey != fmt.Sprintf("version#v123#v%d", oldID) {
		t.Fatal(oldKey, state, boundary, equal, err)
	}
	var count int
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_links WHERE source_id=$1 AND target_id=$2 AND relation='supersedes'`, newID, oldID).Scan(&count); err != nil || count != 1 {
		t.Fatal(count, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_scopes WHERE memory_id=$1 AND scope_value='team'`, newID).Scan(&count); err != nil || count != 1 {
		t.Fatal(count, err)
	}
	for _, id := range []int64{oldID, newID} {
		args, _ := json.Marshal(map[string]any{"id": id, "as_of": boundary})
		historical := runPublicCommand(t, client, "get", string(args))
		if historical["status"] != "ok" || historical["valid_at"] != (id == newID) {
			t.Fatal(historical)
		}
	}
	// The same source cannot be superseded twice.
	if r := runPublicCommand(t, client, "supersede", replacementArgs); r["kind"] != "not_found" {
		t.Fatal(r)
	}
	for _, kind := range []string{"episode", "experience", "instruction", "policy"} {
		args, _ := json.Marshal(map[string]any{"key": kind, "content": "immutable", "epistemic_kind": kind})
		item := put(string(args), false)
		args, _ = json.Marshal(map[string]any{"old_id": item["id"], "new_content": "changed"})
		if r := runPublicCommand(t, client, "supersede", string(args)); r["kind"] != "conflict" {
			t.Fatal(kind, r)
		}
	}
	// A failed extraction enqueue must roll back closing the source and all derived rows.
	if _, err := tx.Exec(ctx, `ALTER TABLE kb_async_jobs ADD CONSTRAINT fail_replacement CHECK (document_id<0) NOT VALID`); err != nil {
		t.Fatal(err)
	}
	args := fmt.Sprintf(`{"old_id":%d,"new_content":"must roll back"}`, newID)
	if r := runPublicCommand(t, client, "supersede", args); r["kind"] != "unavailable" {
		t.Fatal(r)
	}
	if err := tx.QueryRow(ctx, `SELECT lifecycle_state FROM memories WHERE id=$1`, newID).Scan(&state); err != nil || state != "active" {
		t.Fatal(state, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE content='must roll back'`).Scan(&count); err != nil || count != 0 {
		t.Fatal(count, err)
	}
	if _, err := tx.Exec(ctx, `ALTER TABLE kb_async_jobs DROP CONSTRAINT fail_replacement`); err != nil {
		t.Fatal(err)
	}
	// Server views retain complete owner records and the existing flat
	// supersede envelope without routing through fixed native structs.
	longContent := strings.Repeat("完整 memory ", 1024)
	viewArgs, _ := json.Marshal(map[string]any{"key": "server-view", "content": longContent, "view": "server", "scope_context": true, "project": "server-view-project"})
	createdView := put(string(viewArgs), false)
	if createdView["status"] != "ok" || createdView["store"] != "kb" {
		t.Fatal(createdView)
	}
	viewList := runPublicCommand(t, client, "list", `{"view":"server","scope_context":true,"project":"server-view-project"}`)
	if viewList["store"] != "kb" {
		t.Fatal("server list missing store")
	}
	foundView := false
	for _, item := range viewList["memories"].([]any) {
		record := item.(map[string]any)
		if record["id"] == createdView["id"] {
			foundView = true
			if record["content"] != longContent {
				t.Fatal("server list lost content")
			}
		}
	}
	if !foundView {
		t.Fatal("server list omitted scoped record")
	}
	viewArgs, _ = json.Marshal(map[string]any{"old_id": createdView["id"], "new_content": longContent + " corrected", "view": "server", "scope_context": true, "project": "server-view-project"})
	correctedView := runPublicCommand(t, client, "supersede", string(viewArgs))
	if correctedView["status"] != "ok" || correctedView["store"] != "kb" || correctedView["id"] == createdView["id"] || correctedView["content"] != longContent+" corrected" || correctedView["memory"] != nil {
		t.Fatal("server supersede shape", correctedView)
	}
	// Decimal-string IDs cross native JSON transports without double rounding.
	// Replies retain numeric int64 tokens from the owner, including temporal data.
	for i, provenance := range []string{"agent_message", "agent_message", "user_stated"} {
		_, err := tx.Exec(ctx, `INSERT INTO memories(id,key,content,tier,kind,epistemic_kind,scope_type,scope_value,confidence,confidence_ceiling,lifecycle_state,provenance_category)
VALUES($1,$2,'exact integer fixture','L2','fact','world_fact','project','exact-id',.8,.8,'active',$3)`, int64(9007199254740993)+int64(i)*2, fmt.Sprintf("exact-%d", i), provenance)
		if err != nil {
			t.Fatal(err)
		}
	}
	exactArgs := `{"id":"9007199254740993","view":"server","scope_context":true,"project":"exact-id","as_of":"2020-01-01T00:00:00Z"}`
	exactBody, err := client.Command(ctx, 73, "get", json.RawMessage(exactArgs))
	if err != nil {
		t.Fatal(err)
	}
	var exactRecord struct {
		Status string `json:"status"`
		Store  string `json:"store"`
		Memory struct {
			ID int64 `json:"id"`
		} `json:"memory"`
		AsOf string `json:"as_of"`
	}
	if json.Unmarshal(exactBody, &exactRecord) != nil || exactRecord.Status != "ok" || exactRecord.Store != "kb" || exactRecord.Memory.ID != 9007199254740993 || exactRecord.AsOf == "" {
		t.Fatal(string(exactBody))
	}
	deleted := runPublicCommand(t, client, "delete", exactArgs)
	if deleted["status"] != "ok" || deleted["store"] != "kb" || deleted["deleted"] != true || deleted["destroyed"] != false {
		t.Fatal(deleted)
	}
	if r := runPublicCommand(t, client, "supersede", `{"old_id":"9007199254740995","new_content":"exact replacement","view":"server","scope_context":true,"project":"exact-id"}`); r["status"] != "ok" || r["content"] != "exact replacement" {
		t.Fatal(r)
	}
	if r := runPublicCommand(t, client, "delete", `{"id":"9007199254740997","view":"server","scope_context":true,"project":"exact-id"}`); r["kind"] != "review_required" || r["deleted"] != nil {
		t.Fatal(r)
	}
	destroyed, status := invokeContextCommand(t, handler, 0, caller, "delete", `{"id":"9007199254740997","authority":"user","view":"server","scope_context":true,"project":"exact-id"}`)
	if status != bus.ModuleStatusOK || destroyed["status"] != "ok" || destroyed["destroyed"] != true || destroyed["deleted"] != true {
		t.Fatal(status, destroyed)
	}
	for _, id := range []string{`9007199254740993`, `"9223372036854775808"`, `"01"`, `"-1"`} {
		for _, verb := range []string{"get", "delete"} {
			if r := runPublicCommand(t, client, verb, `{"id":`+id+`}`); r["kind"] != "invalid_argument" {
				t.Fatal(verb, id, r)
			}
		}
	}
	// MCP writes retain model admission and render exact IDs inside owner text.
	if _, err := tx.Exec(ctx, `SELECT setval('memories_id_seq',9007199254741993)`); err != nil {
		t.Fatal(err)
	}
	mcp := runPublicCommand(t, client, "store", `{"key":"mcp-view","content":"original","view":"mcp"}`)
	if mcp["text"] != "stored memory id=9007199254741994 key=mcp-view" {
		t.Fatal(mcp)
	}
	var mcpTier string
	if err := tx.QueryRow(ctx, `SELECT tier FROM memories WHERE id=9007199254741994`).Scan(&mcpTier); err != nil || mcpTier != "L2" {
		t.Fatal(mcpTier, err)
	}
	mcp = runPublicCommand(t, client, "update", `{"id":"9007199254741994","content":"replacement","view":"mcp","authority":"user"}`)
	if mcp["text"] != "updated memory id=9007199254741994 (previous content kept as a version; current value is now id=9007199254741995)" {
		t.Fatal(mcp)
	}
	mcp = runPublicCommand(t, client, "supersede", `{"old_id":"9007199254741995","new_content":"corrected","view":"mcp"}`)
	if mcp["text"] != "superseded id=9007199254741995 new id=9007199254741996" {
		t.Fatal(mcp)
	}
	mcp = runPublicCommand(t, client, "touch", `{"id":"9007199254741996","view":"mcp"}`)
	if mcp["text"] != "affirmed memory id=9007199254741996" {
		t.Fatal(mcp)
	}
	mcp = runPublicCommand(t, client, "delete", `{"id":"9007199254741996","view":"mcp"}`)
	if mcp["text"] != "forgot memory id=9007199254741996 (retired, not destroyed: it no longer answers recall but remains in fact history)" {
		t.Fatal(mcp)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM memories WHERE id BETWEEN 9007199254741994 AND 9007199254741996`).Scan(&count); err != nil || count != 3 {
		t.Fatal("MCP destroyed history", count, err)
	}
	if r := runPublicCommand(t, client, "restore", `{"id":"9007199254741996","view":"mcp"}`); r["kind"] != "invalid_argument" {
		t.Fatal(r)
	}
	if _, err := tx.Exec(ctx, `SELECT setval('memories_id_seq',1000)`); err != nil {
		t.Fatal(err)
	}
	if r := put(`{"key":"native-identity","content":"identity text","view":"native"}`, false); r["id_text"] != fmt.Sprintf("%.0f", r["id"]) {
		t.Fatal("missing native identity", r)
	}
	if r := runPublicCommand(t, client, "find_id_by_key_kind", `{"key":"native-identity","kind":"fact","view":"native"}`); r["id_text"] != fmt.Sprintf("%.0f", r["id"]) {
		t.Fatal("missing lookup identity", r)
	}
	// Replacement under a non-owner role cannot reach a different project's source.
	_, err = tx.Exec(ctx, `CREATE ROLE memory_store_test NOINHERIT NOBYPASSRLS;
GRANT USAGE ON SCHEMA store_command_test TO memory_store_test;
GRANT SELECT,UPDATE,DELETE,INSERT ON memories,memory_rejection_tombstones,memory_links,memory_scopes,memory_summaries,memory_fact_actors,kb_async_jobs TO memory_store_test;
GRANT USAGE,SELECT ON SEQUENCE memories_id_seq,memory_links_id_seq,kb_async_jobs_id_seq TO memory_store_test;
ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
CREATE POLICY test_visibility ON memories USING (scope_type='global' OR current_setting('aimee.memory_scope_all',true)='1' OR (scope_type=current_setting('aimee.memory_scope_type',true) AND scope_value=current_setting('aimee.memory_scope_value',true)));
SET LOCAL ROLE memory_store_test;`)
	if err != nil {
		t.Fatal(err)
	}
	args = fmt.Sprintf(`{"old_id":%d,"new_content":"hidden","scope_context":true,"project":"other"}`, newID)
	if r := runPublicCommand(t, client, "supersede", args); r["kind"] != "not_found" {
		t.Fatal(r)
	}
	args = fmt.Sprintf(`{"old_id":%d,"new_content":"visible","scope_context":true,"project":"app"}`, newID)
	if r := runPublicCommand(t, client, "supersede", args); r["status"] != "ok" {
		t.Fatal(r)
	}
	// The owner enforces screening even when no native pre-send client is used.
	for _, args := range []string{
		`{"key":"password=identity","content":"safe"}`,
		`{"key":"pem-note","content":"-----BEGIN PRIVATE KEY-----\nsecret body\n-----END PRIVATE KEY-----"}`,
	} {
		if r := put(args, false); r["kind"] != "unavailable" {
			t.Fatal("sensitive write accepted", r)
		}
	}
	redacted := put(`{"key":"redacted-note","content":"password=first token=second","use_cases":"secret=third"}`, false)
	if redacted["status"] != "ok" || redacted["memory"].(map[string]any)["content"] != "[REDACTED] [REDACTED]" || redacted["memory"].(map[string]any)["use_cases"] != "[REDACTED]" {
		t.Fatal(redacted)
	}
	editRaw := fmt.Sprintf(`{"id":%.0f,"content":"-----BEGIN PRIVATE KEY-----\nsecret"}`, redacted["id"])
	if r := runPublicCommand(t, client, "update", editRaw); r["kind"] != "unavailable" {
		t.Fatal(r)
	}
	editRaw = fmt.Sprintf(`{"old_id":%.0f,"new_content":"password=replacement"}`, redacted["id"])
	if r := runPublicCommand(t, client, "supersede", editRaw); r["status"] != "ok" || r["memory"].(map[string]any)["content"] != "[REDACTED]" {
		t.Fatal(r)
	}

	global := put(`{"key":"explicit-global","content":"global note","scope_context":true,"include_all":true}`, false)
	if global["status"] != "ok" {
		t.Fatal("explicit global store must remain available", global)
	}
	var globalScope string
	if err := tx.QueryRow(ctx, `SELECT scope_type||':'||scope_value FROM memories WHERE id=$1`, int64(global["id"].(float64))).Scan(&globalScope); err != nil || globalScope != "global:_global" {
		t.Fatal(globalScope, err)
	}

}

// An insert conflict must wait for the actual uncommitted identity lock, then
// admit against the committed author's authority rather than overwrite it.
func TestSameKeyMutationConcurrency(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("set AIMEE_DB2_REPLAY_URL")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	aConn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer aConn.Close(context.Background())
	bConn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer bConn.Close(context.Background())
	a, err := aConn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer a.Rollback(context.Background())
	b, err := bConn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer b.Rollback(context.Background())
	request := DataRequest{Scope: Scope{Type: ScopeProject, Value: "mutation-concurrency"}, Tier: "L2", Kind: "fact", Key: fmt.Sprintf("mutation-%d", time.Now().UnixNano()), Content: "verified original", Authority: AuthorityUser}
	first, err := (&postgresDataStore{db: evalQueryer{a}, placement: PlacementKB}).InsertEpistemic(ctx, request)
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		// Release any failed attempt before cleaning only this test's record.
		cancel()
		b.Rollback(context.Background())
		a.Rollback(context.Background())
		cleanup, stop := context.WithTimeout(context.Background(), 5*time.Second)
		defer stop()
		_, err := aConn.Exec(cleanup, `DELETE FROM memories WHERE id=$1 AND key=$2`, first.ID, request.Key)
		if err != nil {
			t.Error(err)
		}
	}()
	done := make(chan error, 1)
	model := request
	model.Content, model.Authority = "model overwrite", AuthorityModel
	go func() {
		_, err := (&postgresDataStore{db: evalQueryer{b}, placement: PlacementKB}).InsertEpistemic(ctx, model)
		done <- err
	}()
	waiting := false
	for !waiting {
		if err := a.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM pg_locks WHERE pid=$1 AND locktype='advisory' AND NOT granted)`, int64(bConn.PgConn().PID())).Scan(&waiting); err != nil {
			t.Fatal(err)
		}
		if !waiting {
			select {
			case err := <-done:
				t.Fatal("conflicting writer bypassed identity lock", err)
			case <-ctx.Done():
				t.Fatal(ctx.Err())
			case <-time.After(5 * time.Millisecond):
			}
		}
	}
	if err := a.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	select {
	case err := <-done:
		if !errors.Is(err, errMutationReviewRequired) {
			t.Fatal(err)
		}
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	if err := b.Rollback(ctx); err != nil {
		t.Fatal(err)
	}
	var content, state string
	if err := aConn.QueryRow(ctx, `SELECT content,lifecycle_state FROM memories WHERE id=$1`, first.ID).Scan(&content, &state); err != nil || content != "verified original" || state != "active" {
		t.Fatal(content, state, err)
	}
}
