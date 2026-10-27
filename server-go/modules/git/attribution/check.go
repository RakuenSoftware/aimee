// Package attribution enforces the same attribution policy for CI and forge merges.
// There are no commit, branch, PR, or integration exemptions.
package attribution

import (
	"context"
	"encoding/json"
	"fmt"
	"regexp"
	"strings"
	"sync"
)

// Get reads a repository-relative GitHub REST endpoint. Implementations must
// reject transport errors, non-2xx responses, and truncated response bodies.
type Get func(context.Context, string) ([]byte, error)

var shaPattern = regexp.MustCompile(`^[0-9a-f]{40}([0-9a-f]{24})?$`)

// A trailer or footer may follow any run of non-letter, non-digit characters --
// the standard footer is "🤖 Generated with [Claude Code](...)", and an emoji,
// bullet or markdown marker in front of it must not hide it. A session link is
// refused wherever it appears, not only at the start of a line.
var attributionText = regexp.MustCompile(`(?im)(^[^\p{L}\p{N}\n]*(co-authored-by\s*:|(claude|codex|chatgpt|copilot)-session\s*:|generated\s+(with|by)\s+\[?(claude|codex|chatgpt|copilot|github copilot)\b)|https?://\S*/(code/)?session[_/-])`)
var assistantName = regexp.MustCompile(`(?i)^(claude( code)?|codex|chatgpt|(github )?copilot|copilot-swe-agent|aimee-wfe)(\[bot\]|[- ]bot|[- ]agent)?$`)
var assistantEmail = regexp.MustCompile(`(?i)^((noreply|claude)@anthropic\.com|(codex|noreply)@openai\.com|wfe@aimee\.local|([0-9]+\+)?(claude|claude-code|codex|chatgpt|copilot|copilot-swe-agent)(\[bot\])?@users\.noreply\.github\.com)$`)

// ValidSHA accepts only full immutable Git object IDs, never ref names/options.
func ValidSHA(sha string) bool {
	return shaPattern.MatchString(sha) && strings.Trim(sha, "0") != ""
}

// CheckIssue requires the issue representation, since pull objects can omit the
// immutable performed_via_github_app badge. Missing fields never mean clean.
func CheckIssue(raw []byte, number int) error {
	var issue struct {
		Number int             `json:"number"`
		Title  *string         `json:"title"`
		Body   json.RawMessage `json:"body"`
		App    json.RawMessage `json:"performed_via_github_app"`
	}
	if json.Unmarshal(raw, &issue) != nil || issue.Number != number || number <= 0 || issue.Title == nil || len(issue.Body) == 0 || len(issue.App) == 0 {
		return fmt.Errorf("PR #%d: incomplete issue attribution metadata", number)
	}
	var body *string
	if json.Unmarshal(issue.Body, &body) != nil {
		return fmt.Errorf("PR #%d: malformed body", number)
	}
	if string(issue.App) != "null" {
		var app struct {
			ID   int64  `json:"id"`
			Slug string `json:"slug"`
		}
		if json.Unmarshal(issue.App, &app) != nil || app.ID <= 0 || app.Slug == "" {
			return fmt.Errorf("PR #%d: malformed app attribution", number)
		}
		// No app identity allowlist: renamed/new assistants and app display names
		// cannot evade an absolute restriction on opening-entry attribution.
		return fmt.Errorf("PR #%d carries GitHub app attribution: %s", number, app.Slug)
	}
	if attributionText.MatchString(*issue.Title) || body != nil && attributionText.MatchString(*body) {
		return fmt.Errorf("PR #%d carries attribution in its title/body", number)
	}
	return nil
}

type commit struct {
	SHA    string `json:"sha"`
	Commit *struct {
		Message   *string   `json:"message"`
		Author    *identity `json:"author"`
		Committer *identity `json:"committer"`
	} `json:"commit"`
}

type identity struct {
	Name  *string `json:"name"`
	Email *string `json:"email"`
}

