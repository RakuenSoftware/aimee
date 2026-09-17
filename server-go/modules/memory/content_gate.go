package memory

import (
	"github.com/JBailes/aimee/server-go/bus"
	"regexp"
)

var (
	sensitivePatterns = []*regexp.Regexp{
		regexp.MustCompile(`(?i)(api[_-]?key|token|secret|password|passwd|credential)[[:space:]]*[:=][[:space:]]*[^[:space:]]+`),
		regexp.MustCompile(`AKIA[0-9A-Z]{16}`),
		regexp.MustCompile(`(?i)-----BEGIN[[:space:]](RSA[[:space:]]|EC[[:space:]]|DSA[[:space:]])?PRIVATE[[:space:]]KEY-----`),
		regexp.MustCompile(`(?i)(social[_. ]security|ssn|date[_. ]of[_. ]birth|dob)[[:space:]]*[:=][[:space:]]*[^[:space:]]+`),
	}
	ephemeralPatterns = []*regexp.Regexp{
		regexp.MustCompile(`[0-9]+ (lines|bytes|files)`),
		regexp.MustCompile(`(?i)(just now|currently|right now|at the moment)`),
	}
	evidencePattern       = regexp.MustCompile(`(?i)(/[a-zA-Z0-9_./]+\.[a-z]+|` + "`[^`]+`" + `|https?://|error:|failed:|output:)`)
	privateKeyPattern     = regexp.MustCompile(`(?i)-----BEGIN.*PRIVATE KEY-----`)
	githubTokenPattern    = regexp.MustCompile(`ghp_[A-Za-z0-9]{10,}`)
	ssnPattern            = regexp.MustCompile(`[0-9]{3}-[0-9]{2}-[0-9]{4}`)
	emailPattern          = regexp.MustCompile(`[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}`)
	privateNetworkPattern = regexp.MustCompile(`(10\.[0-9]+\.[0-9]+\.[0-9]+|192\.168\.[0-9]+\.[0-9]+)`)
)

type contentGateResult struct {
	SensitiveStatus int
	Redacted        string
	Ephemeral       bool
	Evidence        bool
	Classification  string
}

func scanContent(content string, capacity int) contentGateResult {
	result := contentGateResult{Classification: "normal"}
	for _, pattern := range ephemeralPatterns {
		if pattern.MatchString(content) {
			result.Ephemeral = true
			break
		}
	}
	result.Evidence = evidencePattern.MatchString(content)

	// Redact every matching span. Returning a prefix after the first match could
	// transmit a second credential; truncating a replacement could corrupt text.
	redacted := content
	for _, pattern := range sensitivePatterns {
		redacted = pattern.ReplaceAllString(redacted, "[REDACTED]")
	}
	redacted = githubTokenPattern.ReplaceAllString(redacted, "[REDACTED]")
	redacted = ssnPattern.ReplaceAllString(redacted, "[REDACTED]")
	if redacted != content {
		result.SensitiveStatus = 2
		if capacity > 0 && len(redacted) < capacity {
			result.SensitiveStatus = 1
			result.Redacted = redacted
		}
	}
	// Removing a PEM header does not remove its body. Reject the entire input.
	if privateKeyPattern.MatchString(content) {
		result.SensitiveStatus = 2
		result.Redacted = ""
	}

	if privateKeyPattern.MatchString(content) || sensitivePatterns[1].MatchString(content) ||
		githubTokenPattern.MatchString(content) {
		result.Classification = "blocked"
		return result
	}
	if result.SensitiveStatus != 0 || ssnPattern.MatchString(content) {
		result.Classification = "restricted"
	} else if emailPattern.MatchString(content) || privateNetworkPattern.MatchString(content) {
		result.Classification = "sensitive"
	}
	return result
}

// Screening is stateless and shared by both placements. Callers consume this
// command through generic routing; they do not implement a native gate.
func handleScreenCommand(_ handlerOptions, _ bus.ModuleInvocation, _ string, args commandArgs) ([]byte, bus.ModuleStatus) {
	content, ok := args.stringValue("content")
	if !ok {
		return commandResult(commandError("invalid_argument", "content must be a string"))
	}
	capacity := args.integer("capacity", len(content)+32)
	if capacity < 0 || capacity > maxDataBody {
		return commandResult(commandError("invalid_argument", "invalid redaction capacity"))
	}
	result := scanContent(content, capacity)
	verdict := map[int]string{0: "allow", 1: "redact", 2: "reject"}[result.SensitiveStatus]
	return commandResult(map[string]any{"status": "ok", "verdict": verdict, "redacted": result.Redacted})
}
