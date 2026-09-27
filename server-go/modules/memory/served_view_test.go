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

func TestServedViewProtectedPacking(t *testing.T) {
	items := []servedItem{{Kind: "episode", ID: "3", Content: "small", Priority: 4}, {Kind: "constraint", ID: "1", Content: strings.Repeat("required", 200), Priority: 0}, {Kind: "contradiction", ID: "2", Content: []string{"left", "right"}, Priority: 1}}
	got, omitted, text, err := packServedItems(items, 64, 256)
	if err != nil || len(got) != 0 || len(omitted) != 3 || text != "" {
		t.Fatal("required bundle omission admitted discretionary evidence", got, omitted, err)
	}
	got, omitted, text, err = packServedItems(items, 2, 16384)
	if err != nil || len(got) != 2 || got[0].Kind != "constraint" || got[1].Kind != "contradiction" || len(omitted) != 1 || !strings.Contains(text, "right") {
		t.Fatal(got, omitted, err)
	}
	zero, _, text, err := packServedItems(items, 0, 0)
	if err != nil || len(zero) != 0 || text != "" {
		t.Fatal("literal zero ignored")
	}
}

func TestServedViewPublicValidation(t *testing.T) {
	handler := NewHandler(nil, WithDataStore(PlacementKB, nil))
	for _, raw := range []string{`{"view":"bogus","task":"x"}`, `{"view":"briefing","task":""}`, `{"view":"historical_context","task":"x"}`, `{"view":"current_state","task":"x","valid_at":"2025-01-01T00:00:00Z"}`, `{"view":"briefing","task":"x","limit":1.2}`, `{"view":"briefing","task":"x","context_limits":{"schema_version":1,"max_context_tokens":5}}`} {
		frame, _ := bus.EncodeCommand("serve", []byte(raw))
		reply, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
		if status != bus.ModuleStatusOK {
			t.Fatal(status)
		}
		body, _ := bus.DecodeCommandResult(reply)
		if !strings.Contains(string(body), `"status":"error"`) {
			t.Fatal(string(body))
		}
	}
	frame, _ := bus.EncodeCommand("serve", []byte(`{"view":"briefing","task":"x"}`))
	if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 123}, frame); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("plugin forged owner", status)
	}
}

func TestServedViewsOwnerAndCachePostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_DB2_REPLAY_URL")
	if dsn == "" {
		t.Skip("AIMEE_DB2_REPLAY_URL required")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.RepeatableRead})
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
	exec(`SET LOCAL jit=off; DO $$ BEGIN IF NOT EXISTS(SELECT FROM pg_roles WHERE rolname='aimee_store_runtime') THEN CREATE ROLE aimee_store_runtime NOINHERIT NOBYPASSRLS; END IF; END $$; GRANT USAGE ON SCHEMA public TO aimee_store_runtime; DELETE FROM memories; DELETE FROM entity_edges; DELETE FROM rules; SELECT set_config('aimee.memory_scope_all','0',true),set_config('aimee.memory_project','mr12-visible',true),set_config('aimee.memory_scope_type','project',true),set_config('aimee.memory_scope_value','mr12-visible',true),set_config('aimee.principal','mr12-operator',true)`)
	schema, err := os.ReadFile("../../../src/modules/kb/c/schema.sql")
	if err != nil {
		t.Fatal(err)
	}
	start, end := strings.Index(string(schema), "DO $memory_store_grants$"), strings.Index(string(schema), "END\n$memory_store_grants$;")
	if start < 0 || end < start {
		t.Fatal("runtime grants unavailable")
	}
	exec(string(schema[start : end+len("END\n$memory_store_grants$;")]))
	backend := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementKB}
	request := DataRequest{Scope: Scope{Type: ScopeProject, Value: "mr12-visible"}, Project: "mr12-visible", Query: "deployment", Limit: 64, Assertions: &assertionSearchRequest{}, ServedView: &servedViewRequest{View: "active_constraints", Task: "deployment", Limit: 32}}
	call := func() servedViewResult {
		t.Helper()
		exec(`SET LOCAL ROLE aimee_store_runtime`)
		r, err := backend.serveView(ctx, request, true)
		if err != nil {
			t.Fatal(err)
		}
		exec(`RESET ROLE`)
		if r.Accounting.RenderedBytes != len(r.Rendered) || r.Accounting.Digest != r.Receipt["payload_sha256"] {
			t.Fatal("receipt disagrees")
		}
		return r
	}
	empty := call()
	again := call()
	if len(empty.Selected) != 0 || again.Cache["state"] != "projection_reused" || empty.Receipt["invocation_id"] == again.Receipt["invocation_id"] {
		t.Fatal("empty cache or receipt", empty, again)
	}
	var a, b, hidden int64
	insert := func(key, kind, scope, content string) int64 {
		t.Helper()
		var id int64
		if err := tx.QueryRow(ctx, `INSERT INTO memories(key,kind,scope_type,scope_value,content,provenance_category) VALUES($1,$2,'project',$3,$4,'user_stated') RETURNING id`, key, kind, scope, content).Scan(&id); err != nil {
			t.Fatal(err)
		}
		return id
	}
	a = insert("mr12-constraint", "constraint", "mr12-visible", "require approval")
	withConstraint := call()
	if len(withConstraint.Selected) != 1 || withConstraint.Cache["identity"] == empty.Cache["identity"] {
		t.Fatal("insert reused empty view", withConstraint)
	}
	// Rule collection dependencies must invalidate a cached projection even when
	// none of its selected memory records changed.
	exec(`INSERT INTO rules(polarity,title,description,directive_type,created_at,updated_at) VALUES('must','MR12 protected rule','require release review','hard',now()::text,now()::text)`)
	withRule := call()
	if withRule.Cache["identity"] == withConstraint.Cache["identity"] || len(withRule.Selected) != 2 || withRule.Selected[0].Kind != "hard_constraints" || !strings.Contains(withRule.Rendered, "require release review") {
		t.Fatal("new hard rule did not invalidate and precede cached memory", withRule)
	}
	if strings.Contains(withRule.Rendered, "record_revision") || strings.Contains(withRule.Rendered, "calibration_state") {
		t.Fatal("rich diagnostics leaked into model projection")
	}
	exec(`DELETE FROM rules`)
	request.ServedView.Task = "another task"
	request.Query = "another task"
	other := call()
	if other.Cache["identity"] == withConstraint.Cache["identity"] {
		t.Fatal("task identity omitted")
	}
	request.ServedView.View = "open_contradictions"
	emptyConflict := call()
	if len(emptyConflict.Selected) != 0 {
		t.Fatal(emptyConflict)
	}
	b = insert("mr12-other", "fact", "mr12-visible", "deployment can proceed")
	hidden = insert("mr12-hidden", "fact", "mr12-hidden", "secret deployment")
	exec(`INSERT INTO memory_conflicts(memory_a,memory_b,detected_at) VALUES($1,$2,pg_now_text()),($1,$3,pg_now_text())`, a, b, hidden)
	conflicts := call()
	if len(conflicts.Selected) != 1 || conflicts.Cache["identity"] == emptyConflict.Cache["identity"] || strings.Contains(conflicts.Rendered, "secret") {
		t.Fatal("conflict admission", conflicts)
	}
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	card, err := backend.claimCard(ctx, a)
	exec(`RESET ROLE`)
	if err != nil {
		t.Fatal(err)
	}
	encoded, _ := json.Marshal(card)
	if !strings.Contains(string(encoded), "expected_version") || !strings.Contains(string(encoded), `"calibration_state":"unknown"`) {
		t.Fatal("card missed governed correction", string(encoded))
	}
	if err := backend.expandClaimEvidence(ctx, card); err != nil {
		t.Fatal(err)
	}
	if card["expanded_evidence"] == nil {
		t.Fatal("missing authorized expansion")
	}
	// Re-read the unexpanded projection so the comparison isolates a source edit.
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	card, err = backend.claimCard(ctx, a)
	exec(`RESET ROLE`)
	if err != nil {
		t.Fatal(err)
	}
	oldDigest := card["digest"]
	exec(`UPDATE memories SET content='corrected requirement' WHERE id=$1`, a)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	card, err = backend.claimCard(ctx, a)
	exec(`RESET ROLE`)
	if err != nil || card["digest"] == oldDigest {
		t.Fatal("canonical edit did not invalidate card", err)
	}
	unknownFailure := insert("mr12-unbacked-failure", "failure", "mr12-visible", "another task failed")
	request.ServedView.View = "known_failures"
	backed := call()
	if len(backed.Selected) != 1 {
		t.Fatal("canonical origin event unavailable", backed)
	}
	// The owner emits an origin event even for this SQL fixture; add an
	// uninterpretable upstream lineage edge to model incomplete evidence.
	exec(`INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref) VALUES('memory',$1,'unsupported-fixture-origin','missing')`, unknownFailure)
	unbacked := call()
	if len(unbacked.Selected) != 0 || len(unbacked.Omissions) == 0 {
		t.Fatal("failure label alone became evidence", unbacked)
	}
	exec(`DELETE FROM memories WHERE id=$1`, unknownFailure)
	// Every named composition uses the same owner and returns explicit limits.
	savedView, savedTask := request.ServedView.View, request.ServedView.Task
	for _, view := range servedViewNames {
		request.ServedView.View = view
		request.Assertions = &assertionSearchRequest{}
		if view == "historical_context" {
			request.Assertions = &assertionSearchRequest{Historical: true, ValidAt: "2025-01-01T00:00:00Z"}
		}
		got := call()
		if got.View != view {
			t.Fatal("view was relabeled", got.View, view)
		}
		for _, item := range got.Selected {
			for _, ref := range item.Sources {
				if !validTypedSource(ref) {
					t.Fatal("invalid shared release source", ref)
				}
			}
		}
		if view == "historical_context" {
			request.Assertions.ValidAt = "2025-02-01T00:00:00Z"
			later := call()
			if got.Cache["identity"] == later.Cache["identity"] {
				t.Fatal("historical times collided")
			}
		}
	}
	request.ServedView.View, request.ServedView.Task = savedView, savedTask
	request.Assertions = &assertionSearchRequest{}
	changed := call()
	if changed.Cache["identity"] == conflicts.Cache["identity"] {
		t.Fatal("canonical edit reused view")
	}
	exec(`UPDATE memories SET lifecycle_state='revoked' WHERE id=$1`, b)
	if r := call(); len(r.Selected) != 0 {
		t.Fatal("revoked side served", r)
	}
}

func TestServedViewsPrivatePlacementPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("AIMEE_MEMORY_EVAL_URL required")
	}
	ctx := context.Background()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(ctx)
	tx, err := conn.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.RepeatableRead})
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback(ctx)
	exec := func(q string) {
		t.Helper()
		if _, err := tx.Exec(ctx, q); err != nil {
			t.Fatal(err)
		}
	}
	read := func(name string) string {
		t.Helper()
		b, err := os.ReadFile("../aimee/families/" + name)
		if err != nil {
			t.Fatal(err)
		}
		return string(b)
	}
	exec(`CREATE SCHEMA mr12_private_fixture; SET LOCAL search_path=mr12_private_fixture,public; SET LOCAL jit=off`)
	base := read("schema_conversation.sql")
	a, b := strings.Index(base, "CREATE TABLE IF NOT EXISTS user_memories ("), strings.Index(base, "CREATE INDEX IF NOT EXISTS user_memories_recall")
	if a < 0 || b <= a {
		t.Fatal("schema missing")
	}
	exec(base[a:b])
	for _, name := range []string{"schema_personal_memory_changes.sql", "schema_personal_memory_versions.sql", "schema_personal_memory_authority.sql", "schema_personal_memory_proposals.sql", "schema_personal_memory_horizon_index.sql"} {
		exec(read(name))
	}
	exec(`CREATE TEMP TABLE memories(id bigint,content text); INSERT INTO memories VALUES(1,'SHARED SENTINEL MUST NOT APPEAR')`)
	backend := &postgresDataStore{db: evalQueryer{tx}, placement: PlacementServer}
	created, err := backend.mutatePersonal(ctx, "store", Record{Scope: Scope{Type: ScopeUser}, Key: "private-rule", Kind: "constraint", Tier: "L2", Content: "Private scoped requirement", Confidence: .8}, nil)
	if err != nil {
		t.Fatal(err)
	}
	request := DataRequest{Scope: Scope{Type: ScopeUser}, Query: "deployment", Limit: 64, Assertions: &assertionSearchRequest{}, ServedView: &servedViewRequest{View: "briefing", Task: "deployment", Limit: 32}}
	result, err := backend.serveView(ctx, request, false)
	if err != nil {
		t.Fatal(err)
	}
	if len(result.Selected) != 1 || result.Store != "user" || result.Status != "degraded" || strings.Contains(result.Rendered, "SHARED SENTINEL") {
		t.Fatal("private view crossed owners", result)
	}
	for _, item := range result.Selected {
		for _, source := range item.Sources {
			if !validTypedSource(source) {
				t.Fatal("invalid private release reference", source)
			}
		}
	}
	card, err := backend.claimCard(ctx, created.ID)
	if err != nil || card["store"] != "user" {
		t.Fatal(card, err)
	}
	request.ServedView.View = "historical_context"
	request.Assertions = &assertionSearchRequest{Historical: true, ValidAt: "2025-01-01T00:00:00Z"}
	historical, err := backend.serveView(ctx, request, false)
	if err != nil || len(historical.Selected) != 0 || historical.Status != "degraded" {
		t.Fatal("private history mislabeled", historical, err)
	}
}
