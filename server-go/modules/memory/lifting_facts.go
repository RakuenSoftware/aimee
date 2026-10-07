package memory

import (
	"regexp"
	"strings"
)

// Explicit named lifting statements share the source actor and byte-span gates
// used by heights. Questions, speculation, reported speech and negation abstain.
var liftingStatement = regexp.MustCompile(`(?i)^(?:but\s+)?(?:the\s+)?([\p{L}][\p{L}\p{N} '\-]{0,159}?)\s+can\s+lift\s+([0-9]+(?:\.[0-9]+)?)\s+(pounds|pound|lbs|lb|kilograms|kilogram|kgs|kg)!?$`)

func measurementFactCandidates(content, observedAt string, memoryID, jobID int64, actor FactActor) []FactCandidate {
	result := append(heightFactCandidates(content, observedAt, memoryID, jobID, actor), spatialFactCandidates(content, observedAt, memoryID, jobID, actor)...)
	offset := 0
	spans := heightClauseBreak.FindAllStringIndex(content, -1)
	spans = append(spans, []int{len(content), len(content)})
	for _, span := range spans {
		clause := strings.TrimSpace(content[offset:span[0]])
		match := liftingStatement.FindStringSubmatch(clause)
		if match != nil {
			subject := strings.TrimSpace(match[1])
			valid := true
			for _, word := range strings.Fields(strings.ToLower(subject)) {
				switch word {
				case "i", "he", "she", "they", "it", "my", "your", "our", "his", "her", "not", "never", "no", "said", "says", "if", "maybe", "think", "probably", "possibly":
					valid = false
				}
			}
			if valid {
				unit := strings.ToLower(match[3])
				switch unit {
				case "pound", "lb", "lbs":
					unit = "pounds"
				case "kilogram", "kgs", "kg":
					unit = "kilograms"
				}
				result = append(result, FactCandidate{Subject: subject, Relation: "can_lift", Object: match[2] + " " + unit,
					SubjectKind: NodeOther, ObjectKind: NodeScalar, Actor: actor,
					Evidence:      memoryFactEvidence(content, int64(offset), int64(span[0]), actor, observedAt, memoryID, jobID),
					AssertionKind: "world_fact", ValidFrom: observedAt})
			}
		}
		if len(result) >= memoryFactMaxTriples {
			return result[:memoryFactMaxTriples]
		}
		offset = span[1]
	}
	return result
}
