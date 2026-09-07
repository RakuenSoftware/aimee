package memory

import "strings"

var ontologyRelationNames = map[int]string{
	0: "depends_on", 1: "implements", 2: "fixes", 3: "introduced_by", 4: "tests",
	5: "calls", 6: "mutates", 7: "participated_in", 8: "occurred_at", 9: "authored_by",
	10: "supersedes", 11: "co_edited", 12: "co_discussed", 13: "summarises",
}

var ontologyNodeNames = map[int]string{
	0: "file", 1: "function", 2: "struct", 3: "module", 4: "bug", 5: "commit",
	6: "pr", 7: "developer", 8: "concept", 9: "event", 10: "person", 11: "place",
	12: "time_expr", 13: "device", 14: "org", 15: "ip", 16: "scalar",
}

type OntologyRule struct {
	SubjectKind int `json:"subject_kind"`
	Relation    int `json:"relation"`
	ObjectKind  int `json:"object_kind"`
}

func functionalTierName(tier string) string {
	switch strings.ToUpper(strings.TrimSpace(tier)) {
	case "L0", "L1":
		return "Experience"
	case "L2":
		return "Observation"
	case "L3":
		return "World"
	case "L4":
		return "MentalModel"
	case "L5":
		return "Pattern"
	default:
		return "Unknown"
	}
}

func scopeLevelName(level int) string {
	switch level {
	case 1:
		return "global"
	case 2:
		return "workspace"
	case 3:
		return "project"
	default:
		return "none"
	}
}

func relationName(code int) string {
	if name, ok := ontologyRelationNames[code]; ok {
		return name
	}
	return "other"
}

func relationCode(name string) int {
	normalized := normalizeRelType(name)
	for code, candidate := range ontologyRelationNames {
		if normalized == candidate {
			return code
		}
	}
	return 99
}

func nodeName(code int) string {
	if name, ok := ontologyNodeNames[code]; ok {
		return name
	}
	return "other"
}

func nodeCode(name string) int {
	normalized := strings.ToLower(strings.TrimSpace(name))
	for code, candidate := range ontologyNodeNames {
		if normalized == candidate {
			return code
		}
	}
	return 99
}

func ontologyValid(subject, relation, object int) bool {
	if relation == 99 {
		return true
	}
	name, ok := ontologyRelationNames[relation]
	if !ok {
		return false
	}
	return GateCheck(NodeKind(subject), name, NodeKind(object)) == FactAccept
}

func ontologyRules() []OntologyRule {
	rules := make([]OntologyRule, 0)
	for relation, name := range ontologyRelationNames {
		definition := seedLookup(name)
		if definition == nil {
			continue
		}
		for _, subject := range definition.HeadKinds {
			for _, object := range definition.TailKinds {
				rules = append(rules, OntologyRule{SubjectKind: int(subject), Relation: relation,
					ObjectKind: int(object)})
			}
		}
	}
	return rules
}
