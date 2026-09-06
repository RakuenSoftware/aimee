package memory

import (
	"context"
	"encoding/json"
	"math"
	"os"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/jackc/pgx/v5"
)

// Use a real PostgreSQL transaction with temporary tables, so the corpus runs
// the production Go handler and SQL without touching any persistent records.
type evalQueryer struct{ pgx.Tx }

func (q evalQueryer) Exec(ctx context.Context, sql string, args ...any) (store.Tag, error) {
	return q.Tx.Exec(ctx, sql, args...)
}
func (q evalQueryer) Query(ctx context.Context, sql string, args ...any) (store.Rows, error) {
	return q.Tx.Query(ctx, sql, args...)
}
func (q evalQueryer) QueryRow(ctx context.Context, sql string, args ...any) store.Row {
	return q.Tx.QueryRow(ctx, sql, args...)
}

func TestRetrievalCorpus(t *testing.T) {
	url := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if url == "" {
		if os.Getenv("AIMEE_MEMORY_EVAL_REQUIRED") == "1" {
			t.Fatal("AIMEE_MEMORY_EVAL_URL is required for the retrieval regression gate")
		}
		t.Skip("set AIMEE_MEMORY_EVAL_URL to run the PostgreSQL corpus")
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
	_, err = tx.Exec(ctx, `CREATE TEMP TABLE memories (
id bigint PRIMARY KEY, scope_type text, scope_value text, tier text, kind text,
key text, content text, confidence double precision, lifecycle_state text DEFAULT 'active',
use_cases text DEFAULT '', updated_at timestamptz DEFAULT now()) ON COMMIT DROP`)
	if err != nil {
		t.Fatal(err)
	}
	var corpus struct {
		Version  int                                              `json:"version"`
		Fixtures []struct{ FID, Tier, Kind, Key, Content string } `json:"fixtures"`
		Cases    []struct {
			ID, Query string
			Expected  []string
		} `json:"cases"`
	}
	read := func(path string, into any) {
		t.Helper()
		data, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if err := json.Unmarshal(data, into); err != nil {
			t.Fatal(err)
		}
	}
	read("../../../tests/eval/memory_retrieval_corpus.json", &corpus)
	var baseline struct {
		MRR       float64 `json:"mrr"`
		NDCG5     float64 `json:"ndcg_5"`
		NDCG10    float64 `json:"ndcg_10"`
		Recall5   float64 `json:"recall_5"`
		Recall10  float64 `json:"recall_10"`
		Cases     int     `json:"n_cases"`
		Threshold float64 `json:"threshold_pct"`
	}
	read("../../../tests/eval/memory_retrieval_baseline.json", &baseline)
	if corpus.Version != 1 || len(corpus.Fixtures) == 0 || len(corpus.Cases) != baseline.Cases {
		t.Fatal("invalid corpus or changed case denominator")
	}
	ids := make(map[string]int64)
	for i, fixture := range corpus.Fixtures {
		if fixture.FID == "" || ids[fixture.FID] != 0 {
			t.Fatal("invalid fixture ID")
		}
		id := int64(i + 1)
		ids[fixture.FID] = id
		_, err := tx.Exec(ctx, `INSERT INTO memories
(id,scope_type,scope_value,tier,kind,key,content,confidence)
VALUES($1,'global','_global',$2,$3,$4,$5,0.8)`, id, fixture.Tier, fixture.Kind, fixture.Key, fixture.Content)
		if err != nil {
			t.Fatal(err)
		}
	}
	_, err = tx.Exec(ctx, `CREATE TEMP TABLE user_memories (LIKE memories INCLUDING ALL,
valid_until timestamptz) ON COMMIT DROP;
INSERT INTO user_memories SELECT *, NULL::timestamptz FROM memories`)
	if err != nil {
		t.Fatal(err)
	}
	for _, placement := range []Placement{PlacementKB, PlacementServer} {
		t.Run(string(placement), func(t *testing.T) {
			backend, err := NewPostgresDataStore(evalQueryer{tx}, placement)
			if err != nil {
				t.Fatal(err)
			}
			handler := NewHandler(nil, WithDataStore(placement, backend))
			totals := [5]float64{}
			for _, test := range corpus.Cases {
				relevant := make(map[int64]bool)
				for _, fid := range test.Expected {
					if ids[fid] == 0 {
						t.Fatalf("%s: unknown fixture %s", test.ID, fid)
					}
					relevant[ids[fid]] = true
				}
				if len(relevant) == 0 {
					t.Fatalf("%s: no relevance labels", test.ID)
				}
				raw, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, DataRequest{
					Operation: "search", Query: test.Query, Limit: 10,
				}))
				if status != bus.ModuleStatusOK {
					t.Fatalf("%s: status %v", test.ID, status)
				}
				var response DataResponse
				if err := json.Unmarshal(raw, &response); err != nil {
					t.Fatal(err)
				}
				mrr := 0.0
				for i, record := range response.Records {
					if relevant[record.ID] {
						mrr = 1 / float64(i+1)
						break
					}
				}
				totals[0] += mrr
				for index, k := range []int{5, 10} {
					hits, dcg, ideal := 0.0, 0.0, 0.0
					for i, record := range response.Records {
						if i >= k {
							break
						}
						if relevant[record.ID] {
							hits++
							dcg += 1 / math.Log2(float64(i+2))
						}
					}
					for i := 0; i < k && i < len(relevant); i++ {
						ideal += 1 / math.Log2(float64(i+2))
					}
					totals[1+index] += dcg / ideal
					totals[3+index] += hits / float64(len(relevant))
				}
				if mrr == 0 {
					t.Logf("miss %s: %s", test.ID, test.Query)
				}
			}
			wants := []float64{baseline.MRR, baseline.NDCG5, baseline.NDCG10, baseline.Recall5, baseline.Recall10}
			for i, name := range []string{"mrr", "ndcg_5", "ndcg_10", "recall_5", "recall_10"} {
				got := totals[i] / float64(len(corpus.Cases))
				minimum := wants[i] * (1 - baseline.Threshold/100)
				t.Logf("%s=%.6f baseline=%.6f cases=%d", name, got, wants[i], len(corpus.Cases))
				if got < minimum {
					t.Errorf("%s regressed: %.6f < %.6f", name, got, minimum)
				}
			}
		})
	}

	// Matching keywords must not widen scope or resurrect retired/expired rows.
	_, err = tx.Exec(ctx, `INSERT INTO memories
(id,scope_type,scope_value,tier,kind,key,content,confidence,lifecycle_state) VALUES
(1001,'workspace','workspace-a','L2','fact','probe alpha','boundary terms',1,'active'),
(1002,'workspace','workspace-b','L2','fact','probe beta','boundary terms',1,'active'),
(1003,'project','project-a','L2','fact','probe gamma','boundary terms',1,'active'),
(1004,'workspace','workspace-a','L2','fact','probe retired','boundary terms',1,'retired');
INSERT INTO user_memories (id,key,content,kind,tier,confidence,lifecycle_state,valid_until) VALUES
(1005,'probe active','boundary terms','fact','L2',1,'active',NULL),
(1006,'probe expired','boundary terms','fact','L2',1,'active',now()-interval '1 second'),
(1007,'probe retired','boundary terms','fact','L2',1,'retired',NULL)`)
	if err != nil {
		t.Fatal(err)
	}
	for _, test := range []struct {
		placement Placement
		scope     Scope
		id        int64
	}{
		{PlacementKB, Scope{Type: ScopeWorkspace, Value: "workspace-a"}, 1001},
		{PlacementKB, Scope{Type: ScopeWorkspace, Value: "workspace-b"}, 1002},
		{PlacementKB, Scope{Type: ScopeProject, Value: "project-a"}, 1003},
		{PlacementServer, Scope{}, 1005},
	} {
		backend, err := NewPostgresDataStore(evalQueryer{tx}, test.placement)
		if err != nil {
			t.Fatal(err)
		}
		handler := NewHandler(nil, WithDataStore(test.placement, backend))
		raw, status := handler(bus.ModuleInvocation{StageID: StageData}, dataRequest(t, DataRequest{
			Operation: "search", Scope: test.scope, Query: "terms boundary", Limit: 10,
		}))
		var response DataResponse
		if err := json.Unmarshal(raw, &response); err != nil {
			t.Fatal(err)
		}
		if status != bus.ModuleStatusOK || len(response.Records) != 1 || response.Records[0].ID != test.id {
			t.Fatalf("placement=%s scope=%+v: status=%v records=%+v", test.placement, test.scope, status, response.Records)
		}
	}
}
