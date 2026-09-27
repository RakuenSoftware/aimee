package memory

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"time"
)

type evidenceRecoveryAttempt struct {
	Key      string `json:"attempt_key"`
	State    string `json:"state"`
	NewItems int    `json:"new_items"`
}
type evidenceRecoveryExecution struct {
	State          string                    `json:"state"`
	Rounds         int                       `json:"rounds"`
	NewItems       int                       `json:"new_items"`
	Tokens         int                       `json:"token_upper_bound"`
	ElapsedMS      int64                     `json:"elapsed_ms"`
	CostMicrounits int                       `json:"cost_microunits"`
	Attempts       []evidenceRecoveryAttempt `json:"attempts"`
}

// Each reservation/outcome uses a separate metadata-only transaction. It must
// commit BEFORE any recovery work: rollback of the outer context read, lost reply
// or process death cannot erase a consumed round. Pending means unknown outcome,
// never permission to retry. A new revision requires fresh host admission.
func (s *postgresDataStore) recoveryLedger(ctx context.Context, actor, task, digest string, outcome *evidenceRecoveryExecution) (bool, error) {
	if s.recoveryDB == nil || actor == "" {
		return false, fmt.Errorf("memory: recovery host authority unavailable")
	}
	tx, err := s.recoveryDB.Begin(ctx)
	if err != nil {
		return false, err
	}
	defer tx.Rollback(context.Background())
	if _, err = tx.Exec(ctx, `SELECT set_config('aimee.principal',$1,true)`, actor); err != nil {
		return false, err
	}
	hash := fmt.Sprintf("%x", sha256.Sum256([]byte(task)))
	if outcome != nil {
		raw, _ := json.Marshal(outcome)
		tag, err := tx.Exec(ctx, `UPDATE memory_evidence_recovery SET outcome=$4::jsonb WHERE actor_principal=$1 AND task_hash=$2 AND requirement_hash=$3 AND outcome->>'state'='pending'`, actor, hash, digest, string(raw))
		if err != nil {
			return false, err
		}
		if tag.RowsAffected() != 1 {
			return false, fmt.Errorf("memory: recovery reservation changed")
		}
		return true, tx.Commit(ctx)
	}
	tag, err := tx.Exec(ctx, `INSERT INTO memory_evidence_recovery(actor_principal,task_hash,requirement_hash) VALUES($1,$2,$3) ON CONFLICT DO NOTHING`, actor, hash, digest)
	if err != nil {
		return false, err
	}
	admitted := tag.RowsAffected() == 1
	if !admitted {
		if _, err = tx.Exec(ctx, `UPDATE memory_evidence_recovery SET duplicate_attempts=duplicate_attempts+1 WHERE actor_principal=$1 AND task_hash=$2`, actor, hash); err != nil {
			return false, err
		}
	}
	return admitted, tx.Commit(ctx)
}

