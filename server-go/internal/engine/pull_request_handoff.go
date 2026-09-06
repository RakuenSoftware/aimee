package engine

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"unicode"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/internal/workflowstore"
)

const (
	maxPullRequestTitleRunes = 96
	maxRequestBodyBytes      = 12_000
	maxPlanBodyBytes         = 8_000
	maxPullRequestBodyBytes  = 55_000
	maxDiffHighlightBytes    = 64_000
	maxDiffHighlights        = 8
	maxChangedFilesBodyBytes = 8_000
	maxDiffstatBodyBytes     = 4_000
)

// pullRequestTitle turns the admitted request into a reviewer-facing title.
// Packet JSON has a required summary; formal Markdown proposals prefer the
// concrete Goal over their often-generic document heading. Plain interactive
// requests fall back to their first substantive line. A machine work-item
// identifier is never a useful title.
func pullRequestTitle(request string) (string, error) {
	var object map[string]any
	if json.Unmarshal([]byte(request), &object) == nil {
		for _, key := range []string{"summary", "title", "name"} {
			if value, ok := object[key].(string); ok {
				if title := normalizePullRequestTitle(value); title != "" {
					return title, nil
				}
			}
		}
	}
	if goal := markdownSection(request, "goal"); goal != "" {
		if title := normalizePullRequestTitle(firstProseClause(goal)); title != "" {
			return title, nil
		}
	}

	lines := strings.Split(strings.ReplaceAll(request, "\r\n", "\n"), "\n")
	inFrontmatter := false
	var fallback string
	for index, raw := range lines {
		line := strings.TrimSpace(raw)
		if line == "---" && (index == 0 || inFrontmatter) {
			inFrontmatter = !inFrontmatter
			continue
		}
		if inFrontmatter || line == "" {
			continue
		}
		if strings.HasPrefix(line, "# ") {
			if title := normalizePullRequestTitle(strings.TrimSpace(line[2:])); title != "" {
				return title, nil
			}
		}
		if fallback == "" && substantiveTitleLine(line) {
			fallback = line
		}
	}
	if title := normalizePullRequestTitle(fallback); title != "" {
		return title, nil
	}
	return "", errors.New("admitted request has no meaningful pull request title")
}

// firstProseClause turns a proposal's Goal paragraph into a compact review
// title. Goals frequently include a trailing purpose clause ("so operators can
// ..."); that explanation belongs in the PR body, not in a clipped title.
func firstProseClause(value string) string {
	var prose []string
	for _, raw := range strings.Split(strings.ReplaceAll(value, "\r\n", "\n"), "\n") {
		line := strings.TrimSpace(raw)
		if line == "" {
			if len(prose) > 0 {
				break
			}
			continue
		}
		if strings.HasPrefix(line, "#") || strings.HasPrefix(line, "|") ||
			strings.HasPrefix(line, "```") || strings.HasPrefix(line, "-") ||
			strings.HasPrefix(line, "*") {
			continue
		}
		prose = append(prose, line)
	}
	value = strings.Join(strings.Fields(strings.Join(prose, " ")), " ")
	for _, separator := range []string{" so that ", " so ", " in order to "} {
		if index := strings.Index(strings.ToLower(value), separator); index >= 24 {
			value = strings.TrimSpace(value[:index])
			break
		}
	}
	for index, r := range value {
		if strings.ContainsRune("?!", r) || (r == '.' && index+1 < len(value) && value[index+1] == ' ') {
			value = strings.TrimSpace(value[:index])
			break
		}
	}
	return strings.TrimRight(value, ".")
}

func markdownSection(document string, wanted ...string) string {
	wantedHeadings := make(map[string]bool, len(wanted))
	for _, heading := range wanted {
		wantedHeadings[strings.ToLower(strings.TrimSpace(heading))] = true
	}
	lines := strings.Split(strings.ReplaceAll(document, "\r\n", "\n"), "\n")
	start := -1
	for index, raw := range lines {
		line := strings.TrimSpace(raw)
		if !strings.HasPrefix(line, "## ") || strings.HasPrefix(line, "### ") {
			continue
		}
		heading := strings.ToLower(strings.TrimSpace(strings.Trim(line[3:], "#")))
		if start >= 0 {
			return strings.TrimSpace(strings.Join(lines[start:index], "\n"))
		}
		if wantedHeadings[heading] {
			start = index + 1
		}
	}
	if start >= 0 {
		return strings.TrimSpace(strings.Join(lines[start:], "\n"))
	}
	return ""
}

type pullRequestProposal struct {
	Goal  string
	Scope string
}

