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

type RunResult struct {
	Artifact           string              `json:"artifact"`
	Feedback           *wfe.ReviewFeedback `json:"feedback,omitempty"`
	Items              []wfe.Finding       `json:"items"`
	Converged          bool                `json:"converged"`
	Approved           bool                `json:"approved"`
	Degraded           bool                `json:"degraded"`
	DeadlineHit        bool                `json:"deadline_hit"`
	ParticipantsTotal  int                 `json:"participants_total"`
	ParticipantsFailed int                 `json:"participants_failed"`
	ParticipantsUsed   int                 `json:"participants_used"`
	CostUSD            float64             `json:"cost_usd"`
}
