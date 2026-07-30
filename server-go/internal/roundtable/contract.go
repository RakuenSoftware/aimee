package roundtable

import "github.com/JBailes/aimee/server-go/internal/wfe"

type ValidationError struct{ Message string }

func (e ValidationError) Error() string { return e.Message }

type ReviewRequest struct {
	Artifact        string `json:"artifact"`
	OriginalRequest string `json:"original_request"`
	ArtifactStage   string `json:"artifact_stage"`
	Roundtable      string `json:"roundtable"`
	Workdir         string `json:"workdir"`
	RunID           string `json:"run_id"`
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

type RunResult struct {
	RunID               string               `json:"run_id,omitempty"`
	ArtifactHash        string               `json:"artifact_hash,omitempty"`
	Artifact            string               `json:"artifact"`
	Feedback            *wfe.ReviewFeedback  `json:"feedback,omitempty"`
	Items               []wfe.Finding        `json:"items"`
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