func (s *postgresDataStore) executeEvidenceRecovery(ctx context.Context, request DataRequest, exact Scope, r *typedContextResult) error {
	plan := r.Recovery
	if plan == nil || plan.State != "awaiting_host_admission" {
		return nil
	}
	start := time.Now()
	budget := plan.Budget
	// The Go host only admits local canonical reads: no model, network expansion,
	// ambient tool grant, or nonzero cost. Task limits intersect its fixed ceilings
	// and the existing packer token allocation, including a deliberate zero.
	budget.MaxItems = min(budget.MaxItems, 16)
	budget.MaxTokens = min(budget.MaxTokens, r.Budget)
	if budget.MaxRounds != 1 || budget.MaxItems <= 0 || budget.MaxTokens <= 0 || budget.MaxElapsedMS <= 0 || budget.MaxCostMicrounits != 0 {
		return nil
	}
	work, cancel := context.WithTimeout(ctx, time.Duration(min(budget.MaxElapsedMS, 2000))*time.Millisecond)
	defer cancel()
	admitted, err := s.recoveryLedger(work, request.recoveryActor, request.TypedContext.Requirements.TaskRevision, plan.RequirementDigest, nil)
	if err != nil {
		return err
	}
	execution := &evidenceRecoveryExecution{State: "completed", Rounds: 1, Attempts: []evidenceRecoveryAttempt{}}
	r.recoveryExecution = execution
	if !admitted {
		execution.State = "duplicate_blocked"
		execution.Rounds = 0
		for _, a := range plan.Actions {
			execution.Attempts = append(execution.Attempts, evidenceRecoveryAttempt{Key: a.Key, State: "duplicate_blocked"})
		}
		r.planEvidenceRecovery()
		r.Recovery.State = "blocked"
		return nil
	}
	seen := map[string]bool{}
	for _, item := range r.coverageCandidates {
		if item.source != nil {
			raw, _ := json.Marshal(item.source)
			seen[string(raw)] = true
		}
	}
	for _, action := range plan.Actions {
		attempt := evidenceRecoveryAttempt{Key: action.Key, State: "empty"}
		if work.Err() != nil {
			execution.State = "time_exhausted"
			attempt.State = execution.State
			execution.Attempts = append(execution.Attempts, attempt)
			break
		}
		if execution.NewItems >= budget.MaxItems || execution.Tokens >= budget.MaxTokens {
			execution.State = "work_exhausted"
			attempt.State = execution.State
			execution.Attempts = append(execution.Attempts, attempt)
			break
		}
		lookup := request
		lookup.recoveryRole = &action
		// The authorized original temporal policy remains in force. No recovery read
		// can turn a current-only task into historical inspection implicitly.
		if _, err = s.db.Exec(work, `SAVEPOINT evidence_recovery_read`); err != nil {
			return err
		}
		items, readErr := s.evidenceRecoveryCandidates(work, lookup, exact, budget.MaxItems-execution.NewItems+1)
		if readErr != nil {
			// Cleanup uses the outer live deadline, never the exhausted work deadline.
			if _, err = s.db.Exec(ctx, `ROLLBACK TO SAVEPOINT evidence_recovery_read`); err != nil {
				return err
			}
			if work.Err() != nil {
				execution.State = "time_exhausted"
			} else {
				execution.State = "source_unavailable"
			}
			attempt.State = execution.State
		} else {
			for _, recovered := range items {
				item := recovered.item
				source := item.source
				raw, _ := json.Marshal(source)
				if seen[string(raw)] {
					attempt.State = "duplicate_version"
					continue
				}
				seen[string(raw)] = true
				encoded, _ := json.Marshal(item.value)
				// A byte upper bound includes the serialized item and proof;
				// the old bytes/4 estimate cannot enforce a hard token cap.
				tokens := len(encoded) + len(raw)
				if work.Err() != nil {
					execution.State = "time_exhausted"
					attempt.State = execution.State
					break
				}
				if execution.NewItems >= budget.MaxItems || execution.Tokens+tokens > budget.MaxTokens {
					execution.State = "work_exhausted"
					attempt.State = execution.State
					break
				}
				if c := r.Channels[recovered.channel]; c == nil || !c.Enabled {
					continue
				}
				r.add(recovered.channel, item)
				execution.NewItems++
				execution.Tokens += tokens
				attempt.NewItems++
				attempt.State = "retrieved"
			}
		}
		if _, err = s.db.Exec(ctx, `RELEASE SAVEPOINT evidence_recovery_read`); err != nil {
			return err
		}
		execution.Attempts = append(execution.Attempts, attempt)
		if execution.State != "completed" {
			break
		}
	}
	execution.ElapsedMS = time.Since(start).Milliseconds()
	if _, err = s.recoveryLedger(ctx, request.recoveryActor, request.TypedContext.Requirements.TaskRevision, plan.RequirementDigest, execution); err != nil {
		return err
	}
	// Candidate evidence, never caller-supplied outcome IDs, enters the same final
	// packer and evaluator. Dispatch still requires canonical source revalidation.
	if err = r.finish(); err != nil {
		return err
	}
	if r.Recovery != nil {
		r.Recovery.State = "completed"
		if r.Sufficiency != "complete" {
			r.Recovery.State = "exhausted"
		}
	}
	return nil
}

type recoveredEvidenceItem struct {
	channel string
	item    typedItem
}

func (s *postgresDataStore) evidenceRecoveryCandidates(ctx context.Context, request DataRequest, exact Scope, limit int) ([]recoveredEvidenceItem, error) {
	var out []recoveredEvidenceItem
	if request.recoveryRole.Role == "approved_procedure" {
		items, err := s.typedProcedures(ctx, request, exact)
		if err != nil {
			return nil, err
		}
		for _, item := range items {
			if len(out) >= limit {
				break
			}
			out = append(out, recoveredEvidenceItem{"approved_procedures", item})
		}
		return out, nil
	}
	hits, err := s.assertionCandidates(ctx, request, exact, "", limit, "")
	if err != nil {
		return nil, err
	}
	for _, h := range hits {
		if err = s.assertionEvidence(ctx, &h); err != nil {
			return nil, err
		}
		source := h.sourceVersion()
		if source == nil {
			continue
		}
		source.ReadPolicy = &sourceReadPolicy{ValidAt: request.Assertions.ValidAt, BelievedAt: request.Assertions.BelievedAt, Historical: request.Assertions.Historical}
		channel := "current_assertions"
		if h.Historical {
			channel = "historical_assertions"
		}
		out = append(out, recoveredEvidenceItem{channel, typedItem{value: h, id: h.StableID, text: h.Rendered, source: source}})
	}
	return out, nil
}
