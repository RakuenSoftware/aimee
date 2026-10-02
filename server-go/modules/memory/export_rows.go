package memory

import (
	"context"
	"encoding/json"
	"fmt"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"strconv"
	"time"
)

// MemoryExport is a control-plane source snapshot, not a model release. The
// caller supplies a read-only repeatable-read transaction and independently
// authorizes every object before any projection reaches a model.
type MemoryExport struct {
	Scope Scope
	Rows  []json.RawMessage
}

func ExportMemoryRows(ctx context.Context, db store.Queryer, placement Placement, scope Scope) (*MemoryExport, error) {
	return exportMemoryRows(ctx, db, placement, scope, nil)
}

// An empty selected export never means the whole store.
func ExportMemoryRowsSelected(ctx context.Context, db store.Queryer, placement Placement, scope Scope, ids []string) (*MemoryExport, error) {
	if len(ids) > 4096 {
		return nil, fmt.Errorf("too many selected record IDs")
	}
	if len(ids) == 0 {
		normalized, err := normalizeScope(placement, scope)
		if err != nil {
			return nil, err
		}
		return &MemoryExport{Scope: normalized, Rows: []json.RawMessage{}}, nil
	}
	for _, id := range ids {
		n, e := strconv.ParseInt(id, 10, 64)
		if e != nil || n <= 0 || strconv.FormatInt(n, 10) != id {
			return nil, fmt.Errorf("invalid selected record ID")
		}
	}
	return exportMemoryRows(ctx, db, placement, scope, ids)
}
func exportMemoryRows(ctx context.Context, db store.Queryer, placement Placement, scope Scope, ids []string) (*MemoryExport, error) {
	scope, err := normalizeScope(placement, scope)
	if err != nil {
		return nil, err
	}
	// Match the current memory owner's eligibility predicate. An explicit
	// publication rule cannot resurrect a retired, suppressed or expired row.
	query := `SELECT to_jsonb(m) FROM memories m WHERE m.scope_type=$1 AND m.scope_value=$2 AND ` + currentMemorySQL("m.")
	args := []any{scope.Type, scope.Value}
	if placement == PlacementServer {
		query = `SELECT to_jsonb(m) FROM user_memories m WHERE ` + personalCurrentMemorySQL("m.")
		args = nil
	}
	if ids != nil {
		encoded, _ := json.Marshal(ids)
		query += fmt.Sprintf(" AND m.id IN (SELECT jsonb_array_elements_text($%d::jsonb)::bigint)", len(args)+1)
		args = append(args, string(encoded))
	}
	query += " ORDER BY m.id"
	rows, err := db.Query(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	result := &MemoryExport{Scope: scope, Rows: []json.RawMessage{}}
	for rows.Next() {
		var raw []byte
		if err = rows.Scan(&raw); err != nil {
			return nil, err
		}
		result.Rows = append(result.Rows, json.RawMessage(append([]byte{}, raw...)))
	}
	return result, rows.Err()
}

// ParseMemoryTimestamp exposes the authoritative source timestamp codec to exporters.
func ParseMemoryTimestamp(value string) (time.Time, error) { return parseMemoryTime(value) }
