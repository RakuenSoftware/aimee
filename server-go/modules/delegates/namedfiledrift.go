package delegates

import (
	"fmt"
	"strings"
)

// Did the delegate touch the files its brief named?
//
// A brief that names src/parser.c and comes back describing work on something
// else has drifted, and the cheapest place to catch it is against the files
// themselves. This runs twice: PRE-FLIGHT, before the delegate starts, to refuse
// a brief that names a file nobody could write; and POST-RUN, against what
// actually changed.
//
// The severity distinction is the whole point:
//
//   - HARD drift fails the delegate. Reserved for a WRITE delegate that was
//     asked to change a named file and did not.
//   - SOFT drift warns. A path that was context rather than a target -- read,
//     quoted, referenced -- has not gone wrong by being unmodified.
//
// Getting that backwards is expensive in both directions: hard-failing a review
// for not editing the file it reviewed makes the check useless, and softening a
// write delegate that silently produced nothing hides the failure it exists to
// find.
//
// Everything here is a reading of facts the caller gathered: stat, `git diff`,
// the code index, and the response text. No I/O.

// DriftSeverity is what the caller should do about a path.
type DriftSeverity int

const (
	DriftNone DriftSeverity = iota
	DriftSoft               // warn; the delegate still succeeded
	DriftHard               // fail the delegate
)

// createIntent is what asking for a file to be brought into existence looks
// like. Matched case-insensitively: "Implement a foo" at the start of a sentence
// was once rejected by a literal match, which surfaced as a false "no create
// intent found" refusal before the delegate ever ran.
var createIntent = []string{
	"create", "new file", "add file", "implement", "write", "generate",
}

// HasCreateIntent reports whether the brief asks for a file to be created.
//
// This asks whether the brief wants something that does not exist yet to start
// existing. It says nothing about whether the delegate MAY create it: that is
// the repo_write permission, resolved once and passed in.
func HasCreateIntent(prompt string) bool {
	lower := strings.ToLower(prompt)
	for _, kw := range createIntent {
		if strings.Contains(lower, kw) {
			return true
		}
	}
	return false
}

// PathIsExternalToWorktree reports whether an absolute path lies outside the
// worktree, making it a REFERENCED file rather than an output.
//
// An ops brief that mentions admin@host:/mnt/.../aimee.yaml names something the
// delegate reads over SSH; refusing to launch because that path is not in the
// worktree would fail correct work.
func PathIsExternalToWorktree(path, worktree string) bool {
	if path == "" || path[0] != '/' {
		return false
	}
	root := strings.TrimRight(worktree, "/")
	if root == "" {
		return true // absolute, with no worktree root to anchor it to
	}
	if !strings.HasPrefix(path, root) {
		return true
	}
	// Guard against prefix aliasing (root /a/b must not swallow /a/bc): the
	// character after the root has to end the path or start a new segment.
	rest := path[len(root):]
	return rest != "" && !strings.HasPrefix(rest, "/")
}

// NamedPath is one path from the brief, with the facts the caller looked up.
type NamedPath struct {
	Path string
	// Exists is a filesystem check, resolved against the worktree when the
	// path is relative.
	Exists bool
	// InDiff is whether `git diff` mentions the path or its basename. Only
	// meaningful post-run with a worktree.
	//
	// The path/basename fallback is folded into this one bool DELIBERATELY,
	// unlike the response-text matching below where the two carry different
	// severities. Here both mean the same thing -- the file was touched -- so
	// which of them matched is a lookup detail, not a decision. Sending the
	// whole diff instead would put a multi-megabyte artifact on the bus to
	// re-derive an answer the caller already has.
	InDiff bool
	// IndexHitFiles are the files the code index returned for this path's
	// basename stem, and they answer a question the filesystem cannot: is this
	// a real project file at all?
	//
	// EMPTY IS AMBIGUOUS ON PURPOSE and must stay that way: it means either the
	// index was unreachable or the stem is not indexed, and both fall through
	// to the create-intent check so behaviour is unchanged when the index is
	// down. Only a NON-empty list with no entry matching this path is evidence
	// that the path came from prompt examples -- a system include, a snippet --
	// rather than naming a file this repository has.
	IndexHitFiles []string
}

// DriftFacts is everything one check needs.
type DriftFacts struct {
	Paths  []NamedPath
	Prompt string
	// Response is the delegate's reply. Empty means PRE-FLIGHT: the delegate
	// has not run yet.
	Response string
	// WorktreePath is the tree the relative paths are anchored to. Empty means
	// there was none: `git diff` was not available as ground truth, so post-run
	// falls back to matching the response text.
	//
	// It is passed rather than pre-resolved because deciding whether a path
	// lies outside it IS a rule -- see PathIsExternalToWorktree -- and having
	// the caller answer it would put that rule back in C.
	WorktreePath string
	// WritesAllowed is whether this delegate holds repo_write. It is passed,
	// not worked out here: the permission was resolved once when the delegate
	// was created, and every consumer reads that same answer.
	WritesAllowed bool
}

