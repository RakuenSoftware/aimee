package delegates

import (
	"strings"
	"testing"
)

// These mirror the fixtures in src/tests/test_delegate_context_shed.c, one for
// one and by name, so the mapping is auditable: the C proved these cases by
// driving a real filesystem, and the same cases are proved here against the
// rule that replaced it.

func preflight(prompt string, writesAllowed bool, paths ...NamedPath) DriftVerdict {
	return JudgeNamedFileDrift(DriftFacts{Paths: paths, Prompt: prompt, WritesAllowed: writesAllowed})
}

// --- pre-flight -------------------------------------------------------------

func TestPreflightNonexistentNoCreateIntent(t *testing.T) {
	v := preflight("Edit src/nonexistent.c to fix the bug.", true,
		NamedPath{Path: "src/nonexistent.c"})
	if v.Severity != DriftHard {
		t.Fatalf("want hard drift, got %v (%q)", v.Severity, v.Message)
	}
	for _, want := range []string{"src/nonexistent.c", "create intent"} {
		if !strings.Contains(v.Message, want) {
			t.Errorf("message should mention %q: %q", want, v.Message)
		}
	}
}

func TestPreflightNonexistentWithCreateIntent(t *testing.T) {
	v := preflight("Create a new file src/newfile.c with the module.", true,
		NamedPath{Path: "src/newfile.c"})
	if v.Severity != DriftNone {
		t.Fatalf("a brief that asks for the file may launch: %v (%q)", v.Severity, v.Message)
	}
}

// A delegate that cannot write cannot be expected to create anything, so a
// missing path is context rather than an unmet promise.
//
// It is the PERMISSION that says so, not the wording of the brief. A brief that
// says "do not edit" no longer makes a write-capable delegate read-only: that
// was a rule the prompt could rewrite, and it disagreed with the mount.
func TestPreflightReadonlyMissingFileAsContext(t *testing.T) {
	v := preflight("Read-only review of src/newfile.c. Do not edit files.", false,
		NamedPath{Path: "src/newfile.c"})
	if v.Severity != DriftNone {
		t.Fatalf("want no drift, got %v (%q)", v.Severity, v.Message)
	}
}

func TestPreflightExistingFileNoError(t *testing.T) {
	v := preflight("Edit the config parser to handle edge cases.", true,
		NamedPath{Path: "src/config.c", Exists: true})
	if v.Severity != DriftNone {
		t.Fatalf("an existing file is never a pre-flight problem: %v (%q)", v.Severity, v.Message)
	}
}

// The role is the authoritative signal: a read/analysis delegate produces its
// artifact from its reply, so a path scraped out of reference content threaded
// into its prompt must never fail it.
func TestReadRoleScrapedPathNoHardDrift(t *testing.T) {
	v := preflight("Summarise what src/nonexistent_xyz.c is for.", false,
		NamedPath{Path: "src/nonexistent_xyz.c"})
	if v.Severity == DriftHard {
		t.Fatalf("a read role must not hard-fail on a scraped path: %q", v.Message)
	}
}

// --- pre-flight: paths that are referenced, not produced --------------------

func TestPreflightRemoteScpPathSkipped(t *testing.T) {
	v := JudgeNamedFileDrift(DriftFacts{
		Paths: []NamedPath{{Path: "/mnt/media/other/thing.c"}},
		Prompt: "Update aimee-server on the host. The server config lives at " +
			"admin@192.168.1.254:/mnt/media/.plugins/aimee-server/server/home/aimee.yaml.",
		WorktreePath:  "/home/user/repo",
		WritesAllowed: true,
	})
	if v.Severity != DriftNone {
		t.Fatalf("a remote path is referenced, not produced: %v (%q)", v.Severity, v.Message)
	}
}

func TestPreflightAbsoluteOutsideWorktreeSkipped(t *testing.T) {
	v := JudgeNamedFileDrift(DriftFacts{
		Paths:         []NamedPath{{Path: "/etc/aimee/other.c"}},
		Prompt:        "Edit /etc/aimee/other.c to fix the bug.",
		WorktreePath:  "/home/user/repo",
		WritesAllowed: true,
	})
	if v.Severity != DriftNone {
		t.Fatalf("outside the worktree is referenced: %v (%q)", v.Severity, v.Message)
	}
}

