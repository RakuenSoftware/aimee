package panel

import (
	"fmt"
	"strings"
)

func assembleRoundtableArtifact(feedback *ReviewFeedback, approved bool) string {
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

func roundtableResult(feedback *ReviewFeedback, approved, converged bool, analysis Analysis, total int, cost float64) *RunResult {
	failed := total - len(analysis.Reports)
	var items []Finding
	if feedback != nil {
		items = append(items, feedback.Findings...)
	}
	return &RunResult{Artifact: assembleRoundtableArtifact(feedback, approved), Feedback: feedback, Items: items,
		Approved: approved, Converged: converged, Degraded: failed > 0, ParticipantsTotal: total,
		ParticipantsFailed: failed, ParticipantsUsed: len(analysis.Reports), ParticipantFailures: append([]ParticipantFailure(nil), analysis.Failures...), CostUSD: cost}
}
