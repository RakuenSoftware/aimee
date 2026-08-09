// Code generated from src/rel_types.c SEED_ONTOLOGY. DO NOT EDIT BY HAND.
// Regenerate when the C table changes; ontology_conformance_test.go
// re-parses that source and fails if the two drift.

package memory

// seedOntology is the write gate's type table: the relation name and the
// entity kinds allowed at each end. NodeOther in a list is the ANY wildcard.
var seedOntology = []relTypeDef{
	{RelType: "works_for", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodeOrg}},
	{RelType: "member_of", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodeOrg}},
	{RelType: "has_role", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodeScalar}},
	{RelType: "spouse", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodePerson}},
	{RelType: "knows", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodePerson}},
	{RelType: "parent_of", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodePerson}},
	{RelType: "child_of", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodePerson}},
	{RelType: "lives_in", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodePlace}},
	{RelType: "born_in", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodePlace}},
	{RelType: "located_in", HeadKinds: []NodeKind{NodeOther}, TailKinds: []NodeKind{NodePlace}},
	{RelType: "device_has_ip", HeadKinds: []NodeKind{NodeDevice}, TailKinds: []NodeKind{NodeIp}},
	{RelType: "has_hostname", HeadKinds: []NodeKind{NodeDevice}, TailKinds: []NodeKind{NodeScalar}},
	{RelType: "age", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodeScalar}},
	{RelType: "also_known_as", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodeOther}},
	{RelType: "supersedes", HeadKinds: []NodeKind{NodeOther}, TailKinds: []NodeKind{NodeOther}},
	{RelType: "linked_policy", HeadKinds: []NodeKind{NodeOther}, TailKinds: []NodeKind{NodeOther}},
	{RelType: "decided_by", HeadKinds: []NodeKind{NodeOther}, TailKinds: []NodeKind{NodePerson}},
}
