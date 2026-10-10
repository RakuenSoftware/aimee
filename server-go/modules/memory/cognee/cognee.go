// Package cognee adapts Cognee to the memory Store contract. It has no bus,
// module identity, SQL, authorization, audit or process-lifecycle implementation.
package cognee

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	memory "github.com/JBailes/aimee/server-go/memory"
	"math"
	"mime/multipart"
	"net/url"
	"sort"
	"strconv"
	"strings"
)

const MaxRecords = 256

// MaxCandidates bounds synchronous cold indexing within the shipping deadline.
// This is a provider pool limit, not a canonical catalog size limit.
const MaxCandidates = 16
const MaxBody = 1 << 20

// Transport is supplied by the host. Aimee supplies its existing governed
// egress client; tests or other hosts can supply their own HTTP implementation.
type Transport func(context.Context, string, string, string, []byte) (int, []byte, error)

type Backend struct {
	records   memory.Store
	transport Transport
	endpoint  string
	namespace string
}

func New(records memory.Store, transport Transport, endpoint, namespace string) (*Backend, error) {
	target, err := url.Parse(endpoint)
	if records == nil || transport == nil || err != nil || (target.Scheme != "https" && target.Scheme != "http") || target.Host == "" || target.User != nil || target.RawQuery != "" || target.Fragment != "" || (target.Path != "" && target.Path != "/") || namespace == "" || len(namespace) > 256 {
		return nil, memory.ErrUnavailable
	}
	sum := sha256.Sum256([]byte(namespace))
	return &Backend{records: records, transport: transport, endpoint: strings.TrimSuffix(endpoint, "/"), namespace: hex.EncodeToString(sum[:16])}, nil
}
func (b *Backend) Capabilities() memory.Capability {
	return memory.Capability{Name: "cognee", Version: 1, Operations: []string{"get", "search", "put", "delete", "forget", "reset-derived"}}
}
func (b *Backend) Get(ctx context.Context, scope memory.Scope, id int64) (memory.Record, error) {
	return b.records.Get(ctx, scope, id)
}
func (b *Backend) Put(ctx context.Context, scope memory.Scope, record memory.Record) (memory.Record, error) {
	return b.records.Put(ctx, scope, record)
}

// Records remain in Aimee's fixed source store. Cognee owns their derived
// retrieval state; successful deletion requires its matching datasets removed.
func (b *Backend) Delete(ctx context.Context, scope memory.Scope, id int64) (bool, error) {
	if err := b.Forget(ctx, scope, id); err != nil {
		return false, err
	}
	return b.records.Delete(ctx, scope, id)
}

// Forget removes all derived revisions; it does not require a serving-eligible
// Get, so expired or already retired source records can still be cleaned up.
func (b *Backend) Forget(ctx context.Context, scope memory.Scope, id int64) error {
	if id <= 0 {
		return memory.ErrUnavailable
	}
	datasets, err := b.datasets(ctx)
	if err != nil {
		return err
	}
	prefix := b.prefix(scope) + strconv.FormatInt(id, 10) + "_"
	for _, dataset := range datasets {
		if strings.HasPrefix(dataset.Name, prefix) {
			if err = b.remove(ctx, dataset.ID); err != nil {
				return err
			}
		}
	}
	return nil
}

type dataset struct {
	ID   string `json:"id"`
	Name string `json:"name"`
}

