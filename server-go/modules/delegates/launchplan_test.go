package delegates

import (
	"strings"
	"testing"
)

// The C this replaces had no tests of its own, so these are not a port of a
// pinned fixture set -- they are written from reading delegate_launch.c, and
// each one names the behaviour it holds so a later change has to argue with a
// sentence rather than a number.

func implPacket(id string, files ...LaunchOwnedFile) LaunchPacket {
	return LaunchPacket{
		ID:            id,
		Title:         "packet " + id,
		Objective:     "do " + id,
		Role:          "code",
		OwnedFiles:    files,
		HandoffSchema: "delegate_result_v1",
	}
}

func okPlan(packets ...LaunchPacket) LaunchPlan {
	return LaunchPlan{Schema: "delegate_plan_v1", Title: "a plan", Packets: packets}
}

func TestPlanLaunchBuildsOneStepAndTaskPerPacket(t *testing.T) {
	plan := okPlan(
		implPacket("p1", LaunchOwnedFile{Path: "src/a.c", Exists: true}),
		implPacket("p2", LaunchOwnedFile{Path: "src/b.c", Exists: true}),
	)
	d := PlanLaunch(plan, 0)
	if d.Error != "" {
		t.Fatalf("unexpected error: %s", d.Error)
	}
	if len(d.Steps) != 2 || len(d.Tasks) != 2 {
		t.Fatalf("want 2 steps and 2 tasks, got %d and %d", len(d.Steps), len(d.Tasks))
	}
	if d.Steps[0].Action != "packet p1" || d.Steps[0].Precondition != "p1" ||
		d.Steps[0].SuccessPredicate != "do p1" {
		t.Errorf("step 0 not built from the packet: %+v", d.Steps[0])
	}
	if d.MaxConcurrent != DefaultMaxConcurrent {
		t.Errorf("max concurrent should default to %d, got %d", DefaultMaxConcurrent, d.MaxConcurrent)
	}
}

// A reviewer is carried in the plan but never launched: it produces no step and
// no coord task, and its absence does not make the plan empty.
func TestPlanLaunchSkipsReviewerPackets(t *testing.T) {
	reviewer := implPacket("rev")
	reviewer.Role = "review"
	plan := okPlan(implPacket("p1", LaunchOwnedFile{Path: "src/a.c", Exists: true}), reviewer)

	d := PlanLaunch(plan, 2)
	if d.Error != "" {
		t.Fatalf("unexpected error: %s", d.Error)
	}
	if len(d.Steps) != 1 || len(d.Tasks) != 1 {
		t.Fatalf("reviewer should not launch: %d steps, %d tasks", len(d.Steps), len(d.Tasks))
	}
	// A reviewer owning no files must NOT trip the missing-owned_files check.
	if d.Tasks[0].Role != "code" {
		t.Errorf("wrong packet launched: %+v", d.Tasks[0])
	}
}

// A plan made only of reviewers has no work in it, and saying so is better than
// creating an empty coord job that never finishes.
func TestPlanLaunchRejectsPlanWithNoImplementationPackets(t *testing.T) {
	reviewer := implPacket("rev")
	reviewer.Role = "review"
	d := PlanLaunch(okPlan(reviewer), 0)
	if !strings.Contains(d.Error, "no implementation packets") {
		t.Fatalf("want no-implementation-packets error, got %q", d.Error)
	}
	if len(d.Steps) != 0 {
		t.Errorf("a rejected plan must yield nothing to write")
	}
}

func TestPlanLaunchRejectsWrongSchema(t *testing.T) {
	plan := okPlan(implPacket("p1", LaunchOwnedFile{Path: "a.c", Exists: true}))
	plan.Schema = "delegate_plan_v2"
	if d := PlanLaunch(plan, 0); !strings.Contains(d.Error, "invalid delegate plan schema") {
		t.Fatalf("want schema error, got %q", d.Error)
	}
}

