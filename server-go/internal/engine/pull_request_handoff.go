package engine

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"path/filepath"
	"strings"
	"unicode"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/internal/db1"
)

const (
	maxPullRequestTitleRunes = 120
	maxRequestBodyBytes      = 16_000
	maxPlanBodyBytes         = 12_000
	maxPullRequestBodyBytes  = 55_000
)

// pullRequestTitle turns the admitted request into a reviewer-facing title.
// Packet JSON has a required summary; Markdown proposals conventionally have a
// first-level heading. Plain interactive requests fall back to their first
// substantive line. A machine work-item identifier is never a useful title.
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
	value = strings.TrimSpace(value)
	if len(value) <= maxBytes {
		return value
	}
	const suffix = "…\n\n_Content truncated; use the proposal path or workflow artifacts for the complete document._"
	cut := maxBytes - len(suffix)
	for cut > 0 && !utf8.RuneStart(value[cut]) {
		cut--
	}
	return strings.TrimSpace(value[:cut]) + suffix
}

func reviewProposalPath(item db1.WorkItem) string {
	path := filepath.ToSlash(strings.TrimSpace(item.SourcePath))
	if path == "" || filepath.IsAbs(path) || strings.HasPrefix(path, "../") {
		return ""
	}
	return strings.Replace(path, "/proposals/pending/", "/proposals/done/", 1)
}

func (r *NativeRunner) pullRequestSpec(ctx context.Context, req StepRequest, item db1.WorkItem,
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
	stat, err := gitText(ctx, workdir, "diff", "--stat", "--find-renames", baseRef+"...HEAD")
	if err != nil {
		return PullRequestSpec{}, fmt.Errorf("build pull request change summary: %w", err)
	}
	if stat == "" {
		return PullRequestSpec{}, errors.New("refuse pull request handoff with an empty diff")
	}

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
	var body strings.Builder
	body.WriteString("## Summary\n\n")
	body.WriteString(title)
	last, _ := utf8.DecodeLastRuneInString(title)
	if !strings.ContainsRune(".?!", last) {
		body.WriteString(".")
	}
	body.WriteString("\n\n")
	if draft {
		body.WriteString("This is the terminal handoff from the autonomous workflow. The complete admitted request and approved plan are included below so review does not depend on workflow-internal identifiers.\n")
	} else {
		body.WriteString("This implementation slice is part of the parent feature branch and remains subject to its configured CI and merge gates.\n")
	}

	body.WriteString("\n## Workflow context\n\n")
	if path := reviewProposalPath(item); path != "" {
		fmt.Fprintf(&body, "- Proposal: `%s`\n", path)
	}
	fmt.Fprintf(&body, "- Workflow: `%s`", item.WorkflowName)
	if item.WorkflowVersion != "" {
		fmt.Fprintf(&body, " (`%s`)", item.WorkflowVersion)
	}
	fmt.Fprintf(&body, "\n- Work item: `%s`\n- Branches: `%s` → `%s`\n", item.ID, head, base)

	body.WriteString("\n## Changes\n\n```text\n")
	body.WriteString(stat)
	body.WriteString("\n```\n")

	if draft {
		children, err := r.db.Children(ctx, item.ID)
		if err != nil {
			return PullRequestSpec{}, fmt.Errorf("load implementation slices for pull request: %w", err)
		}
		body.WriteString("\n## Automated verification\n\n")
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
		body.WriteString("\n## Integration boundary\n\n")
		body.WriteString("This slice may be merged automatically only into its parent `aimee/feat/...` branch after the configured review and CI gates pass. It must never target or merge the repository default branch.\n")
	}

	body.WriteString("\n<details>\n<summary>Original request</summary>\n\n")
	body.WriteString(boundedMarkdown(req.Proposal, maxRequestBodyBytes))
	body.WriteString("\n\n</details>\n")

	if draft {
		body.WriteString("\n<details>\n<summary>Approved implementation plan</summary>\n\n")
		body.WriteString(boundedMarkdown(string(approvedPlan), maxPlanBodyBytes))
		body.WriteString("\n\n</details>\n")
	}

	return PullRequestSpec{Title: title, Body: boundedMarkdown(body.String(), maxPullRequestBodyBytes), Draft: draft}, nil
}
