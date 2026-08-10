package delegates

import "testing"

func packet(files, tests string) string {
	return `{"schema_version":"delegate_result_v1","status":"done","changed_files":[` + files +
		`],"tests":` + tests + `,"supervisor_actions":[],"summary":"did it"}`
}

const passedTest = `[{"name":"unit","status":"passed"}]`

func stateOf(t *testing.T, r PatchReport, taskID int) PatchTaskReport {
	t.Helper()
	for _, tr := range r.Tasks {
		if tr.TaskID == taskID {
			return tr
		}
	}
	t.Fatalf("task %d missing from report", taskID)
	return PatchTaskReport{}
}

// A packet that cleared every check is the only kind a human should look at.
func TestPatchCleanPacketIsReviewable(t *testing.T) {
	r := BuildPatchReport([]PatchTask{
		{ID: 1, StepID: 1, Status: "done", Files: `["src/a.c"]`,
			Result: packet(`"src/a.c"`, passedTest)},
	})
	if r.Reviewable != 1 || r.ImplementationPackets != 1 || r.Verified != 1 {
		t.Fatalf("report = %+v", r)
	}
	tr := stateOf(t, r, 1)
	if tr.PatchState != "reviewable" || !tr.HandoffValid || tr.PassedTests != 1 {
		t.Errorf("task = %+v", tr)
	}
}

// Packets touching different files do not contend, so both are reviewable.
func TestPatchDisjointPacketsAreBothReviewable(t *testing.T) {
	r := BuildPatchReport([]PatchTask{
		{ID: 1, Status: "done", Files: `["src/a.c"]`, Result: packet(`"src/a.c"`, passedTest)},
		{ID: 2, Status: "done", Files: `["src/b.c"]`, Result: packet(`"src/b.c"`, passedTest)},
	})
	if r.Reviewable != 2 || r.ImplementationPackets != 2 || r.PatchOverlaps != 0 {
		t.Fatalf("report = %+v", r)
	}
	if r.Verified != 2 || r.FocusedTestsPassed != 2 {
		t.Errorf("verification = %d verified, %d tests", r.Verified, r.FocusedTestsPassed)
	}
}

// Overlap is judged against packets ALREADY reviewable, so one collision stalls
// the second packet and not the first. Stalling both would make a single shared
// file block the whole run.
func TestPatchOverlapStopsOnlyTheSecondPacket(t *testing.T) {
	r := BuildPatchReport([]PatchTask{
		{ID: 1, Status: "done", Files: `["src/shared.c"]`,
			Result: packet(`"src/shared.c"`, passedTest)},
		{ID: 2, Status: "done", Files: `["src/shared.c"]`,
			Result: packet(`"src/shared.c"`, passedTest)},
	})
	if r.Reviewable != 1 || r.PatchOverlaps != 1 || r.NeedsSupervisor != 1 {
		t.Fatalf("report = %+v", r)
	}
	if first := stateOf(t, r, 1); first.PatchState != "reviewable" {
		t.Errorf("first packet = %q, want reviewable", first.PatchState)
	}
	second := stateOf(t, r, 2)
	if second.PatchState != "needs_supervisor" || second.OverlapTaskID != 1 {
		t.Errorf("second packet = %+v", second)
	}
}

// Packets that never ran, or are still running, are not integration decisions.
func TestPatchLifecycleStatesBeforeAHandoff(t *testing.T) {
	r := BuildPatchReport([]PatchTask{
		{ID: 1, Status: "pending"},
		{ID: 2, Status: "claimed"},
		{ID: 3, Status: "running"},
		{ID: 4, Status: "failed", Error: "container died"},
	})
	if r.Planned != 1 || r.Running != 2 || r.Failed != 1 {
		t.Fatalf("report = %+v", r)
	}
	if r.ImplementationPackets != 4 {
		t.Errorf("implementation packets = %d, want 4", r.ImplementationPackets)
	}
	if note := stateOf(t, r, 4).Note; note != "container died" {
		t.Errorf("failure note = %q, want the delegate's own error", note)
	}
}

