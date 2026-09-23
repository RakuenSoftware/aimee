package memory

import (
	"context"
	"encoding/json"
	"fmt"
)

const activationMaxRows = 256

// ActivationSnapshot is conversation state, carried by the user-local owner.
// Reading it here neither advances the conversation nor reinforces a memory.
type ActivationSnapshot struct {
	CurrentTurn int64           `json:"current_turn"`
	Rows        []ActivationRow `json:"rows"`
}

type ActivationRow struct {
	MemoryID int64 `json:"memory_id"`
	LastTurn int64 `json:"last_turn"`
}

// Invalid or missing conversation state has no activation opinion. Suppression
// remains a stored policy and applies independently of snapshot availability.
func parseActivation(raw json.RawMessage) *ActivationSnapshot {
	var object commandArgs
	if len(raw) == 0 || len(raw) > 65536 || json.Unmarshal(raw, &object) != nil || object == nil {
		return nil
	}
	turn, ok := object.decimalID("current_turn")
	if !ok {
		return nil
	}
	result := &ActivationSnapshot{CurrentTurn: turn, Rows: make([]ActivationRow, 0)}
	var rows []json.RawMessage
	if json.Unmarshal(object["rows"], &rows) != nil {
		return result
	}
	seen := make(map[int64]bool)
	for _, rawRow := range rows {
		if len(result.Rows) == activationMaxRows {
			break
		}
		var row commandArgs
		if json.Unmarshal(rawRow, &row) != nil {
			continue
		}
		id, validID := row.decimalID("memory_id")
		last, validTurn := row.decimalID("last_turn")
		if !validID || !validTurn || last > turn || seen[id] {
			continue
		}
		seen[id] = true
		result.Rows = append(result.Rows, ActivationRow{MemoryID: id, LastTurn: last})
	}
	return result
}

type activatedRecord struct {
	Record
	Sticky bool `json:"sticky"`
}

// recallActivated applies delay/cooldown/suppression before LIMIT, so held rows
// cannot occupy the entire section and prevent an eligible row from backfilling.
// The query runs on the same scoped transaction as the rest of recall. Sticky
// may extend relevance for active context, but never overrides cooldown.
func (s *postgresDataStore) recallActivated(ctx context.Context, snapshot *ActivationSnapshot,
	where string, limit int, includeSticky, pending bool, args ...any) ([]Record, map[int64]string, int, error) {
	rowsJSON, err := json.Marshal(snapshot.Rows)
	if err != nil {
		return nil, nil, 0, err
	}
	turnParam, rowsParam, limitParam := len(args)+1, len(args)+2, len(args)+3
	sticky := fmt.Sprintf(`(a.last_turn IS NOT NULL AND m.activation_sticky_turns>0
AND $%d-a.last_turn<=m.activation_sticky_turns)`, turnParam)
	match := "(" + where + ")"
	if includeSticky {
		match += " OR " + sticky
	}
	state := "active"
	if pending {
		state = "pending"
	}
	query := fmt.Sprintf(`WITH candidates AS (
 SELECT m.id,m.scope_type,m.scope_value,m.tier,m.kind,m.key,m.content,m.confidence,
 m.use_count,m.updated_at,m.record_revision, `+queryScopeOrder+` AS scope_rank, %s AS sticky,
 (m.activation_suppressed=0 AND $%d>m.activation_delay_turns AND
 (a.last_turn IS NULL OR m.activation_cooldown_turns=0 OR
  $%d-a.last_turn>m.activation_cooldown_turns)) AS eligible
 FROM memories m LEFT JOIN jsonb_to_recordset($%d::jsonb)
 AS a(memory_id bigint,last_turn bigint) ON a.memory_id=m.id
 WHERE m.lifecycle_state='%s' AND `+memoryValiditySQL("m.")+` AND (%s)
), served AS (
 SELECT * FROM candidates WHERE eligible
 ORDER BY scope_rank,confidence+CASE WHEN sticky THEN 0.04 ELSE 0 END DESC,
 use_count DESC,updated_at DESC,id DESC LIMIT $%d
)
SELECT COALESCE((SELECT jsonb_agg(jsonb_build_object(
 'id',id,'scope',jsonb_build_object('type',scope_type,'value',scope_value),
 'tier',tier,'kind',kind,'key',key,'content',content,'confidence',confidence,'sticky',sticky,
 'version',jsonb_build_object('schema_version',1,
 'owner_id',(SELECT owner_id::text FROM memory_collection_owner WHERE id=1),
 'record_id',id::text,'record_revision',record_revision::text))
 ORDER BY scope_rank,confidence+CASE WHEN sticky THEN 0.04 ELSE 0 END DESC,use_count DESC,updated_at DESC,id DESC)
 FROM served),'[]'::jsonb)::text,
 (SELECT COUNT(*) FROM candidates WHERE NOT eligible)`,
		sticky, turnParam, turnParam, rowsParam, state, match, limitParam)
	args = append(args, snapshot.CurrentTurn, string(rowsJSON), limit)
	var payload string
	var held int
	if err := s.db.QueryRow(ctx, query, args...).Scan(&payload, &held); err != nil {
		return nil, nil, 0, err
	}
	var selected []activatedRecord
	if err := json.Unmarshal([]byte(payload), &selected); err != nil {
		return nil, nil, 0, err
	}
	items := make([]Record, 0, len(selected))
	reasons := make(map[int64]string)
	for _, record := range selected {
		if !record.Version.validFor(record.ID) {
			return nil, nil, 0, fmt.Errorf("invalid activated memory version")
		}
		items = append(items, record.Record)
		if record.Sticky {
			reasons[record.ID] = "sticky activation"
		}
	}
	return items, reasons, held, nil
}

// A graph expansion may introduce additional candidates after lexical recall.
// Gate the union again, then preserve the graph ordering and backfill from the
// eligible lexical list. Otherwise graph expansion could bypass activation or
// leave a section empty after replacing its eligible lexical candidates.
func (s *postgresDataStore) activationAfterFusion(ctx context.Context, snapshot *ActivationSnapshot,
	fused, fallback []Record, limit int) ([]Record, map[int64]string, int, error) {
	ids := make([]int64, 0, len(fused)+len(fallback))
	seen := make(map[int64]bool)
	for _, group := range [][]Record{fused, fallback} {
		for _, record := range group {
			if !seen[record.ID] {
				seen[record.ID] = true
				ids = append(ids, record.ID)
			}
		}
	}
	if len(ids) == 0 {
		return []Record{}, nil, 0, nil
	}
	encoded, err := json.Marshal(ids)
	if err != nil {
		return nil, nil, 0, err
	}
	allowed, reasons, held, err := s.recallActivated(ctx, snapshot,
		`m.id IN (SELECT value::bigint FROM jsonb_array_elements_text($1::jsonb))`,
		len(ids), false, false, string(encoded))
	if err != nil {
		return nil, nil, 0, err
	}
	eligible := make(map[int64]bool, len(allowed))
	for _, record := range allowed {
		eligible[record.ID] = true
	}
	items := make([]Record, 0, limit)
	for _, group := range [][]Record{fused, fallback} {
		for _, record := range group {
			if len(items) < limit && eligible[record.ID] {
				items = append(items, record)
				delete(eligible, record.ID)
			}
		}
	}
	return items, reasons, held, nil
}
