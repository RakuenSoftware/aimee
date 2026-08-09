package plane

import (
	"regexp"
	"strings"
)

var diagnosticRedactions = []struct {
	pattern     *regexp.Regexp
	replacement string
}{
	{regexp.MustCompile(`(?i)(authorization\s*:\s*(?:bearer|basic)\s+)[^\s,;]+`), `${1}[REDACTED]`},
	{regexp.MustCompile(`(?i)(cookie\s*:\s*)[^\r\n]+`), `${1}[REDACTED]`},
	{regexp.MustCompile(`(?i)([a-z][a-z0-9+.-]*://)[^/@\s]+@`), `${1}[REDACTED]@`},
	{regexp.MustCompile(`(?i)("?(?:api[_-]?key|access[_-]?token|token|password|secret)"?\s*[:=]\s*)"(?:\\.|[^"\\])*"`), `${1}"[REDACTED]"`},
	{regexp.MustCompile(`(?i)((?:api[_-]?key|access[_-]?token|token|password|secret)\s*[:=]\s*)'(?:\\.|[^'\\])*'`), `${1}'[REDACTED]'`},
	{regexp.MustCompile(`(?im)("?(?:api[_-]?key|access[_-]?token|token|password|secret)"?\s*[:=]\s*)"(?:\\.|[^"\\\r\n])*(?:\\)?$`), `${1}"[REDACTED]`},
	{regexp.MustCompile(`(?im)((?:api[_-]?key|access[_-]?token|token|password|secret)\s*[:=]\s*)'(?:\\.|[^'\\\r\n])*(?:\\)?$`), `${1}'[REDACTED]`},
	{regexp.MustCompile(`(?i)((?:api[_-]?key|access[_-]?token|token|password|secret)["']?\s*[:=]\s*["']?)[^\s,"';}]+`), `${1}[REDACTED]`},
	{regexp.MustCompile(`\bAKIA[0-9A-Z]{16}\b`), `[REDACTED_AWS_ACCESS_KEY]`},
	{regexp.MustCompile(`\beyJ[A-Za-z0-9_-]*\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\b`), `[REDACTED_JWT]`},
	{regexp.MustCompile(`(?s)-----BEGIN [^-\r\n]*PRIVATE KEY-----.*?-----END [^-\r\n]*PRIVATE KEY-----`), `[REDACTED_PRIVATE_KEY]`},
}

// safeDiagnostic preserves the complete diagnostic—including arbitrarily long
// tool output—while removing common credential forms before durable storage.
// Artifact and review payloads are never routed through this function.
func SafeDiagnostic(detail string) string {
	for _, redaction := range diagnosticRedactions {
		detail = redaction.pattern.ReplaceAllString(detail, redaction.replacement)
	}
	return detail
}

// isCapacityBackpressure reports whether err is the provider refusing more work
// right now rather than the delegate failing. Both forms carry their own
// retry-after, so re-dispatching after a wait is the correct recovery and the
// attempt must not count against the no-progress bound.
func IsCapacityBackpressure(err error) bool {
	if err == nil {
		return false
	}
	detail := err.Error()
	return strings.Contains(detail, "aimee_err=concurrency_limit") ||
		strings.Contains(detail, "is rate-limited; retry in")
}

func firstNonempty(value, fallback string) string {
	if strings.TrimSpace(value) == "" {
		return fallback
	}
	return value
}
