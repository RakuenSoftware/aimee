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
		})
	}
}
