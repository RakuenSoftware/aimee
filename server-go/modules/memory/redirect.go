package memory

import (
	"path/filepath"
	"strings"
)

const (
	redirectAllow  = "allow"
	redirectStore  = "redirect"
	redirectReject = "reject"
)

type redirectDecision struct {
	Verdict string
	Name    string
	Reason  string
}

func redirectSurface(client, projectsRoot, memorySegment string) (string, string, bool) {
	if client == "" {
		return "", "", false
	}
	if projectsRoot == "" || memorySegment == "" {
		if client != "claude" {
			return "", "", false
		}
		projectsRoot, memorySegment = ".claude/projects", "memory"
	}
	if filepath.IsAbs(projectsRoot) || filepath.IsAbs(memorySegment) ||
		!relativeSurfacePath(projectsRoot) || !relativeSurfacePath(memorySegment) {
		return "", "", false
	}
	return filepath.Clean(projectsRoot), filepath.Clean(memorySegment), true
}

func relativeSurfacePath(value string) bool {
	normalized := filepath.ToSlash(value)
	if value == "" || strings.HasSuffix(normalized, "/") || strings.Contains(normalized, "//") {
		return false
	}
	for _, part := range strings.Split(normalized, "/") {
		if part == "" || part == "." || part == ".." {
			return false
		}
	}
	return true
}

func classifyRedirect(client, tool, path, home, projectsRoot, memorySegment string) redirectDecision {
	root, segment, ok := redirectSurface(client, projectsRoot, memorySegment)
	if !ok || home == "" || path == "" || (tool != "Write" && tool != "Edit" && tool != "MultiEdit") {
		return redirectDecision{Verdict: redirectAllow}
	}
	prefix := filepath.Clean(home) + string(filepath.Separator) + root + string(filepath.Separator)
	rawPath := filepath.ToSlash(path)
	rawPrefix := filepath.ToSlash(prefix)
	if strings.HasPrefix(rawPath, rawPrefix) {
		for _, part := range strings.Split(strings.TrimPrefix(rawPath, rawPrefix), "/") {
			if part == ".." {
				return redirectDecision{Verdict: redirectReject, Reason: "Invalid memory path (no '..' segments)."}
			}
		}
	}
	cleanPath := filepath.Clean(path)
	if !strings.HasPrefix(cleanPath, prefix) || !strings.EqualFold(filepath.Ext(cleanPath), ".md") {
		return redirectDecision{Verdict: redirectAllow}
	}
	rel, err := filepath.Rel(prefix, cleanPath)
	if err != nil || rel == "." || rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		return redirectDecision{Verdict: redirectAllow}
	}
	rel = filepath.ToSlash(rel)
	marker := "/" + filepath.ToSlash(segment) + "/"
	markerAt := strings.Index("/"+rel, marker)
	if markerAt <= 0 {
		return redirectDecision{Verdict: redirectAllow}
	}
	nameParts := strings.Split(("/" + rel)[markerAt+len(marker):], "/")
	for _, part := range nameParts {
		if part == "" || part == "." || part == ".." {
			return redirectDecision{Verdict: redirectReject, Reason: "Invalid memory path (no '..' segments)."}
		}
	}
	if strings.EqualFold(nameParts[len(nameParts)-1], "MEMORY.md") {
		return redirectDecision{Verdict: redirectReject, Reason: "MEMORY.md is auto-rendered from your memory entries; to add or change one, Write a file under memory/<name>.md."}
	}
	if tool != "Write" {
		return redirectDecision{Verdict: redirectReject, Reason: "Memory files are managed by aimee; use Write to replace the whole file rather than Edit."}
	}
	nameParts[len(nameParts)-1] = strings.TrimSuffix(nameParts[len(nameParts)-1], filepath.Ext(nameParts[len(nameParts)-1]))
	return redirectDecision{Verdict: redirectStore, Name: strings.Join(nameParts, "/")}
}

func regionHasWriteOperator(region string) bool {
	single, double, escaped := false, false, false
	for _, r := range region {
		if escaped {
			escaped = false
			continue
		}
		if r == '\\' {
			escaped = true
			continue
		}
		if r == '\'' && !double {
			single = !single
		} else if r == '"' && !single {
			double = !double
		} else if r == '>' && !single && !double {
			return true
		}
	}
	for _, keyword := range []string{"tee", "sed -i", "dd of=", "truncate ", "cp ", "mv ", "install ", "perl -i", "perl -pi", "patch "} {
		if strings.Contains(region, keyword) {
			return true
		}
	}
	return false
}

func bashTargetsMemory(client, command, home, projectsRoot, memorySegment string) bool {
	root, segment, ok := redirectSurface(client, projectsRoot, memorySegment)
	if !ok || command == "" || home == "" {
		return false
	}
	prefix := filepath.Clean(home) + string(filepath.Separator) + root + string(filepath.Separator)
	for from := 0; from < len(command); {
		i := strings.Index(command[from:], prefix)
		if i < 0 {
			return false
		}
		start := from + i
		end := start
		for end < len(command) && !strings.ContainsRune(" \t;|&\n\"'`)<>", rune(command[end])) {
			end++
		}
		token := filepath.ToSlash(command[start:end])
		if strings.Contains(token, "/"+segment+"/") && strings.EqualFold(filepath.Ext(token), ".md") {
			commandStart := start
			for commandStart > 0 && !strings.ContainsRune(";|&\n", rune(command[commandStart-1])) {
				commandStart--
			}
			if regionHasWriteOperator(command[commandStart:start]) {
				return true
			}
		}
		if end <= start {
			from = start + 1
		} else {
			from = end
		}
	}
	return false
}
