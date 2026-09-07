package memory

import (
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

	for _, pattern := range sensitivePatterns {
		match := pattern.FindStringIndex(content)
		if match == nil {
			continue
		}
		redacted := content[:match[0]] + "[REDACTED]" + content[match[1]:]
		if capacity <= 0 || len(content[:match[0]])+len("[REDACTED]")+1 > capacity {
			result.SensitiveStatus = 2
		} else {
			if len(redacted) >= capacity {
				redacted = redacted[:capacity-1]
			}
			result.SensitiveStatus = 1
			result.Redacted = redacted
		}
		break
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
