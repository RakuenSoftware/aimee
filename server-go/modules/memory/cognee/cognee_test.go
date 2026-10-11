package cognee

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	memory "github.com/JBailes/aimee/server-go/memory"
	"io"
	"net/http"
	"net/http/httptest"
	"sort"
	"strings"
	"sync"
	"testing"
	"time"
)

type catalog struct{ records map[int64]memory.Record }

func (c *catalog) Get(_ context.Context, s memory.Scope, id int64) (memory.Record, error) {
	r, ok := c.records[id]
	if !ok || r.Scope != s {
		return memory.Record{}, memory.ErrNotFound
	}
	return r, nil
}
func (c *catalog) Put(_ context.Context, s memory.Scope, r memory.Record) (memory.Record, error) {
	r.Scope = s
	c.records[r.ID] = r
	return r, nil
}
func (c *catalog) Delete(_ context.Context, s memory.Scope, id int64) (bool, error) {
	if _, err := c.Get(context.Background(), s, id); err != nil {
		return false, err
	}
	delete(c.records, id)
	return true, nil
}
func (c *catalog) Search(_ context.Context, s memory.Scope, q, k, t string, limit int) ([]memory.Record, error) {
	var out []memory.Record
	for _, r := range c.records {
		if r.Scope == s && (k == "" || r.Kind == k) && (t == "" || r.Tier == t) {
			out = append(out, r)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	if len(out) > limit {
		out = out[:limit]
	}
	return out, nil
}

type cogneeFixture struct {
	mu              sync.Mutex
	datasets        map[string]dataset
	contents        map[string]string
	searches        int
	serial          int
	foreign         bool
	started         bool
	lostDelete      bool
	retainDelete    bool
	lostAfterDelete bool
	mutate          func()
}

func (f *cogneeFixture) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	f.mu.Lock()
	defer f.mu.Unlock()
	w.Header().Set("Content-Type", "application/json")
	respond := func(v any) { json.NewEncoder(w).Encode(v) }
	switch {
	case r.Method == "GET" && r.URL.Path == "/api/v1/datasets":
		var all []dataset
		for _, d := range f.datasets {
			all = append(all, d)
		}
		if all == nil {
			all = []dataset{}
		}
		respond(all)
	case r.Method == "POST" && r.URL.Path == "/api/v1/add":
		if err := r.ParseMultipartForm(MaxBody); err != nil {
			http.Error(w, "multipart", 400)
			return
		}
		name := r.FormValue("datasetName")
		file, _, err := r.FormFile("data")
		if err != nil {
			http.Error(w, "missing data", 400)
			return
		}
		defer file.Close()
		raw, _ := io.ReadAll(io.LimitReader(file, MaxBody+1))
		if name == "" || len(raw) > MaxBody {
			http.Error(w, "invalid input", 400)
			return
		}
		if _, ok := f.datasets[name]; !ok {
			f.serial++
			f.datasets[name] = dataset{ID: fmt.Sprintf("00000000-0000-0000-0000-%012x", f.serial), Name: name}
		}
		f.contents[name] = string(raw)
		respond(map[string]string{"status": "success"})
	case r.Method == "POST" && r.URL.Path == "/api/v1/cognify":
		var input struct {
			Datasets   []string `json:"datasets"`
			Background bool     `json:"run_in_background"`
		}
		json.NewDecoder(r.Body).Decode(&input)
		if input.Background {
			http.Error(w, "background forbidden", 400)
			return
		}
		runs := map[string]any{}
		for _, name := range input.Datasets {
			d, ok := f.datasets[name]
			if !ok {
				http.Error(w, "unknown dataset", 400)
				return
			}
			status := "PipelineRunCompleted"
			if f.started {
				status = "PipelineRunStarted"
			}
			runs[d.ID] = map[string]string{"dataset_name": name, "status": status}
		}
		respond(runs)
	case r.Method == "POST" && r.URL.Path == "/api/v1/search":
		f.searches++
		var input struct {
			Datasets    []string `json:"datasets"`
			Query       string   `json:"query"`
			Type        string   `json:"search_type"`
			OnlyContext bool     `json:"only_context"`
		}
		json.NewDecoder(r.Body).Decode(&input)
		if input.Type != "CHUNKS" || input.OnlyContext || len(input.Datasets) == 0 {
			http.Error(w, "unscoped or generative", 400)
			return
		}
		var out []any
		for _, name := range input.Datasets {
			score := .9
			if strings.Contains(f.contents[name], input.Query) {
				score = .1
			}
			if f.foreign {
				name = "foreign-private-dataset"
			}
			out = append(out, map[string]any{"dataset_name": name, "search_result": []any{map[string]any{"score": score, "text": "UNTRUSTED EXTERNAL PAYLOAD"}}})
		}
		if f.mutate != nil {
			f.mutate()
		}
		respond(out)
	case r.Method == "DELETE" && strings.HasPrefix(r.URL.Path, "/api/v1/datasets/"):
		if f.lostDelete {
			http.Error(w, "unknown delete outcome", 502)
			return
		}
		id := strings.TrimPrefix(r.URL.Path, "/api/v1/datasets/")
		for name, d := range f.datasets {
			if d.ID == id {
				if f.retainDelete {
					respond(map[string]bool{"deleted": true})
					return
				}
				delete(f.datasets, name)
				delete(f.contents, name)
				if f.lostAfterDelete {
					http.Error(w, "lost deletion reply", 502)
					return
				}
			}
		}
		respond(nil)
	default:
		http.Error(w, "wrong Cognee route", 404)
	}
}

func TestCogneeContractRoundTripAndIsolation(t *testing.T) {
	scope := memory.Scope{Type: memory.ScopeProject, Value: "aimee"}
	other := memory.Scope{Type: memory.ScopeProject, Value: "private"}
	records := &catalog{records: map[int64]memory.Record{1: {ID: 1, Scope: scope, Key: "first", Content: "ordinary", Tier: "L2", Kind: "fact"}, 9007199254740993: {ID: 9007199254740993, Scope: scope, Key: "second", Content: "needle canonical", Tier: "L2", Kind: "fact"}, 3: {ID: 3, Scope: other, Content: "PRIVATE CANARY"}}}
	fixture := &cogneeFixture{datasets: map[string]dataset{}, contents: map[string]string{}}
	server := httptest.NewServer(fixture)
	defer server.Close()
	transport := func(ctx context.Context, method, target, contentType string, body []byte) (int, []byte, error) {
		req, err := http.NewRequestWithContext(ctx, method, target, strings.NewReader(string(body)))
		if err != nil {
			return 0, nil, err
		}
		if contentType != "" {
			req.Header.Set("Content-Type", contentType)
		}
		reply, err := server.Client().Do(req)
		if err != nil {
			return 0, nil, err
		}
		defer reply.Body.Close()
		raw, err := io.ReadAll(io.LimitReader(reply.Body, MaxBody+1))
		return reply.StatusCode, raw, err
	}
	backend, err := New(records, transport, server.URL, "node-fixture")
	if err != nil {
		t.Fatal(err)
	}
	var provider memory.Backend = backend
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	got, err := provider.Search(ctx, scope, "needle", "fact", "L2", 2)
	if err != nil || len(got) != 2 || got[0].ID != 9007199254740993 || got[0].Content != "needle canonical" {
		t.Fatal("Cognee did not rank canonical records", got, err)
	}
	if len(fixture.datasets) != 2 {
		t.Fatal("indexed foreign scope")
	}
	for _, text := range fixture.contents {
		if strings.Contains(text, "PRIVATE CANARY") {
			t.Fatal("disclosed foreign content")
		}
	}
	if _, err = provider.Get(ctx, scope, 3); !errors.Is(err, memory.ErrNotFound) {
		t.Fatal("foreign exact read admitted", err)
	}
	changed := records.records[1]
	changed.Content = "updated needle"
	if _, err = provider.Put(ctx, scope, changed); err != nil {
		t.Fatal(err)
	}
	if _, err = provider.Search(ctx, scope, "needle", "", "", 2); err != nil {
		t.Fatal(err)
	}
	if len(fixture.datasets) != 2 {
		t.Fatal("old revision retained", fixture.datasets)
	}
	fixture.foreign = true
	if _, err = provider.Search(ctx, scope, "needle", "", "", 2); !errors.Is(err, memory.ErrUnavailable) {
		t.Fatal("foreign result admitted", err)
	}
	fixture.foreign = false
	fixture.started = true
	before := fixture.searches
	if _, err = provider.Search(ctx, scope, "needle", "", "", 2); !errors.Is(err, memory.ErrUnavailable) || fixture.searches != before {
		t.Fatal("unfinished cognify treated as ready", err)
	}
	fixture.started = false
	fixture.mutate = func() { r := records.records[1]; r.Content = "changed after retrieval"; records.records[1] = r }
	if _, err = provider.Search(ctx, scope, "needle", "", "", 2); !errors.Is(err, memory.ErrUnavailable) {
		t.Fatal("stale canonical payload admitted", err)
	}
	fixture.mutate = nil
	fixture.lostDelete = true
	if deleted, err := provider.Delete(ctx, scope, 1); deleted || !errors.Is(err, memory.ErrUnavailable) {
		t.Fatal("unknown remote delete acknowledged", deleted, err)
	}
	if _, err = records.Get(ctx, scope, 1); err != nil {
		t.Fatal("deleted canonical row despite remote failure")
	}
	fixture.lostDelete = false
	if deleted, err := provider.Delete(ctx, scope, 1); !deleted || err != nil {
		t.Fatal("delete failed", deleted, err)
	}
	for _, d := range fixture.datasets {
		if strings.HasPrefix(d.Name, backend.prefix(scope)+"1_") {
			t.Fatal("deleted derived revision survived")
		}
	}
	// Lifecycle cleanup must still work after the source is no longer readable.
	if err := backend.add(ctx, backend.name(memory.Record{ID: 1, Scope: scope}), memory.Record{ID: 1, Scope: scope, Content: "orphan revision"}); err != nil {
		t.Fatal(err)
	}
	if err := backend.Forget(ctx, scope, 1); err != nil {
		t.Fatal("orphan cleanup", err)
	}
	for _, d := range fixture.datasets {
		if strings.HasPrefix(d.Name, backend.prefix(scope)+"1_") {
			t.Fatal("orphan survived")
		}
	}
	cancelled, cancelNow := context.WithCancel(ctx)
	cancelNow()
	if _, err = provider.Search(cancelled, scope, "needle", "", "", 2); !errors.Is(err, context.Canceled) {
		t.Fatal("cancellation lost", err)
	}
}

func TestCogneeSubjectErasureResetAndRetry(t *testing.T) {
	scope := memory.Scope{Type: memory.ScopeProject, Value: "aimee"}
	private := memory.Scope{Type: memory.ScopeUser, Value: "_user"}
	records := &catalog{records: map[int64]memory.Record{2: {ID: 2, Scope: scope, Content: "retained canonical"}}}
	fixture := &cogneeFixture{datasets: map[string]dataset{}, contents: map[string]string{}}
	transport := func(ctx context.Context, method, target, contentType string, body []byte) (int, []byte, error) {
		req, err := http.NewRequestWithContext(ctx, method, target, strings.NewReader(string(body)))
		if err != nil {
			return 0, nil, err
		}
		req.Header.Set("Content-Type", contentType)
		response := httptest.NewRecorder()
		fixture.ServeHTTP(response, req)
		return response.Code, response.Body.Bytes(), nil
	}
	backend, err := New(records, transport, "http://fixture", "erased-node")
	if err != nil {
		t.Fatal(err)
	}
	other, err := New(records, transport, "http://fixture", "other-node")
	if err != nil {
		t.Fatal(err)
	}
	ctx := context.Background()
	foreign := other.name(memory.Record{ID: 1, Scope: private, Content: "other node's private memory"})
	if err := other.add(ctx, foreign, memory.Record{Content: "other node's private memory"}); err != nil {
		t.Fatal(err)
	}
	// An erased record can have multiple revisions, across scopes, and no
	// remaining readable canonical record. Cleanup is not a retrieval scan.
	for i := int64(1); i <= 300; i++ {
		record := memory.Record{ID: i, Scope: scope, Content: fmt.Sprintf("erased revision %d", i)}
		if i%2 == 0 {
			record.Scope = private
		}
		if err := backend.add(ctx, backend.name(record), record); err != nil {
			t.Fatal(err)
		}
	}
	fixture.lostDelete = true
	if err := backend.ResetDerived(ctx); !errors.Is(err, memory.ErrUnavailable) {
		t.Fatal("lost deletion acknowledged", err)
	}
	fixture.lostDelete = false
	fixture.retainDelete = true
	if err := backend.ResetDerived(ctx); !errors.Is(err, memory.ErrUnavailable) {
		t.Fatal("unverified cleanup acknowledged", err)
	}
	fixture.retainDelete = false
	fixture.lostAfterDelete = true
	if err := backend.ResetDerived(ctx); !errors.Is(err, memory.ErrUnavailable) {
		t.Fatal("lost reply after committed delete acknowledged", err)
	}
	fixture.lostAfterDelete = false
	if err := backend.ResetDerived(ctx); err != nil {
		t.Fatal("reset retry", err)
	}
	if len(fixture.datasets) != 1 || fixture.datasets[foreign].Name != foreign {
		t.Fatal("foreign node erased or managed copy survived", len(fixture.datasets))
	}
	if len(records.records) != 1 || records.records[2].Content != "retained canonical" {
		t.Fatal("reset changed canonical records")
	}
	if err := backend.ResetDerived(ctx); err != nil {
		t.Fatal("reset not idempotent", err)
	}
	// A restored external index is subject to the same startup reset.
	restored := memory.Record{ID: 1, Scope: private, Content: "restored erased payload"}
	if err := backend.add(ctx, backend.name(restored), restored); err != nil {
		t.Fatal(err)
	}
	if err := backend.ResetDerived(ctx); err != nil || len(fixture.datasets) != 1 {
		t.Fatal("restore replay left derived payload", err)
	}
	cancelled, cancel := context.WithCancel(ctx)
	cancel()
	if err := backend.ResetDerived(cancelled); !errors.Is(err, context.Canceled) {
		t.Fatal("reset cancellation lost", err)
	}
}

type validationSource struct {
	*catalog
	search func() ([]memory.Record, error)
	get    func(memory.Record) (memory.Record, error)
}

func (s validationSource) Namespace(memory.Scope) string { return "validated-owner" }
func (s validationSource) Search(ctx context.Context, scope memory.Scope, q, k, tier string, n int) ([]memory.Record, error) {
	if s.search != nil {
		return s.search()
	}
	return s.catalog.Search(ctx, scope, q, k, tier, n)
}
func (s validationSource) Get(ctx context.Context, scope memory.Scope, id int64) (memory.Record, error) {
	r, e := s.catalog.Get(ctx, scope, id)
	if e == nil && s.get != nil {
		return s.get(r)
	}
	return r, e
}
func TestCogneeValidationConfigurationAndBounds(t *testing.T) {
	source := &catalog{records: map[int64]memory.Record{}}
	transport := func(context.Context, string, string, string, []byte) (int, []byte, error) {
		t.Fatal("unexpected provider call")
		return 0, nil, nil
	}
	for _, endpoint := range []string{"", "ftp://host", "http://user:password@host", "http://host/path", "http://host?x=1", "http://host#fragment", "://bad"} {
		t.Run("endpoint-"+endpoint, func(t *testing.T) {
			if _, e := New(source, transport, endpoint, "node"); !errors.Is(e, memory.ErrUnavailable) {
				t.Fatal(e)
			}
		})
	}
	for _, namespace := range []string{"", strings.Repeat("n", 257)} {
		if _, e := New(source, transport, "http://host", namespace); !errors.Is(e, memory.ErrUnavailable) {
			t.Fatal(e)
		}
	}
	if _, e := New(nil, transport, "http://host", "node"); !errors.Is(e, memory.ErrUnavailable) {
		t.Fatal(e)
	}
	if _, e := New(source, nil, "http://host", "node"); !errors.Is(e, memory.ErrUnavailable) {
		t.Fatal(e)
	}
	b, _ := New(source, transport, "https://host/", "node")
	if c := b.Capabilities(); c.Name != "cognee" || c.Version != 1 || len(c.Operations) != 6 {
		t.Fatal(c)
	}
	for _, n := range []int{-1, 0, 257} {
		if _, e := b.Search(context.Background(), memory.Scope{}, "query", "", "", n); !errors.Is(e, memory.ErrCapacity) {
			t.Fatal(e)
		}
	}
	if _, e := b.Search(context.Background(), memory.Scope{}, strings.Repeat("q", 16385), "", "", 1); !errors.Is(e, memory.ErrCapacity) {
		t.Fatal(e)
	}
	if _, e := b.Search(context.Background(), memory.Scope{}, "", "", "", 1); e != nil {
		t.Fatal(e)
	}
	if e := b.Forget(context.Background(), memory.Scope{}, 0); !errors.Is(e, memory.ErrUnavailable) {
		t.Fatal(e)
	}
	if e := b.remove(context.Background(), "../foreign"); !errors.Is(e, memory.ErrUnavailable) {
		t.Fatal(e)
	}
}
func TestCogneeValidationTransportAndCatalog(t *testing.T) {
	ctx := context.Background()
	source := &catalog{records: map[int64]memory.Record{}}
	cases := []struct {
		name    string
		status  int
		body    string
		failure error
		want    error
	}{
		{"unreachable", 0, "", errors.New("offline"), memory.ErrUnavailable},
		{"unauthorized", 401, `{}`, nil, memory.ErrUnavailable},
		{"redirect", 302, `[]`, nil, memory.ErrUnavailable},
		{"server-error", 500, `[]`, nil, memory.ErrUnavailable},
		{"malformed", 200, `{`, nil, memory.ErrUnavailable},
		{"oversized", 200, strings.Repeat("x", MaxBody+1), nil, memory.ErrUnavailable},
		{"bad-uuid", 200, `[{"id":"../escape","name":"foreign"}]`, nil, memory.ErrUnavailable},
		{"upper-uuid", 200, `[{"id":"ABCDEF00-0000-0000-0000-000000000000","name":"foreign"}]`, nil, memory.ErrUnavailable},
		{"empty-name", 200, `[{"id":"00000000-0000-0000-0000-000000000001","name":""}]`, nil, memory.ErrUnavailable},
		{"duplicate-name", 200, `[{"id":"00000000-0000-0000-0000-000000000001","name":"same"},{"id":"00000000-0000-0000-0000-000000000002","name":"same"}]`, nil, memory.ErrUnavailable},
	}
	many := make([]map[string]string, 10001)
	for i := range many {
		many[i] = map[string]string{"id": "00000000-0000-0000-0000-000000000001", "name": "x"}
	}
	raw, _ := json.Marshal(many)
	cases = append(cases, struct {
		name    string
		status  int
		body    string
		failure error
		want    error
	}{"catalog-capacity", 200, string(raw), nil, memory.ErrCapacity})
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			b, _ := New(source, func(context.Context, string, string, string, []byte) (int, []byte, error) {
				return c.status, []byte(c.body), c.failure
			}, "http://fixture", "node")
			if _, e := b.datasets(ctx); !errors.Is(e, c.want) {
				t.Fatalf("%v", e)
			}
		})
	}
	b, _ := New(source, func(ctx context.Context, _ string, _ string, _ string, _ []byte) (int, []byte, error) {
		return 0, nil, ctx.Err()
	}, "http://fixture", "node")
	cancelled, cancel := context.WithCancel(ctx)
	cancel()
	if _, e := b.datasets(cancelled); !errors.Is(e, context.Canceled) {
		t.Fatal(e)
	}
	if e := b.call(ctx, "POST", "/api/v1/add", "", make([]byte, MaxBody+1), nil); !errors.Is(e, memory.ErrCapacity) {
		t.Fatal(e)
	}
}
func TestCogneeValidationSearchFailuresAndFilters(t *testing.T) {
	scope := memory.Scope{Type: memory.ScopeProject, Value: "paths"}
	ctx := context.Background()
	for _, fault := range []string{"catalog", "add", "cognify", "search", "delete-stale", "unfinished", "missing-run", "duplicate-run", "foreign-run", "duplicate-result", "too-many-results", "foreign-result", "missing-score", "multiple-chunks", "bad-score", "empty-chunks", "filters", "limit", "equal-score", "source-version", "source-deleted"} {
		t.Run(fault, func(t *testing.T) {
			version := memory.MemoryRecordVersion{OwnerID: "owner", RecordID: "1", RecordRevision: "1"}
			records := validationSource{catalog: &catalog{records: map[int64]memory.Record{1: {ID: 1, Scope: scope, Content: "needle one", Kind: "fact", Tier: "L2", Version: &version}, 2: {ID: 2, Scope: scope, Content: "needle two", Kind: "preference", Tier: "L3"}}}}
			fixture := &cogneeFixture{datasets: map[string]dataset{}, contents: map[string]string{}}
			var b *Backend
			transport := func(c context.Context, method, target, contentType string, body []byte) (int, []byte, error) {
				path := strings.TrimPrefix(target, "http://fixture")
				if fault == "catalog" && method == "GET" || fault == "add" && path == "/api/v1/add" || fault == "cognify" && path == "/api/v1/cognify" || fault == "search" && path == "/api/v1/search" || fault == "delete-stale" && method == "DELETE" {
					return 503, []byte(`{}`), nil
				}
				req := httptest.NewRequest(method, target, strings.NewReader(string(body)))
				req.Header.Set("Content-Type", contentType)
				w := httptest.NewRecorder()
				fixture.ServeHTTP(w, req)
				result := w.Body.Bytes()
				if path == "/api/v1/cognify" {
					switch fault {
					case "missing-run":
						result = []byte(`{}`)
					case "unfinished":
						result = []byte(`{"a":{"dataset_name":"` + b.name(records.records[1]) + `","status":"PipelineRunStarted"}}`)
					case "foreign-run":
						result = []byte(`{"a":{"dataset_name":"foreign","status":"PipelineRunCompleted"}}`)
					case "duplicate-run":
						name := b.name(records.records[1])
						result = []byte(`{"a":{"dataset_name":"` + name + `","status":"PipelineRunCompleted"},"b":{"dataset_name":"` + name + `","status":"PipelineRunCompleted"}}`)
					}
				}
				if path == "/api/v1/search" {
					name := b.name(records.records[1])
					entry := `{"dataset_name":"` + name + `","search_result":[{"score":0.1}]}`
					switch fault {
					case "too-many-results":
						result = []byte(`[` + entry + `,` + entry + `,` + entry + `]`)
					case "duplicate-result":
						result = []byte(`[` + entry + `,` + entry + `]`)
					case "foreign-result":
						result = []byte(`[{"dataset_name":"foreign","search_result":[]}]`)
					case "missing-score":
						result = []byte(`[{"dataset_name":"` + name + `","search_result":[{}]}]`)
					case "multiple-chunks":
						result = []byte(`[{"dataset_name":"` + name + `","search_result":[{"score":0},{"score":1}]}]`)
					case "bad-score":
						result = []byte(`[{"dataset_name":"` + name + `","search_result":[{"score":1e999}]}]`)
					case "empty-chunks":
						result = []byte(`[{"dataset_name":"` + name + `","search_result":[]}]`)
					case "equal-score":
						result = []byte(`[` + entry + `,{"dataset_name":"` + b.name(records.records[2]) + `","search_result":[{"score":0.1}]}]`)
					}
				}
				return w.Code, result, nil
			}
			b, _ = New(records, transport, "http://fixture", "node")
			if fault == "delete-stale" {
				fixture.datasets["stale"] = dataset{ID: "00000000-0000-0000-0000-000000000001", Name: b.prefix(scope) + "1_stale"}
			}
			if fault == "source-version" {
				records.get = func(r memory.Record) (memory.Record, error) {
					v := *r.Version
					v.RecordRevision = "2"
					r.Version = &v
					return r, nil
				}
				b.records = records
			}
			if fault == "source-deleted" {
				records.get = func(memory.Record) (memory.Record, error) { return memory.Record{}, memory.ErrNotFound }
				b.records = records
			}
			kind, tier, n := "", "", 2
			if fault == "filters" {
				kind, tier = "fact", "L2"
			}
			if fault == "limit" {
				n = 1
			}
			found, e := b.Search(ctx, scope, "needle", kind, tier, n)
			switch fault {
			case "empty-chunks":
				if e != nil || len(found) != 0 {
					t.Fatal(found, e)
				}
			case "filters", "limit":
				if e != nil || len(found) != 1 {
					t.Fatal(found, e)
				}
			case "equal-score":
				if e != nil || len(found) != 2 || found[0].ID != 1 {
					t.Fatal(found, e)
				}
			case "source-deleted":
				if !errors.Is(e, memory.ErrNotFound) {
					t.Fatal(e)
				}
			default:
				if !errors.Is(e, memory.ErrUnavailable) {
					t.Fatal(found, e)
				}
			}
		})
	}
	for _, fault := range []string{"source-error", "scope-mismatch", "invalid-id", "duplicate-source", "too-many", "empty-scope"} {
		t.Run(fault, func(t *testing.T) {
			source := validationSource{catalog: &catalog{records: map[int64]memory.Record{}}, search: func() ([]memory.Record, error) {
				switch fault {
				case "source-error":
					return nil, memory.ErrUnavailable
				case "scope-mismatch":
					return []memory.Record{{ID: 1}}, nil
				case "invalid-id":
					return []memory.Record{{Scope: scope}}, nil
				case "duplicate-source":
					return []memory.Record{{ID: 1, Scope: scope}, {ID: 1, Scope: scope}}, nil
				case "too-many":
					return make([]memory.Record, 257), nil
				default:
					return nil, nil
				}
			}}
			b, _ := New(source, func(context.Context, string, string, string, []byte) (int, []byte, error) {
				return 200, []byte(`[]`), nil
			}, "http://fixture", "node")
			found, e := b.Search(ctx, scope, "needle", "", "", 1)
			if fault == "empty-scope" {
				if e != nil || len(found) != 0 {
					t.Fatal(found, e)
				}
			} else if e == nil {
				t.Fatal("invalid source accepted")
			}
		})
	}
}