// Each of these is a reason a human must look before anything is integrated.
func TestPatchReasonsAPacketNeedsASupervisor(t *testing.T) {
	cases := []struct {
		name   string
		task   PatchTask
		state  string
		expect func(*testing.T, PatchReport)
	}{
		{
			name: "edits outside ownership",
			task: PatchTask{ID: 1, Status: "done", Files: `["src/a.c"]`,
				Result: packet(`"src/a.c","src/other.c"`, passedTest)},
			state: "needs_supervisor",
			expect: func(t *testing.T, r PatchReport) {
				if r.OutsideOwnershipTouches != 1 {
					t.Errorf("outside ownership = %d, want 1", r.OutsideOwnershipTouches)
				}
			},
		},
		{
			name: "base differs from the integration base",
			task: PatchTask{ID: 1, Status: "done", Files: `["src/a.c"]`,
				Result: `{"schema_version":"delegate_result_v1","status":"done",` +
					`"changed_files":["src/a.c"],"tests":` + passedTest +
					`,"summary":"x","base_commit":"aaa","integration_base_commit":"bbb"}`},
			state: "needs_supervisor",
			expect: func(t *testing.T, r PatchReport) {
				if r.StaleWorktrees != 1 {
					t.Errorf("stale worktrees = %d, want 1", r.StaleWorktrees)
				}
			},
		},
		{
			name: "no handoff at all",
			task: PatchTask{ID: 1, Status: "done", Files: `["src/a.c"]`,
				Result: `just prose`},
			state: "needs_supervisor",
			expect: func(t *testing.T, r PatchReport) {
				if r.InvalidHandoffs != 1 {
					t.Errorf("invalid handoffs = %d, want 1", r.InvalidHandoffs)
				}
			},
		},
		{
			name: "delegate reported blocked",
			task: PatchTask{ID: 1, Status: "done", Files: `["src/a.c"]`,
				Result: `{"schema_version":"delegate_result_v1","status":"blocked",` +
					`"changed_files":["src/a.c"],"tests":` + passedTest + `,"summary":"stuck"}`},
			state:  "needs_supervisor",
			expect: func(t *testing.T, r PatchReport) {},
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			r := BuildPatchReport([]PatchTask{c.task})
			if got := stateOf(t, r, 1).PatchState; got != c.state {
				t.Errorf("state = %q, want %q", got, c.state)
			}
			c.expect(t, r)
		})
	}
}

// Work that came back unverified is "returned", not reviewable: nothing is
// wrong with it, but nobody has shown it works.
func TestPatchUnverifiedWorkIsReturned(t *testing.T) {
	r := BuildPatchReport([]PatchTask{
		{ID: 1, Status: "done", Files: `["src/a.c"]`, Result: packet(`"src/a.c"`, `[]`)},
	})
	if r.Returned != 1 || r.Reviewable != 0 || r.Verified != 0 {
		t.Fatalf("report = %+v", r)
	}
}

// A reviewer packet is read-only: it is not an implementation packet and does
// not take an integration state.
func TestPatchReviewerPacketIsCountedSeparately(t *testing.T) {
	review := `{"schema_version":"delegate_review_v1","status":"block","findings":[` +
		`{"severity":"high","owner_packet":"p1"},{"severity":"note"},{"owner_packet":"p2"}]}`
	r := BuildPatchReport([]PatchTask{{ID: 9, Status: "done", Result: review}})

	if r.ReviewerPackets != 1 || r.ImplementationPackets != 0 {
		t.Fatalf("report = %+v", r)
	}
	if r.ReviewerStatus != "block" {
		t.Errorf("reviewer status = %q", r.ReviewerStatus)
	}
	// "high" blocks, "note" does not, and the unlabelled finding counts as
	// blocking because unlabelled is not the same as harmless.
	if r.ReviewerBlockingFindings != 2 {
		t.Errorf("blocking findings = %d, want 2", r.ReviewerBlockingFindings)
	}
	if r.ReviewerOwnerPacketRoutes != 2 {
		t.Errorf("owner routes = %d, want 2", r.ReviewerOwnerPacketRoutes)
	}
	if state := stateOf(t, r, 9).PatchState; state != "reviewer" {
		t.Errorf("state = %q, want reviewer", state)
	}
}

// An unknown base is not evidence of staleness; only two KNOWN, differing
// commits are.
func TestPatchStaleBaseNeedsBothCommits(t *testing.T) {
	onlyBase := `{"schema_version":"delegate_result_v1","status":"done",` +
		`"changed_files":["src/a.c"],"tests":` + passedTest + `,"summary":"x","base_commit":"aaa"}`
	r := BuildPatchReport([]PatchTask{
		{ID: 1, Status: "done", Files: `["src/a.c"]`, Result: onlyBase},
	})
	if r.StaleWorktrees != 0 || r.Reviewable != 1 {
		t.Errorf("an unknown integration base was read as stale: %+v", r)
	}
}

// An empty run still answers, and says nothing has been reviewed.
func TestPatchEmptyRun(t *testing.T) {
	r := BuildPatchReport(nil)
	if r.ReviewerStatus != "not_run" || r.RecommendedNextCommand != patchDefaultNext {
		t.Errorf("report = %+v", r)
	}
	if len(r.Tasks) != 0 {
		t.Errorf("tasks = %+v", r.Tasks)
	}
}
