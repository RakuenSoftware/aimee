package memory

import (
	"context"
	"crypto/rand"
	"encoding/json"
	"fmt"
	"math"
	random "math/rand"
	"strconv"
	"time"

	store "github.com/JBailes/aimee/server-go/db"
)

const retrievalDecisionPoint = "kb_memory_retrieval_limit"

var retrievalLimitArms = []string{"10", "20"}

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
	if _, err := s.db.Exec(ctx, `SAVEPOINT memory_retrieval_policy`); err != nil {
		return err
	}
	err := run()
	if err != nil {
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
	if s.settings == nil {
		return nil
	}
	values, err := s.settings()
	if err != nil {
		return err
	}
	command, _ := values["bandit_optimize_command"].(string)
	if configNumber(values, "bandit_live_decision_enabled") == 0 || command == "" {
		return nil
	}
	budget := configNumber(values, "bandit_exploration_fraction")
	if math.IsNaN(budget) || math.IsInf(budget, 0) || budget < 0 || budget > 1 {
		return fmt.Errorf("memory: invalid exploration budget")
	}
	window := configNumber(values, "bandit_exploration_window_seconds")
	if window <= 0 {
		window = 7 * 24 * 3600
	}
	var total, exploring int64
	if err := s.db.QueryRow(ctx, `SELECT count(*),count(*) FILTER (WHERE is_exploration) FROM bandit_decisions
 WHERE decision_point=$1 AND aimee_utc_text_timestamptz(decided_at)>=now()-make_interval(secs=>$2::double precision)`, retrievalDecisionPoint, window).Scan(&total, &exploring); err != nil {
		return err
	}
	allow := random.Float64() < budget
	if total >= 20 && budget > 0 && float64(exploring)/float64(total) >= budget {
		allow = false
	}
	arms := make([]map[string]any, 0, len(retrievalLimitArms))
	for _, arm := range retrievalLimitArms {
		alpha, beta := 1.0, 1.0
		err := s.db.QueryRow(ctx, `SELECT posterior_alpha,posterior_beta FROM bandit_arm_stats WHERE decision_point=$1 AND arm_id=$2`, retrievalDecisionPoint, arm).Scan(&alpha, &beta)
		if err != nil && !store.IsNoRows(err) {
			return err
		}
		if alpha <= 0 || beta <= 0 || math.IsNaN(alpha) || math.IsNaN(beta) || math.IsInf(alpha, 0) || math.IsInf(beta, 0) {
			return fmt.Errorf("memory: invalid arm posterior")
		}
		arms = append(arms, map[string]any{"arm_id": arm, "posterior": map[string]any{"kind": "beta", "alpha": alpha, "beta": beta}})
	}
	input, _ := json.Marshal(map[string]any{"version": 1, "role": "optimize", "model_version": "linear-ts-v1", "prompt_version": "beta-bernoulli-v1", "inputs": map[string]any{"decision_point": retrievalDecisionPoint, "context": map[string]any{}, "allow_explore": allow, "arms": arms}})
	bounded, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	raw, err := runEpisodeCommand(bounded, command, input)
	if err != nil {
		return err
	}
	var reply struct {
		Status     string   `json:"status"`
		Arm        string   `json:"selected_arm"`
		Propensity *float64 `json:"propensity"`
	}
	if json.Unmarshal(raw, &reply) != nil || reply.Status != "ok" || (reply.Arm != "10" && reply.Arm != "20") {
		return fmt.Errorf("memory: invalid retrieval policy response")
	}
	propensity := 1.0
	if reply.Propensity != nil {
		propensity = *reply.Propensity
	}
	if propensity <= 0 || propensity > 1 || math.IsNaN(propensity) || math.IsInf(propensity, 0) {
		return fmt.Errorf("memory: invalid retrieval propensity")
	}
	var id [16]byte
	if _, err := rand.Read(id[:]); err != nil {
		return err
	}
	id[6] = (id[6] & 15) | 64
	id[8] = (id[8] & 63) | 128
	decisionID := fmt.Sprintf("%x-%x-%x-%x-%x", id[:4], id[4:6], id[6:8], id[8:10], id[10:])
	if _, err := s.db.Exec(ctx, `INSERT INTO bandit_decisions(id,decision_point,arm_id,context_hash,propensity,decided_at,is_exploration)
 VALUES($1,$2,$3,'',$4,pg_now_text(),$5)`, decisionID, retrievalDecisionPoint, reply.Arm, propensity, allow); err != nil {
		return err
	}
	decision.id, decision.arm = decisionID, reply.Arm
	decision.limit, _ = strconv.Atoi(reply.Arm)
	return nil
}

func recallSufficiency(count, limit int) float64 {
	if count <= 0 {
		return 0
	}
	if limit > 0 && count >= limit {
		return 0.5
	}
	return 1
}

// Close and update atomically. Repeated feedback cannot double-count a decision,
// and concurrent callers add to the stored posterior rather than overwriting it.
func (s *postgresDataStore) rewardRetrieval(ctx context.Context, decision retrievalDecision, count int) error {
	reward := recallSufficiency(count, decision.limit)
	_, err := s.db.Exec(ctx, `WITH closed AS (
 UPDATE bandit_decisions SET reward=$4,closed_at=pg_now_text()
 WHERE id=$1 AND decision_point=$2 AND arm_id=$3 AND reward IS NULL RETURNING decision_point,arm_id
) INSERT INTO bandit_arm_stats(decision_point,arm_id,n_decisions,n_rewards,sum_reward,sum_reward_sq,posterior_alpha,posterior_beta,updated_at)
 SELECT decision_point,arm_id,1,1,$4::double precision,($4::double precision)*($4::double precision),1+$4::double precision,2-$4::double precision,pg_now_text() FROM closed
 ON CONFLICT(decision_point,arm_id) DO UPDATE SET n_decisions=bandit_arm_stats.n_decisions+1,n_rewards=bandit_arm_stats.n_rewards+1,
 sum_reward=bandit_arm_stats.sum_reward+EXCLUDED.sum_reward,sum_reward_sq=bandit_arm_stats.sum_reward_sq+EXCLUDED.sum_reward_sq,
 posterior_alpha=bandit_arm_stats.posterior_alpha+EXCLUDED.sum_reward,posterior_beta=bandit_arm_stats.posterior_beta+1-EXCLUDED.sum_reward,updated_at=pg_now_text()`, decision.id, retrievalDecisionPoint, decision.arm, reward)
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
		_ = s.retrievalPolicyAttempt(ctx, func() error { return s.rewardRetrieval(ctx, decision, len(records)) })
	}
	return records, nil
}
