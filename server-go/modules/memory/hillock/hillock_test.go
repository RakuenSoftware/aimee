package hillock

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	memory "github.com/JBailes/aimee/server-go/memory"
	"github.com/JBailes/aimee/server-go/modules/memory/backendstore"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
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

func TestHillockValidation(t *testing.T) {
	ctx := context.Background()
	scope := memory.Scope{Type: memory.ScopeProject, Value: "contract-live"}
	original := memory.Record{ID: 1, Scope: scope, Key: "height", Content: "Kibukx's height is 69cm"}
	for _, mode := range []string{"valid", "foreign", "duplicate", "revision", "mutated", "outage", "capacity", "cancel"} {
		t.Run(mode, func(t *testing.T) {
			source := &catalog{map[int64]memory.Record{1: original}}
			transport := func(_ context.Context, method, target, kind string, raw []byte) (int, []byte, error) {
				if mode == "outage" {
					return 503, nil, nil
				}
				if mode == "capacity" {
					return 413, nil, nil
				}
				var input struct {
					Candidates []candidate `json:"candidates"`
				}
				if err := json.Unmarshal(raw, &input); err != nil {
					t.Fatal(err)
				}
				if len(input.Candidates) != 1 {
					t.Fatal("wrong admitted snapshot")
				}
				h := hit{1, input.Candidates[0].Revision, 1}
				if mode == "foreign" {
					h.ID = 2
				}
				if mode == "revision" {
					h.Revision = strings.Repeat("0", 64)
				}
				if mode == "mutated" {
					r := source.records[1]
					r.Content = "different correction"
					source.records[1] = r
				}
				hits := []hit{h}
				if mode == "duplicate" {
					hits = append(hits, h)
				}
				raw, _ = json.Marshal(map[string]any{"hits": hits})
				return 200, raw, nil
			}
			b, err := New(source, transport, "http://127.0.0.1:8097")
			if err != nil {
				t.Fatal(err)
			}
			requestCtx := ctx
			if mode == "cancel" {
				c, cancel := context.WithCancel(ctx)
				cancel()
				requestCtx = c
			}
			got, err := b.Search(requestCtx, scope, "height", "", "", 2)
			if mode == "valid" {
				if err != nil || len(got) != 1 || got[0].Content != original.Content {
					t.Fatal(got, err)
				}
			} else if err == nil {
				t.Fatal("unsafe result accepted", mode)
			}
		})
	}
	source := &catalog{map[int64]memory.Record{}}
	calls := 0
	b, _ := New(source, func(context.Context, string, string, string, []byte) (int, []byte, error) {
		calls++
		return 200, nil, nil
	}, "http://localhost")
	for i := int64(1); i <= MaxRecords+1; i++ {
		source.records[i] = memory.Record{ID: i, Scope: scope}
	}
	if _, err := b.Search(ctx, scope, "height", "", "", 1); !errors.Is(err, memory.ErrCapacity) || calls != 0 {
		t.Fatal("snapshot overflow was truncated", err, calls)
	}
	for _, url := range []string{"", "file:///tmp", "http://user:pass@localhost", "http://localhost/path", "http://localhost?query=x"} {
		if _, err := New(source, b.transport, url); err == nil {
			t.Fatal("invalid endpoint accepted", url)
		}
	}
}

func TestHillockLiveContract(t *testing.T) {
	endpoint := os.Getenv("AIMEE_HILLOCK_TEST_URL")
	if endpoint == "" {
		t.Skip("run scripts/validation/memory/run-hillock-contract.py")
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	transport := func(ctx context.Context, method, target, kind string, body []byte) (int, []byte, error) {
		req, err := http.NewRequestWithContext(ctx, method, target, strings.NewReader(string(body)))
		if err != nil {
			return 0, nil, err
		}
		req.Header.Set("Content-Type", kind)
		req.Header.Set("Authorization", "Bearer contract-fixture")
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return 0, nil, err
		}
		defer resp.Body.Close()
		raw, err := io.ReadAll(io.LimitReader(resp.Body, MaxBody+1))
		return resp.StatusCode, raw, err
	}
	scope := memory.Scope{Type: memory.ScopeProject, Value: "contract-live"}
	dir := filepath.Join(t.TempDir(), "catalog")
	source, err := backendstore.New(dir, "11111111-1111-4111-8111-111111111111")
	if err != nil {
		t.Fatal(err)
	}
	first, err := source.Put(ctx, scope, memory.Record{Key: "Kibukx height", Content: "Kibukx height is 69cm", Kind: "fact"})
	if err != nil {
		t.Fatal(err)
	}
	if _, err = source.Put(ctx, scope, memory.Record{Key: "Mountain elevation", Content: "Mountain elevation is 69 feet", Kind: "fact"}); err != nil {
		t.Fatal(err)
	}
	source, err = backendstore.New(dir, "11111111-1111-4111-8111-111111111111")
	if err != nil {
		t.Fatal(err)
	}
	b, err := New(source, transport, endpoint)
	if err != nil {
		t.Fatal(err)
	}
	if err = b.ResetDerived(ctx); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 3; i++ {
		got, err := b.Search(ctx, scope, "Kibukx height", "fact", "", 1)
		if err != nil || len(got) != 1 || got[0].ID != 1 || got[0].Content != "Kibukx height is 69cm" {
			t.Fatal(got, err)
		}
	}
	corrected := first
	corrected.Content = "Kibukx height is 170cm"
	if _, err = b.Put(ctx, scope, corrected); err != nil {
		t.Fatal(err)
	}
	got, err := b.Search(ctx, scope, "Kibukx height", "fact", "", 1)
	if err != nil || len(got) != 1 || got[0].Content != corrected.Content {
		t.Fatal("stale correction", got, err)
	}
	if _, err = b.Delete(ctx, scope, 1); err != nil {
		t.Fatal(err)
	}
	got, err = b.Search(ctx, scope, "Kibukx height", "fact", "", 1)
	if err != nil {
		t.Fatal(err)
	}
	for _, r := range got {
		if r.ID == 1 {
			t.Fatal("deleted record resurfaced")
		}
	}
	// The service must not retrieve a previously supplied record independently.
	raw, _ := json.Marshal(map[string]any{"query": "Kibukx height", "candidates": []candidate{}, "limit": 1})
	var empty struct {
		Hits []hit `json:"hits"`
	}
	if err = b.call(ctx, "POST", "/v1/rank", raw, &empty); err != nil || len(empty.Hits) != 0 {
		t.Fatal("cross-request retention", empty, err)
	}
	for i := int64(3); i <= MaxRecords+3; i++ {
		if _, err = source.Put(ctx, scope, memory.Record{Kind: "fact", Key: fmt.Sprint(i), Content: fmt.Sprint(i)}); err != nil {
			t.Fatal(err)
		}
	}
	late, err := source.Put(ctx, scope, memory.Record{Kind: "fact", Key: "late lighthouse", Content: "lighthouse height is 69cm"})
	if err != nil {
		t.Fatal(err)
	}
	got, err = b.Search(ctx, scope, "lighthouse height", "fact", "", 1)
	if err != nil || len(got) != 1 || got[0].ID != late.ID {
		t.Fatal("late candidate missing from larger catalog", got, err)
	}
	all, err := source.Search(ctx, scope, "", "", "", MaxRecords+10)
	if err != nil || len(all) <= MaxRecords {
		t.Fatal("catalog was truncated", len(all), err)
	}
}
