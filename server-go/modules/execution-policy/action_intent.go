package executionpolicy

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"time"
)

// ActionIntent is host-issued after argument rewrites and tool/resource lookup.
// It is a commitment, never a bearer capability. The durable owner must also
// authenticate the principal/root and serialize every transition for that root.
type ActionIntent struct {
	SchemaVersion        int       `json:"schema_version"`
	ID                   string    `json:"action_id"`
	Request              string    `json:"request_id"`
	Task                 string    `json:"task_id"`
	Attempt              string    `json:"attempt_id"`
	Principal            string    `json:"principal"`
	Root                 string    `json:"root_id"`
	Tool                 string    `json:"tool"`
	Class                string    `json:"action_class"`
	Destination          string    `json:"destination"`
	PayloadDigest        string    `json:"payload_sha256"`
	Purpose              string    `json:"purpose"`
	EvidenceDigest       string    `json:"evidence_sha256"`
	ContextReceipt       string    `json:"context_receipt"`
	PolicyGeneration     string    `json:"policy_generation"`
	RevocationGeneration string    `json:"revocation_generation"`
	IdempotencyKey       string    `json:"idempotency_key"`
	WorkUnits            string    `json:"work_units"`
	Expires              time.Time `json:"expires_at"`
}

// ActionFreshness is an authenticated owner observation supplied out of band
// from tool arguments. A memory lease is accepted only while its mutation guard
// remains held by the host through the durable dispatch transition.
type ActionFreshness struct {
	IntentDigest         string    `json:"intent_sha256"`
	EvidenceDigest       string    `json:"evidence_sha256"`
	ContextReceipt       string    `json:"context_receipt"`
	PolicyGeneration     string    `json:"policy_generation"`
	RevocationGeneration string    `json:"revocation_generation"`
	MemoryGuard          string    `json:"memory_guard"`
	Checked              time.Time `json:"checked_at"`
	Expires              time.Time `json:"expires_at"`
	Authorized           bool      `json:"authorized"`
	MemoryRequired       bool      `json:"memory_required"`
}

type ActionOutcome struct {
	State         string `json:"state"`
	EvidenceRef   string `json:"evidence_ref"`
	Destination   string `json:"destination"`
	PayloadDigest string `json:"payload_sha256"`
	ObjectVersion string `json:"object_version,omitempty"`
}

type ActionReceipt struct {
	Intent    ActionIntent     `json:"intent"`
	Digest    string           `json:"intent_sha256"`
	State     string           `json:"state"`
	Sequence  uint64           `json:"sequence"`
	At        time.Time        `json:"at"`
	Outcome   *ActionOutcome   `json:"outcome,omitempty"`
	Freshness *ActionFreshness `json:"freshness,omitempty"`
}

type ActionAuditEntry struct {
	Sequence    uint64    `json:"sequence"`
	Action      string    `json:"action_id"`
	State       string    `json:"state"`
	At          time.Time `json:"at"`
	EvidenceRef string    `json:"evidence_ref,omitempty"`
}

// ActionJournal is retained independently of an attempt's prompt and scratch
// projection. Failed and unknown attempts consume their original reservations.
// The store must not discard this state when a session/attempt is reset.
type ActionJournal struct {
	SchemaVersion     int                      `json:"schema_version"`
	Principal         string                   `json:"principal"`
	Root              string                   `json:"root_id"`
	Actions           map[string]ActionReceipt `json:"actions"`
	Audit             []ActionAuditEntry       `json:"audit"`
	ReservedCalls     uint64                   `json:"reserved_calls"`
	ReservedWork      uint64                   `json:"reserved_work"`
	SensitiveRead     bool                     `json:"sensitive_read"`
	PrivilegeElevated bool                     `json:"privilege_elevated"`
	Sequence          uint64                   `json:"sequence"`
}

type ActionCompositionPolicy struct {
	SensitivePathPrefixes  []string `json:"sensitive_path_prefixes,omitempty"`
	PublishedPathPrefixes  []string `json:"published_path_prefixes,omitempty"`
	MaxCalls               *uint64  `json:"max_calls"`
	MaxWork                *uint64  `json:"max_work_units"`
	ForbidSensitivePublish bool     `json:"forbid_sensitive_publish"`
	ForbidElevatedUse      bool     `json:"forbid_elevated_use"`
}

