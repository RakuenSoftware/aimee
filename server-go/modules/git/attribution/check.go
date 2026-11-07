// Package attribution enforces the same attribution policy for CI and forge merges.
// There are no commit, branch, PR, or integration exemptions.
package attribution

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/url"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
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
	Get        Get
	Repository string
	checked    map[int]bool
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

// ExecutionError means the checker could not evaluate policy. It is never a
// successful admission, and is distinct from a recorded content/provenance deny.
type ExecutionError struct {
	Code    string
	Message string
}

func (e *ExecutionError) Error() string { return e.Message }

// PushBase resolves a new branch against the default branch's immutable merge
// base. Ordinary pushes retain their exact previous head; no ref name reaches
// CheckRange. Initial default-branch creation has no trustworthy prior base.
func (c *Checker) PushBase(ctx context.Context, before, head, defaultBranch, ref string, created bool) (string, error) {
	if !ValidSHA(head) {
		return "", &ExecutionError{"invalid_event_range", "attribution requires a full nonzero push head"}
	}
	if ValidSHA(before) {
		return before, nil
	}
	if before != strings.Repeat("0", len(head)) || !created || defaultBranch == "" || !strings.HasPrefix(ref, "refs/heads/") || len(ref) == len("refs/heads/") || ref == "refs/heads/"+defaultBranch {
		return "", &ExecutionError{"invalid_event_range", "attribution push has no verifiable previous base"}
	}
	raw, err := c.Get(ctx, "commits/"+url.PathEscape(defaultBranch))
	if err != nil {
		return "", err
	}
	var current struct {
		SHA string `json:"sha"`
	}
	if json.Unmarshal(raw, &current) != nil || !ValidSHA(current.SHA) {
		return "", fmt.Errorf("incomplete default branch attribution metadata")
	}
	raw, err = c.Get(ctx, "compare/"+current.SHA+"..."+head)
	if err != nil {
		return "", err
	}
	var comparison struct {
		MergeBase struct {
			SHA string `json:"sha"`
		} `json:"merge_base_commit"`
	}
	if json.Unmarshal(raw, &comparison) != nil || !ValidSHA(comparison.MergeBase.SHA) {
		return "", fmt.Errorf("incomplete new branch attribution merge base")
	}
	return comparison.MergeBase.SHA, nil
}

const VerdictTitle = "Aimee attribution verdict v1"

// Verdict is recorded as an ordinary Actions annotation belonging to the exact
// check run. It records evaluation outcome, never an admission override.
type Verdict struct {
	Version    int    `json:"version"`
	Repository string `json:"repository"`
	HeadSHA    string `json:"head_sha"`
	BaseSHA    string `json:"base_sha"`
	RunID      int64  `json:"run_id"`
	RunAttempt int    `json:"run_attempt"`
	Event      string `json:"event"`
	Outcome    string `json:"outcome"`
	Code       string `json:"code"`
}

func decodeVerdict(raw string, v *Verdict) bool {
	// encoding/json otherwise accepts duplicate keys; an ambiguous historical
	// verdict must never override the recorded failed check.
	keys := map[string]bool{}
	scan := json.NewDecoder(strings.NewReader(raw))
	token, err := scan.Token()
	if err != nil || token != json.Delim('{') {
		return false
	}
	for scan.More() {
		key, err := scan.Token()
		if err != nil {
			return false
		}
		name, ok := key.(string)
		if !ok || keys[name] {
			return false
		}
		keys[name] = true
		var value json.RawMessage
		if scan.Decode(&value) != nil || bytes.Equal(bytes.TrimSpace(value), []byte("null")) {
			return false
		}
	}
	if _, err := scan.Token(); err != nil || len(keys) != 9 {
		return false
	}
	decoder := json.NewDecoder(bytes.NewBufferString(raw))
	decoder.DisallowUnknownFields()
	return decoder.Decode(v) == nil && decoder.Decode(new(any)) == io.EOF
}

// Missing, ambiguous or unrecognised evidence remains a historical denial.
func (c *Checker) historicalFailureOutsideRange(ctx context.Context, sha, base string, id int64) bool {
	return c.historicalFailure(ctx, sha, base, id, make(map[int64]bool))
}