func pullRequestProposalDetails(request, title string) pullRequestProposal {
	var object map[string]any
	if json.Unmarshal([]byte(request), &object) == nil {
		details := pullRequestProposal{}
		for _, key := range []string{"summary", "title", "name"} {
			if value, ok := object[key].(string); ok && strings.TrimSpace(value) != "" {
				details.Goal = strings.TrimSpace(value)
				break
			}
		}
		if criteria, ok := object["acceptance_criteria"].([]any); ok {
			var bullets []string
			for _, raw := range criteria {
				if value, ok := raw.(string); ok && strings.TrimSpace(value) != "" {
					bullets = append(bullets, "- "+strings.TrimSpace(value))
				}
			}
			details.Scope = strings.Join(bullets, "\n")
		}
		if details.Goal == "" {
			details.Goal = title
		}
		return details
	}

	details := pullRequestProposal{
		Goal:  markdownSection(request, "goal", "summary", "problem"),
		Scope: markdownSection(request, "scope"),
	}
	if details.Scope == "" {
		details.Scope = markdownSection(request, "acceptance criteria", "acceptance")
	}
	if details.Goal == "" {
		details.Goal = title
	}
	return details
}

func substantiveTitleLine(line string) bool {
	if strings.HasPrefix(line, "#") || strings.HasPrefix(line, "|") ||
		strings.HasPrefix(line, "```") || strings.HasPrefix(line, ">") {
		return false
	}
	lower := strings.ToLower(strings.TrimSpace(strings.TrimLeft(line, "-* ")))
	lower = strings.ReplaceAll(lower, "**", "")
	for _, prefix := range []string{"state:", "author:", "date:", "parent:", "owns:",
		"charter roles:", "implementation dependency:"} {
		if strings.HasPrefix(lower, prefix) {
			return false
		}
	}
	return true
}

func normalizePullRequestTitle(value string) string {
	value = strings.TrimSpace(value)
	value = strings.TrimSpace(strings.Trim(value, "#*_`"))
	for _, prefix := range []string{"proposal:", "title:"} {
		if len(value) >= len(prefix) && strings.EqualFold(value[:len(prefix)], prefix) {
			value = strings.TrimSpace(value[len(prefix):])
		}
	}
	value = strings.Join(strings.Fields(value), " ")
	if value == "" || strings.HasPrefix(strings.ToLower(value), "wi_") {
		return ""
	}
	runes := []rune(value)
	if len(runes) > maxPullRequestTitleRunes {
		cut := maxPullRequestTitleRunes - 1
		for cut > maxPullRequestTitleRunes/2 && !unicode.IsSpace(runes[cut]) {
			cut--
		}
		runes = append([]rune(strings.TrimSpace(string(runes[:cut]))), '…')
	}
	if len(runes) > 0 {
		runes[0] = unicode.ToUpper(runes[0])
	}
	return string(runes)
}

func boundedMarkdown(value string, maxBytes int) string {
	const suffix = "…\n\n_Content truncated; use the proposal path or workflow artifacts for the complete document._"
	return boundedText(value, maxBytes, suffix)
}

func boundedText(value string, maxBytes int, suffix string) string {
	value = strings.TrimSpace(value)
	if len(value) <= maxBytes {
		return value
	}
	cut := maxBytes - len(suffix)
	for cut > 0 && !utf8.RuneStart(value[cut]) {
		cut--
	}
	return strings.TrimSpace(value[:cut]) + suffix
}

func credentialBearingLine(value string) bool {
	lower := strings.ToLower(value)
	normalized := strings.NewReplacer("-", "_", ".", "_", " ", "_").Replace(lower)
	markers := []string{
		"password", "passphrase", "private_key", "api_key", "secret", "token",
		"bearer", "credential", "database_url", "dsn",
	}
	credentialName := false
	for _, marker := range markers {
		if strings.Contains(normalized, marker) {
			credentialName = true
			break
		}
	}
	assignment := strings.Contains(value, "=") || strings.Contains(value, ":")
	if credentialName && assignment {
		return true
	}
	if strings.Contains(lower, "authorization") && strings.Contains(lower, "bearer ") {
		return true
	}
	for _, prefix := range []string{"sk-", "ghp_", "github_pat_", "xoxb-", "xoxp-"} {
		if index := strings.Index(lower, prefix); index >= 0 && len(value)-index >= len(prefix)+16 {
			return true
		}
	}
	if index := strings.Index(value, "AKIA"); index >= 0 && len(value)-index >= 20 {
		return true
	}
	return false
}