type ActionDecision struct {
	Allowed         bool           `json:"allowed"`
	DispatchAllowed bool           `json:"dispatch_allowed"`
	Reason          string         `json:"reason"`
	Remediation     string         `json:"remediation,omitempty"`
	Receipt         *ActionReceipt `json:"receipt,omitempty"`
}

func actionDigest(v any) string {
	b, _ := json.Marshal(v)
	h := sha256.Sum256(b)
	return hex.EncodeToString(h[:])
}
func actionHash(s string) bool {
	b, e := hex.DecodeString(s)
	return e == nil && len(b) == 32 && strings.ToLower(s) == s
}
func actionUnits(s string) (uint64, error) {
	n, e := strconv.ParseUint(s, 10, 63)
	if e != nil || strconv.FormatUint(n, 10) != s {
		return 0, errors.New("invalid exact work units")
	}
	return n, nil
}
func (i ActionIntent) validate(now time.Time) error {
	if i.SchemaVersion != 1 || !actionHash(i.PayloadDigest) || !i.Expires.After(now) || i.Expires.After(now.Add(15*time.Minute)) {
		return errors.New("invalid or expired action intent")
	}
	for _, s := range []string{i.ID, i.Request, i.Task, i.Attempt, i.Principal, i.Root, i.Tool, i.Class, i.Destination, i.Purpose, i.PolicyGeneration, i.RevocationGeneration, i.IdempotencyKey} {
		if s == "" || len(s) > 1024 || strings.ContainsAny(s, "\x00\r\n") {
			return errors.New("invalid action identity")
		}
	}
	if _, err := actionUnits(i.WorkUnits); err != nil {
		return err
	}
	if (i.EvidenceDigest != "" || i.ContextReceipt != "") && (!actionHash(i.EvidenceDigest) || !actionHash(i.ContextReceipt)) {
		return errors.New("invalid evidence commitment")
	}
	switch i.Class {
	case "read_only", "sensitive_read", "file_write", "external_publish", "privilege_elevation", "privileged_use":
	default:
		return errors.New("unsupported trusted action class")
	}
	return nil
}
func (f ActionFreshness) current(i ActionIntent, now time.Time) bool {
	if !f.Authorized || f.IntentDigest != actionDigest(i) || f.PolicyGeneration != i.PolicyGeneration || f.RevocationGeneration != i.RevocationGeneration || f.EvidenceDigest != i.EvidenceDigest || f.ContextReceipt != i.ContextReceipt {
		return false
	}
	if f.Checked.After(now) || !f.Expires.After(now) || f.Expires.Sub(f.Checked) > 2*time.Second || f.Checked.IsZero() {
		return false
	}
	if i.EvidenceDigest != "" && !f.MemoryRequired {
		return false
	}
	return !f.MemoryRequired || (f.MemoryGuard != "" && i.EvidenceDigest != "" && i.ContextReceipt != "")
}

