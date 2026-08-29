package engine

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"sort"
	"strings"
	"unicode"

	"github.com/JBailes/aimee/server-go/internal/wfe"
)

const maxConvergenceBlockers = 64

type convergencePayloadV1 struct {
	Version    int    `json:"version"`
	Mode       string `json:"mode,omitempty"`
	Summary    string `json:"summary,omitempty"`
	BlockerSet string `json:"blocker_set,omitempty"`
}

// normalizeBlockerField collapses presentation-only differences without trying
// to make a model-backed semantic-equivalence decision inside the workflow
// transaction. Reviewers must express a genuinely different obligation with a
// different structured location or summary.
func normalizeBlockerField(value string) string {
	var out strings.Builder
	space := false
	for _, r := range strings.TrimSpace(value) {
		if unicode.IsLetter(r) || unicode.IsNumber(r) {
			if space && out.Len() > 0 {
				out.WriteByte(' ')
			}
			out.WriteRune(unicode.ToLower(r))
			space = false
			continue
		}
		space = true
	}
	return out.String()
}

func isBlockingFinding(finding wfe.Finding) bool {
	switch strings.ToLower(strings.TrimSpace(finding.Severity)) {
	case "foundational", "blocking":
		return strings.TrimSpace(finding.Summary) != ""
	default:
		return false
	}
}

// blockerFingerprintSet returns a sorted, unique set of deterministic hashes.
// Finding IDs are deliberately excluded: a reviewer controls them, so treating
// them as identity would let a loop reset itself by minting new IDs.
func blockerFingerprintSet(gate string, feedback *wfe.ReviewFeedback) string {
	if feedback == nil {
		return ""
	}
	unique := make(map[string]struct{})
	for _, finding := range feedback.Findings {
		if !isBlockingFinding(finding) {
			continue
		}
		fields := []string{
			"v1",
			normalizeBlockerField(gate),
			normalizeBlockerField(finding.Persona),
			normalizeBlockerField(finding.Severity),
			normalizeBlockerField(finding.Location),
			normalizeBlockerField(finding.Summary),
		}
		sum := sha256.Sum256([]byte(strings.Join(fields, "\x00")))
		unique[hex.EncodeToString(sum[:])] = struct{}{}
		if len(unique) > maxConvergenceBlockers {
			// Missing structure must not masquerade as demonstrated progress. An
			// empty set makes the store retain its existing hash-based fallback.
			return ""
		}
	}
	if len(unique) == 0 {
		return ""
	}
	set := make([]string, 0, len(unique))
	for fingerprint := range unique {
		set = append(set, fingerprint)
	}
	sort.Strings(set)
	return strings.Join(set, ",")
}

func convergencePayload(gate, summary string, feedback *wfe.ReviewFeedback, mode string) string {
	mode = strings.ToLower(strings.TrimSpace(mode))
	if mode != "enforce" {
		mode = "observe"
	}
	payload, err := json.Marshal(convergencePayloadV1{
		Version:    1,
		Mode:       mode,
		Summary:    summary,
		BlockerSet: blockerFingerprintSet(gate, feedback),
	})
	if err != nil {
		return summary
	}
	return string(payload)
}