// The planner's own "I could not find these" list blocks the launch. This side
// does not try to repair them: the planner already looked and failed.
func TestPlanLaunchRejectsPlannerReportedMissingFiles(t *testing.T) {
	plan := okPlan(implPacket("p1", LaunchOwnedFile{Path: "a.c", Exists: true}))
	plan.MissingOwnedFiles = []string{"src/gone.c"}
	d := PlanLaunch(plan, 0)
	if !strings.Contains(d.Error, "missing owned_files") || !strings.Contains(d.Error, "src/gone.c") {
		t.Fatalf("error should name the first missing file, got %q", d.Error)
	}
}

func TestPlanLaunchRejectsPacketWithoutOwnedFiles(t *testing.T) {
	d := PlanLaunch(okPlan(implPacket("p1")), 0)
	if !strings.Contains(d.Error, "missing owned_files") {
		t.Fatalf("want missing owned_files error, got %q", d.Error)
	}
}

func TestPlanLaunchRejectsPacketWithoutResultContract(t *testing.T) {
	p := implPacket("p1", LaunchOwnedFile{Path: "a.c", Exists: true})
	p.HandoffSchema = ""
	d := PlanLaunch(okPlan(p), 0)
	if !strings.Contains(d.Error, "handoff_schema delegate_result_v1") {
		t.Fatalf("want handoff schema error, got %q", d.Error)
	}
}

func TestPlanLaunchRejectsMoreThanMaxPlanSteps(t *testing.T) {
	var packets []LaunchPacket
	for i := 0; i <= MaxPlanSteps; i++ {
		packets = append(packets, implPacket("p", LaunchOwnedFile{Path: "a.c", Exists: true}))
	}
	d := PlanLaunch(okPlan(packets...), 0)
	if !strings.Contains(d.Error, "too many implementation packets") {
		t.Fatalf("want too-many error, got %q", d.Error)
	}
}

// --- path repair -----------------------------------------------------------

// An existing file is never looked up, even when its basename is ambiguous in
// the repository. Existence is the whole answer.
func TestPathRepairLeavesExistingPathsAlone(t *testing.T) {
	d := PlanLaunch(okPlan(implPacket("p1", LaunchOwnedFile{
		Path: "src/a.c", Exists: true, Candidates: []string{"src/a.c", "vendor/a.c"},
	})), 0)
	if d.Error != "" {
		t.Fatalf("unexpected error: %s", d.Error)
	}
	if len(d.Repairs) != 0 {
		t.Errorf("existing path should not be repaired: %+v", d.Repairs)
	}
	if d.Tasks[0].OwnedFiles[0] != "src/a.c" {
		t.Errorf("path changed: %q", d.Tasks[0].OwnedFiles[0])
	}
}

// Exactly one file in the repository has the missing path's basename, so the
// planner named the right file in the wrong place. Repair it and say so.
func TestPathRepairRewritesAUniqueBasenameMatch(t *testing.T) {
	d := PlanLaunch(okPlan(implPacket("p1", LaunchOwnedFile{
		Path: "util.c", Candidates: []string{"src/modules/util.c"},
	})), 0)
	if d.Error != "" {
		t.Fatalf("unexpected error: %s", d.Error)
	}
	if len(d.Repairs) != 1 || d.Repairs[0].From != "util.c" || d.Repairs[0].To != "src/modules/util.c" {
		t.Fatalf("want a recorded repair, got %+v", d.Repairs)
	}
	if d.Tasks[0].OwnedFiles[0] != "src/modules/util.c" {
		t.Errorf("task should carry the repaired path, got %q", d.Tasks[0].OwnedFiles[0])
	}
}

// No match is NOT an error: a packet may legitimately be about to create the
// file. The path stands and the caller gets a warning to log.
func TestPathRepairWarnsButAllowsAnUnmatchedPath(t *testing.T) {
	d := PlanLaunch(okPlan(implPacket("p1", LaunchOwnedFile{Path: "src/brand_new.c"})), 0)
	if d.Error != "" {
		t.Fatalf("a new-file packet must launch, got error %q", d.Error)
	}
	if len(d.Warnings) != 1 || !strings.Contains(d.Warnings[0], "src/brand_new.c") {
		t.Fatalf("want one warning naming the path, got %+v", d.Warnings)
	}
	if d.Tasks[0].OwnedFiles[0] != "src/brand_new.c" {
		t.Errorf("unmatched path should stand, got %q", d.Tasks[0].OwnedFiles[0])
	}
}

