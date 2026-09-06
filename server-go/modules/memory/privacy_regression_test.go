package memory

import (
	"context"
	"encoding/json"
	"os"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

// Both tables deliberately contain ID 42. Verify the actual SQL owner, not a
// mock that would return the same row regardless of the table queried.
func TestPersonalMemoryPrivacyRegression(t *testing.T) {
	url := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if url == "" {
		if os.Getenv("AIMEE_MEMORY_EVAL_REQUIRED") == "1" {
			t.Fatal("AIMEE_MEMORY_EVAL_URL required")
		}
		t.Skip("set AIMEE_MEMORY_EVAL_URL for PostgreSQL privacy regression")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, url)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	schema, err := os.ReadFile("../aimee/families/schema_conversation.sql")
	if err != nil {
		t.Fatal(err)
	}
	// Use the shipping user table definition, isolated in the session's temp schema.
	start, end := "CREATE TABLE IF NOT EXISTS user_memories (", "CREATE INDEX IF NOT EXISTS user_memories_recall"
	text := string(schema)
	a, b := strings.Index(text, start), strings.Index(text, end)
	if a < 0 || b <= a {
		t.Fatal("user memory schema missing")
	}
	_, err = tx.Exec(ctx, strings.Replace(text[a:b], "CREATE TABLE IF NOT EXISTS", "CREATE TEMP TABLE", 1)+`
CREATE TEMP TABLE memories (
 id bigint PRIMARY KEY,scope_type text,scope_value text,tier text,kind text,key text,
 content text,confidence double precision,lifecycle_state text);
INSERT INTO memories VALUES(42,'global','_global','L0','fact','kb-fixture','shared knowledge',1,'active');
INSERT INTO user_memories(id,key,content) VALUES(42,'private-fixture','PII fixture: local only');`)
	if err != nil {
		t.Fatal(err)
	}
	call := func(placement Placement, req DataRequest) (DataResponse, bus.ModuleStatus) {
		t.Helper()
		backend, err := NewPostgresDataStore(evalQueryer{tx}, placement)
		if err != nil {
			t.Fatal(err)
		}
		raw, status := NewHandler(nil, WithDataStore(placement, backend))(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, req))
		var reply DataResponse
		if status == bus.ModuleStatusOK {
			if err := json.Unmarshal(raw, &reply); err != nil {
				t.Fatal(err)
			}
			if req.Operation == "get" && len(reply.Records) == 0 {
				var wire map[string]json.RawMessage
				_ = json.Unmarshal(raw, &wire)
				if string(wire["records"]) != "[]" {
					t.Fatalf("empty lookup omitted records: %s", raw)
				}
			}
		}
		return reply, status
	}
	check := func(place Placement, want string) {
		t.Helper()
		r, s := call(place, DataRequest{Operation: "get", ID: 42})
		if s != bus.ModuleStatusOK || len(r.Records) != 1 || r.Records[0].Content != want {
			t.Fatalf("%s get: %+v status=%v", place, r, s)
		}
	}
	check(PlacementServer, "PII fixture: local only")
	check(PlacementKB, "shared knowledge")
	for _, p := range []struct {
		placement Placement
		scope     string
	}{{PlacementServer, "global"}, {PlacementKB, "user"}} {
		_, status := call(p.placement, DataRequest{Operation: "get", ID: 42, Scope: Scope{Type: p.scope}})
		if status != bus.ModuleStatusInvalidRequest {
			t.Fatalf("cross-placement scope accepted: %s %s", p.placement, p.scope)
		}
	}

	// General list/search must not silently collapse the requested KB context
	// to global scope. Exact scope queries remain exact; all appends other KBs.
	_, err = tx.Exec(ctx, `ALTER TABLE memories ADD COLUMN use_cases text DEFAULT '', ADD COLUMN updated_at timestamptz DEFAULT now();
INSERT INTO memories(id,scope_type,scope_value,tier,kind,key,content,confidence,lifecycle_state) VALUES
(43,'project','project-a','L0','fact','project-a','shared project',1,'active'),
(44,'workspace','workspace-a','L0','fact','workspace-a','shared workspace',1,'active'),
(45,'project','project-b','L0','fact','project-b','other project',1,'active');`)
	if err != nil {
		t.Fatal(err)
	}
	for _, op := range []string{"list", "search", "visible-search"} {
		for _, all := range []bool{false, true} {
			reply, status := call(PlacementKB, DataRequest{Operation: op, Project: "project-a", Workspace: "workspace-a", IncludeAll: all, Limit: 10})
			want := []int64{43, 44, 42}
			if all {
				want = append(want, 45)
			}
			if status != bus.ModuleStatusOK || len(reply.Records) != len(want) {
				t.Fatalf("%s all=%v: %+v status=%v", op, all, reply, status)
			}
			for i, id := range want {
				if reply.Records[i].ID != id {
					t.Fatalf("%s all=%v rank %d: got %d want %d", op, all, i, reply.Records[i].ID, id)
				}
			}
		}
	}
	exact, exactStatus := call(PlacementKB, DataRequest{Operation: "search", Scope: Scope{Type: ScopeProject, Value: "project-b"}, IncludeAll: true, Limit: 10})
	if exactStatus != bus.ModuleStatusOK || len(exact.Records) != 1 || exact.Records[0].ID != 45 {
		t.Fatalf("exact query widened: %+v status=%v", exact, exactStatus)
	}
	confidence := 0.8
	r, status := call(PlacementServer, DataRequest{Operation: "supersede", ID: 42, Content: "corrected private fixture", Confidence: &confidence})
	if status != bus.ModuleStatusOK || len(r.Records) != 1 || r.Records[0].ID != 42 {
		t.Fatalf("replace: %+v status=%v", r, status)
	}
	check(PlacementServer, "corrected private fixture")
	check(PlacementKB, "shared knowledge")
	r, status = call(PlacementServer, DataRequest{Operation: "delete", ID: 42})
	if status != bus.ModuleStatusOK || !r.Deleted {
		t.Fatalf("delete: %+v status=%v", r, status)
	}
	r, status = call(PlacementServer, DataRequest{Operation: "get", ID: 42})
	if status != bus.ModuleStatusOK || len(r.Records) != 0 {
		t.Fatalf("retired local lookup: %+v %v", r, status)
	}
	check(PlacementKB, "shared knowledge")
	r, status = call(PlacementServer, DataRequest{Operation: "review-list", State: "retired", Limit: 10})
	if status != bus.ModuleStatusOK || len(r.Reviews) != 1 || r.Reviews[0].ScopeType != "user" {
		t.Fatalf("local review: %+v %v", r, status)
	}
	// Historical KB handles remain readable after retirement, independently of
	// the user's record with the same ID.
	if _, err = tx.Exec(ctx, "UPDATE memories SET lifecycle_state='retired' WHERE id=42"); err != nil {
		t.Fatal(err)
	}
	retired, retiredStatus := call(PlacementKB, DataRequest{Operation: "get", ID: 42})
	if retiredStatus != bus.ModuleStatusOK || len(retired.Records) != 0 {
		t.Fatalf("ordinary lookup exposed retired KB record: %+v %v", retired, retiredStatus)
	}
	historical, historicalStatus := call(PlacementKB, DataRequest{Operation: "get", ID: 42, AsOf: "2020-01-01T00:00:00Z"})
	if historicalStatus != bus.ModuleStatusOK || len(historical.Records) != 1 || historical.Records[0].Content != "shared knowledge" {
		t.Fatalf("historical lookup: %+v %v", historical, historicalStatus)
	}
	_, userHistoryStatus := call(PlacementServer, DataRequest{Operation: "get", ID: 42, AsOf: "2020-01-01T00:00:00Z"})
	if userHistoryStatus != bus.ModuleStatusInvalidRequest {
		t.Fatalf("personal history accepted: %v", userHistoryStatus)
	}
}
