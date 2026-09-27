package families

import (
	"context"
	"encoding/json"
	"strings"
	"time"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
	executionpolicy "github.com/JBailes/aimee/server-go/modules/execution-policy"
)

// This envelope is accepted only on the authenticated host-only DB1 seam. The
// host supplies tool-owner classifications and guarded source observations;
// none of these fields are public tool arguments or client assertions.
type governedActionRequest struct {
	Retry         executionpolicy.RetryRequest    `json:"retry"`
	ActionID      string                          `json:"action_id,omitempty"`
	Arguments     json.RawMessage                 `json:"arguments,omitempty"`
	Operation     string                          `json:"operation"`
	ParentSession string                          `json:"parent_session,omitempty"`
	Intent        executionpolicy.ActionIntent    `json:"intent"`
	RegistryClass string                          `json:"registry_class"`
	Freshness     executionpolicy.ActionFreshness `json:"freshness"`
	Outcome       *executionpolicy.ActionOutcome  `json:"outcome,omitempty"`
}

func governedActionApply(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	principal, sid, body := f[0], f[1], f[2]
	if principal == "" || sid == "" || len(principal) > 128 || len(sid) > 128 || len(body) > 65536 {
		return store.StatusInvalid, nil, nil
	}
	var req governedActionRequest
	if json.Unmarshal([]byte(body), &req) != nil {
		return store.StatusInvalid, nil, nil
	}
	// Lock directory identities in stable order, then lineage, then journal.
	// A fork may bind only an unused child. Existing children never change roots.
	ids := []string{sid}
	if req.Operation == "fork" {
		if req.ParentSession == "" || req.ParentSession == sid || len(req.ParentSession) > 128 {
			return store.StatusInvalid, nil, nil
		}
		ids = append(ids, req.ParentSession)
		if ids[1] < ids[0] {
			ids[0], ids[1] = ids[1], ids[0]
		}
	}
	for _, id := range ids {
		var owner string
		err := q.QueryRow(ctx, `SELECT principal FROM server_sessions WHERE id=$1 FOR UPDATE`, id).Scan(&owner)
		if store.IsNoRows(err) {
			return store.StatusMissing, nil, nil
		}
		if err != nil {
			return 0, nil, err
		}
		if owner != principal {
			return store.StatusInvalid, nil, nil
		}
	}
	source := sid
	if req.Operation == "fork" {
		source = req.ParentSession
	}
	root, err := governedActionRoot(ctx, q, principal, source)
	if err != nil {
		return 0, nil, err
	}
	if req.Operation == "fork" {
		if _, err = q.Exec(ctx, `INSERT INTO governed_action_sessions(principal,session_id,root_id) VALUES($1,$2,$3) ON CONFLICT DO NOTHING`, principal, sid, root); err != nil {
			return 0, nil, err
		}
		var actual string
		if err = q.QueryRow(ctx, `SELECT root_id FROM governed_action_sessions WHERE principal=$1 AND session_id=$2`, principal, sid).Scan(&actual); err != nil {
			return 0, nil, err
		}
		if actual != root {
			return store.StatusInvalid, nil, nil
		}
		reply, _ := json.Marshal(map[string]any{"allowed": true, "root_id": root})
		return store.StatusOK, []string{string(reply)}, nil
	}
	var journal string
	if err = q.QueryRow(ctx, `SELECT journal FROM governed_action_roots WHERE principal=$1 AND root_id=$2 FOR UPDATE`, principal, root).Scan(&journal); err != nil {
		return 0, nil, err
	}
	if strings.HasPrefix(req.Operation, "retry_") {
		var retryState string
		if err = q.QueryRow(ctx, `SELECT retry_journal FROM governed_action_roots WHERE principal=$1 AND root_id=$2`, principal, root).Scan(&retryState); err != nil {
			return 0, nil, err
		}
		policy, generation, e := executionpolicy.CurrentRetryPolicy()
		if e != nil {
			return store.StatusInvalid, nil, nil
		}
		req.Retry.Operation = strings.TrimPrefix(req.Operation, "retry_")
		// Projection revisions are read from the current task owner, never the caller.
		if req.Retry.Operation == "begin" {
			if e = q.QueryRow(ctx, `SELECT COALESCE((SELECT task_projection_state FROM session_state WHERE session_id=$1),'')`, sid).Scan(&req.Retry.Projection); e != nil {
				return 0, nil, e
			}
			var projection struct {
				Revision string `json:"revision"`
			}
			if req.Retry.Projection != "" && json.Unmarshal([]byte(req.Retry.Projection), &projection) != nil {
				return store.StatusInvalid, nil, nil
			}
			req.Retry.Projection = projection.Revision
		}
		next, decision, e := executionpolicy.CleanRetry(principal, root, []byte(retryState), []byte(journal), req.Retry, policy, generation, time.Now().UTC())
		if e != nil {
			return store.StatusInvalid, nil, nil
		}
		if string(next) != retryState {
			if _, e = q.Exec(ctx, `UPDATE governed_action_roots SET retry_journal=$3,updated_at=now() WHERE principal=$1 AND root_id=$2`, principal, root, string(next)); e != nil {
				return 0, nil, e
			}
		}
		reply, e := json.Marshal(decision)
		return store.StatusOK, []string{string(reply)}, e
	}
	if req.Operation == "root" {
		reply, _ := json.Marshal(map[string]any{"allowed": true, "root_id": root})
		return store.StatusOK, []string{string(reply)}, nil
	}
	if req.Operation == "inspect" {
		receipt, e := executionpolicy.InspectAction(principal, root, []byte(journal), req.ActionID)
		if e != nil {
			return store.StatusMissing, nil, nil
		}
		reply, e := json.Marshal(map[string]any{"allowed": true, "receipt": receipt, "completion_claim": executionpolicy.CompletionClaim(receipt)})
		return store.StatusOK, []string{string(reply)}, e
	}
	if req.Operation == "issue" {
		req.Intent, err = executionpolicy.IssueActionIntent(principal, root, []byte(journal), req.Intent, time.Now().UTC())
		if err != nil {
			return store.StatusInvalid, nil, nil
		}
		req.Operation = "prepare"
	}
	policy := executionpolicy.ActionCompositionPolicy{}
	if req.Operation == "admit" || req.Operation == "dispatch" {
		policy, err = executionpolicy.AuthorizeActionAdmission(req.Intent, req.Arguments)
		if err != nil || req.Freshness.PolicyGeneration != req.Intent.PolicyGeneration {
			return store.StatusInvalid, nil, nil
		}
		req.Freshness = executionpolicy.BindActionFreshness(req.Intent, req.Freshness)
	}
	next, decision, err := executionpolicy.GovernedAction(principal, root, []byte(journal), req.Operation, req.Intent, req.RegistryClass, req.Freshness, req.Outcome, policy, time.Now().UTC())
	if err != nil {
		return store.StatusInvalid, nil, nil
	}
	if string(next) != journal {
		if _, err = q.Exec(ctx, `UPDATE governed_action_roots SET journal=$3,updated_at=now() WHERE principal=$1 AND root_id=$2`, principal, root, string(next)); err != nil {
			return 0, nil, err
		}
	}
	claim := "not_dispatched"
	if decision.Receipt != nil {
		claim = executionpolicy.CompletionClaim(*decision.Receipt)
	}
	reply, err := json.Marshal(struct {
		executionpolicy.ActionDecision
		CompletionClaim string `json:"completion_claim"`
	}{decision, claim})
	return store.StatusOK, []string{string(reply)}, err
}

func governedActionRoot(ctx context.Context, q store.Queryer, principal, sid string) (string, error) {
	var root string
	err := q.QueryRow(ctx, `SELECT COALESCE((SELECT root_id FROM governed_action_sessions WHERE principal=$1 AND session_id=$2),'')`, principal, sid).Scan(&root)
	if err != nil {
		return "", err
	}
	if root != "" {
		return root, nil
	}
	// The authenticated session identity, not a caller-selected root ID, starts
	// a lineage. Directory locking serializes initial creation and fork binding.
	root = "session:" + sid
	if _, err = q.Exec(ctx, `INSERT INTO governed_action_roots(principal,root_id) VALUES($1,$2) ON CONFLICT DO NOTHING`, principal, root); err != nil {
		return "", err
	}
	_, err = q.Exec(ctx, `INSERT INTO governed_action_sessions(principal,session_id,root_id) VALUES($1,$2,$3)`, principal, sid, root)
	return root, err
}