// Several matches cannot be resolved safely, and guessing would point a
// delegate at the wrong file. Fail before anything is written, and name the
// candidates so the plan can be repaired by hand.
func TestPathRepairRejectsAnAmbiguousBasename(t *testing.T) {
	d := PlanLaunch(okPlan(implPacket("p1", LaunchOwnedFile{
		Path: "util.c", Candidates: []string{"src/a/util.c", "src/b/util.c"},
	})), 0)
	if !strings.Contains(d.Error, "ambiguous basename") {
		t.Fatalf("want ambiguity error, got %q", d.Error)
	}
	for _, want := range []string{"src/a/util.c", "src/b/util.c"} {
		if !strings.Contains(d.Error, want) {
			t.Errorf("error should list candidate %s: %q", want, d.Error)
		}
	}
	if len(d.Steps) != 0 || len(d.Tasks) != 0 {
		t.Errorf("a rejected plan must yield nothing to write")
	}
}

// --- the brief -------------------------------------------------------------

func TestLaunchPromptStatesObjectiveAndOwnedFiles(t *testing.T) {
	p := implPacket("p1")
	got := LaunchPrompt(p, []string{"src/a.c", "src/b.c"})
	for _, want := range []string{"packet p1", "Objective: do p1", "src/a.c, src/b.c",
		"delegate_result_v1"} {
		if !strings.Contains(got, want) {
			t.Errorf("prompt missing %q:\n%s", want, got)
		}
	}
}

// A packet with no title falls back to something a human can still read, rather
// than briefing a delegate with an empty first line.
func TestLaunchPromptFallsBackWhenTitleAndObjectiveAreEmpty(t *testing.T) {
	got := LaunchPrompt(LaunchPacket{}, nil)
	if !strings.HasPrefix(got, "delegate packet") {
		t.Errorf("want a readable fallback title, got:\n%s", got)
	}
	if !strings.Contains(got, "Owned files (modify only these): (none)") {
		t.Errorf("want an explicit empty file list, got:\n%s", got)
	}
}

// The C built this list into a fixed 1KB buffer and stopped at the boundary, so
// a packet owning enough files told its delegate to "modify only these" while
// withholding some of them. Every owned file is now named.
func TestLaunchPromptNamesEveryOwnedFileNoMatterHowMany(t *testing.T) {
	var files []string
	for i := 0; i < 200; i++ {
		files = append(files, "src/some/moderately/long/path/file"+string(rune('a'+i%26))+".c")
	}
	got := LaunchPrompt(implPacket("p1"), files)
	if len(got) < 1024 {
		t.Fatalf("test does not exercise the old truncation point: prompt is %d bytes", len(got))
	}
	for _, f := range files {
		if !strings.Contains(got, f) {
			t.Fatalf("prompt dropped %q", f)
		}
	}
}

// A packet with no role becomes an execute task rather than a task with no role
// at all, which the coordinator could not dispatch.
func TestTaskRoleDefaultsToExecute(t *testing.T) {
	p := implPacket("p1", LaunchOwnedFile{Path: "a.c", Exists: true})
	p.Role = ""
	d := PlanLaunch(okPlan(p), 0)
	if d.Error != "" {
		t.Fatalf("unexpected error: %s", d.Error)
	}
	if d.Tasks[0].Role != "execute" {
		t.Errorf("want execute, got %q", d.Tasks[0].Role)
	}
}

func TestMaxConcurrentIsHonouredWhenPositive(t *testing.T) {
	d := PlanLaunch(okPlan(implPacket("p1", LaunchOwnedFile{Path: "a.c", Exists: true})), 7)
	if d.MaxConcurrent != 7 {
		t.Errorf("want 7, got %d", d.MaxConcurrent)
	}
}
