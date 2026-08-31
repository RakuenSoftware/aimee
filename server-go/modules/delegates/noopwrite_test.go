package delegates

import "testing"

func writeRun() NoopWriteEvidence {
	return NoopWriteEvidence{
		WritesAllowed: true, Succeeded: true,
		HeadSnapshotTaken: true,
	}
}

// Only a delegate that could write and reported success can be a no-op.
// Everything else has its own explanation already.
func TestOnlyASuccessfulWriteRunCanBeANoop(t *testing.T) {
	cases := map[string]NoopWriteEvidence{
		"cannot write":   {WritesAllowed: false, Succeeded: true},
		"already failed": {WritesAllowed: true, Succeeded: false},
	}
	for name, e := range cases {
		if got := JudgeNoopWrite(e); got.Noop {
			t.Errorf("%s was called a no-op: %+v", name, got)
		}
	}
}

func TestNamedPathThatChangedIsWork(t *testing.T) {
	e := writeRun()
	e.NamedPathCount = 2
	e.AnyNamedChanged = true
	if got := JudgeNoopWrite(e); got.Noop || got.Benign {
		t.Errorf("%+v, want a clean pass", got)
	}
}

// The named-path heuristic reads filenames out of prose, so it routinely names
// files a task merely REFERS to. Unchanged named paths are only damning when
// nothing else moved either.
func TestUnchangedNamedPathsAreForgivenWhenSomethingElseMoved(t *testing.T) {
	e := writeRun()
	e.NamedPathCount = 2

	committed := e
	committed.HeadAdvanced = true
	got := JudgeNoopWrite(committed)
	if got.Noop || !got.Benign {
		t.Errorf("a delegate that committed was failed: %+v", got)
	}
	if got.Message == "" {
		t.Error("the benign case said nothing, so the heuristic's miss is invisible")
	}

	dirty := e
	dirty.WorktreeChanged = true
	got = JudgeNoopWrite(dirty)
	if got.Noop || !got.Benign {
		t.Errorf("a delegate with other changes was failed: %+v", got)
	}
}

// The case this exists for.
func TestNamedPathsUnchangedAndNothingElseIsANoop(t *testing.T) {
	e := writeRun()
	e.NamedPathCount = 2
	got := JudgeNoopWrite(e)
	if !got.Noop {
		t.Fatalf("%+v, want a no-op", got)
	}
	if got.Message == "" {
		t.Error("a refusal with no explanation")
	}
}

// With no named paths the question is just "did anything change at all".
func TestNoNamedPathsFallsBackToAnyChange(t *testing.T) {
	e := writeRun()

	if got := JudgeNoopWrite(e); !got.Noop {
		t.Errorf("%+v, want a no-op when nothing changed at all", got)
	}

	moved := e
	moved.WorktreeChanged = true
	if got := JudgeNoopWrite(moved); got.Noop {
		t.Errorf("%+v, want a pass when the worktree changed", got)
	}

	committed := e
	committed.HeadAdvanced = true
	if got := JudgeNoopWrite(committed); got.Noop {
		t.Errorf("%+v, want a pass when HEAD advanced", got)
	}
}

// A handoff carries its own structured result, so producing no diff is not
// evidence that nothing happened.
func TestHandoffIsNotJudgedByItsDiff(t *testing.T) {
	e := writeRun()
	e.HandoffJSON = true
	if got := JudgeNoopWrite(e); got.Noop {
		t.Errorf("%+v, want a handoff left alone", got)
	}
}

// "HEAD did not advance" and "we never looked" are the same observation. Failing
// a run on that would fail it for OUR missing measurement.
func TestUnsnapshottedHeadIsNotJudged(t *testing.T) {
	e := writeRun()
	e.HasWorktree = true
	e.HeadSnapshotTaken = false

	got := JudgeNoopWrite(e)
	if got.Noop {
		t.Errorf("%+v, want no judgement without a snapshot", got)
	}
	if !got.Benign || got.Message == "" {
		t.Error("the skipped check was silent, so nobody learns the guard did not run")
	}
}

// ...but without a worktree there is nothing to snapshot, so the fallback still
// applies and a genuinely empty run is still caught.
func TestNoWorktreeStillJudgesTheFallback(t *testing.T) {
	e := writeRun()
	e.HasWorktree = false
	e.HeadSnapshotTaken = false
	if got := JudgeNoopWrite(e); !got.Noop {
		t.Errorf("%+v, want a no-op: there was nothing to snapshot and nothing changed", got)
	}
}
