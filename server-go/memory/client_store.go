package memory

import (
	"context"
	"encoding/json"
)

// ClientStore applies the generic Store contract over the existing memory API.
// Consumers do not import the native implementation. The module bus supplies
// authentication and routing exactly as it does for other module contracts.
type ClientStore struct {
	Client  *Client
	TraceID uint64
}

func (c ClientStore) Capabilities() Capability {
	return Capability{Name: "memory-service", Version: 1, Operations: []string{"get", "search", "put", "delete"}}
}

type storeReply struct {
	Failure *Failure `json:"failure,omitempty"`
	Records []Record `json:"records"`
	Deleted bool     `json:"deleted"`
	Code    *int32   `json:"code"`
}

func (c ClientStore) call(ctx context.Context, request any) (storeReply, error) {
	raw, err := json.Marshal(request)
	if err != nil {
		return storeReply{}, ErrClientRequest
	}
	raw, err = c.Client.DataJSON(ctx, c.TraceID, raw)
	if err != nil {
		return storeReply{}, err
	}
	var result storeReply
	if json.Unmarshal(raw, &result) != nil {
		return storeReply{}, ErrClientResponse
	}
	if result.Failure != nil {
		return storeReply{}, result.Failure
	}
	if result.Code != nil {
		return storeReply{}, &RefusalError{Code: uint32(*result.Code)}
	}
	return result, nil
}
func (c ClientStore) Get(ctx context.Context, scope Scope, id int64) (Record, error) {
	canonical, scopeErr := scope.Canonical()
	if scopeErr != nil {
		return Record{}, scopeErr
	}
	scope = canonical
	result, err := c.call(ctx, map[string]any{"operation": "get", "scope": scope, "id": id})
	if err != nil {
		return Record{}, err
	}
	if len(result.Records) == 0 {
		return Record{}, ErrNotFound
	}
	if len(result.Records) != 1 || result.Records[0].ID != id || result.Records[0].Scope != scope {
		return Record{}, ErrClientResponse
	}
	return result.Records[0], nil
}
func (c ClientStore) Search(ctx context.Context, scope Scope, query, kind, tier string, limit int) ([]Record, error) {
	canonical, scopeErr := scope.Canonical()
	if scopeErr != nil {
		return nil, scopeErr
	}
	scope = canonical
	result, err := c.call(ctx, map[string]any{"operation": "search", "scope": scope, "query": query, "kind": kind, "tier": tier, "limit": limit})
	if err != nil {
		return nil, err
	}
	if limit <= 0 || len(result.Records) > limit {
		return nil, ErrClientResponse
	}
	seen := map[int64]bool{}
	for _, record := range result.Records {
		if record.ID <= 0 || record.Scope != scope || seen[record.ID] {
			return nil, ErrClientResponse
		}
		seen[record.ID] = true
	}
	return result.Records, nil
}
func (c ClientStore) Put(ctx context.Context, scope Scope, record Record) (Record, error) {
	canonical, scopeErr := scope.Canonical()
	if scopeErr != nil {
		return Record{}, scopeErr
	}
	scope = canonical
	result, err := c.call(ctx, map[string]any{"operation": "store", "scope": scope, "key": record.Key, "content": record.Content, "kind": record.Kind, "tier": record.Tier, "confidence": record.Confidence})
	if err != nil {
		return Record{}, err
	}
	if len(result.Records) != 1 || result.Records[0].ID <= 0 || result.Records[0].Scope != scope {
		return Record{}, ErrClientResponse
	}
	return result.Records[0], nil
}
func (c ClientStore) Delete(ctx context.Context, scope Scope, id int64) (bool, error) {
	canonical, scopeErr := scope.Canonical()
	if scopeErr != nil {
		return false, scopeErr
	}
	scope = canonical
	result, err := c.call(ctx, map[string]any{"operation": "delete", "scope": scope, "id": id})
	return result.Deleted, err
}

var _ Backend = ClientStore{}

// RefusalError preserves the existing memory API's structured admission code.
// A refusal is distinct from an operation absent from a provider contract.
type RefusalError struct{ Code uint32 }

func (e *RefusalError) Error() string { return "memory: request refused" }
