package qdrant

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/db3"
	"github.com/JBailes/aimee/server-go/modules/vectordb"
)

// A recording fake Qdrant. The REST shapes are the contract with a server this
// repository does not own, so they are asserted rather than assumed: a silent
// change to a field name would otherwise show up as an empty corpus.
type fakeQdrant struct {
	server   *httptest.Server
	mu       chan struct{}
	requests []recorded
	hits     []map[string]any
	status   map[string]int
}

type recorded struct {
	method string
	path   string
	body   map[string]any
}

func newFake(t *testing.T) *fakeQdrant {
	t.Helper()
	fake := &fakeQdrant{mu: make(chan struct{}, 1), status: map[string]int{}}
	fake.mu <- struct{}{}
	fake.server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body := map[string]any{}
		if raw, _ := io.ReadAll(r.Body); len(raw) > 0 {
			_ = json.Unmarshal(raw, &body)
		}
		<-fake.mu
		fake.requests = append(fake.requests, recorded{r.Method, r.URL.Path + "?" + r.URL.RawQuery, body})
		status := fake.status[r.URL.Path]
		hits := fake.hits
		fake.mu <- struct{}{}
		if status != 0 {
			w.WriteHeader(status)
			_, _ = w.Write([]byte(`{"status":{"error":"nope"}}`))
			return
		}
		w.Header().Set("Content-Type", "application/json")
		if strings.HasSuffix(r.URL.Path, "/points/search") {
			_ = json.NewEncoder(w).Encode(map[string]any{"result": hits})
			return
		}
		_, _ = w.Write([]byte(`{"result":true}`))
	}))
	t.Cleanup(fake.server.Close)
	return fake
}

func (f *fakeQdrant) seen(t *testing.T) []recorded {
	t.Helper()
	<-f.mu
	defer func() { f.mu <- struct{}{} }()
	return append([]recorded(nil), f.requests...)
}

func (f *fakeQdrant) setHits(hits []map[string]any) {
	<-f.mu
	f.hits = hits
	f.mu <- struct{}{}
}

func newBackend(t *testing.T, fake *fakeQdrant, metric vectordb.Metric) *Backend {
	t.Helper()
	backend, err := New(Config{
		URL: fake.server.URL, Dimension: 3, Metric: metric,
		HTTPClient: fake.server.Client(),
	})
	if err != nil {
		t.Fatal(err)
	}
	return backend
}

func find(t *testing.T, fake *fakeQdrant, method, contains string) recorded {
	t.Helper()
	for _, request := range fake.seen(t) {
		if request.method == method && strings.Contains(request.path, contains) {
			return request
		}
	}
	t.Fatalf("no %s request matching %q in %+v", method, contains, fake.seen(t))
	return recorded{}
}

func TestUpsertCreatesTheCollectionOnceAndCarriesLabels(t *testing.T) {
	fake := newFake(t)
	backend := newBackend(t, fake, vectordb.Cosine)
	ctx := context.Background()

	for i := 0; i < 3; i++ {
		if err := backend.Upsert(ctx, "memory", int64(i+1), []float32{1, 0, 0},
			[]db3.ExactLabel{{Key: "project", Value: "alpha"}}); err != nil {
			t.Fatalf("upsert %d: %v", i, err)
		}
	}

	// Created once, not before every write.
	creates := 0
	for _, request := range fake.seen(t) {
		if request.method == http.MethodPut && strings.HasPrefix(request.path, "/collections/memory?") {
			creates++
		}
	}
	if creates != 1 {
		t.Errorf("collection was created %d times, want once", creates)
	}

	write := find(t, fake, http.MethodPut, "/collections/memory/points")
	points, _ := write.body["points"].([]any)
	if len(points) != 1 {
		t.Fatalf("upsert body carried %d points, want 1", len(points))
	}
	point, _ := points[0].(map[string]any)
	payload, _ := point["payload"].(map[string]any)
	if payload["project"] != "alpha" {
		t.Errorf("payload project = %v, want alpha", payload["project"])
	}
	// An upsert clears the tombstone; DB2 re-upserting means reachable again.
	if payload[tombstonePayloadKey] != false {
		t.Errorf("upsert did not clear the tombstone flag (%v)", payload[tombstonePayloadKey])
	}
}

func TestSearchSendsEveryFilterAsMustAndExcludesTombstones(t *testing.T) {
	fake := newFake(t)
	fake.setHits([]map[string]any{{"id": 41, "score": 0.9}, {"id": 42, "score": 0.5}})
	backend := newBackend(t, fake, vectordb.Cosine)

	candidates, err := backend.Search(context.Background(), "memory", []float32{1, 0, 0}, 2,
		[]db3.ExactLabel{
			{Key: "project", Value: "alpha"},
			{Key: "workspace", Value: "w1"},
		})
	if err != nil {
		t.Fatal(err)
	}
	if len(candidates) != 2 || candidates[0].PointID != 41 || candidates[1].PointID != 42 {
		t.Fatalf("candidates = %+v", candidates)
	}

	search := find(t, fake, http.MethodPost, "/points/search")
	filter, _ := search.body["filter"].(map[string]any)
	must, _ := filter["must"].([]any)
	if len(must) != 2 {
		t.Fatalf("filter carried %d must clauses, want 2 (a widened filter returns another workspace's ids)", len(must))
	}
	mustNot, _ := filter["must_not"].([]any)
	if len(mustNot) != 1 {
		t.Fatalf("filter carried %d must_not clauses, want the tombstone exclusion", len(mustNot))
	}
	// The provider returns opaque ids and scores only.
	if search.body["with_payload"] != false || search.body["with_vector"] != false {
		t.Error("search asked Qdrant for payload or vectors; a provider must return neither")
	}
}

