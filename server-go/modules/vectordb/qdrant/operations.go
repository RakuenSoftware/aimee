package qdrant

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"

	"github.com/JBailes/aimee/server-go/db3"
)

// maxErrorBody bounds what is read from a failed response.
//
// A store that answers an error with a gigabyte would otherwise be able to
// exhaust the provider through its error path.
const maxErrorBody = 4096

// do performs one JSON request and decodes into out when out is non-nil.
func (b *Backend) do(ctx context.Context, method, path string, body, out any) error {
	var reader io.Reader
	if body != nil {
		encoded, err := json.Marshal(body)
		if err != nil {
			return fmt.Errorf("qdrant: encoding %s %s: %w", method, path, err)
		}
		reader = bytes.NewReader(encoded)
	}
	request, err := http.NewRequestWithContext(ctx, method, b.config.URL+path, reader)
	if err != nil {
		return fmt.Errorf("qdrant: building %s %s: %w", method, path, err)
	}
	if body != nil {
		request.Header.Set("Content-Type", "application/json")
	}
	if b.config.APIKey != "" {
		request.Header.Set("api-key", b.config.APIKey)
	}
	response, err := b.client.Do(request)
	if err != nil {
		return fmt.Errorf("qdrant: %s %s: %w", method, path, err)
	}
	defer response.Body.Close()
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		detail, _ := io.ReadAll(io.LimitReader(response.Body, maxErrorBody))
		return fmt.Errorf("qdrant: %s %s: status %d: %s",
			method, path, response.StatusCode, bytes.TrimSpace(detail))
	}
	if out == nil {
		return nil
	}
	if err := json.NewDecoder(response.Body).Decode(out); err != nil {
		return fmt.Errorf("qdrant: decoding %s %s: %w", method, path, err)
	}
	return nil
}

// ensureCollection creates the collection on first use.
//
// Created here rather than required in advance because the DB3 collection set
// is decided by DB2's projection catalog, not by whoever installed Qdrant. An
// operator who had to pre-create them would have to track that catalog by hand,
// and a missing one would surface as an empty corpus.
func (b *Backend) ensureCollection(ctx context.Context, collection string) error {
	name := b.collectionName(collection)
	if _, done := b.ensured.Load(name); done {
		return nil
	}
	// A conflict means somebody else created it first, which is success.
	err := b.do(ctx, http.MethodPut, "/collections/"+name, map[string]any{
		"vectors": map[string]any{
			"size":     b.config.Dimension,
			"distance": b.distanceName(),
		},
	}, nil)
	if err != nil && !isConflict(err) {
		return err
	}
	b.ensured.Store(name, struct{}{})
	return nil
}

func isConflict(err error) bool {
	return err != nil && (containsStatus(err, 409) || containsStatus(err, 400))
}

func containsStatus(err error, status int) bool {
	return err != nil && bytes.Contains([]byte(err.Error()), []byte(fmt.Sprintf("status %d", status)))
}

// Upsert stores one vector and the labels a filter may match on.
func (b *Backend) Upsert(ctx context.Context, collection string, pointID int64,
	vector []float32, labels []db3.ExactLabel) error {
	if len(vector) != b.config.Dimension {
		return fmt.Errorf("qdrant: vector width %d is not the store's %d",
			len(vector), b.config.Dimension)
	}
	if err := b.ensureCollection(ctx, collection); err != nil {
		return err
	}
	payload := make(map[string]any, len(labels)+1)
	for _, label := range labels {
		payload[label.Key] = label.Value
	}
	// An upsert clears any tombstone: DB2 re-upserting a point is DB2 saying it
	// is reachable again, and leaving the flag would keep it hidden forever.
	payload[tombstonePayloadKey] = false
	err := b.do(ctx, http.MethodPut,
		"/collections/"+b.collectionName(collection)+"/points?wait=true",
		map[string]any{"points": []map[string]any{{
			"id": pointID, "vector": vector, "payload": payload,
		}}}, nil)
	if err != nil {
		return err
	}
	b.generation.Add(1)
	return nil
}

// Delete removes a point entirely.
func (b *Backend) Delete(ctx context.Context, collection string, pointID int64) error {
	if err := b.ensureCollection(ctx, collection); err != nil {
		return err
	}
	err := b.do(ctx, http.MethodPost,
		"/collections/"+b.collectionName(collection)+"/points/delete?wait=true",
		map[string]any{"points": []int64{pointID}}, nil)
	if err != nil {
		return err
	}
	b.generation.Add(1)
	return nil
}

// Tombstone makes a point unreachable while keeping its identity.
//
// A payload write rather than a delete, so the id stays taken. Every search
// excludes it; see tombstonePayloadKey.
func (b *Backend) Tombstone(ctx context.Context, collection string, pointID int64) error {
	if err := b.ensureCollection(ctx, collection); err != nil {
		return err
	}
	err := b.do(ctx, http.MethodPost,
		"/collections/"+b.collectionName(collection)+"/points/payload?wait=true",
		map[string]any{
			"payload": map[string]any{tombstonePayloadKey: true},
			"points":  []int64{pointID},
		}, nil)
	if err != nil {
		return err
	}
	b.generation.Add(1)
	return nil
}

type searchResponse struct {
	Result []struct {
		ID    int64   `json:"id"`
		Score float64 `json:"score"`
	} `json:"result"`
}

// Search returns at most topK candidates matching every filter, nearest first.
func (b *Backend) Search(ctx context.Context, collection string, vector []float32,
	topK int, filters []db3.ExactLabel) ([]db3.Candidate, error) {
	if topK <= 0 || len(vector) != b.config.Dimension {
		return nil, nil
	}
	if err := b.ensureCollection(ctx, collection); err != nil {
		return nil, err
	}

	// Every filter is a MUST. The DB3 contract ANDs them, and a store that
	// widened here would return another workspace's point ids -- which DB2
	// refusing to rehydrate still leaks through the timing.
	must := make([]map[string]any, 0, len(filters))
	for _, filter := range filters {
		must = append(must, map[string]any{
			"key":   filter.Key,
			"match": map[string]any{"value": filter.Value},
		})
	}
	body := map[string]any{
		"vector":       vector,
		"limit":        topK,
		"with_payload": false,
		"with_vector":  false,
		"filter": map[string]any{
			"must": must,
			// Tombstoned points are unreachable but still present.
			"must_not": []map[string]any{{
				"key":   tombstonePayloadKey,
				"match": map[string]any{"value": true},
			}},
		},
	}

	var decoded searchResponse
	if err := b.do(ctx, http.MethodPost,
		"/collections/"+b.collectionName(collection)+"/points/search", body, &decoded); err != nil {
		return nil, err
	}

	candidates := make([]db3.Candidate, 0, len(decoded.Result))
	for _, hit := range decoded.Result {
		// The provider returns opaque ids and scores and nothing else. Payload
		// is not requested above and is not carried here even if a future
		// Qdrant returned it unasked.
		candidates = append(candidates, db3.Candidate{
			PointID: hit.ID,
			Score:   b.score(hit.Score),
		})
	}
	return candidates, nil
}