func TestCogneeValidationInterruptedTransportAndResetVerification(t *testing.T) {
	source := &catalog{records: map[int64]memory.Record{}}
	ctx, cancel := context.WithCancel(context.Background())
	b, _ := New(source, func(context.Context, string, string, string, []byte) (int, []byte, error) {
		cancel()
		return 0, nil, errors.New("connection lost")
	}, "http://fixture", "node")
	if _, e := b.datasets(ctx); !errors.Is(e, context.Canceled) {
		t.Fatal(e)
	}
	for _, operation := range []string{"forget-catalog", "reset-catalog", "reset-verification"} {
		t.Run(operation, func(t *testing.T) {
			calls := 0
			b, _ := New(source, func(context.Context, string, string, string, []byte) (int, []byte, error) {
				calls++
				if operation != "reset-verification" || calls > 1 {
					return 503, []byte(`{}`), nil
				}
				return 200, []byte(`[]`), nil
			}, "http://fixture", "node")
			var e error
			if operation == "forget-catalog" {
				e = b.Forget(context.Background(), memory.Scope{}, 1)
			} else {
				e = b.ResetDerived(context.Background())
			}
			if !errors.Is(e, memory.ErrUnavailable) {
				t.Fatal(e)
			}
		})
	}
	if validUUID("000000000000-0000-0000-000000000001") {
		t.Fatal("misplaced UUID separator admitted")
	}
}

