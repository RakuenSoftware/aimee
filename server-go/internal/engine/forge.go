package engine

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os/exec"
	"strings"
)

type PullRequest struct {
	Ref  string
	URL  string
	Base string
	Head string
}
type CIState string

const (
	CIPending CIState = "pending"
	CIPassed  CIState = "passed"
	CIFailed  CIState = "failed"
)

type Forge interface {
	Open(context.Context, string, string, string, string, string) (PullRequest, error)
	CI(context.Context, string, string) (CIState, error)
	Merge(context.Context, string, string, string) error
}

type GHForge struct{}

func (GHForge) Mergeable(ctx context.Context, workdir, ref string) (bool, error) {
	out, err := ghText(ctx, workdir, "pr", "view", ref, "--json", "mergeStateStatus")
	if err != nil {
		return false, err
	}
	var view struct {
		State string `json:"mergeStateStatus"`
	}
	if err := json.Unmarshal([]byte(out), &view); err != nil {
		return false, err
	}
	switch strings.ToUpper(view.State) {
	case "CLEAN", "HAS_HOOKS", "UNSTABLE":
		return true, nil
	case "BLOCKED", "BEHIND", "UNKNOWN":
		return false, nil
	default:
		return false, nil
	}
}

func (GHForge) Open(ctx context.Context, repo, workdir, head, base, title string) (PullRequest, error) {
	if !managedBranch(head) {
		return PullRequest{}, fmt.Errorf("refuse push of unmanaged branch %q", head)
	}
	if base == "" || strings.HasPrefix(base, "-") {
		return PullRequest{}, fmt.Errorf("invalid PR base %q", base)
	}
	repoOrigin, err := gitText(ctx, repo, "remote", "get-url", "origin")
	if err != nil {
		return PullRequest{}, err
	}
	workOrigin, err := gitText(ctx, workdir, "remote", "get-url", "origin")
	if err != nil || repoOrigin != workOrigin {
		return PullRequest{}, errors.New("worktree origin does not match admitted repository")
	}
	if _, err := gitText(ctx, workdir, "push", "-u", "origin", head); err != nil {
		return PullRequest{}, err
	}
	var existing []struct {
		Number int    `json:"number"`
		URL    string `json:"url"`
		Base   string `json:"baseRefName"`
		Head   string `json:"headRefName"`
	}
	if out, err := ghText(ctx, workdir, "pr", "list", "--state", "open", "--head", head, "--base", base, "--json", "number,url,baseRefName,headRefName"); err == nil && json.Unmarshal([]byte(out), &existing) == nil && len(existing) > 0 {
		return PullRequest{Ref: existing[0].URL, URL: existing[0].URL, Base: existing[0].Base, Head: existing[0].Head}, nil
	}
	if title == "" {
		title = "aimee workflow " + head
	}
	url, err := ghText(ctx, workdir, "pr", "create", "--head", head, "--base", base, "--title", title, "--body", "Automated workflow output ready for human review.")
	if err != nil {
		return PullRequest{}, err
	}
	url = strings.TrimSpace(url)
	if url == "" {
		return PullRequest{}, errors.New("forge returned an empty PR reference")
	}
	return PullRequest{Ref: url, URL: url, Base: base, Head: head}, nil
}

func (GHForge) CI(ctx context.Context, workdir, ref string) (CIState, error) {
	out, err := ghText(ctx, workdir, "pr", "checks", ref, "--json", "state")
	if err != nil && strings.TrimSpace(out) == "" {
		return CIPending, err
	}
	var checks []struct {
		State string `json:"state"`
	}
	if decodeErr := json.Unmarshal([]byte(out), &checks); decodeErr != nil {
		return CIPending, decodeErr
	}
	if len(checks) == 0 {
		return CIPending, nil
	}
	pending := false
	for _, check := range checks {
		switch strings.ToUpper(check.State) {
		case "SUCCESS", "SKIPPED", "NEUTRAL":
		case "PENDING", "QUEUED", "IN_PROGRESS", "EXPECTED":
			pending = true
		default:
			return CIFailed, nil
		}
	}
	if pending {
		return CIPending, nil
	}
	return CIPassed, nil
}

func (GHForge) Merge(ctx context.Context, workdir, ref, requiredBase string) error {
	if !strings.HasPrefix(requiredBase, "aimee/feat/wi_") {
		return fmt.Errorf("refuse merge into unmanaged base %q", requiredBase)
	}
	out, err := ghText(ctx, workdir, "pr", "view", ref, "--json", "baseRefName,state,mergedAt,mergeStateStatus")
	if err != nil {
		return err
	}
	var view struct {
		Base       string  `json:"baseRefName"`
		State      string  `json:"state"`
		MergedAt   *string `json:"mergedAt"`
		MergeState string  `json:"mergeStateStatus"`
	}
	if err := json.Unmarshal([]byte(out), &view); err != nil {
		return err
	}
	if view.Base != requiredBase {
		return fmt.Errorf("refuse merge into %q; required feature base is %q", view.Base, requiredBase)
	}
	if view.MergedAt != nil || strings.EqualFold(view.State, "MERGED") {
		return nil
	}
	if strings.EqualFold(view.MergeState, "CONFLICTING") || strings.EqualFold(view.MergeState, "DIRTY") {
		return errors.New("pull request is not mergeable")
	}
	_, err = ghText(ctx, workdir, "pr", "merge", ref, "--merge")
	return err
}

func managedBranch(branch string) bool {
	return strings.HasPrefix(branch, "aimee/wi/wi_") || strings.HasPrefix(branch, "aimee/feat/wi_")
}

func ghText(ctx context.Context, workdir string, args ...string) (string, error) {
	cmd := exec.CommandContext(ctx, "gh", args...)
	cmd.Dir = workdir
	output, err := cmd.CombinedOutput()
	if err != nil {
		return string(output), fmt.Errorf("gh %s: %s", strings.Join(args, " "), strings.TrimSpace(string(output)))
	}
	return strings.TrimSpace(string(output)), nil
}