// redactPullRequestMarkdown prevents the handoff itself from becoming a second
// credential store. Proposals and plans are operator/agent-authored input, and
// representative diff lines come from arbitrary repository content; all three
// must be treated as untrusted before GitHub persists the generated body.
func redactPullRequestMarkdown(value string) string {
	lines := strings.Split(strings.ReplaceAll(value, "\r\n", "\n"), "\n")
	redacted := make([]string, 0, len(lines))
	inPrivateKey := false
	for _, line := range lines {
		upper := strings.ToUpper(line)
		if strings.Contains(upper, "-----BEGIN ") && strings.Contains(upper, "PRIVATE KEY-----") {
			if !inPrivateKey {
				redacted = append(redacted, "[REDACTED PRIVATE KEY — supply through Vault first boot]")
			}
			inPrivateKey = true
			continue
		}
		if inPrivateKey {
			if strings.Contains(upper, "-----END ") && strings.Contains(upper, "PRIVATE KEY-----") {
				inPrivateKey = false
			}
			continue
		}
		if credentialBearingLine(line) {
			redacted = append(redacted, "[REDACTED CREDENTIAL — supply through Vault first boot]")
			continue
		}
		redacted = append(redacted, line)
	}
	return strings.Join(redacted, "\n")
}

func reviewProposalPath(item workflowstore.WorkItem) string {
	path := filepath.ToSlash(strings.TrimSpace(item.SourcePath))
	if path == "" || filepath.IsAbs(path) || strings.HasPrefix(path, "../") {
		return ""
	}
	return strings.Replace(path, "/proposals/pending/", "/proposals/done/", 1)
}

type boundedOutput struct {
	bytes.Buffer
	limit int
}

func (output *boundedOutput) Write(value []byte) (int, error) {
	written := len(value)
	remaining := output.limit - output.Len()
	if remaining <= 0 {
		return written, nil
	}
	if len(value) > remaining {
		value = value[:remaining]
	}
	_, _ = output.Buffer.Write(value)
	return written, nil
}

// boundedGitDiff captures enough of a diff to show representative concrete
// edits without allowing a generated or binary-heavy patch to consume
// unbounded server memory while the handoff is being assembled.
func boundedGitDiff(ctx context.Context, workdir, revision string) (string, error) {
	command := exec.CommandContext(ctx, "git", "-C", workdir, "--no-pager", "diff", "--no-color",
		"--unified=0", "--find-renames", revision)
	output := boundedOutput{limit: maxDiffHighlightBytes}
	stderr := boundedOutput{limit: 4_000}
	command.Stdout = &output
	command.Stderr = &stderr
	if err := command.Run(); err != nil {
		return "", fmt.Errorf("git diff highlights: %s", strings.TrimSpace(stderr.String()))
	}
	return output.String(), nil
}

func parsePullRequestFiles(nameStatus string) []string {
	fields := strings.Split(nameStatus, "\x00")
	files := make([]string, 0, len(fields)/2)
	for index := 0; index < len(fields); {
		status := strings.TrimSpace(fields[index])
		index++
		if status == "" || index >= len(fields) {
			continue
		}
		from := fields[index]
		index++
		switch status[0] {
		case 'R', 'C':
			if index >= len(fields) {
				return files
			}
			to := fields[index]
			index++
			if status[0] == 'R' && strings.Contains(filepath.ToSlash(from), "/proposals/pending/") &&
				strings.Contains(filepath.ToSlash(to), "/proposals/done/") {
				unchanged := ""
				if status == "R100" {
					unchanged = " without changing its contents"
				}
				files = append(files, fmt.Sprintf("- Archived `%s` as `%s`%s.", from, to, unchanged))
			} else {
				verb := "Renamed"
				if status[0] == 'C' {
					verb = "Copied"
				}
				files = append(files, fmt.Sprintf("- %s `%s` to `%s`.", verb, from, to))
			}
		default:
			verb := map[byte]string{'A': "Added", 'D': "Removed", 'M': "Updated", 'T': "Changed"}[status[0]]
			if verb == "" {
				verb = "Changed"
			}
			files = append(files, fmt.Sprintf("- %s `%s`.", verb, from))
		}
	}
	return files
}

func markdownCode(value string) string {
	value = strings.Join(strings.Fields(strings.TrimSpace(value)), " ")
	longest := 0
	current := 0
	for _, character := range value {
		if character == '`' {
			current++
			if current > longest {
				longest = current
			}
		} else {
			current = 0
		}
	}
	delimiter := strings.Repeat("`", longest+1)
	return delimiter + value + delimiter
}

