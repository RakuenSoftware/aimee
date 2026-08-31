package engine

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"unicode"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/wfe"
)

const (
	admittedRegressionSelectorVersion = "exact-path-v1"
	admittedRegressionMaxCandidates   = 128
)

type admittedRegressionEntry struct {
	Signature   string `json:"signature"`
	TaskName    string `json:"task_name"`
	Suite       string `json:"suite"`
	Reason      string `json:"reason"`
	MatchedPath string `json:"matched_path"`
}

type admittedRegressionManifest struct {
	RepositoryRevision  string                    `json:"repository_revision"`
	DiffDigest          string                    `json:"diff_digest"`
	SelectorVersion     string                    `json:"selector_version"`
	GraphSnapshotDigest string                    `json:"graph_snapshot_digest"`
	Selected            []admittedRegressionEntry `json:"selected"`
	Excluded            map[string]int            `json:"excluded_counts_by_reason"`
	IncompleteReasons   []string                  `json:"incomplete_reasons,omitempty"`
}

type admittedRegressionTask struct {
	Prompt     string `json:"prompt"`
	Provenance struct {
		Paths []string `json:"paths"`
	} `json:"provenance"`
}

type admittedRegressionVerifier interface {
	VerifyAdmitted(context.Context, string, []db1.EvalCandidate) error
}

func normalizeRepositoryPath(raw string) (string, bool) {
	raw = strings.TrimSpace(filepath.ToSlash(raw))
	if raw == "" || filepath.IsAbs(raw) || strings.ContainsRune(raw, 0) {
		return "", false
	}
	clean := filepath.ToSlash(filepath.Clean(raw))
	if clean == "." || clean == ".." || strings.HasPrefix(clean, "../") {
		return "", false
	}
	return clean, true
}

func pathTokenRune(r rune) bool {
	return unicode.IsLetter(r) || unicode.IsNumber(r) || strings.ContainsRune("_./\\:-", r)
}

func containsExactPath(text, path string) bool {
	for offset := 0; offset <= len(text)-len(path); {
		i := strings.Index(text[offset:], path)
		if i < 0 {
			return false
		}
		i += offset
		beforeOK := i == 0
		if !beforeOK {
			r, _ := utf8.DecodeLastRuneInString(text[:i])
			beforeOK = !pathTokenRune(r)
		}
		after := i + len(path)
		afterOK := after == len(text)
		if !afterOK {
			r, _ := utf8.DecodeRuneInString(text[after:])
			afterOK = !pathTokenRune(r)
		}
		if beforeOK && afterOK {
			return true
		}
		offset = i + 1
	}
	return false
}

func candidatePathMatch(candidate db1.EvalCandidate, changed []string) (reason, matched string) {
	var task admittedRegressionTask
	if json.Unmarshal([]byte(candidate.TaskJSON), &task) != nil {
		return "", ""
	}
	explicit := make(map[string]struct{})
	for _, raw := range task.Provenance.Paths {
		if path, ok := normalizeRepositoryPath(raw); ok {
			explicit[path] = struct{}{}
		}
	}
	for _, path := range changed {
		if _, ok := explicit[path]; ok {
			return "provenance_path", path
		}
	}
	for _, path := range changed {
		if candidate.OriginRef == path || strings.HasPrefix(candidate.OriginRef, path+":") {
			return "origin_ref", path
		}
	}
	for _, path := range changed {
		if containsExactPath(task.Prompt, path) {
			return "prompt_exact_path", path
		}
	}
	return "", ""
}

