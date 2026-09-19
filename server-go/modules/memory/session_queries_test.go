package memory

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

// Exercise the public commands through the restricted-role transaction owner.
// These assertions replace the native top-L2/session-priority wrapper tests.
func exerciseSessionQueryReplay(t *testing.T, ctx context.Context, tx pgx.Tx, handler bus.ModuleHandler) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT session_queries`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT session_queries; RELEASE SAVEPOINT session_queries`) }()
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	seed := func(tier, kind, scope, value, content string, confidence float64, useCount int) int64 {
		t.Helper()
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value,confidence,use_count)
 VALUES($1,$2,'session-query-fixture',$3,$4,$5,$6,$7) RETURNING id`, tier, kind, content, scope, value, confidence, useCount).Scan(&id); err != nil {
			t.Fatal(err)
		}
		return id
	}
	content := "session-query-fixture " + strings.Repeat("界", 2000)
	local := seed("L2", "fact", "project", "session-query-project", content, .1, 0)
	workspace := seed("L2", "fact", "workspace", "session-query-workspace", "session-query-fixture workspace", .2, 1)
	var private int64
	for i := 0; i < 12; i++ {
		seed("L2", "fact", "global", "_global", "session-query-fixture global", 1, 1000)
		private = seed("L2", "fact", "project", "session-query-private", "session-query-fixture private", 1, 1000)
	}
	run := func(verb string, max int, all bool, project, workspace, pattern string) []publicMemoryRecord {
		t.Helper()
		args, _ := json.Marshal(map[string]any{"max": max, "include_all": all, "project": project, "workspace": workspace, "scope_context": true, "pattern": pattern})
		frame, _ := bus.EncodeCommand(verb, args)
		encoded, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
		if status != bus.ModuleStatusOK {
			t.Fatal(verb, status)
		}
		body, err := bus.DecodeCommandResult(encoded)
		if err != nil {
			t.Fatal(err)
		}
		var result struct {
			Status   string               `json:"status"`
			Memories []publicMemoryRecord `json:"memories"`
		}
		if err = json.Unmarshal(body, &result); err != nil || result.Status != "ok" {
			t.Fatal(verb, string(body), err)
		}
		return result.Memories
	}
	for _, verb := range []string{"top_l2_facts", "list_session_scope_priority", "list_session_scope_priority_like"} {
		rows := run(verb, 2, false, "session-query-project", "session-query-workspace", "%session-query-fixture%")
		if len(rows) != 2 || rows[0].ID != local || rows[1].ID != workspace || rows[0].Content != content {
			t.Fatalf("%s scope/full text: %+v", verb, rows)
		}
		rows = run(verb, 1, false, "session-query-project", "session-query-workspace", "%session-query-fixture%")
		if len(rows) != 1 || rows[0].ID != local {
			t.Fatal(verb, rows)
		}
		rows = run(verb, 64, true, "session-query-project", "session-query-workspace", "%session-query-fixture%")
		if len(rows) < 26 || rows[0].ID != local || rows[1].ID != workspace {
			t.Fatal(verb, "all scope prefix")
		}
		foundPrivate := false
		for _, r := range rows {
			if r.Content == "session-query-fixture private" {
				foundPrivate = true
			}
		}
		if !foundPrivate {
			t.Fatal(verb, "all scope omitted foreign rows")
		}
		rows = run(verb, 64, false, "", "", "%session-query-fixture%")
		for _, r := range rows {
			if r.ID == local || r.ID == workspace || r.Content == "session-query-fixture private" {
				t.Fatal(verb, "scope leak", r.ID)
			}
		}
	}
	// Once visibility order is satisfied, retain the distinct within-scope
	// ranking and eligibility of each query mode.
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	workflow := seed("L3", "workflow", "project", "session-query-project", "session-query-fixture workflow", .1, 0)
	decision := seed("L1", "decision", "project", "session-query-project", "session-query-fixture decision", .1, 0)
	bestFact := seed("L2", "fact", "project", "session-query-project", "session-query-fixture best fact", .9, 5)
	seed("L4", "fact", "project", "session-query-project", "session-query-fixture excluded L4", 1, 10000)
	seed("L0", "scratch", "project", "session-query-project", "session-query-fixture excluded L0", 1, 10000)
	rows := run("top_l2_facts", 2, false, "session-query-project", "session-query-workspace", "")
	if len(rows) != 2 || rows[0].ID != bestFact || rows[1].ID != local {
		t.Fatal("top L2 ranking", rows)
	}
	for _, verb := range []string{"list_session_scope_priority", "list_session_scope_priority_like"} {
		rows = run(verb, 3, false, "session-query-project", "session-query-workspace", "%session-query-fixture%")
		if len(rows) != 3 || rows[0].ID != workflow || rows[1].ID != decision || rows[2].ID != bestFact {
			t.Fatal("kind/tier ranking", verb, rows)
		}
	}
	rows = run("list_session_scope_priority_like", 64, false, "session-query-project", "session-query-workspace", "%nonexistent-session-query-pattern%")
	if len(rows) != 0 {
		t.Fatal("pattern ignored", rows)
	}
	// Batch touch replaces the retired native single/batch adapters. Duplicates
	// count once, RLS excludes foreign rows, and absent/nonpositive IDs do not
	// invent updates. Read the stored effect, not just a successful envelope.
	request := DataRequest{Operation: "touch", Project: "session-query-project", IDs: []int64{local, local, workspace, private, -1, 9223372036854775807}}
	raw, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, request))
	var response DataResponse
	if status != bus.ModuleStatusOK || json.Unmarshal(raw, &response) != nil || response.Count == nil || *response.Count != 1 {
		t.Fatal("batch touch", status, string(raw))
	}
	exec(`SELECT set_config('aimee.memory_scope_all','1',true)`)
	for id, want := range map[int64]int{local: 1, workspace: 1, private: 1000} {
		var count int
		var lastUsed string
		if err := tx.QueryRow(ctx, `SELECT use_count,COALESCE(last_used_at,'') FROM memories WHERE id=$1`, id).Scan(&count, &lastUsed); err != nil || count != want || id == local && lastUsed == "" {
			t.Fatal("touch scope/count", id, count, lastUsed, err)
		}
	}
	for _, ids := range [][]int64{nil, make([]int64, 257)} {
		request.IDs = ids
		if _, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, request)); status != bus.ModuleStatusInvalidRequest {
			t.Fatal("touch bounds", len(ids), status)
		}
	}

}