func validUUID(id string) bool {
	if len(id) != 36 {
		return false
	}
	for i, c := range id {
		if i == 8 || i == 13 || i == 18 || i == 23 {
			if c != '-' {
				return false
			}
		} else if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}
func (b *Backend) call(ctx context.Context, method, path, contentType string, body []byte, out any) error {
	if ctx.Err() != nil {
		return ctx.Err()
	}
	if len(body) > MaxBody {
		return memory.ErrCapacity
	}
	status, raw, err := b.transport(ctx, method, b.endpoint+path, contentType, body)
	if err != nil {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		return memory.ErrUnavailable
	}
	if status < 200 || status >= 300 || len(raw) > MaxBody {
		return memory.ErrUnavailable
	}
	if out != nil && json.Unmarshal(raw, out) != nil {
		return memory.ErrUnavailable
	}
	return nil
}
func (b *Backend) datasets(ctx context.Context) ([]dataset, error) {
	var out []dataset
	if err := b.call(ctx, "GET", "/api/v1/datasets", "", nil, &out); err != nil {
		return nil, err
	}
	if len(out) > 10000 {
		return nil, memory.ErrCapacity
	}
	seen := map[string]bool{}
	for _, d := range out {
		if !validUUID(d.ID) || d.Name == "" || seen[d.Name] {
			return nil, memory.ErrUnavailable
		}
		seen[d.Name] = true
	}
	return out, nil
}
func (b *Backend) remove(ctx context.Context, id string) error {
	if !validUUID(id) {
		return memory.ErrUnavailable
	}
	return b.call(ctx, "DELETE", "/api/v1/datasets/"+id, "", nil, nil)
}
func (b *Backend) prefix(scope memory.Scope) string {
	raw, _ := json.Marshal(scope)
	if source, ok := b.records.(memory.NamespaceStore); ok {
		raw = append(raw, []byte("/"+source.Namespace(scope))...)
	}
	sum := sha256.Sum256(raw)
	return "aimee_" + b.namespace + "_" + hex.EncodeToString(sum[:16]) + "_"
}
func (b *Backend) name(record memory.Record) string {
	raw, _ := json.Marshal(record)
	sum := sha256.Sum256(raw)
	return b.prefix(record.Scope) + strconv.FormatInt(record.ID, 10) + "_" + hex.EncodeToString(sum[:16])
}
func (b *Backend) add(ctx context.Context, name string, record memory.Record) error {
	var buffer bytes.Buffer
	writer := multipart.NewWriter(&buffer)
	if err := writer.WriteField("datasetName", name); err != nil {
		return err
	}
	file, err := writer.CreateFormFile("data", "memory.txt")
	if err != nil {
		return err
	}
	if _, err = file.Write([]byte(record.Key + "\n" + record.Content)); err != nil {
		return err
	}
	if err = writer.Close(); err != nil {
		return err
	}
	var result json.RawMessage
	return b.call(ctx, "POST", "/api/v1/add", writer.FormDataContentType(), buffer.Bytes(), &result)
}

// Search first reconciles the exact authorized source scope, then asks Cognee
// for retrieval-only CHUNKS. Each dataset represents one exact source record.
// Scores are compared only within this Cognee backend; generated prose never
// becomes an Aimee record. Final records are re-read through the same contract.
func (b *Backend) Search(ctx context.Context, scope memory.Scope, query, kind, tier string, limit int) ([]memory.Record, error) {
	if ctx.Err() != nil {
		return nil, ctx.Err()
	}
	if query == "" {
		return b.records.Search(ctx, scope, query, kind, tier, limit)
	}
	if limit <= 0 || limit > MaxRecords || len(query) > 16384 {
		return nil, memory.ErrCapacity
	}
	candidateLimit := MaxRecords
	if _, ok := b.records.(memory.CandidateStore); ok {
		candidateLimit = MaxCandidates
	}
	source, err := memory.RetrievalCandidates(ctx, b.records, scope, query, kind, tier, candidateLimit)
	if err != nil {
		return nil, err
	}
	if len(source) > MaxRecords {
		return nil, memory.ErrCapacity
	}
	wanted := map[string]memory.Record{}
	for _, record := range source {
		if record.ID <= 0 || record.Scope != scope {
			return nil, memory.ErrUnavailable
		}
		name := b.name(record)
		if _, exists := wanted[name]; exists {
			return nil, memory.ErrUnavailable
		}
		wanted[name] = record
	}
	datasets, err := b.datasets(ctx)
	if err != nil {
		return nil, err
	}
	known := map[string]bool{}
	prefix := b.prefix(scope)
	for _, d := range datasets {
		if !strings.HasPrefix(d.Name, prefix) {
			continue
		}
		if _, ok := wanted[d.Name]; !ok {
			// Keep valid previously indexed records outside this query's pool.
			// Search names remain restricted to the admitted current candidates.
			parts := strings.Split(strings.TrimPrefix(d.Name, prefix), "_")
			if len(parts) == 2 {
				id, parseErr := strconv.ParseInt(parts[0], 10, 64)
				if parseErr == nil {
					current, getErr := b.records.Get(ctx, scope, id)
					if getErr == nil && b.name(current) == d.Name {
						continue
					}
					if getErr != nil && !errors.Is(getErr, memory.ErrNotFound) {
						return nil, getErr
					}
				}
			}
			if err = b.remove(ctx, d.ID); err != nil {
				return nil, err
			}
		} else {
			known[d.Name] = true
		}
	}
	names := make([]string, 0, len(wanted))
	for name := range wanted {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		if !known[name] {
			if err = b.add(ctx, name, wanted[name]); err != nil {
				return nil, err
			}
		}
	}
	if len(names) == 0 {
		return []memory.Record{}, nil
	}
	// Synchronous cognification reconciles an earlier unknown indexing outcome.
	body, _ := json.Marshal(map[string]any{"datasets": names, "run_in_background": false})
	var runs map[string]struct {
		Status string `json:"status"`
		Name   string `json:"dataset_name"`
	}
	if err = b.call(ctx, "POST", "/api/v1/cognify", "application/json", body, &runs); err != nil {
		return nil, err
	}
	completed := map[string]bool{}
	for _, run := range runs {
		if _, known := wanted[run.Name]; !known || completed[run.Name] || (run.Status != "PipelineRunCompleted" && run.Status != "PipelineRunAlreadyCompleted") {
			return nil, memory.ErrUnavailable
		}
		completed[run.Name] = true
	}
	if len(completed) != len(names) {
		return nil, memory.ErrUnavailable
	}
	body, _ = json.Marshal(map[string]any{"datasets": names, "query": query, "search_type": "CHUNKS", "top_k": 1, "only_context": false})
	var results []struct {
		DatasetName string `json:"dataset_name"`
		Chunks      []struct {
			Score *float64 `json:"score"`
		} `json:"search_result"`
	}
	if err = b.call(ctx, "POST", "/api/v1/search", "application/json", body, &results); err != nil {
		return nil, err
	}
	if len(results) > len(names) {
		return nil, memory.ErrUnavailable
	}
	type hit struct {
		record   memory.Record
		distance float64
	}
	var hits []hit
	seen := map[string]bool{}
	for _, result := range results {
		record, ok := wanted[result.DatasetName]
		if !ok || seen[result.DatasetName] {
			return nil, memory.ErrUnavailable
		}
		seen[result.DatasetName] = true
		if len(result.Chunks) == 0 {
			continue
		}
		if len(result.Chunks) > 1 || result.Chunks[0].Score == nil {
			return nil, memory.ErrUnavailable
		}
		score := *result.Chunks[0].Score
		if math.IsNaN(score) || math.IsInf(score, 0) {
			return nil, memory.ErrUnavailable
		}
		if (kind == "" || record.Kind == kind) && (tier == "" || record.Tier == tier) {
			hits = append(hits, hit{record, score})
		}
	}
	sort.SliceStable(hits, func(i, j int) bool {
		if hits[i].distance == hits[j].distance {
			return hits[i].record.ID < hits[j].record.ID
		}
		return hits[i].distance < hits[j].distance
	})
	if len(hits) > limit {
		hits = hits[:limit]
	}
	out := make([]memory.Record, 0, len(hits))
	for _, hit := range hits {
		current, err := b.records.Get(ctx, scope, hit.record.ID)
		if err != nil {
			return nil, err
		}
		if !sameSource(hit.record, current) {
			return nil, memory.ErrUnavailable
		}
		out = append(out, current)
	}
	return out, nil
}

var _ memory.Backend = (*Backend)(nil)

func sameSource(before, after memory.Record) bool {
	if before.ID != after.ID || before.Scope != after.Scope || before.Key != after.Key || before.Content != after.Content || before.Kind != after.Kind || before.Tier != after.Tier || before.Confidence != after.Confidence {
		return false
	}
	if before.Version != nil && after.Version != nil && *before.Version != *after.Version {
		return false
	}
	return true
}

// ResetDerived deliberately discards the node's entire derived index. This
// covers historical revisions and records already removed by subject erasure
// without requiring another subject/provenance database inside the provider.
func (b *Backend) ResetDerived(ctx context.Context) error {
	datasets, err := b.datasets(ctx)
	if err != nil {
		return err
	}
	prefix := "aimee_" + b.namespace + "_"
	for _, d := range datasets {
		if strings.HasPrefix(d.Name, prefix) {
			if err := b.remove(ctx, d.ID); err != nil {
				return err
			}
		}
	}
	// A lost deletion response or a concurrently reappearing dataset must not
	// become a successful erasure receipt. The host serializes its writers.
	remaining, err := b.datasets(ctx)
	if err != nil {
		return err
	}
	for _, d := range remaining {
		if strings.HasPrefix(d.Name, prefix) {
			return memory.ErrUnavailable
		}
	}
	return nil
}

var _ memory.DerivedResetter = (*Backend)(nil)
