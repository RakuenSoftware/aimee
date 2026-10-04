package memory

import (
	"context"
	"encoding/json"
	memorycontract "github.com/JBailes/aimee/server-go/memory"
)

// NativeStore adapts the existing implementation, including its bound authority
// and transaction, to the public memory contract. No alternative storage or
// lifecycle path is introduced.
type NativeStore struct{ Data DataStore }

func toContractRecord(record Record) (memorycontract.Record, error) {
	if record.Version == nil && record.observedVersion != nil {
		record.Version = record.observedVersion
	}
	raw, err := json.Marshal(record)
	if err != nil {
		return memorycontract.Record{}, err
	}
	var result memorycontract.Record
	err = json.Unmarshal(raw, &result)
	return result, err
}
func fromContractRecord(record memorycontract.Record) (Record, error) {
	raw, err := json.Marshal(record)
	if err != nil {
		return Record{}, err
	}
	var result Record
	err = json.Unmarshal(raw, &result)
	return result, err
}
func (n NativeStore) Capabilities() memorycontract.Capability {
	return memorycontract.Capability{Name: "aimee-native", Version: 1, Operations: []string{"get", "search", "put", "delete"}}
}
func (n NativeStore) Get(ctx context.Context, scope Scope, id int64) (memorycontract.Record, error) {
	if n.Data == nil {
		return memorycontract.Record{}, memorycontract.ErrUnavailable
	}
	var record Record
	var err error
	if native, ok := n.Data.(*postgresDataStore); ok {
		record, err = native.getAtVersioned(ctx, scope, id, false, "", true)
	} else {
		record, err = n.Data.Get(ctx, scope, id)
	}
	if err != nil {
		return memorycontract.Record{}, err
	}
	return toContractRecord(record)
}
func (n NativeStore) Search(ctx context.Context, scope Scope, query, kind, tier string, limit int) ([]memorycontract.Record, error) {
	if n.Data == nil {
		return nil, memorycontract.ErrUnavailable
	}
	records, err := n.Data.Search(ctx, scope, query, kind, tier, limit)
	if err != nil {
		return nil, err
	}
	result := make([]memorycontract.Record, 0, len(records))
	for _, record := range records {
		out, err := toContractRecord(record)
		if err != nil {
			return nil, err
		}
		result = append(result, out)
	}
	return result, nil
}
func (n NativeStore) Put(ctx context.Context, scope Scope, record memorycontract.Record) (memorycontract.Record, error) {
	if n.Data == nil {
		return memorycontract.Record{}, memorycontract.ErrUnavailable
	}
	input, err := fromContractRecord(record)
	if err != nil {
		return memorycontract.Record{}, err
	}
	stored, err := n.Data.Put(ctx, scope, input)
	if err != nil {
		return memorycontract.Record{}, err
	}
	return toContractRecord(stored)
}
func (n NativeStore) Delete(ctx context.Context, scope Scope, id int64) (bool, error) {
	if n.Data == nil {
		return false, memorycontract.ErrUnavailable
	}
	return n.Data.Delete(ctx, scope, id)
}

var _ memorycontract.Backend = NativeStore{}

// ContractDataStore lets any implementation of the generic Store serve the
// existing baseline memory data API. Optional native APIs remain absent unless
// that implementation explicitly supplies them.
type ContractDataStore struct{ Store memorycontract.Store }

func (a ContractDataStore) Get(ctx context.Context, scope Scope, id int64) (Record, error) {
	record, err := a.Store.Get(ctx, scope, id)
	if err != nil {
		return Record{}, err
	}
	return fromContractRecord(record)
}
func (a ContractDataStore) Search(ctx context.Context, scope Scope, q, k, t string, limit int) ([]Record, error) {
	records, err := a.Store.Search(ctx, scope, q, k, t, limit)
	if err != nil {
		return nil, err
	}
	out := make([]Record, 0, len(records))
	for _, record := range records {
		r, err := fromContractRecord(record)
		if err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	return out, nil
}
func (a ContractDataStore) Put(ctx context.Context, scope Scope, record Record) (Record, error) {
	input, err := toContractRecord(record)
	if err != nil {
		return Record{}, err
	}
	stored, err := a.Store.Put(ctx, scope, input)
	if err != nil {
		return Record{}, err
	}
	return fromContractRecord(stored)
}
func (a ContractDataStore) Delete(ctx context.Context, scope Scope, id int64) (bool, error) {
	return a.Store.Delete(ctx, scope, id)
}

var _ DataStore = ContractDataStore{}
