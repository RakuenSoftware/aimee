package delegates

import (
	"encoding/binary"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

// Which files a brief actually names as targets.
//
// Used to notice drift: a delegate that was told to edit src/foo.c and did not.
// Every rule below is deliberately conservative, because the two failure modes
// are not equally costly. Missing a path means one drift warning that was not
// raised. Inventing one means telling an operator a successful run failed, which
// trains them to ignore the check.

const (
	StagePaths uint32 = 4
	EventPaths uint32 = 6660

	pathsRequestMagic  uint32 = 0x54415044 /* "DPAT" */
	pathsResponseMagic uint32 = 0x53415044 /* "DPAS" */
	pathsHeaderLen            = 12
	pathsRespHeaderLen        = 8
	pathsPromptMax            = 1 << 20
	// Mirrors DELEGATE_DRIFT_MAX_PATHS / DELEGATE_DRIFT_PATH_MAX.
	pathsMax    = 16
	pathMaxLen  = 256
	pathsExtMax = 16
)

var driftSrcExts = map[string]bool{
	".c": true, ".h": true, ".cpp": true, ".cc": true, ".cxx": true, ".py": true,
	".js": true, ".ts": true, ".go": true, ".rs": true, ".java": true, ".rb": true,
	".sh": true, ".yaml": true, ".yml": true, ".json": true, ".toml": true,
	".md": true, ".sql": true, ".mk": true, ".conf": true, ".cfg": true,
}

// Everything after one of these markers is appended evidence, not the brief. A
// path mentioned only in a diff or a prompt file is not something the delegate
// was asked to produce.
var surfaceMarkers = []string{
	"\n\n# Prompt File\n",
	"\n# Prompt File\n",
	"\n\n---\n## Parent Worktree Diff Evidence\n",
	"\n---\n## Parent Worktree Diff Evidence\n",
	"\n\n---\n## Validation Evidence Bundle\n",
	"\n---\n## Validation Evidence Bundle\n",
}

var negations = []string{
	"do not ", "don't ", "does not ", "doesn't ", "must not ", "mustn't ",
	"cannot ", "can't ", "should not ", "shouldn't ", "will not ", "won't ",
	"skip ", "off-limits", "off limits", "no touch", "do not touch", "not touch",
	"leave alone", "ignore ",
}

func isPathChar(c byte) bool {
	return c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' ||
		c == '.' || c == '/' || c == '_' || c == '-'
}

// taskSurfaceLen bounds the scan to the brief itself.
func taskSurfaceLen(prompt string) int {
	limit := len(prompt)
	for _, marker := range surfaceMarkers {
		if at := strings.Index(prompt, marker); at >= 0 && at < limit {
			limit = at
		}
	}
	return limit
}

// isNegated reports whether the brief tells the delegate to leave this path
// alone: "do NOT touch src/x.c", "skip src/y.c". Scans back to the start of the
// line, because a negation binds to the sentence it appears in.
func isNegated(prompt string, pathStart int) bool {
	lineStart := strings.LastIndexByte(prompt[:pathStart], '\n') + 1
	prefix := strings.ToLower(prompt[lineStart:pathStart])
	for _, n := range negations {
		if strings.Contains(prefix, n) {
			return true
		}
	}
	return false
}

// isJSONExampleValue reports whether the token is a quoted path sitting in a
// JSON value position, i.e. an illustrative schema rather than a target. Prose
// like `create "src/foo.c"` survives, because the character before the opening
// quote is then a word or space rather than a structural delimiter.
func isJSONExampleValue(prompt string, start, end int) bool {
	if start == 0 || prompt[start-1] != '"' {
		return false
	}
	if end >= len(prompt) || prompt[end] != '"' {
		return false
	}
	b := start - 2
	for b > 0 && (prompt[b] == ' ' || prompt[b] == '\t' || prompt[b] == '\n' || prompt[b] == '\r') {
		b--
	}
	if b < 0 {
		return false
	}
	return prompt[b] == '[' || prompt[b] == ',' || prompt[b] == ':'
}

// ExtractNamedPaths returns the repo paths a brief names as targets, in order,
// without duplicates and capped at max.
func ExtractNamedPaths(prompt string, max int) []string {
	if prompt == "" || max <= 0 {
		return nil
	}
	end := taskSurfaceLen(prompt)
	var out []string

	for i := 0; i < end && len(out) < max; {
		if !isPathChar(prompt[i]) {
			i++
			continue
		}
		start := i
		for i < end && isPathChar(prompt[i]) {
			i++
		}
		tokEnd := i
		token := prompt[start:tokEnd]
		// A trailing dot is sentence punctuation, not part of the name.
		token = strings.TrimRight(token, ".")
		if token == "" {
			continue
		}
		// `#include <sys/stat.h>` names a system header, not a repo file.
		if start > 0 && prompt[start-1] == '<' {
			continue
		}
		if isNegated(prompt, start) {
			continue
		}
		if isJSONExampleValue(prompt, start, tokEnd) {
			continue
		}
		// Strip a diff's a/ or b/ prefix.
		if len(token) > 2 && (token[0] == 'a' || token[0] == 'b') && token[1] == '/' {
			token = token[2:]
		}
		// A path has a directory separator; a bare word is prose.
		if !strings.Contains(token, "/") {
			continue
		}
		dot := strings.LastIndexByte(token, '.')
		if dot < 0 {
			continue
		}
		ext := token[dot:]
		if len(ext) >= pathsExtMax || !driftSrcExts[ext] {
			continue
		}
		if len(token) >= pathMaxLen {
			continue
		}
		duplicate := false
		for _, seen := range out {
			if seen == token {
				duplicate = true
				break
			}
		}
		if !duplicate {
			out = append(out, token)
		}
	}
	return out
}

func handlePaths(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < pathsHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != pathsRequestMagic ||
		request[4] != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	max := int(request[5])
	length := int(binary.LittleEndian.Uint32(request[8:12]))
	if max == 0 || max > pathsMax || length > pathsPromptMax ||
		len(request) != pathsHeaderLen+length {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	found := ExtractNamedPaths(string(request[pathsHeaderLen:]), max)
	response := make([]byte, pathsRespHeaderLen, pathsRespHeaderLen+len(found)*32)
	binary.LittleEndian.PutUint32(response[0:4], pathsResponseMagic)
	binary.LittleEndian.PutUint32(response[4:8], uint32(len(found)))
	// Each path is length-prefixed rather than NUL-terminated: the caller reads
	// a bounded count and never scans for a terminator it has to trust.
	for _, p := range found {
		var n [2]byte
		binary.LittleEndian.PutUint16(n[:], uint16(len(p)))
		response = append(response, n[0], n[1])
		response = append(response, p...)
	}
	return response, bus.ModuleStatusOK
}
