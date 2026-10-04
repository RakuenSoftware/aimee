package cognee

import (
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
