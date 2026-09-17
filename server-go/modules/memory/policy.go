package memory

import (
	"github.com/JBailes/aimee/server-go/bus"
	"sort"
	"strings"
)

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

// The graph has persisted numeric relation codes and its own allowed triples.
// The identity/world-fact seed is a different vocabulary: deriving this table
// from that seed silently dropped every graph rule except supersedes.
var graphOntologyRules = []OntologyRule{
	{0, 11, 0},   // file co_edited file
	{99, 12, 99}, // any co_discussed any
	{1, 0, 1},    // function depends_on function
	{3, 0, 3},    // module depends_on module
	{1, 5, 1},    // function calls function
	{1, 6, 2},    // function mutates struct
	{1, 1, 8},    // function implements concept
	{5, 2, 4},    // commit fixes bug
	{5, 3, 7},    // commit introduced_by developer
	{99, 9, 7},   // any authored_by developer
	{99, 9, 10},  // any authored_by person
	{99, 4, 99},  // any tests any
	{10, 7, 9},   // person participated_in event
	{7, 7, 9},    // developer participated_in event
	{99, 8, 12},  // any occurred_at time_expr
	{99, 8, 11},  // any occurred_at place
	{99, 10, 99}, // any supersedes any
	{99, 13, 99}, // any summarises any
}

func ontologyValid(subject, relation, object int) bool {
	if relation == 99 {
		return true
	}
	for _, r := range graphOntologyRules {
		if r.Relation == relation && (r.SubjectKind == 99 || r.SubjectKind == subject) && (r.ObjectKind == 99 || r.ObjectKind == object) {
			return true
		}
	}
	return false
}

func ontologyRules() []OntologyRule {
	rules := append([]OntologyRule{}, graphOntologyRules...)
	sort.Slice(rules, func(i, j int) bool {
		a, b := rules[i], rules[j]
		if a.Relation != b.Relation {
			return a.Relation < b.Relation
		}
		if a.SubjectKind != b.SubjectKind {
			return a.SubjectKind < b.SubjectKind
		}
		return a.ObjectKind < b.ObjectKind
	})
	return rules
}

func handleSchemaCommand(_ handlerOptions, _ bus.ModuleInvocation, _ string, _ commandArgs) ([]byte, bus.ModuleStatus) {
	rules := ontologyRules()
	rows := make([]map[string]any, 0, len(rules))
	for _, r := range rules {
		rows = append(rows, map[string]any{"relation_id": r.Relation, "subject_kind": r.SubjectKind, "object_kind": r.ObjectKind,
			"relation": relationName(r.Relation), "subject": nodeName(r.SubjectKind), "object": nodeName(r.ObjectKind)})
	}
	return commandResult(map[string]any{"status": "ok", "rows": rows})
}