func (c *Checker) historicalFailure(ctx context.Context, sha, base string, id int64, seen map[int64]bool) bool {
	if seen[id] || len(seen) >= 32 {
		return false
	}
	seen[id] = true

	if c.Repository == "" {
		return false
	}
	raw, err := c.Get(ctx, fmt.Sprintf("check-runs/%d", id))
	if err != nil {
		return false
	}
	var check struct {
		ID   int64  `json:"id"`
		Head string `json:"head_sha"`
		Name string `json:"name"`
		App  struct {
			Slug string `json:"slug"`
		} `json:"app"`
	}
	if json.Unmarshal(raw, &check) != nil || check.ID != id || check.Head != sha || check.Name != "no-coauthor-trailers" || check.App.Slug != "github-actions" {
		return false
	}
	raw, err = c.Get(ctx, fmt.Sprintf("actions/jobs/%d", id))
	if err != nil {
		return false
	}
	var job struct {
		ID         int64  `json:"id"`
		RunID      int64  `json:"run_id"`
		Attempt    int    `json:"run_attempt"`
		Head       string `json:"head_sha"`
		CheckURL   string `json:"check_run_url"`
		Name       string `json:"name"`
		Status     string `json:"status"`
		Conclusion string `json:"conclusion"`
	}
	if json.Unmarshal(raw, &job) != nil || job.ID != id || job.RunID <= 0 || job.Attempt <= 0 || job.Head != sha || job.Name != check.Name || job.Status != "completed" || job.Conclusion != "failure" || job.CheckURL != fmt.Sprintf("https://api.github.com/repos/%s/check-runs/%d", c.Repository, id) {
		return false
	}
	raw, err = c.Get(ctx, fmt.Sprintf("actions/runs/%d", job.RunID))
	if err != nil {
		return false
	}
	var run struct {
		ID         int64  `json:"id"`
		Head       string `json:"head_sha"`
		Event      string `json:"event"`
		Path       string `json:"path"`
		Repository struct {
			FullName string `json:"full_name"`
		} `json:"repository"`
	}
	if json.Unmarshal(raw, &run) != nil || run.ID != job.RunID || run.Head != sha || run.Repository.FullName != c.Repository || (run.Path != ".github/workflows/pr-attribution.yml" && run.Path != ".github/workflows/attribution.yml") {
		return false
	}
	var verdict *Verdict
	for page := 1; page <= 100; page++ {
		raw, err = c.Get(ctx, fmt.Sprintf("check-runs/%d/annotations?per_page=100&page=%d", id, page))
		if err != nil {
			return false
		}
		var annotations []struct {
			Title   string `json:"title"`
			Message string `json:"message"`
		}
		if json.Unmarshal(raw, &annotations) != nil || annotations == nil || len(annotations) > 100 {
			return false
		}
		for _, a := range annotations {
			if a.Title != VerdictTitle {
				continue
			}
			if verdict != nil {
				return false
			}
			var v Verdict
			if !decodeVerdict(a.Message, &v) || v.Version != 1 || v.Repository != c.Repository || v.HeadSHA != sha || v.RunID != job.RunID || v.RunAttempt != job.Attempt || v.Event != run.Event ||
				(v.BaseSHA != "" && !ValidSHA(v.BaseSHA) && v.BaseSHA != strings.Repeat("0", len(sha))) {
				return false
			}
			verdict = &v
		}
		if len(annotations) < 100 {
			break
		}
		if page == 100 {
			return false
		}
	}
	if verdict != nil {
		if verdict.Outcome == "execution_error" {
			return verdict.Code == "api_read" || verdict.Code == "invalid_event_range" && run.Event == "push"
		}
		if verdict.Outcome != "denied" || verdict.Code != "policy_denied_or_unknown" {
			return false
		}
	}
	// Legacy diagnostics remain immutable. Only an explicit input failure or
	// a fully identified rejected PR already contained in the accepted base
	// can be outside this range; unknown causes remain historical denials.
	raw, err = c.Get(ctx, fmt.Sprintf("actions/jobs/%d/logs", id))
	if err != nil {
		return false
	}
	failures := 0
	reason := ""
	for _, line := range strings.Split(string(raw), "\n") {
		if !strings.Contains(line, "attribution denied after") {
			continue
		}
		failures++
		match := regexp.MustCompile(`^\d{4}-\d\d-\d\dT[0-9:.]+Z attribution denied after ([0-9a-z.µ]+) : (.+?)\r?$`).FindStringSubmatch(line)
		if len(match) != 3 {
			return false
		}
		if elapsed, err := time.ParseDuration(match[1]); err != nil || elapsed < 0 {
			return false
		}
		reason = match[2]
	}
	if failures != 1 {
		return false
	}
	if verdict == nil && run.Event == "push" && reason == "attribution requires full nonzero base/head SHAs" {
		return true
	}
	// A later failed run can merely report an earlier failed check. Follow
	// that exact immutable evidence chain instead of creating a new subject.
	prior := regexp.MustCompile(`^commit ([0-9a-f]{40}(?:[0-9a-f]{24})?) has a recorded attribution rejection \(check ([1-9][0-9]*)\)$`).FindStringSubmatch(reason)
	if len(prior) == 3 {
		previous, err := strconv.ParseInt(prior[2], 10, 64)
		if err != nil || !ValidSHA(prior[1]) || prior[1] != sha && !c.ancestor(ctx, prior[1], sha) {
			return false
		}
		return c.historicalFailure(ctx, prior[1], base, previous, seen)
	}
	subject := regexp.MustCompile(`^PR #([1-9][0-9]*) carries GitHub app attribution: [a-zA-Z0-9_-]+$`).FindStringSubmatch(reason)
	if len(subject) != 2 {
		return false
	}
	number, err := strconv.Atoi(subject[1])
	if err != nil {
		return false
	}
	return c.rejectedSubjectInBase(ctx, number, reason, sha, base)

}

