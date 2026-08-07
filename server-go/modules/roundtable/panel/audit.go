package panel

import (
	"log"
	"strings"
)

// logReviewDecision records why a review reached its verdict.
//
// The panel exists to judge an artifact against a request. When it approves
// something the request never asked for, the operator needs to see which of the
// two inputs failed: the seats ignoring the scope rules, or the caller supplying
// a request that does not constrain anything.
//
// Deliberately does NOT log the artifact or the request text. The decision is
// auditable from shape alone, and the artifact can be large and sensitive.
func logReviewDecision(run Run, analysis Analysis) {
	requirements, unaddressed := 0, 0
	alignments := map[string]int{}
	for _, report := range analysis.Reports {
		for _, req := range report.Response.Coverage {
			requirements++
			if !req.Addressed {
				unaddressed++
			}
		}
		status := strings.ToLower(strings.TrimSpace(report.Response.OriginalRequestAlignment.Status))
		if status == "" {
			status = "(omitted)"
		}
		alignments[status]++
	}
	log.Printf("roundtable review run=%s seats=%d request_chars=%d requirements=%d unaddressed=%d alignment=%v approvals=%d findings=%d",
		run.ID, len(analysis.Reports), len(strings.TrimSpace(run.OriginalRequest)),
		requirements, unaddressed, alignments, analysis.Approvals, len(analysis.Feedback.Findings))
}