// GovernedAction is the reducer at the existing execution-policy boundary.
// registryClass must come from the trusted tool registry/resource resolver;
// freshness and outcomes must come from their host adapters, never public JSON.
// Only a successful store COMMIT may release a DispatchAllowed decision.
func GovernedAction(principal, root string, state []byte, operation string, intent ActionIntent, registryClass string, freshness ActionFreshness, outcome *ActionOutcome, policy ActionCompositionPolicy, now time.Time) ([]byte, ActionDecision, error) {
	var j ActionJournal
	if principal == "" || root == "" || len(state) > 4<<20 {
		return nil, ActionDecision{}, errors.New("invalid action owner")
	}
	if len(state) == 0 {
		j = ActionJournal{SchemaVersion: 1, Principal: principal, Root: root, Actions: map[string]ActionReceipt{}}
	} else if json.Unmarshal(state, &j) != nil || j.SchemaVersion != 1 || j.Principal != principal || j.Root != root || j.Actions == nil {
		return nil, ActionDecision{}, errors.New("invalid durable action journal")
	}
	if err := j.validate(); err != nil {
		return nil, ActionDecision{}, err
	}
	if len(j.Actions) > 1024 || len(j.Audit) > 4096 {
		return nil, ActionDecision{}, errors.New("action journal capacity exceeded")
	}
	deny := func(reason, remediation string) ([]byte, ActionDecision, error) {
		return state, ActionDecision{Reason: reason, Remediation: remediation}, nil
	}
	for key, receipt := range j.Actions {
		if receipt.Intent.ID == intent.ID && key != intent.IdempotencyKey {
			return deny("action_identity_conflict", "retain the original action identity")
		}
	}
	key := intent.IdempotencyKey
	prior, exists := j.Actions[key]
	if exists && prior.Digest != actionDigest(intent) {
		return deny("idempotency_conflict", "issue a new authorized intent for changed material")
	}
	if intent.Principal != principal || intent.Root != root || intent.Class != registryClass {
		return deny("untrusted_action_binding", "resolve the tool and resource through the host registry")
	}
	record := func(receipt ActionReceipt, evidence string) ([]byte, ActionDecision, error) {
		if len(j.Audit) >= 4096 {
			return nil, ActionDecision{}, errors.New("action audit capacity exceeded")
		}
		j.Sequence++
		receipt.Sequence = j.Sequence
		receipt.At = now
		j.Actions[key] = receipt
		j.Audit = append(j.Audit, ActionAuditEntry{Sequence: j.Sequence, Action: intent.ID, State: receipt.State, At: now, EvidenceRef: evidence})
		raw, err := json.Marshal(j)
		if len(raw) > 4<<20 {
			return nil, ActionDecision{}, errors.New("action journal byte capacity exceeded")
		}
		return raw, ActionDecision{Allowed: true, DispatchAllowed: operation == "dispatch", Receipt: &receipt}, err
	}
	switch operation {
	case "prepare":
		if exists {
			return state, ActionDecision{Allowed: true, Reason: "existing_receipt", Receipt: &prior}, nil
		}
		if len(j.Actions) >= 1024 {
			return deny("action_history_full", "retain the journal and request an operator decision")
		}
		if err := intent.validate(now); err != nil {
			return deny("invalid_intent", "issue a current host intent")
		}
		return record(ActionReceipt{Intent: intent, Digest: actionDigest(intent), State: "prepared"}, "")
	case "admit", "dispatch":
		if !exists {
			return deny("intent_unavailable", "prepare an exact host intent")
		}
		required := "prepared"
		if operation == "dispatch" {
			required = "admitted"
		}
		if prior.State != required {
			if prior.State == "dispatching" || prior.State == "outcome_unknown" {
				return state, ActionDecision{Reason: "outcome_unknown", Remediation: "reconcile the exact destination before any retry", Receipt: &prior}, nil
			}
			return state, ActionDecision{Allowed: prior.State != "failed_before_effect", Reason: "existing_receipt", Receipt: &prior}, nil
		}
		if err := intent.validate(now); err != nil || !freshness.current(intent, now) {
			return deny("freshness_or_authority_unavailable", "refresh evidence and authorization; do not replay an old permit")
		}
		if operation == "admit" {
			for otherKey, other := range j.Actions {
				if intent.Class != "read_only" && intent.Class != "sensitive_read" && otherKey != key && other.Intent.Destination == intent.Destination && other.Intent.Class != "read_only" && other.Intent.Class != "sensitive_read" && (other.State == "dispatching" || other.State == "outcome_unknown" || (other.State == "acknowledged" && other.Intent.Class == "external_publish")) {
					return deny("destination_outcome_unresolved", "reconcile the existing action before changing its attempt or key")
				}
			}
			units, _ := actionUnits(intent.WorkUnits)
			if (policy.MaxCalls != nil && j.ReservedCalls >= *policy.MaxCalls) || j.ReservedCalls == ^uint64(0) || units > ^uint64(0)-j.ReservedWork || (policy.MaxWork != nil && (j.ReservedWork > *policy.MaxWork || units > *policy.MaxWork-j.ReservedWork)) {
				return deny("task_resource_ceiling", "request an operator budget revision for the same lineage")
			}
			if policy.ForbidSensitivePublish && j.SensitiveRead && intent.Class == "external_publish" {
				return deny("composition_restricted", "request an authorized review without exposing protected history")
			}
			if policy.ForbidElevatedUse && j.PrivilegeElevated && intent.Class == "privileged_use" {
				return deny("composition_restricted", "request an authorized privilege review")
			}
			j.ReservedCalls++
			j.ReservedWork += units
			prior.State = "admitted"
			// Reservation participates in composition before concurrent delegates can
			// reserve a prohibited successor, even if the first effect later times out.
			if intent.Class == "sensitive_read" {
				j.SensitiveRead = true
			}
			if intent.Class == "privilege_elevation" {
				j.PrivilegeElevated = true
			}
		} else {
			prior.State = "dispatching"
		}
		proof := freshness
		// The opaque guard is a transient capability; retain its commitment only.
		if proof.MemoryGuard != "" {
			proof.MemoryGuard = actionDigest(proof.MemoryGuard)
		}
		prior.Freshness = &proof
		return record(prior, "")
	case "cancel":
		if !exists {
			return deny("intent_unavailable", "inspect an existing owned action")
		}
		if prior.State == "failed_before_effect" && prior.Outcome != nil && prior.Outcome.EvidenceRef == "host_cancel:"+intent.ID {
			return state, ActionDecision{Allowed: true, Reason: "existing_receipt", Receipt: &prior}, nil
		}
		if prior.State != "prepared" && prior.State != "admitted" {
			return state, ActionDecision{Reason: "cancellation_requires_reconciliation", Remediation: "a started effect cannot be cancelled by relabeling its receipt", Receipt: &prior}, nil
		}
		prior.State = "failed_before_effect"
		prior.Outcome = &ActionOutcome{State: prior.State, EvidenceRef: "host_cancel:" + intent.ID, Destination: intent.Destination, PayloadDigest: intent.PayloadDigest}
		return record(prior, prior.Outcome.EvidenceRef)
	case "outcome", "reconcile":
		if !exists || outcome == nil || outcome.EvidenceRef == "" || len(outcome.EvidenceRef) > 1024 || outcome.Destination != intent.Destination || outcome.PayloadDigest != intent.PayloadDigest {
			return deny("invalid_outcome_binding", "verify the intended object and exact approved material")
		}
		if prior.Outcome != nil && actionDigest(prior.Outcome) == actionDigest(outcome) {
			return state, ActionDecision{Allowed: true, Reason: "existing_receipt", Receipt: &prior}, nil
		}
		if prior.State != "dispatching" && prior.State != "outcome_unknown" && !(operation == "reconcile" && prior.State == "acknowledged") {
			if prior.Outcome != nil && actionDigest(prior.Outcome) == actionDigest(outcome) {
				return state, ActionDecision{Allowed: true, Reason: "existing_receipt", Receipt: &prior}, nil
			}
			return deny("outcome_conflict", "retain the original receipt and reconcile conflicting evidence")
		}
		switch outcome.State {
		case "acknowledged", "outcome_unknown":
		case "effect_confirmed":
			if outcome.ObjectVersion == "" {
				return deny("verification_required", "obtain an owner verification of the exact object")
			}
		case "failed_before_effect":
			if operation != "reconcile" && prior.State == "outcome_unknown" {
				return deny("reconciliation_required", "verify absence of the effect before retry")
			}
		default:
			return deny("invalid_outcome_state", "use an explicit durable outcome state")
		}
		if prior.State == "acknowledged" && outcome.State == "failed_before_effect" {
			return deny("outcome_conflict", "an acknowledgement cannot be relabeled as no dispatch")
		}
		prior.State = outcome.State
		prior.Outcome = outcome
		return record(prior, outcome.EvidenceRef)
	default:
		return nil, ActionDecision{}, fmt.Errorf("unsupported action operation %q", operation)
	}
}

// CompletionClaim never equates acknowledgement with verified external effect.
func CompletionClaim(receipt ActionReceipt) string {
	switch receipt.State {
	case "effect_confirmed":
		return "effect_confirmed"
	case "acknowledged":
		return "provider_acknowledged"
	case "failed_before_effect":
		return "failed_before_effect"
	case "dispatching", "outcome_unknown":
		return "outcome_unknown"
	default:
		return "not_dispatched"
	}
}