func (c commit) check() error {
	if !ValidSHA(c.SHA) || c.Commit == nil || c.Commit.Message == nil {
		return fmt.Errorf("incomplete commit attribution metadata: %s", c.SHA)
	}
	if attributionText.MatchString(*c.Commit.Message) {
		return fmt.Errorf("commit %s carries an attribution trailer/footer", c.SHA)
	}
	for _, who := range []*identity{c.Commit.Author, c.Commit.Committer} {
		if who == nil || who.Name == nil || who.Email == nil || strings.TrimSpace(*who.Name) == "" || strings.TrimSpace(*who.Email) == "" {
			return fmt.Errorf("commit %s has incomplete raw author/committer metadata", c.SHA)
		}
		if assistantName.MatchString(strings.TrimSpace(*who.Name)) || assistantEmail.MatchString(strings.TrimSpace(*who.Email)) {
			return fmt.Errorf("commit %s carries an assistant author/committer identity", c.SHA)
		}
	}
	return nil
}

// Checker caches checked PRs only for this single admission attempt.
type Checker struct {
	Get     Get
	checked map[int]bool
}

func (c *Checker) issue(ctx context.Context, number int) error {
	if c.checked[number] {
		return nil
	}
	raw, err := c.Get(ctx, fmt.Sprintf("issues/%d", number))
	if err != nil {
		return err
	}
	if err := CheckIssue(raw, number); err != nil {
		return err
	}
	if c.checked == nil {
		c.checked = make(map[int]bool)
	}
	c.checked[number] = true
	return nil
}

// CheckRange checks every incoming commit and every PR GitHub associates with
// it, including closed/merged PRs. An empty child commit or replacement PR does
// not erase an ancestor's provenance. Pagination/schema failures are denials.
func (c *Checker) CheckRange(ctx context.Context, base, head string) error {
	if !ValidSHA(base) || !ValidSHA(head) {
		return fmt.Errorf("attribution requires full nonzero base/head SHAs")
	}
	seen := make(map[string]bool)
	total := -1
	for page := 1; page <= 10000; page++ {
		raw, err := c.Get(ctx, fmt.Sprintf("compare/%s...%s?per_page=100&page=%d", base, head, page))
		if err != nil {
			return err
		}
		var result struct {
			Total   *int     `json:"total_commits"`
			Commits []commit `json:"commits"`
		}
		if json.Unmarshal(raw, &result) != nil || result.Total == nil || *result.Total < 0 || result.Commits == nil {
			return fmt.Errorf("incomplete attribution comparison")
		}
		if total == -1 {
			total = *result.Total
		}
		if *result.Total != total || len(result.Commits) > 100 {
			return fmt.Errorf("inconsistent attribution comparison")
		}
		for _, entry := range result.Commits {
			if seen[entry.SHA] {
				return fmt.Errorf("duplicate commit in attribution comparison")
			}
			if err := entry.check(); err != nil {
				return err
			}
			seen[entry.SHA] = true
			if err := c.associated(ctx, entry.SHA); err != nil {
				return err
			}
			if err := c.history(ctx, entry.SHA); err != nil {
				return err
			}
		}
		if len(seen) == total {
			return c.catalog(ctx, seen)
		}
		if len(result.Commits) == 0 || len(seen) > total {
			return fmt.Errorf("truncated attribution comparison")
		}
	}
	return fmt.Errorf("attribution comparison exceeds pagination limit")
}

// GitHub's commit/pulls endpoint omits closed unmerged PRs and, once a
// commit reaches the default branch, can report only the PR that introduced it.
// Walk the issue catalog too: replacement/closed PRs must not erase provenance.
func (c *Checker) catalog(ctx context.Context, incoming map[string]bool) error {
	if len(incoming) == 0 {
		return nil
	}
	seen := make(map[int]bool)
	for page := 1; page <= 10000; page++ {
		raw, err := c.Get(ctx, fmt.Sprintf("issues?state=all&filter=all&sort=created&direction=asc&per_page=100&page=%d", page))
		if err != nil {
			return err
		}
		var issues []json.RawMessage
		if json.Unmarshal(raw, &issues) != nil || issues == nil || len(issues) > 100 {
			return fmt.Errorf("incomplete PR attribution catalog")
		}
		// Bound concurrent evidence reads so repository history does not turn a
		// merge admission into hundreds of serial network round trips.
		var pending sync.WaitGroup
		limit := make(chan struct{}, 8)
		errors := make(chan error, len(issues))
		for _, rawIssue := range issues {
			var issue struct {
				Number int             `json:"number"`
				Pull   json.RawMessage `json:"pull_request"`
			}
			if json.Unmarshal(rawIssue, &issue) != nil || issue.Number <= 0 || seen[issue.Number] {
				errors <- fmt.Errorf("invalid/duplicate attribution catalog entry")
				continue
			}
			seen[issue.Number] = true
			if len(issue.Pull) == 0 {
				continue
			} // ordinary issues are not PR provenance
			if string(issue.Pull) == "null" {
				errors <- fmt.Errorf("malformed PR attribution catalog entry")
				continue
			}
			if err := CheckIssue(rawIssue, issue.Number); err != nil {
				// A malformed catalog entry is not permission to skip this PR.
				// Examine all of its commits, with no exceptions for old SHAs.
				limit <- struct{}{}
				pending.Add(1)
				go func(number int, reason error) {
					defer pending.Done()
					defer func() { <-limit }()
					if matchErr := c.rejectedPull(ctx, number, incoming, reason); matchErr != nil {
						errors <- matchErr
					}
				}(issue.Number, err)
			}
		}
		pending.Wait()
		close(errors)
		for err := range errors {
			return err
		}
		if len(issues) < 100 {
			return nil
		}
	}
	return fmt.Errorf("PR attribution catalog exceeds pagination limit")
}

