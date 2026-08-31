package memory

import "strings"

// The typed-fact write gate, ported from src/modules/memory/memory_fact_gate.c
// and the pure core of src/rel_types.c.
//
// It validates a candidate triple (head kind, relation, tail kind) against the
// seed ontology before anything commits it as a semantic edge. It is the single
// commit point for those edges: every triple emitter routes through it, and no
// emitter writes a semantic edge directly.
//
// Pure by construction — seed table only, no database. That is what lets it run
// in the module process at all: a module reaches core over the bus and has no
// DB2 handle of its own.

// NodeKind is an entity kind. Values match memory_node_kind_t exactly; they are
// persisted as integer codes, so they are assigned rather than derived.
type NodeKind uint32

const (
	NodeFile      NodeKind = 0
	NodeFunction  NodeKind = 1
	NodeStruct    NodeKind = 2
	NodeModule    NodeKind = 3
	NodeBug       NodeKind = 4
	NodeCommit    NodeKind = 5
	NodePr        NodeKind = 6
	NodeDeveloper NodeKind = 7
	NodeConcept   NodeKind = 8
	NodeEvent     NodeKind = 9
	NodePerson    NodeKind = 10
	NodePlace     NodeKind = 11
	NodeTimeExpr  NodeKind = 12
	NodeDevice    NodeKind = 13
	NodeOrg       NodeKind = 14
	NodeIp        NodeKind = 15
	NodeScalar    NodeKind = 16
	// NodeOther is the ANY wildcard when it appears in a kind list.
	NodeOther NodeKind = 99
)

// FactVerdict mirrors fact_gate_verdict_t. Only the values the pure gate can
// return are defined here; DEFER and REJECT_SENSITIVE belong to the DB-backed
// commit path in core and are never produced by this stage.
type FactVerdict uint32

const (
	// FactAccept means a known relation whose ends satisfy its kind constraints.
	FactAccept FactVerdict = 0
	// FactRejectKind means a known relation used with a disallowed end kind.
	FactRejectKind FactVerdict = 1
	// FactNovel means the relation is not in the seed ontology; the caller
	// consults the live table and stages or defers.
	FactNovel FactVerdict = 2
	// FactBadArg means no relation was supplied.
	FactBadArg FactVerdict = 3
)

// RelSensitivity is a relation's PII gating tier. Values match
// rel_sensitivity_t; they are persisted as text and compared as integers, so
// the numbering is assigned rather than derived.
type RelSensitivity uint32

const (
	// SensNormal is an identity or operational fact: injected above the
	// confidence floor.
	SensNormal RelSensitivity = 0
	// SensPII is a regulated identifier: injected only when the turn asks for it.
	SensPII RelSensitivity = 1
	// SensSecret is a credential: never injected, served through the vault.
	SensSecret RelSensitivity = 2
)

// relTypeDef is the slice of a seed row this module reads. The C row carries
// more (inverse, correction behaviour, category, hierarchy flag, status); those
// belong to the DB-backed commit path in core, not to either gate here.
type relTypeDef struct {
	RelType     string
	HeadKinds   []NodeKind
	TailKinds   []NodeKind
	Sensitivity RelSensitivity
}

// relTypeNameMax mirrors REL_TYPE_NAME_MAX. Normalization truncates to it, so a
// pathological label cannot grow unbounded through the gate.
const relTypeNameMax = 64

// normalizeRelType canonicalises a relation label, matching rel_type_normalize:
// alphanumerics are lower-cased and kept, every other run collapses to a single
// underscore, leading underscores are suppressed, and a camelCase boundary
// (upper after lower or digit) inserts one — so "worksFor", "Works For" and
// "works-for" all yield "works_for".
//
// The C writes into a fixed buffer and stops one byte short of it, so the
// result is capped at relTypeNameMax-1 bytes; that cap is reproduced here
// because a longer label must miss the table in the same way on both sides.
func normalizeRelType(in string) string {
	out := make([]byte, 0, len(in))
	prevUnderscore := true // leading-underscore suppression
	prevLowerOrDigit := false
	for i := 0; i < len(in) && len(out) < relTypeNameMax-1; i++ {
		c := in[i]
		switch {
		case isAlnum(c):
			if isUpper(c) && prevLowerOrDigit && !prevUnderscore && len(out) < relTypeNameMax-1 {
				out = append(out, '_')
			}
			if len(out) < relTypeNameMax-1 {
				out = append(out, toLower(c))
			}
			prevUnderscore = false
			// Only a lowercase letter or digit opens a camelCase boundary. A run
			// of capitals does not split, so "HTTPServer" normalizes to
			// "httpserver" rather than "http_server" -- verified against
			// rel_type_normalize, not assumed.
			prevLowerOrDigit = isLower(c) || isDigit(c)
		default:
			if !prevUnderscore && len(out) < relTypeNameMax-1 {
				out = append(out, '_')
				prevUnderscore = true
			}
			prevLowerOrDigit = false
		}
	}
	// A trailing separator is not part of the name.
	return strings.TrimSuffix(string(out), "_")
}

func isUpper(c byte) bool { return c >= 'A' && c <= 'Z' }
func isLower(c byte) bool { return c >= 'a' && c <= 'z' }
func isDigit(c byte) bool { return c >= '0' && c <= '9' }
func isAlnum(c byte) bool { return isUpper(c) || isLower(c) || isDigit(c) }
func toLower(c byte) byte {
	if isUpper(c) {
		return c + ('a' - 'A')
	}
	return c
}

// seedLookup finds a seed row by normalized name, mirroring
// rel_types_seed_lookup. Returns nil when the relation is not seeded.
func seedLookup(relType string) *relTypeDef {
	if relType == "" {
		return nil
	}
	norm := normalizeRelType(relType)
	for i := range seedOntology {
		if seedOntology[i].RelType == norm {
			return &seedOntology[i]
		}
	}
	return nil
}

// kindAllowed reports whether kind may sit at one end of the relation.
// NodeOther anywhere in the list is the ANY wildcard.
func kindAllowed(list []NodeKind, kind NodeKind) bool {
	for _, allowed := range list {
		if allowed == NodeOther || allowed == kind {
			return true
		}
	}
	return false
}

// GateCheck validates a candidate triple against the seed ontology.
//
// The ladder is the C's, in order: a missing relation is BadArg; an unseeded
// one is Novel and the caller decides whether to stage it; a seeded one with a
// disallowed end kind is RejectKind. Only a seeded relation with both ends
// satisfied is accepted.
func GateCheck(headKind NodeKind, relType string, tailKind NodeKind) FactVerdict {
	if relType == "" {
		return FactBadArg
	}
	def := seedLookup(relType)
	if def == nil {
		return FactNovel
	}
	if !kindAllowed(def.HeadKinds, headKind) || !kindAllowed(def.TailKinds, tailKind) {
		return FactRejectKind
	}
	return FactAccept
}
