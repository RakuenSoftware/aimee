package engine

import (
	"context"
	"crypto/sha256"
	"errors"
	"fmt"
	"strings"

	"github.com/JBailes/aimee/server-go/internal/db1"
	roundtablecfg "github.com/JBailes/aimee/server-go/internal/roundtable"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

func (r *NativeRunner) Review(ctx context.Context, request roundtablecfg.ReviewRequest) (roundtablecfg.RunResult, error) {
	if r == nil || r.agents == nil {
		return roundtablecfg.RunResult{}, errors.New("roundtable agent resource plane is unavailable")
	}
	if len(strings.TrimSpace(request.Artifact)) < 20 {
		return roundtablecfg.RunResult{}, roundtablecfg.ValidationError{Message: "roundtable artifact must be at least 20 characters"}
	}
	if len(request.Artifact) > 16<<20 {
		return roundtablecfg.RunResult{}, roundtablecfg.ValidationError{Message: "roundtable artifact exceeds 16 MiB limit"}
	}
	stage, ok := normalizeRoundtableStage(request.ArtifactStage)
	if !ok {
		if strings.TrimSpace(request.ArtifactStage) != "" {
			return roundtablecfg.RunResult{}, roundtablecfg.ValidationError{Message: fmt.Sprintf("unsupported artifact stage %q", request.ArtifactStage)}
		}
		stage = "frozen_diff"
	}
	original := strings.TrimSpace(request.OriginalRequest)
	if original == "" {
		original = "Review the supplied artifact for correctness, completeness, security, and test quality."
	}
	id := strings.TrimSpace(request.RunID)
	if id == "" {
		sum := sha256.Sum256([]byte(original + "\x00" + request.Artifact))
		id = fmt.Sprintf("roundtable-%x", sum[:12])
	}
	node := wfe.Node{ID: "roundtable", Block: "gate.roundtable", Params: map[string]any{}}
	if request.Roundtable != "" {
		node.Params["roundtable"] = request.Roundtable
	}
	artifact := wfe.Artifact{Type: stage, Content: []byte(request.Artifact), Hash: wfe.Hash([]byte(request.Artifact))}
	step := StepRequest{WorkItem: db1.WorkItem{ID: id, Worktree: request.Workdir}, Node: node,
		Proposal: original, Inputs: map[string]wfe.Artifact{"src": artifact}}
	result, err := r.roundtable(ctx, step)
	if err != nil {
		return roundtablecfg.RunResult{}, err
	}
	if result.Roundtable == nil {
		if result.Detail == "" {
			result.Detail = string(result.Status)
		}
		return roundtablecfg.RunResult{}, errors.New(result.Detail)
	}
	result.Roundtable.RunID = id
	result.Roundtable.ArtifactHash = artifact.Hash
	if result.Roundtable.Feedback == nil || result.Roundtable.Feedback.ArtifactHash != artifact.Hash {
		return *result.Roundtable, errors.New("roundtable result artifact identity mismatch")
	}
	if result.Status == StepPending {
		return *result.Roundtable, errors.New(result.Detail)
	}
	return *result.Roundtable, nil
}

func assembleRoundtableArtifact(feedback *wfe.ReviewFeedback, approved bool) string {
	if approved {
		return "Roundtable approved the artifact with no findings.\n"
	}
	if feedback == nil || len(feedback.Findings) == 0 {
		return "Roundtable did not approve the artifact and returned no usable findings.\n"
	}
	var out strings.Builder
	out.WriteString("Roundtable requested changes.\n")
	for _, finding := range feedback.Findings {
		fmt.Fprintf(&out, "\n- **%s**", firstNonempty(finding.Severity, "blocking"))
		if finding.Location != "" {
			fmt.Fprintf(&out, " (%s)", finding.Location)
		}
		fmt.Fprintf(&out, ": %s", finding.Summary)
		if finding.Recommendation != "" {
			fmt.Fprintf(&out, " — %s", finding.Recommendation)
		}
	}
	out.WriteByte('\n')
	return out.String()
}

func roundtableResult(feedback *wfe.ReviewFeedback, approved, converged bool, analysis panelAnalysis, total int, cost float64) *roundtablecfg.RunResult {
	failed := total - len(analysis.Reports)
	var items []wfe.Finding
	if feedback != nil {
		items = append(items, feedback.Findings...)
	}
	return &roundtablecfg.RunResult{Artifact: assembleRoundtableArtifact(feedback, approved), Feedback: feedback, Items: items,
		Approved: approved, Converged: converged, Degraded: failed > 0, ParticipantsTotal: total,
		ParticipantsFailed: failed, ParticipantsUsed: len(analysis.Reports), CostUSD: cost}
}
