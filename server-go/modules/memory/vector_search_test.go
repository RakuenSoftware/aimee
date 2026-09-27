package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5"
)

// Exercise the real typed vector column at both shipping widths. A successful
// upsert must be retrievable; a mismatched width must never be counted or stored.
func TestVectorSearchDimensionsAndScopePostgres(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("set AIMEE_MEMORY_EVAL_URL for PostgreSQL vector coverage")
	}
	ctx := context.Background()
	for _, dim := range []int{1024, 2560} {
		t.Run(fmt.Sprint(dim), func(t *testing.T) {
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
			_, err = tx.Exec(ctx, fmt.Sprintf(`CREATE EXTENSION IF NOT EXISTS vector; CREATE SCHEMA vector_search_test; SET LOCAL search_path TO pg_temp,vector_search_test,public;
CREATE FUNCTION vector_search_test.pg_now_text() RETURNS text LANGUAGE sql AS $$ SELECT now()::text $$;
CREATE TEMP TABLE memory_collection_owner(id int PRIMARY KEY,owner_id uuid);
INSERT INTO memory_collection_owner VALUES(1,'00000000-0000-4000-8000-000000000001');
CREATE TEMP TABLE memories(id bigint PRIMARY KEY,record_revision bigint DEFAULT 1,scope_type text,scope_value text,lifecycle_state text DEFAULT 'active',activation_suppressed int DEFAULT 0,valid_from text DEFAULT '',valid_until text DEFAULT '');
CREATE TEMP TABLE memory_units(id bigint PRIMARY KEY,memory_id bigint,unit_type text,unit_key text,unit_text text,memory_kind text,weight float8,is_episode_card int DEFAULT 0);
CREATE INDEX memory_units_card_fixture_idx ON memory_units(memory_id);
CREATE TEMP TABLE rules(id bigint,record_revision bigint DEFAULT 1,domain text DEFAULT '',expires_at text DEFAULT '');
GRANT SELECT ON rules TO PUBLIC;
CREATE TEMP TABLE memory_lineage(object_type text,object_id bigint,source_kind text,source_ref text);
CREATE INDEX memory_lineage_card_fixture_idx ON memory_lineage(object_type,object_id);
CREATE TEMP TABLE memory_summaries(id bigint PRIMARY KEY,memory_id bigint,record_revision bigint);
CREATE TEMP TABLE derived_memory_dependencies(derived_kind text,derived_memory_id text,input_kind text,input_id text,input_version text,extractor_version text,derivation_policy_version text);
CREATE TEMP TABLE memory_embeddings(point_id bigint PRIMARY KEY,embedding vector(%d),record_type text,primary_scope text,workspace text,project text,kind text,payload_json text);
 CREATE TEMP TABLE vector_index_ops(point_id bigint PRIMARY KEY,collection text,memory_id bigint,status text,attempts int,last_error text,indexed_at text,updated_at text);`, dim))
			if err != nil {
				t.Fatal(err)
			}
			backend := &postgresDataStore{db: runtimeRoleDB{evalQueryer{tx}, t}, placement: PlacementKB}
			vector := make([]float32, dim)
			query := make([]float64, dim)
			for i := range vector {
				vector[i] = 0.01
				query[i] = float64(vector[i])
			}
			scopes := []Scope{{Type: ScopeGlobal, Value: "_global"}, {Type: ScopeProject, Value: "app"}, {Type: ScopeProject, Value: "private"}, {Type: ScopeWorkspace, Value: "team"}}
			for i, scope := range scopes {
				if _, err := tx.Exec(ctx, `INSERT INTO memories(id,scope_type,scope_value) VALUES($1,$2,$3)`, i+1, scope.Type, scope.Value); err != nil {
					t.Fatal(err)
				}
				if err := backend.UpsertEmbedding(ctx, Record{ID: int64(i + 1), Scope: scope, Kind: "fact"}, vector); err != nil {
					t.Fatal(err)
				}
			}
			for _, wrong := range []int{384, 1024} {
				if wrong == dim {
					continue
				}
				bad := make([]float32, wrong)
				bad[0] = 1
				if err := backend.UpsertEmbedding(ctx, Record{ID: 99, Scope: scopes[0], Kind: "fact"}, bad); err == nil {
					t.Fatal("accepted mismatched width", wrong)
				}
			}
			var count int
			if err := tx.QueryRow(ctx, `SELECT count(*) FROM memory_embeddings`).Scan(&count); err != nil || count != 4 {
				t.Fatal(count, err)
			}
			if _, err := backend.SearchVectors(ctx, query, "memory", "team", "app", false, 16); err != nil {
				t.Fatal(err)
			}
			handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
			for _, tc := range []struct {
				name string
				args map[string]any
				want []float64
			}{
				{"no-context", map[string]any{}, []float64{1}},
				{"visible", map[string]any{"workspace": "team", "project": "app"}, []float64{1, 2, 4}},
				{"exact-project", map[string]any{"workspace": "team", "project": "app", "scope_type": "project", "scope_value": "app"}, []float64{2}},
				{"exact-global", map[string]any{"project": "app", "scope_type": "global", "scope_value": "_global"}, []float64{1}},
				{"all-exact", map[string]any{"include_all": true, "scope_type": "project", "scope_value": "private"}, []float64{3}},
				{"denied-exact", map[string]any{"project": "app", "scope_type": "project", "scope_value": "private"}, nil},
			} {
				t.Run(tc.name, func(t *testing.T) {
					tc.args["operation"] = "vector-search"
					tc.args["record_type"] = "memory"
					tc.args["scope_context"] = true
					tc.args["vector"] = query
					raw, _ := json.Marshal(tc.args)
					result := runHostRuntime(t, handler, string(raw))
					hits, ok := result["hits"].([]any)
					if !ok || len(hits) != len(tc.want) {
						t.Fatal(result)
					}
					for i, hit := range hits {
						if hit.(map[string]any)["id"] != tc.want[i] {
							t.Fatal(hits, tc.want)
						}
					}
					frame, _ := bus.EncodeCommand("runtime", raw)
					if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 200}, frame); status != bus.ModuleStatusInvalidRequest {
						t.Fatal(status)
					}
				})
			}
			// Apply eligibility before the vector limit: a hidden best match must
			// neither leak its identity nor crowd out the next authorized record.
			if _, err := tx.Exec(ctx, `INSERT INTO memory_units(id,memory_id) VALUES(1,1);
 INSERT INTO memory_embeddings SELECT 1000000000001,embedding,'unit',primary_scope,workspace,project,kind,payload_json FROM memory_embeddings WHERE point_id=1`); err != nil {
				t.Fatal(err)
			}
			for _, update := range []string{
				"valid_from='2099-01-01'", "valid_until='2000-01-01'", "activation_suppressed=1",
				"lifecycle_state='retired'", "scope_type='project',scope_value='moved-hidden'",
			} {
				if _, err := tx.Exec(ctx, `UPDATE memories SET valid_from='',valid_until='',activation_suppressed=0,lifecycle_state='active',scope_type='global',scope_value='_global' WHERE id=1;
 UPDATE memories SET `+update+` WHERE id=1`); err != nil {
					t.Fatal(err)
				}
				hits, err := backend.SearchVectors(ctx, query, "memory", "team", "app", false, 1)
				if err != nil || len(hits) != 1 || hits[0].ID != 2 {
					t.Fatal("ineligible vector consumed limit", update, hits, err)
				}
				hits, err = backend.SearchVectors(ctx, query, "unit", "team", "app", false, 1)
				if err != nil || len(hits) != 0 {
					t.Fatal("ineligible unit leaked", update, hits, err)
				}
			}
			// Other vector families retain their established owner contract. The
			// memory-parent gate must not interpret assertion IDs as memory IDs.
			if _, err := tx.Exec(ctx, `INSERT INTO memory_embeddings SELECT 99,embedding,'semantic_assertion',primary_scope,workspace,project,kind,payload_json FROM memory_embeddings WHERE point_id=1`); err != nil {
				t.Fatal(err)
			}
			if hits, err := backend.SearchVectors(ctx, query, "semantic_assertion", "team", "app", false, 1); err != nil || len(hits) != 1 || hits[0].ID != 99 {
				t.Fatal("unrelated vector family reinterpreted", hits, err)
			}
			// A healthy empty search and an unavailable vector table must remain
			// distinguishable at the owner boundary. The retired C smoke test
			// accepted either zero results or failure without proving this.
			raw, err := json.Marshal(map[string]any{"operation": "vector-search", "record_type": "absent-record-type", "vector": query})
			if err != nil {
				t.Fatal(err)
			}
			result := runHostRuntime(t, handler, string(raw))
			if hits, ok := result["hits"].([]any); !ok || len(hits) != 0 {
				t.Fatal("healthy empty search", result)
			}
			if _, err := tx.Exec(ctx, `DROP TABLE memory_embeddings`); err != nil {
				t.Fatal(err)
			}
			if _, status := invokeContextCommand(t, handler, 0, bus.CommandContext{}, "runtime", string(raw)); status != bus.ModuleStatusInternal {
				t.Fatalf("unavailable storage became status %d", status)
			}
		})
	}
}