func (c *Checker) ancestor(ctx context.Context, ancestor, head string) bool {
	if !ValidSHA(ancestor) || !ValidSHA(head) {
		return false
	}
	raw, err := c.Get(ctx, "compare/"+ancestor+"..."+head)
	if err != nil {
		return false
	}
	var comparison struct {
		Status string `json:"status"`
		Base   struct {
			SHA string `json:"sha"`
		} `json:"base_commit"`
		MergeBase struct {
			SHA string `json:"sha"`
		} `json:"merge_base_commit"`
	}
	return json.Unmarshal(raw, &comparison) == nil && (comparison.Status == "ahead" || comparison.Status == "identical") && comparison.Base.SHA == ancestor && comparison.MergeBase.SHA == ancestor
}

func (c *Checker) rejectedSubjectInBase(ctx context.Context, number int, reason, oldHead, base string) bool {
	raw, err := c.Get(ctx, fmt.Sprintf("issues/%d", number))
	if err != nil {
		return false
	}
	if denied := CheckIssue(raw, number); denied == nil || denied.Error() != reason {
		return false
	}
	raw, err = c.Get(ctx, fmt.Sprintf("pulls/%d", number))
	if err != nil {
		return false
	}
	var pull struct {
		Number   int    `json:"number"`
		Merged   bool   `json:"merged"`
		State    string `json:"state"`
		MergeSHA string `json:"merge_commit_sha"`
		Head     struct {
			SHA string `json:"sha"`
		} `json:"head"`
	}
	if json.Unmarshal(raw, &pull) != nil || pull.Number != number || !pull.Merged || pull.State != "closed" || !ValidSHA(pull.MergeSHA) || !ValidSHA(pull.Head.SHA) {
		return false
	}
	// Both the original PR head and its actual merge result must be in the
	// current accepted base. The historical assessed head must contain that
	// result too. No historical content deny is relabelled as successful.
	return c.ancestor(ctx, pull.Head.SHA, pull.MergeSHA) && c.ancestor(ctx, pull.MergeSHA, oldHead) && c.ancestor(ctx, pull.Head.SHA, base) && c.ancestor(ctx, pull.MergeSHA, base)
}

// CheckRange checks every incoming commit and every PR GitHub associates with
// it, including closed/merged PRs. An empty child commit or replacement PR does
// not erase an ancestor's provenance. Pagination/schema failures are denials.
func (c *Checker) CheckRange(ctx context.Context, base, head string) error {
	if !ValidSHA(base) || !ValidSHA(head) {
		return &ExecutionError{"invalid_event_range", "attribution requires full nonzero base/head SHAs"}
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
			if err := c.history(ctx, entry.SHA, base); err != nil {
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
		if err := catalogFailure(errors); err != nil {
			return err
		}
		if len(issues) < 100 {
			return nil
		}
	}
	return fmt.Errorf("PR attribution catalog exceeds pagination limit")
}

// Concurrent evidence failures must not hide a content/unknown denial that
// was also observed. Only an execution-only result can be labelled as such.
func catalogFailure(failures <-chan error) error {
	var execution error
	for err := range failures {
		if err == nil {
			continue
		}
		var transient *ExecutionError
		if !errors.As(err, &transient) {
			return err
		}
		if execution == nil {
			execution = err
		}
	}
	return execution
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
func (c *Checker) history(ctx context.Context, sha, base string) error {
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
				var evidenceError error
				scoped := *c
				scoped.Get = func(ctx context.Context, path string) ([]byte, error) {
					raw, err := c.Get(ctx, path)
					if err != nil && evidenceError == nil {
						evidenceError = err
					}
					return raw, err
				}
				outside := scoped.historicalFailureOutsideRange(ctx, sha, base, run.ID)
				if evidenceError != nil {
					return fmt.Errorf("historical attribution evidence unavailable: %w", evidenceError)
				}
				if !outside {
					return fmt.Errorf("commit %s has a recorded attribution rejection (check %d)", sha, run.ID)
				}
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
