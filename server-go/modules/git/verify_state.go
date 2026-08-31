package git

// The verify-result ledger: the tree/commit hashes verify keys itself on, the
// dirty-worktree probe, and the per-repository .aimee/.last-verify file. This
// is the first of git's I/O paths to move out of C, and it was chosen because
// it is the only part of verify that touches no credential: nothing under
// git_verify*.c references the vault, so it can move before the vault gains a
// bus surface (docs/proposals/pending/vault-bus-only-access.md).
//
// The C side keeps producing and consuming the step-results string; it crosses
// this boundary opaquely. Its format belongs to whoever runs the steps, which
// is still C.

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

// StateMax is the rolling window kept in the ledger. It matches
// VERIFY_STATE_MAX so a file written by either side reads back identically
// during the migration.
const StateMax = 16

// StateEntry is one verified tree: when it passed, which tree, and how its
// steps fared. StepResults is opaque here -- see the package comment.
type StateEntry struct {
	Timestamp   int64  `json:"timestamp"`
	Hash        string `json:"hash"`
	Failed      int    `json:"failed"`
	Total       int    `json:"total"`
	StepResults string `json:"step_results,omitempty"`
}

// VerifyStateRequest is the stage-6 request. Op selects the operation; the
// remaining fields are read only by the operations that need them.
type VerifyStateRequest struct {
	Op          string `json:"op"`
	ProjectRoot string `json:"project_root"`
	Timestamp   int64  `json:"timestamp"`
	Hash        string `json:"hash"`
	Failed      int    `json:"failed"`
	Total       int    `json:"total"`
	StepResults string `json:"step_results"`
}

// VerifyStateResponse carries whichever field the operation produces. OK is
// false whenever the answer is unknown, so a caller can never read a zero
// value as a real one -- an unverified tree and an unreadable ledger must not
// look alike to the gate.
type VerifyStateResponse struct {
	OK      bool         `json:"ok"`
	Hash    string       `json:"hash,omitempty"`
	Dirty   bool         `json:"dirty,omitempty"`
	Entries []StateEntry `json:"entries,omitempty"`
}

// gitOutput runs git in root and returns its trimmed stdout. Unlike the C it
// replaces it does not go through a shell, so a repository path containing a
// quote is data rather than syntax.
func gitOutput(root string, args ...string) (string, bool) {
	full := args
	if root != "" {
		full = append([]string{"-C", root}, args...)
	}
	out, err := exec.Command("git", full...).Output()
	if err != nil {
		return "", false
	}
	return strings.TrimRight(string(out), " \t\r\n"), true
}

// mainRepoRoot resolves a worktree to the checkout that owns its object store,
// so every worktree of a repository shares one ledger and the pre-push hook --
// which runs from the main checkout -- sees state recorded anywhere.
func mainRepoRoot(dir string) string {
	if common, ok := gitOutput(dir, "rev-parse", "--git-common-dir"); ok && strings.HasPrefix(common, "/") {
		if index := strings.Index(common, "/.git"); index >= 0 {
			return common[:index]
		}
	}
	if top, ok := gitOutput(dir, "rev-parse", "--show-toplevel"); ok && top != "" {
		return top
	}
	return dir
}

// statePath is the ledger for the repository containing projectRoot.
func statePath(projectRoot string) string {
	base := projectRoot
	if projectRoot != "" {
		if resolved := mainRepoRoot(projectRoot); resolved != "" {
			base = resolved
		}
	}
	return filepath.Join(base, ".aimee", ".last-verify")
}

// parseEntry reads one current-format line:
//
//	<unix_timestamp> <hash> failed=N/total=M[ steps=<opaque>]
func parseEntry(line string) (StateEntry, bool) {
	fields := strings.Fields(line)
	if len(fields) < 2 {
		return StateEntry{}, false
	}
	timestamp, err := strconv.ParseInt(fields[0], 10, 64)
	if err != nil {
		return StateEntry{}, false
	}
	entry := StateEntry{Timestamp: timestamp, Hash: fields[1]}
	for _, field := range fields[2:] {
		// Scanned rather than split so a malformed counter leaves the entry
		// readable: a tree that verified is still a tree that verified.
		fmt.Sscanf(field, "failed=%d/total=%d", &entry.Failed, &entry.Total)
	}
	if index := strings.Index(line, " steps="); index >= 0 {
		entry.StepResults = line[index+len(" steps="):]
	}
	return entry, true
}

