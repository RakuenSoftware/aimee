package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestPersonalMemoryAuthority(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		if os.Getenv("AIMEE_MEMORY_EVAL_REQUIRED") == "1" {
			t.Fatal("AIMEE_MEMORY_EVAL_URL required")
		}
		t.Skip("set AIMEE_MEMORY_EVAL_URL")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(context.Background())
	name := fmt.Sprintf("personal_authority_%d", time.Now().UnixNano())
	schema, role := pgx.Identifier{name}.Sanitize(), pgx.Identifier{name + "_runtime"}.Sanitize()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := conn.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec("CREATE SCHEMA " + schema + "; CREATE ROLE " + role + " NOINHERIT NOBYPASSRLS; GRANT USAGE ON SCHEMA " + schema + " TO " + role + "; ALTER DEFAULT PRIVILEGES IN SCHEMA " + schema + " GRANT ALL ON TABLES TO " + role + "; ALTER DEFAULT PRIVILEGES IN SCHEMA " + schema + " GRANT EXECUTE ON FUNCTIONS TO " + role + "; SET search_path=" + schema + ",public")
	defer func() {
		if _, err := conn.Exec(context.Background(), "DROP SCHEMA "+schema+" CASCADE; DROP ROLE "+role); err != nil {
			t.Error(err)
		}
	}()
	read := func(name string) string {
		t.Helper()
		b, err := os.ReadFile("../aimee/families/" + name)
		if err != nil {
			t.Fatal(err)
		}
		return string(b)
	}
	base := read("schema_conversation.sql")
	exec(base[strings.Index(base, "CREATE TABLE IF NOT EXISTS user_memories ("):strings.Index(base, "CREATE INDEX IF NOT EXISTS user_memories_recall")])
	exec(`INSERT INTO user_memories(id,key,content) VALUES(42,'legacy','unknown author')`)
	for _, name := range []string{"schema_personal_memory_changes.sql", "schema_personal_memory_versions.sql", "schema_personal_memory_acl.sql", "schema_personal_memory_authority.sql", "schema_personal_memory_proposals.sql"} {
		exec(read(name))
	}
	user := &bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "fixture:human", TransportIdentity: "fixture:http"}
	model := &bus.CommandContext{Authenticated: true, Principal: "fixture:agent", TransportIdentity: "fixture:mcp"}
	callOn := func(connection *pgx.Conn, caller *bus.CommandContext, req DataRequest) DataResponse {
		t.Helper()
		tx, err := connection.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		defer tx.Rollback(context.Background())
		if _, err := tx.Exec(ctx, "SET LOCAL ROLE "+role); err != nil {
			t.Fatal(err)
		}
		backend, err := NewPostgresDataStore(evalQueryer{tx}, PlacementServer)
		if err != nil {
			t.Fatal(err)
		}
		raw, status := handleData(handlerOptions{placement: PlacementServer, data: backend, commandContext: caller}, bus.ModuleInvocation{StageID: StageData}, dataRequest(t, req))
		var out DataResponse
		if status != bus.ModuleStatusOK || json.Unmarshal(raw, &out) != nil {
			t.Fatalf("operation %s status %v", req.Operation, status)
		}
		if err := tx.Commit(ctx); err != nil {
			t.Fatal(err)
		}
		return out
	}
	call := func(caller *bus.CommandContext, req DataRequest) DataResponse { return callOn(conn, caller, req) }
	one := func(out DataResponse) Record {
		t.Helper()
		if len(out.Records) != 1 {
			t.Fatal(out)
		}
		return out.Records[0]
	}
	confidence := 1.0
	put := func(caller *bus.CommandContext, key, kind, tier, content string) Record {
		t.Helper()
		return one(call(caller, DataRequest{Operation: "store", Authority: AuthorityUser, Key: key, Kind: kind, Tier: tier, Content: content, Confidence: &confidence}))
	}
	human := put(user, "human", "fact", "L2", "human assertion")
	agent := put(model, "agent", "fact", "L2", "model inference")
	if human.Authorship.Category != "user_stated" || human.Authorship.Principal != user.Principal || human.Confidence != 1 || agent.Authorship.Category != "agent_message" || agent.Authorship.Principal != model.Principal || agent.Confidence != .8 {
		t.Fatal("incorrect verified provenance", human.Authorship, agent.Authorship)
	}
	for _, caller := range []*bus.CommandContext{nil, {Authenticated: false, UserAuthority: true, Principal: "forged"}, {Authenticated: true, UserAuthority: true}, {Authenticated: true, Principal: "model"}} {
		record := put(caller, fmt.Sprintf("forged-%p", caller), "fact", "L5", "untrusted assertion")
		if record.Authorship.Category != "agent_message" || record.Confidence != .5 {
			t.Fatal("body or incomplete context elevated authority", record)
		}
	}
	generation := func() int64 {
		t.Helper()
		var n int64
		if err := conn.QueryRow(ctx, `SELECT generation FROM user_memory_collection_generation`).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}
	for _, id := range []int64{42, human.ID} {
		before := generation()
		for _, operation := range []string{"supersede", "delete", "store"} {
			key := "human"
			if id == 42 {
				key = "legacy"
			}
			out := call(model, DataRequest{Operation: operation, Authority: AuthorityUser, ID: id, Key: key, Kind: "fact", Tier: "L2", Content: "attempted overwrite", Confidence: &confidence})
			if out.Code == nil || *out.Code != MutationReviewRequired {
				t.Fatalf("%s accepted authoritative mutation: %+v", operation, out)
			}
		}
		if generation() != before {
			t.Fatal("refusal published a change")
		}
	}
	// The same host identity on MCP remains a model; context is not authority by itself.
	sameUserModel := *model
	sameUserModel.Principal = user.Principal
	if out := call(&sameUserModel, DataRequest{Operation: "supersede", Authority: AuthorityUser, ID: human.ID, Content: "MCP rewrite", Confidence: &confidence}); out.Code == nil || *out.Code != MutationReviewRequired {
		t.Fatal(out)
	}
	version := one(call(nil, DataRequest{Operation: "get", ID: agent.ID, IncludeVersion: true})).Version
	corrected := one(call(model, DataRequest{Operation: "supersede", ID: agent.ID, Content: "corrected inference", Confidence: &confidence, ExpectedVersion: version}))
	if corrected.ID != agent.ID || corrected.Confidence != .8 || corrected.Version.RecordRevision == version.RecordRevision {
		t.Fatal(corrected)
	}
	prior := one(call(nil, DataRequest{Operation: "get", ID: agent.ID, AtVersion: version}))
	if prior.Content != agent.Content || prior.Authorship.Category != "agent_message" || prior.Authorship.Principal != model.Principal {
		t.Fatal(prior)
	}
	legacy := one(call(user, DataRequest{Operation: "supersede", Authority: AuthorityUser, ID: 42, Content: "verified legacy correction", Confidence: &confidence}))
	if legacy.Authorship.Category != "user_stated" {
		t.Fatal(legacy)
	}
	var legacyOrigin string
	if err := conn.QueryRow(ctx, `SELECT record->>'provenance_category' FROM user_memory_versions WHERE memory_id=42`).Scan(&legacyOrigin); err != nil || legacyOrigin != "unknown" {
		t.Fatal("invented legacy author", legacyOrigin, err)
	}
	for _, kind := range []string{"episode", "experience", "instruction", "policy"} {
		record := put(user, kind, kind, "L2", "protected assertion")
		for _, caller := range []*bus.CommandContext{user, model} {
			out := call(caller, DataRequest{Operation: "supersede", Authority: AuthorityUser, ID: record.ID, Content: "rewrite protected assertion", Confidence: &confidence})
			if out.Code == nil {
				t.Fatal("protected kind rewritten", kind, out)
			}
		}
		if out := call(user, DataRequest{Operation: "delete", Authority: AuthorityUser, ID: record.ID}); !out.Deleted {
			t.Fatal("authorized revocation failed", out)
		}
	}

	t.Run("delayed automatic archive cannot reactivate human retirement", func(t *testing.T) {
		archive := put(model, "archive:synthetic-project/retired-instruction", "archive", "L1", "synthetic archived instruction")
		if !call(user, DataRequest{Operation: "delete", Authority: AuthorityUser, ID: archive.ID}).Deleted {
			t.Fatal("human archive retirement failed")
		}
		retiredGeneration := generation()
		fresh, err := pgx.Connect(ctx, dsn)
		if err != nil {
			t.Fatal(err)
		}
		defer fresh.Close(context.Background())
		if _, err := fresh.Exec(ctx, "SET search_path="+schema+",public"); err != nil {
			t.Fatal(err)
		}
		// The automatic archive hook supplies no user authority. Even a fresh
		// connection and the original payload must honor the retained same key.
		request := DataRequest{Operation: "store", Authority: AuthorityModel, Kind: "archive", Key: archive.Key, Tier: "L1", Content: archive.Content, Confidence: &confidence}
		refused := callOn(fresh, model, request)
		if refused.Code == nil || *refused.Code != MutationReviewRequired || len(refused.Records) != 0 {
			t.Fatal("delayed automatic archive reactivated retirement", refused)
		}
		if generation() != retiredGeneration || len(callOn(fresh, nil, DataRequest{Operation: "get", ID: archive.ID}).Records) != 0 {
			t.Fatal("automatic archive changed generation or current read")
		}
		search := callOn(fresh, nil, DataRequest{Operation: "search", Query: "synthetic archived instruction", Kind: "archive", Limit: 10})
		for _, row := range search.Records {
			if row.ID == archive.ID {
				t.Fatal("retired archive returned by current search")
			}
		}
		var retainedID int64
		var retainedState string
		if err := fresh.QueryRow(ctx, "SELECT id,lifecycle_state FROM user_memories WHERE kind='archive' AND key=$1", archive.Key).Scan(&retainedID, &retainedState); err != nil || retainedID != archive.ID || retainedState != "retired" {
			t.Fatal("canonical retirement did not survive fresh-connection upsert", err, retainedID, retainedState)
		}
		// This is suppression of automatic recreation, not a permanent ban on
		// a new authenticated user instruction restoring the same identity.
		request.Authority = AuthorityUser
		restored := callOn(fresh, user, request)
		if len(restored.Records) != 1 || restored.Records[0].ID != archive.ID || len(callOn(fresh, nil, DataRequest{Operation: "get", ID: archive.ID}).Records) != 1 {
			t.Fatal("explicit user reactivation contract changed", restored)
		}
	})
	for _, sql := range []string{
		`UPDATE user_memories SET content='raw overwrite' WHERE key='human'`,
		`UPDATE user_memories SET provenance_category='agent_message' WHERE key='human'`,
		`DELETE FROM user_memories WHERE key='human'`, `TRUNCATE user_memories`,
	} {
		tx, err := conn.Begin(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = tx.Exec(ctx, "SET LOCAL ROLE "+role); err != nil {
			t.Fatal(err)
		}
		_, err = tx.Exec(ctx, sql)
		_ = tx.Rollback(ctx)
		if err == nil {
			t.Fatal("runtime bypassed private authority", sql)
		}
	}
	// Read accounting and maintenance must not rewrite protected authorship.
	exec(`UPDATE user_memories SET use_count=9,last_used_at=now() WHERE key='human'`)
	before := generation()
	tx, err := conn.Begin(ctx)
	if err != nil {
		t.Fatal(err)
	}
	backend := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementServer}
	if _, _, _, err = backend.Maintenance(ctx, Scope{Type: ScopeUser}); err != nil {
		t.Fatal(err)
	}
	if err = tx.Commit(ctx); err != nil {
		t.Fatal(err)
	}
	if generation() != before {
		t.Fatal("maintenance changed protected high-confidence content")
	}
}