type candidateCatalog struct {
	*catalog
	requested int
	offset    int
}

func (c *candidateCatalog) Candidates(ctx context.Context, s memory.Scope, q, k, tier string, limit int) ([]memory.Record, error) {
	c.requested = limit
	all, err := c.catalog.Search(ctx, s, q, k, tier, MaxRecords+100)
	if len(all) > c.offset {
		all = all[c.offset:]
	} else {
		all = nil
	}
	if len(all) > limit {
		all = all[:limit]
	}
	return all, err
}
func TestBoundedCandidatesRetainPriorValidIndexes(t *testing.T) {
	scope := memory.Scope{Type: memory.ScopeProject, Value: "bulk"}
	source := &candidateCatalog{catalog: &catalog{records: map[int64]memory.Record{}}}
	for i := int64(1); i <= 300; i++ {
		source.records[i] = memory.Record{ID: i, Scope: scope, Key: fmt.Sprint(i), Content: "needle", Kind: "fact", Tier: "L2"}
	}
	fixture := &cogneeFixture{datasets: map[string]dataset{}, contents: map[string]string{}}
	server := httptest.NewServer(fixture)
	defer server.Close()
	transport := func(ctx context.Context, method, target, contentType string, body []byte) (int, []byte, error) {
		req, _ := http.NewRequestWithContext(ctx, method, target, bytes.NewReader(body))
		req.Header.Set("Content-Type", contentType)
		response, err := server.Client().Do(req)
		if err != nil {
			return 0, nil, err
		}
		defer response.Body.Close()
		raw, err := io.ReadAll(response.Body)
		return response.StatusCode, raw, err
	}
	backend, _ := New(source, transport, server.URL, "bounded")
	if _, err := backend.Search(context.Background(), scope, "needle", "fact", "L2", 1); err != nil {
		t.Fatal(err)
	}
	if source.requested != MaxCandidates || len(fixture.datasets) != MaxCandidates {
		t.Fatalf("unbounded cold pool: %d/%d", source.requested, len(fixture.datasets))
	}
	source.offset = 1
	if _, err := backend.Search(context.Background(), scope, "needle", "fact", "L2", 1); err != nil {
		t.Fatal(err)
	}
	if len(fixture.datasets) != MaxCandidates+1 {
		t.Fatal("valid prior index was discarded on candidate-pool change")
	}
}
