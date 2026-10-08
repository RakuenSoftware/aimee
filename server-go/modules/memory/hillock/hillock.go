// Package hillock adapts Hillock's stateless HDC retrieval profile to Store.
// Canonical authority, admission, mutations and lifecycle remain with the host.
package hillock

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"math"
	"net/url"
	"strings"

	memory "github.com/JBailes/aimee/server-go/memory"
)

const MaxRecords = 256
const MaxBody = 1 << 20
const UpstreamRevision = "1edd166ead75b85a9ab95cd6ba4faf7011ad567c"

type Transport func(context.Context, string, string, string, []byte) (int, []byte, error)
type Backend struct {
	records   memory.Store
	transport Transport
	endpoint  string
}

func New(records memory.Store, transport Transport, endpoint string) (*Backend, error) {
	target, err := url.Parse(endpoint)
	if records == nil || transport == nil || err != nil || (target.Scheme != "http" && target.Scheme != "https") || target.Host == "" || target.User != nil || target.RawQuery != "" || target.Fragment != "" || (target.Path != "" && target.Path != "/") {
		return nil, memory.ErrUnavailable
	}
	return &Backend{records, transport, strings.TrimSuffix(endpoint, "/")}, nil
}
func (b *Backend) Capabilities() memory.Capability {
	return memory.Capability{Name: "hillock", Version: 1, Operations: []string{"get", "search", "put", "delete", "reset-derived"}}
}
func (b *Backend) Get(ctx context.Context, scope memory.Scope, id int64) (memory.Record, error) {
	return b.records.Get(ctx, scope, id)
}
func (b *Backend) Put(ctx context.Context, scope memory.Scope, record memory.Record) (memory.Record, error) {
	return b.records.Put(ctx, scope, record)
}
func (b *Backend) Delete(ctx context.Context, scope memory.Scope, id int64) (bool, error) {
	return b.records.Delete(ctx, scope, id)
}
func (b *Backend) call(ctx context.Context, method, path string, body []byte, out any) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if len(body) > MaxBody {
		return memory.ErrCapacity
	}
	status, raw, err := b.transport(ctx, method, b.endpoint+path, "application/json", body)
	if ctx.Err() != nil {
		return ctx.Err()
	}
	if err != nil {
		return memory.ErrUnavailable
	}
	if status == 413 {
		return memory.ErrCapacity
	}
	if status != 200 || len(raw) > MaxBody || json.Unmarshal(raw, out) != nil {
		return memory.ErrUnavailable
	}
	return nil
}

// The profile retains no record data, vectors, graph, query history or learned
// associations between requests. Verify that invariant before startup/erasure.
func (b *Backend) ResetDerived(ctx context.Context) error {
	var health struct {
		Version   int    `json:"version"`
		Revision  string `json:"upstream_revision"`
		Stateless bool   `json:"stateless"`
	}
	if err := b.call(ctx, "GET", "/v1/health", nil, &health); err != nil {
		return err
	}
	if health.Version != 1 || health.Revision != UpstreamRevision || !health.Stateless {
		return memory.ErrUnsupported
	}
	return nil
}
func revision(record memory.Record) string {
	raw, _ := json.Marshal(record)
	sum := sha256.Sum256(raw)
	return hex.EncodeToString(sum[:])
}

type candidate struct {
	ID       int64  `json:"id"`
	Revision string `json:"revision"`
	Text     string `json:"text"`
}
type hit struct {
	ID       int64   `json:"id"`
	Revision string  `json:"revision"`
	Score    float64 `json:"score"`
}

func (b *Backend) Search(ctx context.Context, scope memory.Scope, query, kind, tier string, limit int) ([]memory.Record, error) {
	if limit <= 0 || limit > MaxRecords || len(query) > 16384 {
		return nil, memory.ErrCapacity
	}
	if query == "" {
		return b.records.Search(ctx, scope, query, kind, tier, limit)
	}
	source, err := memory.RetrievalCandidates(ctx, b.records, scope, query, kind, tier, MaxRecords)
	if err != nil {
		return nil, err
	}
	if len(source) > MaxRecords {
		return nil, memory.ErrCapacity
	}
	candidates := make([]candidate, 0, len(source))
	wanted := map[int64]memory.Record{}
	for _, record := range source {
		if record.ID <= 0 || record.Scope != scope || (kind != "" && record.Kind != kind) || (tier != "" && record.Tier != tier) {
			return nil, memory.ErrUnavailable
		}
		if _, exists := wanted[record.ID]; exists {
			return nil, memory.ErrUnavailable
		}
		wanted[record.ID] = record
		candidates = append(candidates, candidate{record.ID, revision(record), record.Key + "\n" + record.Content})
	}
	if len(candidates) == 0 {
		return []memory.Record{}, nil
	}
	body, err := json.Marshal(struct {
		Query      string      `json:"query"`
		Candidates []candidate `json:"candidates"`
		Limit      int         `json:"limit"`
	}{query, candidates, limit})
	if err != nil {
		return nil, memory.ErrUnavailable
	}
	var response struct {
		Hits []hit `json:"hits"`
	}
	if err = b.call(ctx, "POST", "/v1/rank", body, &response); err != nil {
		return nil, err
	}
	if response.Hits == nil || len(response.Hits) > limit {
		return nil, memory.ErrUnavailable
	}
	result := make([]memory.Record, 0, len(response.Hits))
	seen := map[int64]bool{}
	previous := math.Inf(1)
	for _, h := range response.Hits {
		record, ok := wanted[h.ID]
		if !ok || seen[h.ID] || h.Revision != revision(record) || math.IsNaN(h.Score) || math.IsInf(h.Score, 0) || h.Score < -1 || h.Score > 1 || h.Score > previous {
			return nil, memory.ErrUnavailable
		}
		current, err := b.records.Get(ctx, scope, h.ID)
		if err != nil {
			return nil, err
		}
		if current.Scope != scope || revision(current) != h.Revision {
			return nil, memory.ErrUnavailable
		}
		seen[h.ID] = true
		previous = h.Score
		result = append(result, current)
	}
	return result, nil
}