func (c *Checker) rejectedPull(ctx context.Context, number int, incoming map[string]bool, reason error) error {
	raw, err := c.Get(ctx, fmt.Sprintf("pulls/%d", number))
	if err != nil {
		return err
	}
	var pull struct {
		Number   int             `json:"number"`
		Commits  *int            `json:"commits"`
		MergeSHA json.RawMessage `json:"merge_commit_sha"`
		Head     struct {
			SHA string `json:"sha"`
		} `json:"head"`
		Base struct {
			SHA string `json:"sha"`
		} `json:"base"`
	}
	if json.Unmarshal(raw, &pull) != nil || pull.Number != number || pull.Commits == nil || *pull.Commits < 0 || !ValidSHA(pull.Base.SHA) || !ValidSHA(pull.Head.SHA) || len(pull.MergeSHA) == 0 {
		return fmt.Errorf("cannot completely verify rejected PR #%d commit inventory", number)
	}
	// Squashing/rebasing into an intermediate branch can replace the child
	// commit IDs. The original PR's recorded merge result remains provenance.
	var mergeSHA *string
	if json.Unmarshal(pull.MergeSHA, &mergeSHA) != nil || mergeSHA != nil && !ValidSHA(*mergeSHA) {
		return fmt.Errorf("invalid rejected PR merge result")
	}
	if incoming[pull.Head.SHA] || mergeSHA != nil && incoming[*mergeSHA] {
		return fmt.Errorf("incoming head/merge result belongs to rejected PR #%d: %w", number, reason)
	}
	seen := make(map[string]bool)
	// /pulls/{number}/commits stops at 250 entries even with pagination.
	// The immutable base/head comparison reports a total and supports all pages.
	for page := 1; page <= 10000; page++ {
		raw, err := c.Get(ctx, fmt.Sprintf("compare/%s...%s?per_page=100&page=%d", pull.Base.SHA, pull.Head.SHA, page))
		if err != nil {
			return err
		}
		var comparison struct {
			Total   *int `json:"total_commits"`
			Commits []struct {
				SHA string `json:"sha"`
			} `json:"commits"`
		}
		if json.Unmarshal(raw, &comparison) != nil || comparison.Total == nil || *comparison.Total != *pull.Commits || comparison.Commits == nil || len(comparison.Commits) > 100 {
			return fmt.Errorf("incomplete rejected PR #%d commit comparison", number)
		}
		for _, entry := range comparison.Commits {
			if !ValidSHA(entry.SHA) || seen[entry.SHA] {
				return fmt.Errorf("invalid/duplicate rejected PR commit")
			}
			seen[entry.SHA] = true
			if incoming[entry.SHA] {
				return fmt.Errorf("incoming commit %s belongs to rejected PR #%d: %w", entry.SHA, number, reason)
			}
		}
		if len(seen) == *pull.Commits {
			return nil
		}
		if len(comparison.Commits) == 0 || len(seen) > *pull.Commits {
			return fmt.Errorf("truncated rejected PR #%d commit inventory", number)
		}
	}
	return fmt.Errorf("rejected PR #%d exceeds comparison pagination limit", number)
}

