package memory

import (
	"regexp"
	"sort"
	"strings"
)

var spatialClauseBreak = regexp.MustCompile(`[;\n]|\.(?:\s|$)`)

const spatialSubject = `(?:but\s+)?(?:the\s+)?([\p{L}][\p{L}\p{N} '\-]{0,159}?)\s+(?:is|are)\s+`
const spatialDistance = `([0-9]+(?:\.[0-9]+)?)\s+(meters|metres|m|kilometers|kilometres|km|feet|foot|ft|miles|mile)\s+(?:away\s+)?from\s+`

var spatialCombined = regexp.MustCompile(`(?i)^` + spatialSubject + spatialDistance + `(.{1,200}?),?\s+and\s+(?:is|are)\s+located\s+in\s+(.{1,200}?)!?$`)
var spatialDistanceOnly = regexp.MustCompile(`(?i)^` + spatialSubject + spatialDistance + `(.{1,200}?)!?$`)
var spatialLocation = regexp.MustCompile(`(?i)^` + spatialSubject + `located\s+in\s+(.{1,200}?)!?$`)
var spatialMention = regexp.MustCompile(`^<@!?[0-9]{17,20}>(?:\s*['’]s)?(?:\s+[\p{L}][\p{L}\p{N} '\-]*)?$`)

func spatialName(value string) string {
	value = strings.Join(strings.Fields(value), " ")
	value = strings.TrimPrefix(value, "the ")
	value = strings.TrimPrefix(value, "The ")
	value = strings.ReplaceAll(value, " 's", "'s")
	if spatialMention.MatchString(value) {
		return strings.ReplaceAll(value, "<@!", "<@")
	}
	if strings.ContainsAny(value, `<>?"`) {
		return ""
	}
	for _, word := range strings.Fields(strings.ToLower(value)) {
		switch word {
		case "i", "he", "she", "they", "it", "my", "your", "our", "here", "there", "not", "no", "never", "said", "says", "if", "maybe", "think", "probably", "possibly", "and":
			return ""
		}
	}
	return value
}

// Distances are measurements of an explicit unordered pair, not an attribute
// of just one place. Both endpoint names survive typed recall and corrections.
// An unspecified "away" reference, questions and inferred house locations abstain.
func spatialFactCandidates(content, observedAt string, memoryID, jobID int64, actor FactActor) []FactCandidate {
	result := []FactCandidate{}
	offset := 0
	spans := spatialClauseBreak.FindAllStringIndex(content, -1)
	spans = append(spans, []int{len(content), len(content)})
	for _, span := range spans {
		clause := strings.TrimSpace(content[offset:span[0]])
		match := spatialCombined.FindStringSubmatch(clause)
		if match == nil {
			match = spatialDistanceOnly.FindStringSubmatch(clause)
		}
		evidence := memoryFactEvidence(content, int64(offset), int64(span[0]), actor, observedAt, memoryID, jobID)
		if match != nil {
			subject, reference := spatialName(match[1]), spatialName(match[4])
			if subject != "" && reference != "" && !strings.Contains(reference, ",") {
				endpoints := []string{subject, reference}
				sort.Slice(endpoints, func(i, j int) bool { return factIdentityComponent(endpoints[i]) < factIdentityComponent(endpoints[j]) })
				unit := strings.ToLower(match[3])
				switch unit {
				case "metres", "m":
					unit = "meters"
				case "kilometres", "km":
					unit = "kilometers"
				case "foot", "ft":
					unit = "feet"
				case "mile":
					unit = "miles"
				}
				result = append(result, FactCandidate{Subject: "Distance between " + endpoints[0] + " and " + endpoints[1], Relation: "has_distance", Object: match[2] + " " + unit,
					SubjectKind: NodeOther, ObjectKind: NodeScalar, Actor: actor, Evidence: evidence, AssertionKind: "world_fact", ValidFrom: observedAt})
				if len(match) > 5 {
					place := spatialName(match[5])
					if place != "" {
						result = append(result, FactCandidate{Subject: subject, Relation: "located_in", Object: place, SubjectKind: NodeOther, ObjectKind: NodePlace, Actor: actor, Evidence: evidence, AssertionKind: "world_fact", ValidFrom: observedAt})
					}
				}
			}
		} else if match = spatialLocation.FindStringSubmatch(clause); match != nil {
			subject, place := spatialName(match[1]), spatialName(match[2])
			if subject != "" && place != "" {
				result = append(result, FactCandidate{Subject: subject, Relation: "located_in", Object: place, SubjectKind: NodeOther, ObjectKind: NodePlace, Actor: actor, Evidence: evidence, AssertionKind: "world_fact", ValidFrom: observedAt})
			}
		}
		if len(result) >= memoryFactMaxTriples {
			return result[:memoryFactMaxTriples]
		}
		offset = span[1]
	}
	return result
}