// DriftVerdict is the answer for the whole set.
type DriftVerdict struct {
	Severity DriftSeverity
	// Message is the operator-facing wording, naming the first path that
	// justified the severity.
	Message string
}

// JudgeNamedFileDrift reads the named paths against what the caller found.
func JudgeNamedFileDrift(f DriftFacts) DriftVerdict {
	if len(f.Paths) == 0 {
		return DriftVerdict{}
	}

	// A delegate that cannot write produces its artifact from its reply, so a
	// path scraped out of reference content threaded into its prompt must never
	// fail it.
	writesAllowed := f.WritesAllowed

	v := DriftVerdict{}
	// A hard verdict wins outright and stops. A soft one keeps the FIRST
	// wording, because the first named path is the one the operator will
	// recognise from the brief they wrote.
	note := func(severity DriftSeverity, format string, args ...any) bool {
		if severity == DriftHard {
			v.Severity = DriftHard
			v.Message = fmt.Sprintf(format, args...)
			return true
		}
		if v.Severity == DriftNone {
			v.Severity = DriftSoft
			v.Message = fmt.Sprintf(format, args...)
		}
		return false
	}

	for _, p := range f.Paths {
		if p.Path == "" {
			continue
		}

		switch {
		case f.Response == "":
			if preflightDrift(p, f, writesAllowed, note) {
				return v
			}
		case f.WorktreePath != "":
			if postRunAgainstDiff(p, writesAllowed, note) {
				return v
			}
		default:
			if postRunAgainstResponse(p, f.Response, writesAllowed, note) {
				return v
			}
		}
	}
	return v
}

// preflightDrift refuses a brief that names a file nobody is going to write.
func preflightDrift(p NamedPath, f DriftFacts, writesAllowed bool,
	note func(DriftSeverity, string, ...any) bool) bool {

	if p.Exists || f.Prompt == "" {
		return false
	}
	// A path that cannot be an in-worktree create target is a file the brief
	// REFERS to, not one it promises to produce. Merely naming it must not
	// refuse the delegate before it runs.
	if PathIsExternalToWorktree(p.Path, f.WorktreePath) {
		return false
	}
	if !writesAllowed {
		return false
	}
	if indexSaysNotAProjectFile(p) {
		return false
	}
	if HasCreateIntent(f.Prompt) {
		return false
	}
	return note(DriftHard,
		"named file '%s' does not exist and no create intent found in prompt; "+
			"use 'create', 'new file', or 'implement' to create it", p.Path)
}

// indexSaysNotAProjectFile reports whether the code index positively contradicts
// this path. See NamedPath.IndexHitFiles for why empty is not a contradiction.
func indexSaysNotAProjectFile(p NamedPath) bool {
	if len(p.IndexHitFiles) == 0 {
		return false
	}
	for _, hit := range p.IndexHitFiles {
		if strings.HasSuffix(hit, p.Path) {
			return false
		}
	}
	return true
}

// postRunAgainstDiff uses `git diff` as ground truth.
func postRunAgainstDiff(p NamedPath, writesAllowed bool,
	note func(DriftSeverity, string, ...any) bool) bool {

	if p.InDiff {
		return false
	}
	if p.Exists {
		return note(DriftSoft,
			"named file '%s' was not modified by delegate (possible context-only reference)",
			p.Path)
	}
	if !writesAllowed {
		return note(DriftSoft, "named file '%s' was read-only context and was not created", p.Path)
	}
	return note(DriftHard, "named file '%s' was not created by delegate", p.Path)
}

// postRunAgainstResponse is the fallback when there was no worktree to diff.
func postRunAgainstResponse(p NamedPath, response string, writesAllowed bool,
	note func(DriftSeverity, string, ...any) bool) bool {

	if !p.Exists || strings.Contains(response, p.Path) {
		return false
	}

	base := pathBase(p.Path)
	if strings.Contains(response, base) {
		return note(DriftSoft,
			"named file '%s' matched only by basename '%s' in response (ambiguous)", p.Path, base)
	}
	if !writesAllowed {
		return note(DriftSoft,
			"named file '%s' was read-only context and not repeated in response", p.Path)
	}
	return note(DriftHard, "named file '%s' not found in delegate response (possible drift)", p.Path)
}

func pathBase(path string) string {
	if i := strings.LastIndexByte(path, '/'); i >= 0 {
		return path[i+1:]
	}
	return path
}
