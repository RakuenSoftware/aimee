// Package panel owns the roundtable: the panel of reviewing agents, how their
// verdicts are gathered, discussed and chaired, and what a review returns.
//
// It deliberately imports nothing from internal/. The roundtable runs as a
// module process on the event bus, and a module's sources have to build on
// their own -- so the review domain lives here and the control plane converts
// at its edge, rather than the review borrowing control-plane types.
package panel

import (
	"crypto/sha256"
	"encoding/hex"
)

type ValidationError struct{ Message string }

func (e ValidationError) Error() string { return e.Message }

// Finding is one reviewer's objection. Severity is the field that decides
// whether the artifact ships: foundational and blocking hold the gate, while
// suggestion and nit are recorded as debt and must not.
type Finding struct {
	ID             string `json:"id"`
	Persona        string `json:"persona"`
	Severity       string `json:"severity"`
	Location       string `json:"location,omitempty"`
	Summary        string `json:"summary"`
	Recommendation string `json:"recommendation"`
}

// ReviewFeedback binds findings to the exact bytes they were made about.
// ArtifactHash is not decoration: a verdict that cannot be tied back to the
// reviewed content is not evidence that the content was reviewed.
type ReviewFeedback struct {
	SchemaVersion int       `json:"schema_version"`
	ArtifactHash  string    `json:"artifact_hash"`
	Findings      []Finding `json:"findings"`
}

// Hash is the artifact identity every seat echoes back, so a report about
// different bytes than the ones under review is detectable rather than merely
// plausible.
func Hash(content []byte) string {
	sum := sha256.Sum256(content)
	return hex.EncodeToString(sum[:])
}

type ReviewRequest struct {
	Artifact        string `json:"artifact"`
	OriginalRequest string `json:"original_request"`
	ArtifactStage   string `json:"artifact_stage"`
	Roundtable      string `json:"roundtable"`
	Workdir         string `json:"workdir"`
	RunID           string `json:"run_id"`

	// Stage and ExecutionVersion identify which attempt of which step this is.
	// They key the durable slot each seat reserves, so a caller that omits them
	// gets a fresh panel on every retry -- paying again for work it already has.
	Stage            string `json:"stage,omitempty"`
	ExecutionVersion string `json:"execution_version,omitempty"`
	// ReplayOnly forbids launching fresh seats: the spend was already reconciled,
	// so only an existing durable result may be consumed.
	ReplayOnly bool `json:"replay_only,omitempty"`
	// CostLimitUSD is the ceiling for the whole review, zero meaning unbounded.
	CostLimitUSD float64 `json:"cost_limit_usd,omitempty"`
	// Focus, Lenses and Pins are the caller's overlay on a saved roundtable. The
	// saved panel owns its seats; these only name review angles and pin
	// individual personas to agents.
	Focus  string            `json:"focus,omitempty"`
	Lenses []string          `json:"lenses,omitempty"`
	Pins   map[string]string `json:"pins,omitempty"`
}

// ParticipantFailure keeps degraded-panel diagnostics attached to the result.
// Counts alone cannot distinguish an admission rejection from a provider
// failure, a deadline, or an unusable verdict, and that ambiguity previously
// made a successful 2/3 panel impossible to root-cause after the fact.
type ParticipantFailure struct {
	Seat     int    `json:"seat"`
	Persona  string `json:"persona"`
	Category string `json:"category"`
	Detail   string `json:"detail"`
}

// Status is what the review concluded, in the panel's own terms. The workflow
// engine maps these onto its step outcomes; a standalone review reads them
// directly. They are part of the wire contract because the panel runs as a
// module process, and a park that arrives as a bare failure loses the reason a
// human needs.
const (
	// StatusApproved means the artifact cleared the gate.
	StatusApproved = "approved"
	// StatusChanges means the artifact must be revised; Feedback says how.
	StatusChanges = "changes"
	// StatusPending means no verdict was reached and PauseReason says why. It is
	// never a verdict about the artifact.
	StatusPending = "pending"
)

type RunResult struct {
	// Status is StatusApproved, StatusChanges or StatusPending.
	Status string `json:"status"`
	// PauseReason names why a pending review stopped: panel_capacity,
	// panel_capacity_deadline, panel_deadline, panel_unreachable,
	// roundtable_discussion, roundtable_chairman, request_unimplementable.
	PauseReason string `json:"pause_reason,omitempty"`
	Detail      string `json:"detail,omitempty"`
	// CostUnknown means at least one participant recorded no measurement, so
	// CostUSD is a lower bound and must not be committed as actual spend.
	CostUnknown         bool                 `json:"cost_unknown,omitempty"`
	RunID               string               `json:"run_id,omitempty"`
	ArtifactHash        string               `json:"artifact_hash,omitempty"`
	Artifact            string               `json:"artifact"`
	Feedback            *ReviewFeedback      `json:"feedback,omitempty"`
	Items               []Finding            `json:"items"`
	Converged           bool                 `json:"converged"`
	Approved            bool                 `json:"approved"`
	Degraded            bool                 `json:"degraded"`
	DeadlineHit         bool                 `json:"deadline_hit"`
	ParticipantsTotal   int                  `json:"participants_total"`
	ParticipantsFailed  int                  `json:"participants_failed"`
	ParticipantsUsed    int                  `json:"participants_used"`
	ParticipantFailures []ParticipantFailure `json:"participant_failures,omitempty"`
	CostUSD             float64              `json:"cost_usd"`
}