func TestEuclidScoresAreNegatedSoLargerIsAlwaysNearer(t *testing.T) {
	// Qdrant returns a DISTANCE for Euclid and a similarity for the others.
	// Passing a distance through unchanged inverts every ranking while still
	// returning plausible ids, which no caller can detect.
	fake := newFake(t)
	fake.setHits([]map[string]any{{"id": 1, "score": 0.25}, {"id": 2, "score": 4.0}})

	euclid := newBackend(t, fake, vectordb.L2)
	candidates, err := euclid.Search(context.Background(), "memory", []float32{1, 0, 0}, 2, nil)
	if err != nil {
		t.Fatal(err)
	}
	if candidates[0].Score != -0.25 || candidates[1].Score != -4.0 {
		t.Fatalf("euclid scores = %+v, want negated", candidates)
	}
	if !(candidates[0].Score > candidates[1].Score) {
		t.Error("the nearer point did not score higher")
	}

	cosine := newBackend(t, fake, vectordb.Cosine)
	candidates, err = cosine.Search(context.Background(), "memory", []float32{1, 0, 0}, 2, nil)
	if err != nil {
		t.Fatal(err)
	}
	if candidates[0].Score != 0.25 {
		t.Errorf("cosine score was altered (%v); only Euclid is negated", candidates[0].Score)
	}
}

func TestTombstoneWritesAPayloadFlagRatherThanDeleting(t *testing.T) {
	// DB2's tombstone keeps the identity while making the point unreachable, so
	// the id is never reused. A delete would preserve only one of those.
	fake := newFake(t)
	backend := newBackend(t, fake, vectordb.Cosine)
	if err := backend.Tombstone(context.Background(), "memory", 7); err != nil {
		t.Fatal(err)
	}
	request := find(t, fake, http.MethodPost, "/points/payload")
	payload, _ := request.body["payload"].(map[string]any)
	if payload[tombstonePayloadKey] != true {
		t.Errorf("tombstone payload = %v", request.body["payload"])
	}
	for _, seen := range fake.seen(t) {
		if strings.Contains(seen.path, "/points/delete") {
			t.Error("tombstone deleted the point; the id must stay taken")
		}
	}
}

func TestWrongWidthIsRefusedRatherThanSent(t *testing.T) {
	fake := newFake(t)
	backend := newBackend(t, fake, vectordb.Cosine)
	if err := backend.Upsert(context.Background(), "memory", 1, []float32{1, 0}, nil); err == nil {
		t.Fatal("a vector of the wrong width was accepted")
	}
	if candidates, err := backend.Search(context.Background(), "memory", []float32{1, 0}, 2, nil); err != nil || candidates != nil {
		t.Errorf("a search at the wrong width returned (%v, %v), want (nil, nil)", candidates, err)
	}
}

func TestConfigurationIsRefusedRatherThanDefaulted(t *testing.T) {
	if _, err := New(Config{URL: "", Dimension: 3}); err == nil {
		t.Error("an empty URL was accepted")
	}
	if _, err := New(Config{URL: "http://x", Dimension: 0}); err == nil {
		t.Error("a zero dimension was accepted")
	}
}

func TestCollectionPrefixNamespacesOneQdrantAcrossDeployments(t *testing.T) {
	fake := newFake(t)
	backend, err := New(Config{
		URL: fake.server.URL, Dimension: 3, Metric: vectordb.Cosine,
		CollectionPrefix: "tenant1", HTTPClient: fake.server.Client(),
	})
	if err != nil {
		t.Fatal(err)
	}
	if err := backend.Upsert(context.Background(), "memory", 1, []float32{1, 0, 0}, nil); err != nil {
		t.Fatal(err)
	}
	find(t, fake, http.MethodPut, "/collections/tenant1_memory/points")
}

func TestAFailedWriteIsReported(t *testing.T) {
	// The store's own version is no longer this backend's concern -- PostgreSQL
	// stamps the generation and the provider carries it -- but a write that did
	// not land must still say so, or the outbox would mark it applied.
	fake := newFake(t)
	backend := newBackend(t, fake, vectordb.Cosine)
	ctx := context.Background()
	if err := backend.Upsert(ctx, "memory", 1, []float32{1, 0, 0}, nil); err != nil {
		t.Fatal(err)
	}

	<-fake.mu
	fake.status["/collections/memory/points"] = http.StatusInternalServerError
	fake.mu <- struct{}{}

	if err := backend.Upsert(ctx, "memory", 2, []float32{0, 1, 0}, nil); err == nil {
		t.Fatal("a failing store reported a successful upsert")
	}
}