func selectAdmittedRegressions(changed []string, candidates []db1.EvalCandidate) (admittedRegressionManifest, []db1.EvalCandidate) {
	manifest := admittedRegressionManifest{
		SelectorVersion:     admittedRegressionSelectorVersion,
		GraphSnapshotDigest: "unavailable",
		Selected:            []admittedRegressionEntry{},
		Excluded:            map[string]int{},
		IncompleteReasons:   []string{"code_graph_selection_unavailable"},
	}
	sort.Strings(changed)
	selected := make([]db1.EvalCandidate, 0)
	for _, candidate := range candidates {
		if candidate.State != "admitted" {
			manifest.Excluded["state_not_admitted"]++
			continue
		}
		reason, path := candidatePathMatch(candidate, changed)
		if reason == "" {
			manifest.Excluded["no_exact_provenance_match"]++
			continue
		}
		selected = append(selected, candidate)
		manifest.Selected = append(manifest.Selected, admittedRegressionEntry{
			Signature: candidate.Signature, TaskName: candidate.TaskName, Suite: candidate.Suite,
			Reason: reason, MatchedPath: path,
		})
	}
	sort.SliceStable(selected, func(i, j int) bool { return selected[i].Signature < selected[j].Signature })
	sort.SliceStable(manifest.Selected, func(i, j int) bool {
		return manifest.Selected[i].Signature < manifest.Selected[j].Signature
	})
	return manifest, selected
}

func frozenChangedPaths(ctx context.Context, item db1.WorkItem, workdir string) ([]string, string, string, error) {
	base, err := frozenWorktreeBase(ctx, item, workdir)
	if err != nil {
		return nil, "", "", err
	}
	cmd := exec.CommandContext(ctx, "git", "-C", workdir, "diff", "--name-only", "--no-renames", "-z", base+"...HEAD")
	out, err := cmd.CombinedOutput()
	if err != nil {
		return nil, "", "", fmt.Errorf("list regression-selection paths: %w: %s", err, strings.TrimSpace(string(out)))
	}
	unique := make(map[string]struct{})
	for _, raw := range bytes.Split(out, []byte{0}) {
		if path, ok := normalizeRepositoryPath(string(raw)); ok {
			unique[path] = struct{}{}
		}
	}
	paths := make([]string, 0, len(unique))
	for path := range unique {
		paths = append(paths, path)
	}
	sort.Strings(paths)
	diff, err := frozenWorktreeDiff(ctx, item, workdir)
	if err != nil {
		return nil, "", "", err
	}
	revision, err := gitText(ctx, workdir, "rev-parse", "HEAD")
	if err != nil {
		return nil, "", "", err
	}
	return paths, strings.TrimSpace(revision), wfe.Hash([]byte(diff)), nil
}

func manifestDigest(manifest admittedRegressionManifest) string {
	raw, _ := json.Marshal(manifest)
	sum := sha256.Sum256(raw)
	return hex.EncodeToString(sum[:])
}

func manifestDetail(mode string, manifest admittedRegressionManifest) string {
	raw, _ := json.Marshal(manifest)
	return fmt.Sprintf("admitted_regressions=%s selected=%d manifest_digest=%s selection_manifest=%s",
		mode, len(manifest.Selected), manifestDigest(manifest), raw)
}

func (r *NativeRunner) verifyAdmittedRegressions(ctx context.Context, req StepRequest, workdir string) (string, error) {
	mode := strings.ToLower(strings.TrimSpace(paramString(req.Node, "admitted_regressions", "observe")))
	if mode == "off" {
		return "admitted_regressions=off", nil
	}
	if mode != "observe" && mode != "enforce" {
		return "", fmt.Errorf("admitted_regressions must be off, observe, or enforce")
	}
	changed, revision, diffDigest, err := frozenChangedPaths(ctx, req.WorkItem, workdir)
	if err != nil {
		if mode == "observe" {
			return "admitted_regressions=observe selection_incomplete=" + safeDiagnostic(err.Error()), nil
		}
		return "", err
	}
	candidates, err := r.db.EvalCandidates(ctx, "admitted", admittedRegressionMaxCandidates)
	if err != nil {
		if mode == "observe" {
			return "admitted_regressions=observe selection_incomplete=" + safeDiagnostic(err.Error()), nil
		}
		return "", fmt.Errorf("list admitted regressions: %w", err)
	}
	manifest, selected := selectAdmittedRegressions(changed, candidates)
	manifest.RepositoryRevision = revision
	manifest.DiffDigest = diffDigest
	if len(candidates) == admittedRegressionMaxCandidates {
		manifest.IncompleteReasons = append(manifest.IncompleteReasons, "candidate_scan_limit_reached")
	}
	digest := manifestDigest(manifest)
	detail := manifestDetail(mode, manifest)
	if mode == "enforce" && len(candidates) == admittedRegressionMaxCandidates {
		return "", fmt.Errorf("admitted regression manifest %s is incomplete: candidate scan limit reached", digest)
	}
	if mode == "observe" || len(selected) == 0 {
		return detail, nil
	}
	verifier, ok := r.verifier.(admittedRegressionVerifier)
	if !ok {
		return "", errors.New("admitted regression enforcement is enabled but the verifier cannot run eval tasks")
	}
	if err := verifier.VerifyAdmitted(ctx, workdir, selected); err != nil {
		return "", fmt.Errorf("admitted regression manifest %s failed: %w", digest, err)
	}
	return detail, nil
}

