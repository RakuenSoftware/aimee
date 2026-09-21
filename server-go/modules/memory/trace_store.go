package memory

import (
	"context"
	"errors"
	"sort"
	"strconv"
	"strings"
)

// This cursor belongs to the legacy single-source miner. It is deliberately
// host-only until collection supplies an authenticated source/epoch and carries
// incomplete plans across pages. Never expose it as a multi-Server endpoint.
type traceMiningRow struct {
	ID   int64 `json:"id,string"`
	Turn int64 `json:"turn"`
	traceObservation
}
type traceMiningBatch struct {
	AfterID int64            `json:"after_id,string"`
	Rows    []traceMiningRow `json:"rows"`
}

func validTraceBatch(batch *traceMiningBatch) bool {
	if batch == nil || batch.AfterID < 0 || batch.Rows == nil || len(batch.Rows) > 512 {
		return false
	}
	previous := batch.AfterID
	for _, row := range batch.Rows {
		if row.ID <= previous || row.PlanID < 0 || row.Turn < 0 || len(row.Tool) > 1024 || strings.ContainsRune(row.Tool, '\x00') {
			return false
		}
		previous = row.ID
	}
	return true
}
func (s *postgresDataStore) traceCursor(ctx context.Context) (int64, error) {
	var id int64
	err := s.db.QueryRow(ctx, `SELECT COALESCE(MAX(last_trace_id),0) FROM trace_mining_log`).Scan(&id)
	return id, err
}
func (s *postgresDataStore) applyTraceBatch(ctx context.Context, request DataRequest) (map[string]any, error) {
	batch := request.TraceBatch
	if !validTraceBatch(batch) {
		return nil, errors.New("memory: invalid trace batch")
	}
	// Serialize compare/apply/advance, including the initially empty cursor table.
	// handleData requires a transaction, so every finding and the cursor commit
	// together, including canonical provenance and derived extraction scheduling.
	if _, err := s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(74109,1)`); err != nil {
		return nil, err
	}
	current, err := s.traceCursor(ctx)
	if err != nil {
		return nil, err
	}
	if current != batch.AfterID {
		return commandError("conflict", "trace cursor changed; reload before retry"), nil
	}
	ordered := append([]traceMiningRow(nil), batch.Rows...)
	sort.SliceStable(ordered, func(i, j int) bool {
		a, b := ordered[i], ordered[j]
		if a.PlanID != b.PlanID {
			return a.PlanID < b.PlanID
		}
		if a.Turn != b.Turn {
			return a.Turn < b.Turn
		}
		return a.ID < b.ID
	})
	observations := make([]traceObservation, len(ordered))
	for i, row := range ordered {
		observations[i] = row.traceObservation
	}
	patterns, err := tracePatterns(observations)
	if err != nil {
		return nil, err
	}
	emitted := 0
	for _, pattern := range patterns {
		if pattern.Type == "anti-pattern" {
			var exists bool
			if err = s.db.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM anti_patterns WHERE pattern=$1)`, pattern.Key).Scan(&exists); err != nil {
				return nil, err
			}
			if exists {
				continue
			}
			if _, err = s.db.Exec(ctx, `INSERT INTO anti_patterns(pattern,description,source,source_ref,confidence) VALUES($1,$2,'trace_mining','',$3)`, pattern.Key, pattern.Content, pattern.Confidence); err != nil {
				return nil, err
			}
		} else {
			exists, err := s.KeyExists(ctx, pattern.Key)
			if err != nil {
				return nil, err
			}
			if exists {
				continue
			}
			record, err := s.InsertEpistemic(ctx, DataRequest{Tier: "L0", Kind: "procedure", Key: pattern.Key, Content: pattern.Content, Confidence: &pattern.Confidence, Scope: request.Scope, Authority: AuthorityModel, SessionID: request.SessionID})
			if proposedCorrection(err) != nil {
				continue
			}
			if err != nil {
				return nil, err
			}
			if err = s.captureStoredFactActor(ctx, record.ID, AuthorityModel, nil); err != nil {
				return nil, err
			}
		}
		emitted++
	}
	last := current
	if len(batch.Rows) > 0 {
		last = batch.Rows[len(batch.Rows)-1].ID
		if _, err = s.db.Exec(ctx, `INSERT INTO trace_mining_log(last_trace_id,mined_at) VALUES($1,pg_now_text())`, last); err != nil {
			return nil, err
		}
	}
	return map[string]any{"status": "ok", "emitted": emitted, "last_id": strconv.FormatInt(last, 10)}, nil
}
