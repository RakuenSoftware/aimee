// Run from server-go: go run ../scripts/validation/memory/qualify-external.go
// All records are synthetic and each run uses a new disposable catalog.
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	memory "github.com/JBailes/aimee/server-go/memory"
	"github.com/JBailes/aimee/server-go/modules/memory/backendstore"
	"github.com/JBailes/aimee/server-go/modules/memory/cognee"
	"github.com/JBailes/aimee/server-go/modules/memory/hillock"
)

const owner = "11111111-1111-4111-8111-111111111111"

type result struct {
	Backend           string                `json:"backend"`
	Records           int                   `json:"records"`
	Workers           int                   `json:"workers"`
	Samples           int                   `json:"samples"`
	CatalogBytes      int                   `json:"catalog_bytes"`
	ImportMS          float64               `json:"import_ms"`
	RestartMS         float64               `json:"restart_ms"`
	ColdMS            float64               `json:"cold_ms"`
	P50MS             float64               `json:"p50_ms"`
	P95MS             float64               `json:"p95_ms"`
	MaxMS             float64               `json:"max_ms"`
	RequestsPerSecond float64               `json:"requests_per_second"`
	Errors            int                   `json:"errors"`
	WrongIdentity     int                   `json:"wrong_identity"`
	Passed            bool                  `json:"passed"`
	HTTP              map[string]httpTiming `json:"provider_http,omitempty"`
}

type httpTiming struct {
	Calls   int     `json:"calls"`
	TotalMS float64 `json:"total_ms"`
	MaxMS   float64 `json:"max_ms"`
}

func millis(d time.Duration) float64 { return float64(d) / float64(time.Millisecond) }

func run(name, endpoint, token, auth string, count, workers, samples int, budget time.Duration) (r result, err error) {
	r = result{Backend: name, Records: count, Workers: workers, Samples: samples}
	dir, err := os.MkdirTemp("", "aimee-memory-qualification-")
	if err != nil {
		return r, err
	}
	defer os.RemoveAll(dir)
	scope := memory.Scope{Type: memory.ScopeProject, Value: "synthetic-scale"}
	state := backendstore.Snapshot{Version: 1, Owner: owner, Next: int64(count + 1), Entries: map[int64]backendstore.Entry{}, Erased: map[int64]bool{}, Receipts: map[string]backendstore.Receipt{}}
	for i := 1; i <= count; i++ {
		content := fmt.Sprintf("Workshop %d discussed scheduling equipment delivery with its participants. Next review is Friday.", i)
		if i == 1 {
			content = "Kibukx is a person. Virant reported Kibukx's height as exactly 69cm, not 69 feet. Source: synthetic-message-alpha."
		}
		id := int64(i)
		state.Entries[id] = backendstore.Entry{Record: memory.Record{ID: id, Scope: scope, Tier: "L2", Kind: "fact", Key: fmt.Sprintf("workshop-%d", i), Content: content, Confidence: 1, Version: &memory.MemoryRecordVersion{SchemaVersion: 1, OwnerID: owner, RecordID: strconv.Itoa(i), RecordRevision: "1"}, Authorship: json.RawMessage(`{"principal":"synthetic-speaker","category":"user_stated"}`)}}
	}
	raw, err := json.Marshal(state)
	if err != nil {
		return r, err
	}
	r.CatalogBytes = len(raw)
	store, err := backendstore.New(filepath.Join(dir, "catalog"), owner)
	if err != nil {
		return r, err
	}
	start := time.Now()
	err = store.Import(context.Background(), raw)
	r.ImportMS = millis(time.Since(start))
	if err != nil {
		return r, err
	}
	start = time.Now()
	store, err = backendstore.New(filepath.Join(dir, "catalog"), owner)
	r.RestartMS = millis(time.Since(start))
	if err != nil {
		return r, err
	}
	var backend memory.Store = store
	var timingMu sync.Mutex
	r.HTTP = map[string]httpTiming{}
	client := &http.Client{Timeout: budget, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	transport := func(ctx context.Context, method, target, contentType string, body []byte) (int, []byte, error) {
		started := time.Now()
		defer func() {
			parsed, _ := url.Parse(target)
			path := parsed.Path
			if strings.HasPrefix(path, "/api/v1/datasets/") {
				path = "/api/v1/datasets/:id"
			}
			key := method + " " + path
			elapsed := millis(time.Since(started))
			timingMu.Lock()
			metric := r.HTTP[key]
			metric.Calls++
			metric.TotalMS += elapsed
			if elapsed > metric.MaxMS {
				metric.MaxMS = elapsed
			}
			r.HTTP[key] = metric
			timingMu.Unlock()
		}()
		request, e := http.NewRequestWithContext(ctx, method, target, bytes.NewReader(body))
		if e != nil {
			return 0, nil, e
		}
		if contentType != "" {
			request.Header.Set("Content-Type", contentType)
		}
		if token != "" {
			if auth == "api-key" {
				request.Header.Set("X-Api-Key", token)
			} else {
				request.Header.Set("Authorization", "Bearer "+token)
			}
		}
		response, e := client.Do(request)
		if e != nil {
			return 0, nil, e
		}
		defer response.Body.Close()
		body, e = io.ReadAll(io.LimitReader(response.Body, (1<<20)+1))
		return response.StatusCode, body, e
	}
	switch name {
	case "catalog":
	case "hillock":
		backend, err = hillock.New(store, transport, endpoint)
	case "cognee":
		backend, err = cognee.New(store, transport, endpoint, fmt.Sprintf("qualification-%d", time.Now().UnixNano()))
	default:
		return r, fmt.Errorf("unknown backend")
	}
	if err != nil {
		return r, err
	}
	if reset, ok := backend.(memory.DerivedResetter); ok {
		ctx, cancel := context.WithTimeout(context.Background(), budget)
		err = reset.ResetDerived(ctx)
		cancel()
		if err != nil {
			return r, err
		}
		defer func() {
			ctx, cancel := context.WithTimeout(context.Background(), budget)
			defer cancel()
			if e := reset.ResetDerived(ctx); e != nil {
				err = fmt.Errorf("provider cleanup failed: %w", e)
				r.Passed = false
			}
		}()
	}
	// The shipping memory handler serializes selected-provider calls before
	// opening their source snapshots. Include that queue in request latency.
	gate := make(chan struct{}, 1)
	search := func() (time.Duration, error, bool) {
		ctx, cancel := context.WithTimeout(context.Background(), budget)
		defer cancel()
		start := time.Now()
		if name != "catalog" {
			select {
			case gate <- struct{}{}:
				defer func() { <-gate }()
			case <-ctx.Done():
				return time.Since(start), ctx.Err(), false
			}
		}
		var records []memory.Record
		var e error
		if name == "catalog" {
			records, e = store.Candidates(ctx, scope, "Kibukx height", "", "", 16)
		} else {
			records, e = backend.Search(ctx, scope, "Kibukx height", "", "", 5)
		}
		found := false
		for _, record := range records {
			if record.ID == 1 && record.Scope == scope && record.Content == state.Entries[1].Record.Content {
				found = true
			}
		}
		return time.Since(start), e, found
	}
	d, e, found := search()
	r.ColdMS = millis(d)
	if e != nil {
		r.Errors++
	}
	if !found {
		r.WrongIdentity++
	}
	durations := make([]float64, samples)
	var mu sync.Mutex
	var wg sync.WaitGroup
	jobs := make(chan int)
	start = time.Now()
	for i := 0; i < workers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for index := range jobs {
				d, e, found := search()
				mu.Lock()
				durations[index] = millis(d)
				if e != nil {
					r.Errors++
				}
				if !found {
					r.WrongIdentity++
				}
				mu.Unlock()
			}
		}()
	}
	for i := 0; i < samples; i++ {
		jobs <- i
	}
	close(jobs)
	wg.Wait()
	r.RequestsPerSecond = float64(samples) / time.Since(start).Seconds()
	sort.Float64s(durations)
	r.P50MS = durations[(samples-1)/2]
	r.P95MS = durations[(samples*95+99)/100-1]
	r.MaxMS = durations[samples-1]
	r.Passed = r.Errors == 0 && r.WrongIdentity == 0 && r.MaxMS < millis(budget)
	return r, nil
}