type diffHighlight struct {
	markdown string
	priority int
	order    int
}

func diffHighlightPriority(path string) int {
	path = filepath.ToSlash(path)
	if strings.HasPrefix(path, "docs/proposals/") {
		return 2
	}
	if strings.HasPrefix(path, "docs/") {
		return 1
	}
	return 0
}

func parseDiffHighlights(diff string) []string {
	var candidates []diffHighlight
	var path string
	var removed, added []string
	flush := func() {
		if path != "" && len(path) <= 300 && len(removed) == 1 && len(added) >= 1 && len(added) <= 4 {
			before := strings.TrimSpace(removed[0])
			after := strings.TrimSpace(strings.Join(added, " "))
			safe := !credentialBearingLine(before)
			for _, line := range added {
				safe = safe && !credentialBearingLine(line)
			}
			if before != "" && after != "" && len(before) <= 240 && len(after) <= 480 && safe {
				candidates = append(candidates, diffHighlight{
					markdown: fmt.Sprintf("- `%s`: changed %s to %s.", path,
						markdownCode(before), markdownCode(after)),
					priority: diffHighlightPriority(path),
					order:    len(candidates),
				})
			}
		}
		removed, added = nil, nil
	}
	for _, line := range strings.Split(strings.ReplaceAll(diff, "\r\n", "\n"), "\n") {
		switch {
		case strings.HasPrefix(line, "diff --git "):
			flush()
			path = ""
		case strings.HasPrefix(line, "+++ b/"):
			path = strings.TrimPrefix(line, "+++ b/")
		case strings.HasPrefix(line, "@@"):
			flush()
		case strings.HasPrefix(line, "-") && !strings.HasPrefix(line, "---"):
			removed = append(removed, strings.TrimPrefix(line, "-"))
		case strings.HasPrefix(line, "+") && !strings.HasPrefix(line, "+++"):
			added = append(added, strings.TrimPrefix(line, "+"))
		default:
			if len(removed) > 0 || len(added) > 0 {
				flush()
			}
		}
	}
	flush()
	sort.SliceStable(candidates, func(left, right int) bool {
		if candidates[left].priority != candidates[right].priority {
			return candidates[left].priority < candidates[right].priority
		}
		return candidates[left].order < candidates[right].order
	})
	count := len(candidates)
	if count > maxDiffHighlights {
		count = maxDiffHighlights
	}
	highlights := make([]string, 0, count)
	for _, candidate := range candidates[:count] {
		highlights = append(highlights, candidate.markdown)
	}
	return highlights
}

