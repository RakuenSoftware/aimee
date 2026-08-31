package delegates

// Did a write delegate that reported success actually change anything?
//
// A write role returning rc==0 having touched nothing is the failure this
// catches, and it is worth catching because it is INVISIBLE otherwise: the
// delegate reports success, the supervisor believes it, and the work silently
// did not happen. Observed as "write role reported success but produced no
// diff".
//
// The evidence is gathered by the caller -- file snapshots, `git status`, a
// HEAD comparison -- because all three are I/O. What that evidence MEANS is
// decided here, including the cases where nothing changed and that is fine.

// NoopWriteEvidence is what the caller observed. Every field is a fact it
// established; none is inferred here.
type NoopWriteEvidence struct {
	// The run itself.
	// WritesAllowed is the delegate's repo_write permission, resolved once
	// when it was created. There is no second write signal to agree with.
	WritesAllowed  bool
	HandoffJSON    bool
	Succeeded      bool // the delegate's own rc == 0
	NamedPathCount int

	// AnyNamedChanged is whether any path the brief NAMED changed. Only
	// meaningful when NamedPathCount > 0.
	AnyNamedChanged bool

	// WorktreeChanged is `git status` finding anything at all.
	WorktreeChanged bool
	// HeadAdvanced is whether HEAD moved, which is real progress even with a
	// clean worktree: the delegate committed.
	HeadAdvanced bool

	// HeadSnapshotTaken is whether a pre-run HEAD was captured. Without one,
	// "HEAD did not advance" cannot be distinguished from "we never looked".
	HeadSnapshotTaken bool
	// HasWorktree is whether the delegate ran in its own worktree.
	HasWorktree bool
}

// NoopWriteVerdict is the answer.
type NoopWriteVerdict struct {
	// Noop means the run produced nothing and must be treated as incomplete.
	Noop bool
	// Message is the operator-facing explanation. Present for a Noop, and also
	// for the benign cases worth a note.
	Message string
	// Benign marks a case where nothing looked changed but the run was real, or
	// where the check could not be made. Worth logging, never a failure.
	Benign bool
}

// JudgeNoopWrite decides whether a completed delegate did anything.
//
// Only a delegate that COULD write and REPORTED SUCCESS can be a no-op. One
// that cannot write and changed nothing did exactly what it was for, and one
// that already failed has its own error.
func JudgeNoopWrite(e NoopWriteEvidence) NoopWriteVerdict {
	if !e.WritesAllowed || !e.Succeeded {
		return NoopWriteVerdict{}
	}

	if e.NamedPathCount > 0 {
		if e.AnyNamedChanged {
			return NoopWriteVerdict{}
		}
		// The named paths were stable. That is only damning if nothing else
		// moved either -- the path heuristic reads names out of prose, so it
		// routinely names files the task merely REFERS to.
		if e.HeadAdvanced {
			return NoopWriteVerdict{Benign: true,
				Message: "named paths unchanged but HEAD advanced (delegate committed); " +
					"named-path heuristic likely matched context refs"}
		}
		if e.WorktreeChanged {
			return NoopWriteVerdict{Benign: true,
				Message: "named paths unchanged but the worktree has other changes; " +
					"named-path heuristic likely matched context refs"}
		}
		return NoopWriteVerdict{Noop: true,
			Message: "no owned files changed; result treated as incomplete"}
	}

	// No named paths: fall back to "did anything at all change". A handoff
	// carries its own structured result, so its absence of a diff is not
	// evidence of nothing happening.
	if e.HandoffJSON {
		return NoopWriteVerdict{}
	}

	// A delegate in its own worktree whose HEAD was never snapshotted cannot be
	// judged: "HEAD did not advance" and "we never looked" are the same
	// observation, and failing a run on that would fail it for our own missing
	// measurement.
	if e.HasWorktree && !e.HeadSnapshotTaken {
		return NoopWriteVerdict{Benign: true,
			Message: "skipping the no-op check: the delegate's HEAD could not be snapshotted"}
	}

	if e.WorktreeChanged || e.HeadAdvanced {
		return NoopWriteVerdict{}
	}
	return NoopWriteVerdict{Noop: true,
		Message: "no file changes detected; result treated as incomplete"}
}