func main() {
	name := flag.String("backend", "catalog", "catalog, hillock or cognee")
	endpoint := flag.String("endpoint", "", "disposable provider origin")
	tokenFile := flag.String("token-file", "", "private provider token file (never printed)")
	auth := flag.String("auth", "bearer", "bearer or api-key")
	sizes := flag.String("sizes", "100,1000,10000", "synthetic corpus sizes")
	workers := flag.Int("workers", 4, "concurrent readers")
	samples := flag.Int("samples", 12, "warm requests per corpus")
	deadline := flag.Duration("deadline", 30*time.Second, "per-request acceptance deadline")
	output := flag.String("output", "", "JSON evidence file")
	flag.Parse()
	if *workers < 1 || *samples < 1 || *workers > 64 || *samples > 10000 || *deadline <= 0 || (*auth != "bearer" && *auth != "api-key") || (*name != "catalog" && *endpoint == "") {
		fmt.Fprintln(os.Stderr, "invalid qualification configuration")
		os.Exit(2)
	}
	token := ""
	if *tokenFile != "" {
		raw, e := os.ReadFile(*tokenFile)
		if e != nil {
			fmt.Fprintln(os.Stderr, "cannot read provider credential")
			os.Exit(2)
		}
		token = strings.TrimSpace(string(raw))
	}
	results := []result{}
	passed := true
	for _, size := range strings.Split(*sizes, ",") {
		count, e := strconv.Atoi(size)
		if e != nil || count < 1 || count > 100000 {
			fmt.Fprintln(os.Stderr, "invalid corpus size")
			os.Exit(2)
		}
		r, e := run(*name, *endpoint, token, *auth, count, *workers, *samples, *deadline)
		if e != nil {
			fmt.Fprintln(os.Stderr, "qualification failed:", e)
			r.Passed = false
		}
		results = append(results, r)
		passed = passed && r.Passed
	}
	raw, _ := json.MarshalIndent(map[string]any{"workload": "synthetic attributed records; one exact lexical needle; concurrent readers", "deadline_ms": millis(*deadline), "passed": passed, "results": results}, "", "  ")
	raw = append(raw, '\n')
	if *output != "" {
		if e := os.WriteFile(*output, raw, 0600); e != nil {
			fmt.Fprintln(os.Stderr, e)
			os.Exit(1)
		}
	}
	fmt.Print(string(raw))
	if !passed {
		os.Exit(1)
	}
}