// A path UNDER the worktree is a genuine create target and still fails.
func TestPreflightRelativeUnderWorktreeStillFails(t *testing.T) {
	v := JudgeNamedFileDrift(DriftFacts{
		Paths:         []NamedPath{{Path: "/home/user/repo/src/missing.c"}},
		Prompt:        "Edit src/missing.c to fix the bug.",
		WorktreePath:  "/home/user/repo",
		WritesAllowed: true,
	})
	if v.Severity != DriftHard {
		t.Fatalf("a path under the worktree is a create target: %v (%q)", v.Severity, v.Message)
	}
}

// The prefix guard: /home/user/repo must not swallow /home/user/repository.
func TestAWorktreePrefixDoesNotSwallowASiblingDirectory(t *testing.T) {
	if !PathIsExternalToWorktree("/home/user/repository/a.c", "/home/user/repo") {
		t.Error("/home/user/repository is not inside /home/user/repo")
	}
	if PathIsExternalToWorktree("/home/user/repo/a.c", "/home/user/repo") {
		t.Error("a path under the root is not external")
	}
	if PathIsExternalToWorktree("/home/user/repo/a.c", "/home/user/repo/") {
		t.Error("a trailing slash on the root must not change the answer")
	}
}

// --- pre-flight: what the code index adds -----------------------------------

// Hits that exist but none in this path mean the path came from prompt examples
// -- a system include, a quoted snippet -- not from this repository.
func TestIndexHitsElsewhereMarkThePathAsNotOurs(t *testing.T) {
	v := preflight("Edit stdio.h to fix the bug.", true,
		NamedPath{Path: "stdio.h", IndexHitFiles: []string{"src/vendor/other.h", "src/a.c"}})
	if v.Severity != DriftNone {
		t.Fatalf("not a project file, so not a create target: %v (%q)", v.Severity, v.Message)
	}
}

func TestAnIndexHitInThisPathLeavesTheCheckAlone(t *testing.T) {
	v := preflight("Edit src/parser.c to fix the bug.", true,
		NamedPath{Path: "src/parser.c", IndexHitFiles: []string{"/repo/src/parser.c"}})
	if v.Severity != DriftHard {
		t.Fatalf("a real project file still needs create intent: %v (%q)", v.Severity, v.Message)
	}
}

// An empty hit list is AMBIGUOUS -- index down, or stem not indexed -- and must
// fall through to the create-intent check rather than excusing the path.
func TestAnUnreachableIndexDoesNotExcuseAMissingFile(t *testing.T) {
	v := preflight("Edit src/parser.c to fix the bug.", true, NamedPath{Path: "src/parser.c"})
	if v.Severity != DriftHard {
		t.Fatalf("an index outage must not change the verdict: %v (%q)", v.Severity, v.Message)
	}
}

// --- post-run, with a worktree to diff --------------------------------------

func postRunWithWorktree(roleIsWrite bool, paths ...NamedPath) DriftVerdict {
	return JudgeNamedFileDrift(DriftFacts{
		Paths:         paths,
		Prompt:        "implement the fix in the named file",
		Response:      "done",
		WorktreePath:  "/repo",
		WritesAllowed: roleIsWrite,
	})
}

func TestPostRunPathInDiffIsClean(t *testing.T) {
	if v := postRunWithWorktree(true, NamedPath{Path: "src/a.c", Exists: true, InDiff: true}); v.Severity != DriftNone {
		t.Fatalf("a touched file has not drifted: %v (%q)", v.Severity, v.Message)
	}
}

// An existing file the delegate did not touch was context, not a target.
func TestPostRunExistingUntouchedFileIsSoft(t *testing.T) {
	v := postRunWithWorktree(true, NamedPath{Path: "src/a.c", Exists: true})
	if v.Severity != DriftSoft {
		t.Fatalf("want soft, got %v (%q)", v.Severity, v.Message)
	}
	if !strings.Contains(v.Message, "context-only") {
		t.Errorf("message should say why it is only a warning: %q", v.Message)
	}
}

// A write delegate that was asked for a file and produced nothing is the case
// this whole check exists to catch.
func TestPostRunUncreatedFileIsHardForAWriteDelegate(t *testing.T) {
	v := postRunWithWorktree(true, NamedPath{Path: "src/new.c"})
	if v.Severity != DriftHard {
		t.Fatalf("want hard, got %v (%q)", v.Severity, v.Message)
	}
	if !strings.Contains(v.Message, "was not created") {
		t.Errorf("message should name the failure: %q", v.Message)
	}
}

