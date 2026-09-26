package memory

import "sort"

type healthTraceReference struct {
	Request string `json:"request_id"`
	Attempt string `json:"attempt_id"`
	Stage   string `json:"stage"`
}
type healthTracePage struct {
	References     []healthTraceReference `json:"references"`
	MissingRequest int                    `json:"missing_request_identity"`
	Truncated      bool                   `json:"truncated"`
	Command        string                 `json:"inspect_command"`
}

// Explicit opt-in only, after authenticating and loading this exact journal.
// These locators lead to the existing principal-owned receipt reader. Neither
// public health nor this page offers cross-principal administrative access.
func (s *healthJournal) traceReferences(p healthPopulation) healthTracePage {
	r := healthTracePage{References: []healthTraceReference{}, Command: "aimee memory receipt <request-id> --json"}
	if p.Namespace != s.Namespace || p.Principal != s.Principal || p.Project != s.Project || p.Workspace != s.Workspace {
		return r
	}
	var rows []healthStoredAttempt
	for _, row := range s.Attempts {
		event := row.Invocation
		event.Stage = row.stage()
		if !p.includes(event) {
			continue
		}
		if event.Request == "" {
			r.MissingRequest++
			continue
		}
		rows = append(rows, row)
	}
	sort.Slice(rows, func(i, j int) bool { return rows[i].PreparedSequence > rows[j].PreparedSequence })
	r.Truncated = len(rows) > 16
	if r.Truncated {
		rows = rows[:16]
	}
	for _, row := range rows {
		r.References = append(r.References, healthTraceReference{Request: row.Invocation.Request, Attempt: row.Invocation.Attempt, Stage: row.stage()})
	}
	return r
}
