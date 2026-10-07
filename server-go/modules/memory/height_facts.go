package memory

import (
	"regexp"
	"strings"
	"unicode"
)

// A narrow, grounded statement template; questions, pronouns, quoted text and
// negation are left to model extraction at model authority.
var heightClauseBreak = regexp.MustCompile(`[,;\n]|\.(?:\s|$)`)
var heightStatement = regexp.MustCompile(`(?i)^(?:remember\s+that\s+)?(?:but\s+)?(?:the\s+)?(<@!?[0-9]{17,20}>|[\p{L}][\p{L}\p{N} '\-]{0,159}?)\s+(?:is|are)\s+(?:really\s+)?([0-9]+(?:\.[0-9]+)?)\s*(feet|foot|ft|metres|meters|m|inches|inch|in|centimetres|centimeters|cm)\s+tall!?$`)

var discordSubjectID = regexp.MustCompile(`^<@[0-9]{17,20}>$`)
var discordNumericID = regexp.MustCompile(`^[0-9]{17,20}$`)
var selfHeightStatement = regexp.MustCompile(`(?i)^(?:but\s+)?I(?:\s+am|['’]m)\s+(?:really\s+)?([0-9]+(?:\.[0-9]+)?)\s*(feet|foot|ft|metres|meters|m|inches|inch|in|centimetres|centimeters|cm)(?:\s+tall)?(?:\s+not\s+[0-9]+(?:\.[0-9]+)?(?:\s*(?:feet|foot|ft|cm|meters|metres))?(?:\s+ok)?)?(?:\s+I\s+told\s+you\s+already)?!?$`)

func heightFactCandidates(content, observedAt string, memoryID, jobID int64, actor FactActor) []FactCandidate {
	var result []FactCandidate
	offset := 0
	for _, span := range heightClauseBreak.FindAllStringIndex(content, -1) {
		result = append(result, heightClauseCandidate(content, offset, span[0], observedAt, memoryID, jobID, actor)...)
		if len(result) >= memoryFactMaxTriples {
			return result[:memoryFactMaxTriples]
		}
		offset = span[1]
	}
	result = append(result, heightClauseCandidate(content, offset, len(content), observedAt, memoryID, jobID, actor)...)
	return result
}

func heightClauseCandidate(content string, start, end int, observedAt string, memoryID, jobID int64, actor FactActor) []FactCandidate {
	clause := strings.TrimSpace(content[start:end])
	match := heightStatement.FindStringSubmatch(clause)
	if match == nil && actor.Role == "user" && actor.Rank == 30 && actor.Authenticated == 1 && discordSubjectID.MatchString(actor.DiscordSubject) {
		if self := selfHeightStatement.FindStringSubmatch(clause); self != nil {
			match = []string{self[0], actor.DiscordSubject, self[1], self[2]}
		}
	}
	if match == nil {
		return nil
	}
	subject := strings.ReplaceAll(strings.TrimSpace(match[1]), "<@!", "<@")
	words := strings.Fields(strings.ToLower(subject))
	for _, w := range words {
		switch w {
		case "i", "he", "she", "they", "it", "my", "your", "our", "his", "her", "not", "never", "no", "said", "says", "if", "maybe":
			return nil
		}
	}
	if len(subject) == 0 || (!strings.HasPrefix(subject, "<@") && !unicode.IsLetter([]rune(subject)[0])) {
		return nil
	}
	value := match[2] + " " + strings.ToLower(match[3])
	if strings.HasSuffix(value, " foot") || strings.HasSuffix(value, " ft") {
		value = match[2] + " feet"
	}
	evidence := memoryFactEvidence(content, int64(start), int64(end), actor, observedAt, memoryID, jobID)
	return []FactCandidate{{Subject: subject, Relation: "has_height", Object: value,
		SubjectKind: NodeOther, ObjectKind: NodeScalar, Actor: actor, Evidence: evidence, AssertionKind: "world_fact", ValidFrom: observedAt}}
}