// readEntries parses the ledger. A missing, empty or corrupt file is not an
// error: it means nothing is verified, which is the safe answer.
func readEntries(projectRoot string) []StateEntry {
	file, err := os.Open(statePath(projectRoot))
	if err != nil {
		return nil
	}
	defer file.Close()

	scanner := bufio.NewScanner(file)
	if !scanner.Scan() {
		return nil
	}
	first := strings.TrimRight(scanner.Text(), " \t\r\n")

	// The legacy layout put the timestamp, hash and result on three lines. It
	// is still read so a ledger written before the split keeps its meaning;
	// the next write upgrades it.
	if !strings.Contains(first, " ") {
		timestamp, err := strconv.ParseInt(first, 10, 64)
		if err != nil || !scanner.Scan() {
			return nil
		}
		hash := strings.TrimRight(scanner.Text(), " \t\r\n")
		if hash == "" {
			return nil
		}
		entry := StateEntry{Timestamp: timestamp, Hash: hash}
		if scanner.Scan() {
			fmt.Sscanf(strings.TrimSpace(scanner.Text()), "failed=%d/total=%d", &entry.Failed, &entry.Total)
		}
		return []StateEntry{entry}
	}

	var entries []StateEntry
	for line := first; ; {
		if entry, ok := parseEntry(line); ok && len(entries) < StateMax {
			entries = append(entries, entry)
		}
		if !scanner.Scan() {
			break
		}
		line = strings.TrimRight(scanner.Text(), " \t\r\n")
	}
	return entries
}

// formatEntry renders one line. steps= is written last so the opaque tail can
// contain spaces without shifting any field before it.
func formatEntry(entry StateEntry) string {
	line := fmt.Sprintf("%d %s failed=%d/total=%d", entry.Timestamp, entry.Hash, entry.Failed, entry.Total)
	if entry.StepResults != "" {
		line += " steps=" + entry.StepResults
	}
	return line + "\n"
}

// writeState puts the new entry at the head, drops any prior entry for the
// same tree, and keeps the window at StateMax. The file is replaced by rename
// so a reader sees either the old ledger or the new one, never a half-written
// gate.
func writeState(projectRoot string, entry StateEntry) error {
	path := statePath(projectRoot)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}

	var builder strings.Builder
	builder.WriteString(formatEntry(entry))
	kept := 1
	for _, old := range readEntries(projectRoot) {
		if kept >= StateMax || sameTree(old.Hash, entry.Hash) {
			continue
		}
		builder.WriteString(formatEntry(old))
		kept++
	}

	temporary := path + ".tmp"
	if err := os.WriteFile(temporary, []byte(builder.String()), 0o644); err != nil {
		return err
	}
	if err := os.Rename(temporary, path); err != nil {
		os.Remove(temporary)
		return err
	}
	return nil
}

// sameTree compares on the first 40 hex characters, matching the C ledger:
// entries are keyed by tree hash and an abbreviation must not alias.
func sameTree(a, b string) bool {
	if len(a) > 40 {
		a = a[:40]
	}
	if len(b) > 40 {
		b = b[:40]
	}
	return a == b
}

// handleVerifyState serves stage 6. An operation it does not know is refused
// rather than answered, so a newer caller against an older module fails closed.
func handleVerifyState(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	var decoded VerifyStateRequest
	if err := json.Unmarshal(request, &decoded); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	var response VerifyStateResponse
	switch decoded.Op {
	case "tree-hash":
		// The tree, not the commit: a squash-merge or a rebase that changes no
		// content keeps the verification that was recorded before it.
		response.Hash, response.OK = gitOutput(decoded.ProjectRoot, "rev-parse", "HEAD^{tree}")
	case "commit-hash":
		response.Hash, response.OK = gitOutput(decoded.ProjectRoot, "rev-parse", "HEAD")
	case "worktree-dirty":
		status, ok := gitOutput(decoded.ProjectRoot, "status", "--porcelain")
		response.OK, response.Dirty = ok, ok && status != ""
	case "state-read":
		response.Entries, response.OK = readEntries(decoded.ProjectRoot), true
	case "state-write":
		if decoded.Hash == "" {
			return nil, bus.ModuleStatusInvalidRequest
		}
		err := writeState(decoded.ProjectRoot, StateEntry{
			Timestamp:   decoded.Timestamp,
			Hash:        decoded.Hash,
			Failed:      decoded.Failed,
			Total:       decoded.Total,
			StepResults: decoded.StepResults,
		})
		response.OK = err == nil
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}

	encoded, err := json.Marshal(response)
	if err != nil || uint32(len(encoded)) > bus.ModuleMessageMaxBody {
		return nil, bus.ModuleStatusInternal
	}
	return encoded, bus.ModuleStatusOK
}
