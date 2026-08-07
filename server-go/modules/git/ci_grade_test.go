package git

import "testing"

// These cases are ported one-for-one from src/tests/test_git_pr_ci_grade.c, the
// C grader's own suite, so the migration is pinned to the behaviour it replaces
// rather than to a fresh reading of the rules.

func runs(inner string) string { return `{"total_count":9,"check_runs":[` + inner + `]}` }
func run(status, conclusion string) string {
	return `{"status":"` + status + `","conclusion":` + conclusion + `}`
}

const emptyRuns = `{"total_count":0,"check_runs":[]}`

func TestGradeCICheckRuns(t *testing.T) {
	cases := []struct {
		name  string
		input string
		want  CIVerdict
	}{
		{
			"all green (neutral and skipped count as green)",
			runs(run("completed", `"success"`) + "," + run("completed", `"neutral"`) + "," +
				run("completed", `"skipped"`)),
			CISuccess,
		},
		{
			"one still running is pending",
			runs(run("completed", `"success"`) + "," + run("in_progress", "null")),
			CIPending,
		},
		{
			// The ordering rule: red beats still-running, so a gate never reports
			// "waiting" for a build that has already failed.
			"a failure wins even while others still run",
			runs(run("in_progress", "null") + "," + run("completed", `"failure"`)),
			CIFailure,
		},
		{"cancelled blocks", runs(run("completed", `"cancelled"`)), CIFailure},
		{"completed with null conclusion is not a pass", runs(run("completed", "null")), CIPending},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := GradeCI(tc.input, ""); got != tc.want {
				t.Fatalf("GradeCI = %q, want %q", got, tc.want)
			}
		})
	}
}

func TestGradeCIFallsBackToCombinedStatus(t *testing.T) {
	cases := []struct {
		name     string
		combined string
		want     CIVerdict
	}{
		{"success", `{"state":"success","statuses":[{}]}`, CISuccess},
		{"pending", `{"state":"pending","statuses":[{}]}`, CIPending},
		{"failure", `{"state":"failure","statuses":[{}]}`, CIFailure},
		{
			// GitHub calls an empty combined status "pending", but it means no CI
			// exists at all -- so the gate parks rather than waiting forever.
			"zero statuses is NONE, not pending",
			`{"state":"pending","statuses":[]}`,
			CINone,
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := GradeCI(emptyRuns, tc.combined); got != tc.want {
				t.Fatalf("GradeCI = %q, want %q", got, tc.want)
			}
		})
	}

	if got := GradeCI(emptyRuns, ""); got != CINone {
		t.Fatalf("nothing reported anywhere = %q, want none", got)
	}
	if got := GradeCI("", ""); got != CINone {
		t.Fatalf("no payloads at all = %q, want none", got)
	}
}

// The fail-open this grader exists to close: NONE permits merge, so a payload we
// could not read must never reach NONE.
func TestGradeCIUnreadablePayloadIsErrorNotNone(t *testing.T) {
	cases := []struct {
		name           string
		checkRuns      string
		combinedStatus string
	}{
		{"both unparseable", "not json", "also not json"},
		{"combined unparseable", emptyRuns, "not json"},
		{"parsed but not a check-runs payload", `{"message":"Not Found"}`, ""},
		{"parsed but not a combined-status payload", emptyRuns, `{"message":"Not Found"}`},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := GradeCI(tc.checkRuns, tc.combinedStatus)
			if got != CIError {
				t.Fatalf("GradeCI = %q, want error", got)
			}
			if CIPermitsMerge(got) {
				t.Fatal("an unreadable payload must never permit a merge")
			}
		})
	}
}

func TestCIPermitsMerge(t *testing.T) {
	for verdict, want := range map[CIVerdict]bool{
		CISuccess: true,
		CINone:    true, // no CI reported -> nothing to fail
		CIPending: false,
		CIFailure: false,
		CIError:   false, // unknown state is never a pass
	} {
		if got := CIPermitsMerge(verdict); got != want {
			t.Fatalf("CIPermitsMerge(%q) = %v, want %v", verdict, got, want)
		}
	}
	// An unrecognised value must fail closed rather than inherit "may merge".
	if CIPermitsMerge(CIVerdict("something-new")) {
		t.Fatal("an unclassified verdict must fail closed")
	}
}
