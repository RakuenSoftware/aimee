package git

// CI grading: fold a forge's check-runs / combined-status payloads into one
// verdict.
//
// This moved from C (src/modules/git/git_pr_ci_grade.c) because it is parsing of
// an untrusted remote payload and pure policy over the result -- feature work,
// not transport -- and the same argument that moved the apt parser into the
// sandbox module applies here.
//
// The semantics are load-bearing and are preserved exactly:
//   - check runs decide when any exist; the legacy combined status is consulted
//     only when there are none;
//   - a failed run beats a pending one (FAILURE outranks PENDING);
//   - success / neutral / skipped are green, everything else red;
//   - NONE means "the forge reported no CI", which permits merge, so it must
//     never be reachable from "we could not read the answer" -- an unparseable
//     or unexpected body is ERROR, which refuses.
//
// That last distinction is the fail-open this grader exists to close, so the
// tests pin it directly.

import "encoding/json"

// CIVerdict mirrors git_pr_ci_t. The values are carried as strings on the wire
// rather than as the C enum's integers: a renumbered enum would otherwise
// silently change meaning across the process boundary.
type CIVerdict string

const (
	CINone    CIVerdict = "none"
	CISuccess CIVerdict = "success"
	CIPending CIVerdict = "pending"
	CIFailure CIVerdict = "failure"
	CIError   CIVerdict = "error"
)

// CIGradeRequest carries the two raw payloads as the forge returned them.
// They are strings, not nested objects, because the C caller has them as
// received bodies and re-encoding them would only add a way to differ.
type CIGradeRequest struct {
	CheckRuns      string `json:"check_runs"`
	CombinedStatus string `json:"combined_status"`
}

type CIGradeResponse struct {
	Verdict CIVerdict `json:"verdict"`
}

type checkRunsPayload struct {
	CheckRuns *[]struct {
		Status     *string `json:"status"`
		Conclusion *string `json:"conclusion"`
	} `json:"check_runs"`
}

type combinedStatusPayload struct {
	State    *string     `json:"state"`
	Statuses *[]struct{} `json:"statuses"`
}

// GradeCI folds the payloads into one verdict.
func GradeCI(checkRunsJSON, combinedStatusJSON string) CIVerdict {
	pending, failed, seen := 0, 0, 0

	if checkRunsJSON != "" {
		var payload checkRunsPayload
		if err := json.Unmarshal([]byte(checkRunsJSON), &payload); err != nil || payload.CheckRuns == nil {
			// Unparseable, or not a check-runs payload. Undetermined refuses;
			// it must not fall through to NONE, which permits merge.
			return CIError
		}
		for _, run := range *payload.CheckRuns {
			seen++
			if run.Status != nil && *run.Status != "completed" {
				pending++
				continue
			}
			if run.Conclusion == nil || *run.Conclusion == "" {
				pending++
				continue
			}
			switch *run.Conclusion {
			case "success", "neutral", "skipped":
				// green
			default:
				// failure / cancelled / timed_out / action_required / stale
				failed++
			}
		}
	}

	if seen == 0 {
		// No check runs: the legacy combined status decides.
		if combinedStatusJSON == "" {
			return CINone // nothing reported anywhere
		}
		var payload combinedStatusPayload
		if err := json.Unmarshal([]byte(combinedStatusJSON), &payload); err != nil || payload.Statuses == nil {
			return CIError
		}
		if len(*payload.Statuses) > 0 {
			state := ""
			if payload.State != nil {
				state = *payload.State
			}
			switch state {
			case "success":
				return CISuccess
			case "pending":
				return CIPending
			default:
				return CIFailure
			}
		}
		return CINone // genuinely zero checks
	}

	// A failure outranks a pending run: the gate must not report "still running"
	// for a build that has already gone red.
	if failed > 0 {
		return CIFailure
	}
	if pending > 0 {
		return CIPending
	}
	return CISuccess
}

// CIPermitsMerge is the policy half, enumerated rather than defaulted so a new
// verdict must be classified deliberately instead of inheriting "may merge".
//
// The C caller keeps its own copy of this on the merge path -- it is a handful
// of comparisons on a hot path and a bus round trip would be pure cost -- so
// this exists for module-side callers and to keep the two in one reviewable
// place.
func CIPermitsMerge(verdict CIVerdict) bool {
	switch verdict {
	case CISuccess, CINone:
		return true
	case CIPending, CIFailure, CIError:
		return false
	}
	return false // unknown value: fail closed
}
