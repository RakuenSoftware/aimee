package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"reflect"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

func TestValidityServingParityPostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("set AIMEE_MEMORY_EVAL_URL")
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
	exec := func(sql string) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql); err != nil {
			t.Fatal(err)
		}
	}
	exec(`CREATE TEMP TABLE memories(id bigint PRIMARY KEY,record_revision bigint DEFAULT 1,
 scope_type text DEFAULT 'project',scope_value text DEFAULT 'visible',tier text DEFAULT 'L2',kind text DEFAULT 'fact',key text DEFAULT 'fixture',content text DEFAULT 'not returned by diagnostic',confidence float8 DEFAULT 1,
 lifecycle_state text DEFAULT 'active',activation_suppressed int DEFAULT 0,valid_from text DEFAULT '',valid_until text DEFAULT '');
 CREATE TEMP TABLE memory_collection_owner(id int PRIMARY KEY,owner_id uuid);
 INSERT INTO memory_collection_owner VALUES(1,'00000000-0000-4000-8000-000000000001');
 CREATE TEMP TABLE memory_units(id bigint,memory_id bigint,unit_type text,unit_key text,unit_text text,memory_kind text,weight float8,is_episode_card int);
 CREATE TEMP TABLE memory_lineage(object_type text,object_id bigint,source_kind text,source_ref text);
 CREATE TEMP TABLE user_memories(id bigint PRIMARY KEY,record_revision bigint DEFAULT 1,lifecycle_state text DEFAULT 'active',valid_until timestamptz,tier text DEFAULT 'L2',kind text DEFAULT 'fact',key text DEFAULT 'fixture',content text DEFAULT 'private',confidence float8 DEFAULT 1);
 CREATE TEMP TABLE user_memory_collection_generation(id int PRIMARY KEY,owner_id uuid);
 INSERT INTO user_memory_collection_generation SELECT * FROM memory_collection_owner;
 INSERT INTO memories(id) SELECT generate_series(1,11);
 UPDATE memories SET valid_from=(now()+interval '1 day')::text WHERE id=2;
 UPDATE memories SET valid_until=(now()-interval '1 day')::text WHERE id=3;
 UPDATE memories SET activation_suppressed=1 WHERE id=4;
 UPDATE memories SET lifecycle_state=CASE id WHEN 5 THEN 'superseded' WHEN 6 THEN 'archived' WHEN 7 THEN 'quarantined' WHEN 8 THEN 'deleted' WHEN 9 THEN 'revoked' END WHERE id BETWEEN 5 AND 9;
 UPDATE memories SET scope_value='hidden' WHERE id=10;
 INSERT INTO memory_units VALUES(11,11,'episode_card','fixture','unobserved','episodic',1,1);
 INSERT INTO user_memories(id,lifecycle_state,valid_until) VALUES(1,'active',NULL),(2,'active',now()-interval '1 day'),(3,'revoked',NULL);
 CREATE ROLE memory_validity_test NOINHERIT NOBYPASSRLS;
 GRANT SELECT ON memories,memory_collection_owner,memory_units,memory_lineage,user_memories,user_memory_collection_generation TO memory_validity_test;
 ALTER TABLE memories ENABLE ROW LEVEL SECURITY;
 CREATE POLICY scoped ON memories USING(scope_value=current_setting('aimee.memory_scope_value',true));
 SET LOCAL ROLE memory_validity_test`)
	caller := bus.CommandContext{Authenticated: true, UserAuthority: true, Principal: "user:diagnostic", TransportIdentity: "fixture"}
	for _, placement := range []Placement{PlacementKB, PlacementServer} {
		backend := &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: placement}
		handler := NewHandler(nil, WithDataStore(placement, backend))
		max := 11
		if placement == PlacementServer {
			max = 3
		}
		for id := int64(1); id <= int64(max); id++ {
			args := map[string]any{"id": id, "mode": "current"}
			request := DataRequest{Operation: "get", ID: id}
			if placement == PlacementKB {
				args["project"] = "visible"
				request.Project = "visible"
			}
			body, _ := json.Marshal(args)
			result, status := invokeContextCommand(t, handler, 0, caller, "validity", string(body))
			if status != bus.ModuleStatusOK || result["status"] != "ok" {
				t.Fatal(placement, id, result, status)
			}
			encoded, _ := json.Marshal(result["decision"])
			var decision EligibilityDecision
			if json.Unmarshal(encoded, &decision) != nil {
				t.Fatal(result)
			}
			raw, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, request))
			var read DataResponse
			if status != bus.ModuleStatusOK || json.Unmarshal(raw, &read) != nil {
				t.Fatal(status, string(raw))
			}
			if decision.Eligible != (len(read.Records) == 1) || decision.Eligible != (id == 1) {
				t.Fatal("serving mismatch", placement, id, decision, read)
			}
			if decision.EvidenceState != "unknown" || decision.AuthorityClass != "unknown" {
				t.Fatal("invented evidence", decision)
			}
			if placement == PlacementKB && id == 10 {
				missing, _ := invokeContextCommand(t, handler, 0, caller, "validity", `{"id":999,"project":"visible"}`)
				if !reflect.DeepEqual(result, missing) {
					t.Fatal("hidden metadata disclosed", result, missing)
				}
			} else if !decision.Version.validFor(id) || decision.CheckedAt == "" {
				t.Fatal("missing observation", decision)
			}
		}
		denied, _ := invokeContextCommand(t, handler, 0, bus.CommandContext{Authenticated: true, Principal: "model:fixture"}, "validity", `{"id":1}`)
		if denied["kind"] != "unauthorized" {
			t.Fatal(denied)
		}
		if placement == PlacementKB {
			scopedCaller := caller
			scopedCaller.ScopeKind, scopedCaller.ScopeID = "project", "visible"
			accepted, _ := invokeContextCommand(t, handler, 0, scopedCaller, "validity", `{"id":1}`)
			if accepted["status"] != "ok" || accepted["decision"].(map[string]any)["eligible"] != true {
				t.Fatal("verified scope not inherited", accepted)
			}
			for _, body := range []string{`{"id":10,"project":"hidden"}`, `{"id":1,"project":"visible","workspace":"foreign"}`, `{"id":10,"scope":{"type":"project","value":"hidden"}}`} {
				denied, _ := invokeContextCommand(t, handler, 0, scopedCaller, "validity", body)
				if denied["kind"] != "unauthorized" {
					t.Fatal("authenticated scope widened", body, denied)
				}
			}
			serviceCaller := caller
			serviceCaller.ScopeKind, serviceCaller.ScopeID = "service", "fixture-deployment"
			for _, sample := range []struct {
				id      int
				project string
			}{{1, "visible"}, {10, "hidden"}} {
				result, _ := invokeContextCommand(t, handler, 0, serviceCaller, "validity", fmt.Sprintf(`{"id":%d,"project":%q}`, sample.id, sample.project))
				if result["status"] != "ok" || result["decision"].(map[string]any)["eligible"] != true {
					t.Fatal("verified service lost data-plane access", result)
				}
			}
			serviceCaller.UserAuthority = false
			denied, _ = invokeContextCommand(t, handler, 0, serviceCaller, "validity", `{"id":1,"project":"visible"}`)
			if denied["kind"] != "unauthorized" {
				t.Fatal("service scope invented user purpose", denied)
			}
			for _, id := range []int{3, 5, 6, 7, 8, 9, 10} {
				result, _ := invokeContextCommand(t, handler, 0, caller, "validity", fmt.Sprintf(`{"id":%d,"project":"visible","mode":"historical","valid_at":"2020-01-01"}`, id))
				decision := result["decision"].(map[string]any)
				if decision["eligible"] != (id == 3 || id == 5 || id == 6) {
					t.Fatal("historical mismatch", id, result)
				}
			}
		} else {
			result, _ := invokeContextCommand(t, handler, 0, caller, "validity", `{"id":1,"mode":"historical","valid_at":"2020-01-01"}`)
			if result["kind"] != "unsupported_mode" {
				t.Fatal(result)
			}
		}
		for _, args := range []string{`{"id":1,"mode":false}`, `{"id":1,"include_all":true}`, `{"id":1,"authority":"user"}`, `{"id":1,"valid_at":null}`, `{"id":1,"scope":null}`} {
			result, _ := invokeContextCommand(t, handler, 0, caller, "validity", args)
			if result["kind"] != "invalid_argument" {
				t.Fatal(args, result)
			}
		}
	}
}