func safeSuiteName(raw string) (string, bool) {
	if raw == "" || len(raw) > 64 {
		return "", false
	}
	for _, r := range raw {
		if !(unicode.IsLetter(r) || unicode.IsNumber(r) || r == '-' || r == '_') {
			return "", false
		}
	}
	return raw, true
}

// VerifyAdmitted runs only task bytes that still match their admitted ledger
// row. A stale or replaced suite file cannot inherit an earlier admission.
func (v CommandVerifier) VerifyAdmitted(ctx context.Context, workdir string, candidates []db1.EvalCandidate) error {
	release, err := v.acquire(ctx)
	if err != nil {
		return err
	}
	defer release()
	parent, err := os.MkdirTemp("", "aimee-admitted-regressions-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(parent)
	groups := make(map[string][]db1.EvalCandidate)
	for _, candidate := range candidates {
		if candidate.State != "admitted" {
			return fmt.Errorf("candidate %s is no longer admitted", candidate.Signature)
		}
		suite, ok := safeSuiteName(candidate.Suite)
		if !ok {
			return fmt.Errorf("candidate %s has unsafe suite name", candidate.Signature)
		}
		if len(candidate.Signature) != 32 {
			return fmt.Errorf("candidate has malformed signature %q", candidate.Signature)
		}
		for _, c := range candidate.Signature {
			if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
				return fmt.Errorf("candidate has malformed signature %q", candidate.Signature)
			}
		}
		raw, err := os.ReadFile(candidate.AdmittedPath)
		if err != nil {
			return fmt.Errorf("read admitted task %s: %w", candidate.Signature, err)
		}
		if !bytes.Equal(bytes.TrimSpace(raw), bytes.TrimSpace([]byte(candidate.TaskJSON))) {
			return fmt.Errorf("admitted task %s no longer matches its ledger bytes", candidate.Signature)
		}
		groups[suite] = append(groups[suite], candidate)
	}
	suites := make([]string, 0, len(groups))
	for suite := range groups {
		suites = append(suites, suite)
	}
	sort.Strings(suites)
	for _, suite := range suites {
		dir := filepath.Join(parent, suite)
		if err := os.Mkdir(dir, 0o700); err != nil {
			return err
		}
		for _, candidate := range groups[suite] {
			path := filepath.Join(dir, candidate.Signature+".json")
			if err := os.WriteFile(path, append([]byte(candidate.TaskJSON), '\n'), 0o600); err != nil {
				return err
			}
		}
		command := v.RegressionCommand
		if len(command) == 0 {
			command = []string{"aimee", "--json", "eval", "run"}
		}
		args := append([]string(nil), command[1:]...)
		args = append(args, dir)
		cmd := exec.CommandContext(ctx, command[0], args...)
		cmd.Dir = workdir
		output, err := cmd.CombinedOutput()
		if err != nil {
			return fmt.Errorf("eval suite %s could not run: %w: %s", suite, err, strings.TrimSpace(string(output)))
		}
		var result struct {
			Status string `json:"status"`
			Passes int    `json:"passes"`
			Total  int    `json:"total"`
		}
		if json.Unmarshal(output, &result) != nil || result.Status != "ok" ||
			result.Total != len(groups[suite]) || result.Passes != result.Total {
			return fmt.Errorf("eval suite %s did not pass all selected tasks: %s", suite, strings.TrimSpace(string(output)))
		}
	}
	return nil
}