func TestPostRunWorktreeReadonlyMissingFileIsSoft(t *testing.T) {
	v := postRunWithWorktree(false, NamedPath{Path: "src/new.c"})
	if v.Severity != DriftSoft {
		t.Fatalf("a read delegate promised nothing: %v (%q)", v.Severity, v.Message)
	}
}

// --- post-run, falling back to the response text ----------------------------

func postRunNoWorktree(response string, roleIsWrite bool, paths ...NamedPath) DriftVerdict {
	return JudgeNamedFileDrift(DriftFacts{
		Paths:         paths,
		Prompt:        "implement the fix in the named file",
		Response:      response,
		WritesAllowed: roleIsWrite,
	})
}

func TestPostRunPathInResponse(t *testing.T) {
	v := postRunNoWorktree("I edited src/config.c to fix the parsing bug.", true,
		NamedPath{Path: "src/config.c", Exists: true})
	if v.Severity != DriftNone {
		t.Fatalf("the response names the file: %v (%q)", v.Severity, v.Message)
	}
}

func TestPostRunPathAbsentIsHard(t *testing.T) {
	v := postRunNoWorktree("I completed the task without touching that file.", true,
		NamedPath{Path: "src/config.c", Exists: true})
	if v.Severity != DriftHard {
		t.Fatalf("want hard, got %v (%q)", v.Severity, v.Message)
	}
}

func TestPostRunReadonlyPathAbsentIsSoft(t *testing.T) {
	v := postRunNoWorktree("I completed the task without touching that file.", false,
		NamedPath{Path: "src/config.c", Exists: true})
	if v.Severity != DriftSoft {
		t.Fatalf("want soft, got %v (%q)", v.Severity, v.Message)
	}
}

// A bare basename could name any of several files, so it is evidence but not
// proof.
func TestPostRunBasenameOnlyIsSoft(t *testing.T) {
	v := postRunNoWorktree("I fixed config.c as requested.", true,
		NamedPath{Path: "src/config.c", Exists: true})
	if v.Severity != DriftSoft {
		t.Fatalf("want soft, got %v (%q)", v.Severity, v.Message)
	}
	if !strings.Contains(v.Message, "ambiguous") {
		t.Errorf("message should say why: %q", v.Message)
	}
}

func TestPostRunNonexistentPathSkipped(t *testing.T) {
	v := postRunNoWorktree("done", true, NamedPath{Path: "src/never.c"})
	if v.Severity != DriftNone {
		t.Fatalf("with no worktree there is nothing to conclude: %v (%q)", v.Severity, v.Message)
	}
}

// --- severity precedence ----------------------------------------------------

// One hard path outranks any number of soft ones, whatever the order.
func TestHardDriftWinsOverSoft(t *testing.T) {
	v := postRunWithWorktree(true,
		NamedPath{Path: "src/context.c", Exists: true},
		NamedPath{Path: "src/new.c"},
	)
	if v.Severity != DriftHard {
		t.Fatalf("want hard, got %v (%q)", v.Severity, v.Message)
	}
	if !strings.Contains(v.Message, "src/new.c") {
		t.Errorf("the hard path should be the one named: %q", v.Message)
	}
}

// Among soft findings the FIRST is reported: it is the one the operator will
// recognise from the brief they wrote.
func TestTheFirstSoftFindingIsTheOneReported(t *testing.T) {
	v := postRunWithWorktree(true,
		NamedPath{Path: "src/first.c", Exists: true},
		NamedPath{Path: "src/second.c", Exists: true},
	)
	if v.Severity != DriftSoft || !strings.Contains(v.Message, "src/first.c") {
		t.Fatalf("want the first soft path, got %v (%q)", v.Severity, v.Message)
	}
}

func TestNoPathsIsNoVerdict(t *testing.T) {
	if v := JudgeNamedFileDrift(DriftFacts{Prompt: "anything"}); v.Severity != DriftNone {
		t.Fatalf("nothing named, nothing to check: %v", v.Severity)
	}
}

// --- create intent ----------------------------------------------------------

func TestCreateIntentIsCaseInsensitive(t *testing.T) {
	for _, prompt := range []string{
		"Implement a foo", "CREATE the module", "Add file src/a.c", "Generate the stub",
		"write the guard",
	} {
		if !HasCreateIntent(prompt) {
			t.Errorf("should read as create intent: %q", prompt)
		}
	}
	for _, prompt := range []string{"review the diff", "explain what this does"} {
		if HasCreateIntent(prompt) {
			t.Errorf("should not read as create intent: %q", prompt)
		}
	}
}
