package memory

import (
	"context"
	"encoding/json"
	"strconv"

	store "github.com/JBailes/aimee/server-go/db"
)

const retrievalDecisionPoint = "kb_memory_retrieval_limit"

type retrievalDecision struct {
	limit   int
	id, arm string
}

func configNumber(values map[string]any, key string) float64 {
	switch v := values[key].(type) {
	case float64:
		return v
	case int:
		return float64(v)
	case bool:
		if v {
			return 1
		}
	case json.Number:
		n, _ := v.Float64()
		return n
	}
	return 0
}

// Exploration is optional. Its savepoint isolates unavailable policy tables or
// telemetry writes from a successful memory lookup, including under PostgreSQL's
// aborted-transaction semantics. The caller's memory scope remains unchanged.
func (s *postgresDataStore) retrievalPolicyAttempt(ctx context.Context, run func() error) error {
	rewindAudit := s.auditSavepoint()
	if _, err := s.db.Exec(ctx, `SAVEPOINT memory_retrieval_policy`); err != nil {
		return err
	}
	err := run()
	if err != nil {
		rewindAudit()
		if _, rollbackErr := s.db.Exec(ctx, `ROLLBACK TO SAVEPOINT memory_retrieval_policy`); rollbackErr != nil {
			return rollbackErr
		}
	}
	_, releaseErr := s.db.Exec(ctx, `RELEASE SAVEPOINT memory_retrieval_policy`)
	if releaseErr != nil {
		return releaseErr
	}
	return err
}

func (s *postgresDataStore) selectRetrievalLimit(ctx context.Context, decision *retrievalDecision) error {
	var promoted string
	err := s.db.QueryRow(ctx, `SELECT arm_id FROM bandit_promotions WHERE decision_point=$1`, retrievalDecisionPoint).Scan(&promoted)
	if err != nil && !store.IsNoRows(err) {
		return err
	}
	if n, err := strconv.Atoi(promoted); err == nil && n > 0 {
		decision.limit = min(n, 64)
	}
	// MR-15: preserve explicit operator configuration and historical artifacts,
	// but do not sample posteriors trained from a result-count proxy. Promotion
	// of a new verified-outcome policy requires the frozen release gate.
	return nil
}

type retrievalAvailability struct {
	Count     int
	Truncated bool
}

func resultCountAvailability(count, limit int) retrievalAvailability {
	return retrievalAvailability{Count: count, Truncated: limit > 0 && count >= limit}
}

// Availability is first-observation metadata, never a reward or decision close.
func (s *postgresDataStore) recordRetrievalAvailability(ctx context.Context, decision retrievalDecision, count int) error {
	availability := resultCountAvailability(count, decision.limit)
	_, err := s.db.Exec(ctx, `UPDATE bandit_decisions SET result_count=$4,result_truncated=$5
 WHERE id=$1 AND decision_point=$2 AND arm_id=$3 AND result_count IS NULL`, decision.id, retrievalDecisionPoint, decision.arm, availability.Count, availability.Truncated)
	return err
}

func (s *postgresDataStore) adaptiveSearch(ctx context.Context, request DataRequest) ([]Record, error) {
	decision := retrievalDecision{limit: request.Limit}
	if request.AutomaticLimit {
		_ = s.retrievalPolicyAttempt(ctx, func() error { return s.selectRetrievalLimit(ctx, &decision) })
	}
	request.Limit = decision.limit
	records, err := s.SearchVisible(ctx, request)
	if err != nil {
		return nil, err
	}
	if decision.id != "" {
		_ = s.retrievalPolicyAttempt(ctx, func() error { return s.recordRetrievalAvailability(ctx, decision, len(records)) })
	}
	return records, nil
}
