package memory

import (
	"regexp"
	"strings"
)

// Explicit identity assertions are grounded in a complete source clause. A
// display name by itself is not proof that two entities are the same person.
var discordAliasStatement = regexp.MustCompile(`(?i)^(?:remember\s+that\s+)?(<@!?[0-9]{17,20}>)\s+is\s+also\s+known\s+as\s+([\p{L}][\p{L}\p{N} '\-]{0,79})!?$`)

func discordAliasFactCandidates(content, observedAt string, memoryID, jobID int64, actor FactActor) []FactCandidate {
	var result []FactCandidate
	offset := 0
	spans := heightClauseBreak.FindAllStringIndex(content, -1)
	spans = append(spans, []int{len(content), len(content)})
	for _, span := range spans {
		clause := strings.TrimSpace(content[offset:span[0]])
		if match := discordAliasStatement.FindStringSubmatch(clause); match != nil {
			name := strings.TrimSpace(match[2])
			valid := true
			for _, word := range strings.Fields(strings.ToLower(name)) {
				switch word {
				case "i", "he", "she", "it", "they", "not", "if", "maybe", "said", "says":
					valid = false
				}
			}
			if valid {
				result = append(result, FactCandidate{Subject: strings.ReplaceAll(match[1], "<@!", "<@"), Relation: "also_known_as", Object: name, SubjectKind: NodePerson, ObjectKind: NodeOther, Actor: actor, Evidence: memoryFactEvidence(content, int64(offset), int64(span[0]), actor, observedAt, memoryID, jobID), AssertionKind: "world_fact", ValidFrom: observedAt})
			}
		}
		if len(result) >= memoryFactMaxTriples {
			return result[:memoryFactMaxTriples]
		}
		offset = span[1]
	}
	return result
}