// A new head must not launder an observed attribution failure on an ancestor.
// Success on a replacement PR or rerun does not erase that recorded failure.
func (c *Checker) history(ctx context.Context, sha string) error {
	seen := make(map[int64]bool)
	total := -1
	for page := 1; page <= 10000; page++ {
		raw, err := c.Get(ctx, fmt.Sprintf("commits/%s/check-runs?filter=all&per_page=100&page=%d", sha, page))
		if err != nil {
			return err
		}
		var runs struct {
			Total *int `json:"total_count"`
			Runs  []struct {
				ID         int64   `json:"id"`
				Name       string  `json:"name"`
				Status     string  `json:"status"`
				Conclusion *string `json:"conclusion"`
			} `json:"check_runs"`
		}
		if json.Unmarshal(raw, &runs) != nil || runs.Total == nil || *runs.Total < 0 || runs.Runs == nil || len(runs.Runs) > 100 {
			return fmt.Errorf("incomplete attribution check history")
		}
		if total == -1 {
			total = *runs.Total
		}
		if total != *runs.Total {
			return fmt.Errorf("attribution check history changed; retry verification")
		}
		for _, run := range runs.Runs {
			if run.ID <= 0 || run.Name == "" || seen[run.ID] {
				return fmt.Errorf("invalid/duplicate attribution check history")
			}
			seen[run.ID] = true
			if run.Name == "no-coauthor-trailers" && run.Conclusion != nil && (*run.Conclusion == "failure" || *run.Conclusion == "timed_out" || *run.Conclusion == "action_required" || *run.Conclusion == "stale") {
				return fmt.Errorf("commit %s has a recorded attribution rejection (check %d)", sha, run.ID)
			}
		}
		if len(seen) == total {
			return nil
		}
		if len(runs.Runs) == 0 || len(seen) > total {
			return fmt.Errorf("truncated attribution check history")
		}
	}
	return fmt.Errorf("attribution check history exceeds pagination limit")
}

func (c *Checker) associated(ctx context.Context, sha string) error {
	seen := make(map[int]bool)
	for page := 1; page <= 10000; page++ {
		raw, err := c.Get(ctx, fmt.Sprintf("commits/%s/pulls?per_page=100&page=%d", sha, page))
		if err != nil {
			return err
		}
		var pulls []struct {
			Number int `json:"number"`
		}
		if json.Unmarshal(raw, &pulls) != nil || pulls == nil || len(pulls) > 100 {
			return fmt.Errorf("incomplete associated PR metadata for %s", sha)
		}
		for _, pull := range pulls {
			if pull.Number <= 0 || seen[pull.Number] {
				return fmt.Errorf("invalid/duplicate associated PR metadata for %s", sha)
			}
			seen[pull.Number] = true
			if err := c.issue(ctx, pull.Number); err != nil {
				return err
			}
		}
		if len(pulls) < 100 {
			return nil
		}
	}
	return fmt.Errorf("associated PRs exceed pagination limit")
}

// CheckPull returns the verified immutable head for the merge's sha guard.
// Re-read the PR after traversal so a moved base/head cannot reuse the result.
func (c *Checker) CheckPull(ctx context.Context, number int, expected string) (string, error) {
	if number <= 0 {
		return "", fmt.Errorf("positive PR number is required")
	}
	c.checked = nil
	read := func() (string, string, error) {
		raw, err := c.Get(ctx, fmt.Sprintf("pulls/%d", number))
		if err != nil {
			return "", "", err
		}
		var pull struct {
			Number int `json:"number"`
			Head   struct {
				SHA string `json:"sha"`
			} `json:"head"`
			Base struct {
				SHA string `json:"sha"`
			} `json:"base"`
		}
		if json.Unmarshal(raw, &pull) != nil || pull.Number != number || !ValidSHA(pull.Head.SHA) || !ValidSHA(pull.Base.SHA) {
			return "", "", fmt.Errorf("incomplete PR base/head metadata")
		}
		return pull.Base.SHA, pull.Head.SHA, nil
	}
	base, head, err := read()
	if err != nil {
		return "", err
	}
	if expected != "" && head != expected {
		return "", fmt.Errorf("PR head moved since admission")
	}
	if err := c.issue(ctx, number); err != nil {
		return "", err
	}
	if err := c.CheckRange(ctx, base, head); err != nil {
		return "", err
	}
	latestBase, latestHead, err := read()
	if err != nil {
		return "", err
	}
	if latestBase != base || latestHead != head {
		return "", fmt.Errorf("PR base/head moved during attribution verification")
	}
	// Title/body can be edited while the history traversal is running. Do not
	// reuse the admission-start snapshot for the final merge decision.
	delete(c.checked, number)
	if err := c.issue(ctx, number); err != nil {
		return "", err
	}
	return head, nil
}
