package memory

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestTypedFactCompatibilityGate(t *testing.T) {
	for relation, kinds := range typedFactKinds {
		in, err := typedFactAssertion("subject", kinds[0], relation, "object", kinds[1], 130, "source", "2026-01-01")
		if err != nil || in.Confidence != 1 || in.Actor.Rank != 20 || in.Actor.Role != "system" || !in.Functional {
			t.Fatalf("%s: %+v %v", relation, in, err)
		}
		if _, err := typedFactAssertion("subject", "wrong", relation, "object", kinds[1], 70, "source", ""); err == nil {
			t.Fatal("accepted wrong subject kind")
		}
	}
	for _, kind := range []string{"", "scalar", "value"} {
		in, err := typedFactAssertion("p", "project", "naming_convention", "BEM", kind, -1, "s", "")
		if err != nil || in.Confidence != 0 {
			t.Fatal(in, err)
		}
	}
	for _, spec := range [][3]string{{"project", "vibes", "scalar"}, {"project", "should_match", "convention"}, {"project", "naming_convention", "device"}} {
		if _, err := typedFactAssertion("p", spec[0], spec[1], "v", spec[2], 50, "s", ""); err == nil {
			t.Fatal(spec)
		}
	}
}

func TestCSSConventionsHostBoundary(t *testing.T) {
	for _, op := range []string{"css-conventions", "css-convention-sync"} {
		frame, _ := bus.EncodeCommand("runtime", []byte(`{"operation":"`+op+`","project":"css-replay"}`))
		if _, status := NewHandler(nil, WithDataStore(PlacementKB, nil))(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 73}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(status)
		}
		if _, status := NewHandler(nil, WithDataStore(PlacementServer, nil))(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusCapabilityAbsent {
			t.Fatal(status)
		}
	}
}

func exerciseCSSConventionsReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT css_conventions_replay`)
	defer func() { exec(`ROLLBACK TO SAVEPOINT css_conventions_replay; RELEASE SAVEPOINT css_conventions_replay`) }()
	exec(`RESET ROLE; SELECT set_config('aimee.memory_scope_all','1',true)`)
	exec(`INSERT INTO projects(name,root,scanned_at) VALUES('css-replay','/css-replay','2026-01-01')`)
	exec(`INSERT INTO files(project_id,path,scanned_at) SELECT id,'styles.css','2026-01-01' FROM projects WHERE name='css-replay'`)
	exec(`INSERT INTO css_rules(file_id,selector) SELECT f.id,'.card__title' FROM files f JOIN projects p ON p.id=f.project_id WHERE p.name='css-replay'`)
	exec(`INSERT INTO css_declarations(rule_id,property,value) SELECT c.id,'--brand','#fff' FROM css_rules c JOIN files f ON f.id=c.file_id JOIN projects p ON p.id=f.project_id WHERE p.name='css-replay'`)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	enabled := false
	bound := *backend
	bound.settings = func() (map[string]any, error) { return map[string]any{"css_style_graph_enabled": enabled}, nil }
	handler := NewHandler(nil, WithDataStore(PlacementKB, &bound))
	call := func(op string, want bus.ModuleStatus) map[string]json.RawMessage {
		t.Helper()
		// A host-supplied actor never upgrades the derivation's system authority.
		frame, _ := bus.EncodeCommand("runtime", []byte(`{"operation":"`+op+`","project":"css-replay","actor":{"rank":40,"role":"operator"}}`))
		raw, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
		if status != want {
			t.Fatalf("%s: status=%v wanted=%v reply=%s", op, status, want, raw)
		}
		if status != bus.ModuleStatusOK {
			return nil
		}
		raw, decodeErr := bus.DecodeCommandResult(raw)
		if decodeErr != nil {
			t.Fatal(decodeErr)
		}
		var envelope struct {
			JSON string `json:"json"`
		}
		if err := json.Unmarshal(raw, &envelope); err != nil {
			t.Fatal(err)
		}
		var out map[string]json.RawMessage
		if err := json.Unmarshal([]byte(envelope.JSON), &out); err != nil {
			t.Fatal(err)
		}
		return out
	}
	if got := string(call("css-convention-sync", bus.ModuleStatusOK)["asserted"]); got != "0" {
		t.Fatal(got)
	}
	enabled = true
	for i := 0; i < 2; i++ {
		if got := string(call("css-convention-sync", bus.ModuleStatusOK)["asserted"]); got != "2" {
			t.Fatal(got)
		}
	}
	out := call("css-conventions", bus.ModuleStatusOK)
	var items []cssConvention
	if err := json.Unmarshal(out["results"], &items); err != nil {
		t.Fatal(err)
	}
	if len(items) != 2 || items[0].Value != "BEM" || items[1].Value != "css-custom-properties" || items[0].Confidence != 75 || items[0].Source != "exemplar-scan" || items[0].AssertedAt == "" {
		t.Fatal(items)
	}
	var assertions, evidence, commits int
	if err := tx.QueryRow(ctx, `SELECT count(*),min(authority_rank),max(authority_rank) FROM entity_edges WHERE source='css-replay'`).Scan(&assertions, &evidence, &commits); err != nil || assertions != 2 || evidence != 20 || commits != 20 {
		t.Fatal(assertions, evidence, commits, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM fact_evidence f JOIN entity_edges e ON e.id=f.assertion_id WHERE e.source='css-replay'`).Scan(&evidence); err != nil || evidence != 2 {
		t.Fatal(evidence, err)
	}
	// A new generation with no current CSS cannot reuse the previous checkout.
	exec(`RESET ROLE; UPDATE projects SET current_generation=2 WHERE name='css-replay'; SET LOCAL ROLE aimee_store_runtime`)
	if got := string(call("css-convention-sync", bus.ModuleStatusOK)["asserted"]); got != "0" {
		t.Fatal(got)
	}
	exec(`RESET ROLE; UPDATE projects SET current_generation=1 WHERE name='css-replay'; UPDATE css_rules SET selector='.flat' WHERE file_id IN(SELECT f.id FROM files f JOIN projects p ON p.id=f.project_id WHERE p.name='css-replay'); DELETE FROM css_declarations WHERE rule_id IN(SELECT c.id FROM css_rules c JOIN files f ON f.id=c.file_id JOIN projects p ON p.id=f.project_id WHERE p.name='css-replay')`)
	// Fail the second assertion after the first and its sealed graph commit.
	exec(`CREATE FUNCTION pg_temp.css_fail_second() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN IF NEW.source='css-replay' AND NEW.relation='token_strategy' THEN RAISE EXCEPTION 'CSS late failure'; END IF; RETURN NEW; END $$;
 CREATE TRIGGER css_fail_second BEFORE INSERT ON entity_edges FOR EACH ROW EXECUTE FUNCTION pg_temp.css_fail_second(); SET LOCAL ROLE aimee_store_runtime`)
	call("css-convention-sync", bus.ModuleStatusInternal)
	out = call("css-conventions", bus.ModuleStatusOK)
	if err := json.Unmarshal(out["results"], &items); err != nil || len(items) != 2 || items[0].Value != "BEM" || items[1].Value != "css-custom-properties" {
		t.Fatal(items, err)
	}
	if err := tx.QueryRow(ctx, `SELECT count(*) FROM entity_edges WHERE source='css-replay'`).Scan(&assertions); err != nil || assertions != 2 {
		t.Fatal(assertions, err)
	}
	exec(`RESET ROLE; DROP TRIGGER css_fail_second ON entity_edges; SET LOCAL ROLE aimee_store_runtime`)
	call("css-convention-sync", bus.ModuleStatusOK)
	out = call("css-conventions", bus.ModuleStatusOK)
	if err := json.Unmarshal(out["results"], &items); err != nil || len(items) != 2 || items[0].Value != "flat-utility" || items[1].Value != "literal-values" {
		t.Fatal(items, err)
	}
	// Typed writes retain full text and functional supersession; system inference
	// cannot replace a higher-authority human correction.
	in, err := typedFactAssertion("css-replay", "project", "naming_convention", strings.Repeat("界", 300), "scalar", 90, "human-correction", "2026-01-01")
	if err != nil {
		t.Fatal(err)
	}
	in.Actor = FactActor{Principal: "user:css", TransportIdentity: "fixture", Role: "user", Rank: 30, Authenticated: 1}
	direct := *backend
	direct.db = runtimeRoleTx{evalQueryer{tx}, t}
	if _, err := direct.assertFact(ctx, in); err != nil {
		t.Fatal(err)
	}
	call("css-convention-sync", bus.ModuleStatusOK)
	out = call("css-conventions", bus.ModuleStatusOK)
	if err := json.Unmarshal(out["results"], &items); err != nil || len(items) != 2 || items[0].Value != in.Object || items[0].Confidence != 90 {
		t.Fatal(items, err)
	}
	// Hidden evidence is filtered before the 64-row cap. A hidden parent must
	// deny its assertion even if another source is visible.
	exec(`RESET ROLE; SELECT set_config('aimee.memory_scope_all','1',true)`)
	var hidden int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact','css-hidden','hidden','project','other-css-project') RETURNING id`).Scan(&hidden); err != nil {
		t.Fatal(err)
	}
	exec(`INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status) VALUES('css-hidden-fixture','assert','fixture','system',20,'open')`)
	exec(`WITH edges AS (INSERT INTO entity_edges(source,relation,target,edge_class,lifecycle_state,commit_id) SELECT 'css-replay','aaa_hidden_'||i,'hidden','semantic','persistent','css-hidden-fixture' FROM generate_series(1,70)i RETURNING id)
 INSERT INTO fact_evidence(assertion_id,source_kind,source_id,stance) SELECT id,'memory','memory:'||$1::bigint::text,'supports' FROM edges`, hidden)
	exec(`INSERT INTO fact_evidence(assertion_id,source_kind,source_id,stance) SELECT id,'typed_fact_source','visible-source','supports' FROM entity_edges WHERE commit_id='css-hidden-fixture'`)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	out = call("css-conventions", bus.ModuleStatusOK)
	if err := json.Unmarshal(out["results"], &items); err != nil || len(items) != 2 || strings.Contains(string(out["results"]), "hidden") {
		t.Fatal(items, err)
	}
	// Missing configuration and failed SQL remain failures, never healthy empties.
	bound.settings = func() (map[string]any, error) { return nil, errors.New("config offline") }
	call("css-convention-sync", bus.ModuleStatusInternal)
	bound.settings = func() (map[string]any, error) { return map[string]any{"css_style_graph_enabled": true}, nil }
	exec(`RESET ROLE; REVOKE SELECT(property) ON css_declarations FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	call("css-convention-sync", bus.ModuleStatusInternal)
	exec(`RESET ROLE; REVOKE SELECT ON entity_edges FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	call("css-conventions", bus.ModuleStatusInternal)
}