func (r *NativeRunner) pullRequestSpec(ctx context.Context, req StepRequest, item workflowstore.WorkItem,
	workdir, head, base string) (PullRequestSpec, error) {
	title := strings.TrimSpace(paramString(req.Node, "title", ""))
	if title == "" {
		var err error
		title, err = pullRequestTitle(req.Proposal)
		if err != nil {
			return PullRequestSpec{}, err
		}
	} else {
		title = normalizePullRequestTitle(title)
		if title == "" {
			return PullRequestSpec{}, errors.New("configured pull request title is not meaningful")
		}
	}

	baseRef := "refs/remotes/origin/" + base
	if _, err := gitText(ctx, workdir, "rev-parse", "--verify", baseRef); err != nil {
		baseRef = base
	}
	revision := baseRef + "...HEAD"
	stat, err := gitText(ctx, workdir, "diff", "--stat", "--find-renames", revision)
	if err != nil {
		return PullRequestSpec{}, fmt.Errorf("build pull request change summary: %w", err)
	}
	if stat == "" {
		return PullRequestSpec{}, errors.New("refuse pull request handoff with an empty diff")
	}
	nameStatus, err := gitText(ctx, workdir, "diff", "--name-status", "--find-renames", "-z", revision)
	if err != nil {
		return PullRequestSpec{}, fmt.Errorf("build pull request file summary: %w", err)
	}
	files := parsePullRequestFiles(nameStatus)
	if len(files) == 0 {
		return PullRequestSpec{}, errors.New("refuse pull request handoff without changed-file details")
	}
	diff, err := boundedGitDiff(ctx, workdir, revision)
	if err != nil {
		return PullRequestSpec{}, err
	}
	highlights := parseDiffHighlights(diff)

	draft := item.ParentID == ""
	var approvedPlan []byte
	if draft {
		approvedPlan, err = r.artifacts.Plan(item.ID)
		if err != nil {
			return PullRequestSpec{}, fmt.Errorf("load approved plan for pull request: %w", err)
		}
		if strings.TrimSpace(string(approvedPlan)) == "" {
			return PullRequestSpec{}, errors.New("refuse final pull request handoff without an approved plan")
		}
	}
	details := pullRequestProposalDetails(req.Proposal, title)
	var body strings.Builder
	if draft {
		body.WriteString("## What this proposal does\n\n")
	} else {
		body.WriteString("## Slice outcome\n\n")
	}
	body.WriteString(boundedMarkdown(redactPullRequestMarkdown(details.Goal), 3_000))
	body.WriteString("\n\n## What changed\n\n")
	if strings.TrimSpace(details.Scope) != "" {
		body.WriteString(boundedMarkdown(redactPullRequestMarkdown(details.Scope), 8_000))
		body.WriteString("\n\n")
	}
	body.WriteString("### Files in this PR\n\n")
	body.WriteString(boundedText(strings.Join(files, "\n"), maxChangedFilesBodyBytes,
		"…\n\n_Changed-file list truncated; use the PR Files tab for the complete list._"))
	body.WriteString("\n")
	if len(highlights) > 0 {
		body.WriteString("\n### Representative concrete edits\n\n")
		body.WriteString(strings.Join(highlights, "\n"))
		body.WriteString("\n")
	}
	body.WriteString("\n<details>\n<summary>Diffstat</summary>\n\n```text\n")
	body.WriteString(boundedText(stat, maxDiffstatBodyBytes, "…\n(diffstat truncated; see the PR Files tab)"))
	body.WriteString("\n```\n\n</details>\n")

	if draft {
		children, err := r.db.Children(ctx, item.ID)
		if err != nil {
			return PullRequestSpec{}, fmt.Errorf("load implementation slices for pull request: %w", err)
		}
		body.WriteString("\n## Verification\n\n")
		fmt.Fprintf(&body, "- Approved implementation plan completed.\n- %d implementation slice(s) completed their review, CI, and feature-branch integration gates.\n", len(children))
		for _, child := range children {
			if strings.TrimSpace(child.PRRef) != "" {
				label := child.ID
				if proposal, proposalErr := r.artifacts.Proposal(child.ID); proposalErr == nil {
					if meaningful, titleErr := pullRequestTitle(string(proposal)); titleErr == nil {
						label = meaningful
					}
				}
				if strings.HasPrefix(child.PRRef, "https://") {
					fmt.Fprintf(&body, "  - [%s](%s)\n", label, child.PRRef)
				} else {
					fmt.Fprintf(&body, "  - %s: %s\n", label, child.PRRef)
				}
			}
		}
		body.WriteString("- The assembled diff passed the acceptance roundtable.\n- The documentation diff passed the documentation roundtable.\n- Final-branch CI runs on this PR and must be checked by the human reviewer.\n")

		body.WriteString("\n## Human review boundary\n\n")
		body.WriteString("This PR is intentionally a draft. The autonomous workflow stops here and must not mark it ready, approve it, or merge it. A human must review the request, diff, and final CI, then explicitly mark the PR ready and decide whether to merge.\n")
	} else {
		body.WriteString("\n## Verification and integration boundary\n\n")
		body.WriteString("This slice may be merged automatically only into its parent `aimee/feat/...` branch after the configured review and CI gates pass. It must never target or merge the repository default branch.\n")
	}

	body.WriteString("\n<details>\n<summary>Workflow trace</summary>\n\n")
	if path := reviewProposalPath(item); path != "" {
		fmt.Fprintf(&body, "- Proposal: `%s`\n", path)
	}
	fmt.Fprintf(&body, "- Workflow: `%s`", item.WorkflowName)
	if item.WorkflowVersion != "" {
		fmt.Fprintf(&body, " (`%s`)", item.WorkflowVersion)
	}
	fmt.Fprintf(&body, "\n- Work item: `%s`\n- Branches: `%s` → `%s`\n\n</details>\n", item.ID, head, base)

	body.WriteString("\n<details>\n<summary>Original request</summary>\n\n")
	body.WriteString(boundedMarkdown(redactPullRequestMarkdown(req.Proposal), maxRequestBodyBytes))
	body.WriteString("\n\n</details>\n")

	if draft {
		body.WriteString("\n<details>\n<summary>Approved implementation plan</summary>\n\n")
		body.WriteString(boundedMarkdown(redactPullRequestMarkdown(string(approvedPlan)), maxPlanBodyBytes))
		body.WriteString("\n\n</details>\n")
	}

	return PullRequestSpec{Title: title, Body: boundedMarkdown(body.String(), maxPullRequestBodyBytes), Draft: draft}, nil
}
