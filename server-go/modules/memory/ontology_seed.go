// The shared Go memory owner is the authority for the seed ontology.
// Regenerate schema/native compatibility data with:
// go run ./modules/memory/cmd/aimee-memory-seed --root ..

package memory

// seedOntology is the memory module's relation table: the entity kinds the
// write gate allows at each end, and the sensitivity tier the recall gate
// reads. NodeOther in a kind list is the ANY wildcard.
var seedOntology = []relTypeDef{
	{RelType: "works_for", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodeOrg}, Sensitivity: SensNormal, Correction: "supersede", Category: "work"},
	{RelType: "member_of", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodeOrg}, Sensitivity: SensNormal, Correction: "supersede", Category: "work"},
	{RelType: "has_role", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodeScalar}, Sensitivity: SensNormal, Correction: "supersede", Category: "work"},
	{RelType: "spouse", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodePerson}, Sensitivity: SensPII, Correction: "supersede", Category: "family", Symmetric: true, Inverse: "spouse"},
	{RelType: "knows", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodePerson}, Sensitivity: SensPII, Correction: "supersede", Category: "social", Symmetric: true, Inverse: "knows"},
	{RelType: "parent_of", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodePerson}, Sensitivity: SensPII, Correction: "immutable", Category: "family", Inverse: "child_of", Hierarchy: true},
	{RelType: "child_of", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodePerson}, Sensitivity: SensPII, Correction: "immutable", Category: "family", Inverse: "parent_of", Hierarchy: true},
	{RelType: "lives_in", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodePlace}, Sensitivity: SensPII, Correction: "supersede", Category: "identity"},
	{RelType: "born_in", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodePlace}, Sensitivity: SensPII, Correction: "immutable", Category: "identity"},
	{RelType: "located_in", HeadKinds: []NodeKind{NodeOther}, TailKinds: []NodeKind{NodePlace}, Sensitivity: SensNormal, Correction: "supersede", Category: "geo", Hierarchy: true},
	{RelType: "device_has_ip", HeadKinds: []NodeKind{NodeDevice}, TailKinds: []NodeKind{NodeIp}, Sensitivity: SensNormal, Correction: "supersede", Category: "network"},
	{RelType: "has_hostname", HeadKinds: []NodeKind{NodeDevice}, TailKinds: []NodeKind{NodeScalar}, Sensitivity: SensNormal, Correction: "supersede", Category: "network"},
	{RelType: "age", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodeScalar}, Sensitivity: SensPII, Correction: "supersede", Category: "identity"},
	{RelType: "also_known_as", HeadKinds: []NodeKind{NodePerson}, TailKinds: []NodeKind{NodeOther}, Sensitivity: SensNormal, Correction: "hard_delete", Category: "identity"},
	{RelType: "supersedes", HeadKinds: []NodeKind{NodeOther}, TailKinds: []NodeKind{NodeOther}, Sensitivity: SensNormal, Correction: "supersede", Category: "governance"},
	{RelType: "linked_policy", HeadKinds: []NodeKind{NodeOther}, TailKinds: []NodeKind{NodeOther}, Sensitivity: SensNormal, Correction: "supersede", Category: "governance"},
	{RelType: "decided_by", HeadKinds: []NodeKind{NodeOther}, TailKinds: []NodeKind{NodePerson}, Sensitivity: SensNormal, Correction: "supersede", Category: "governance"},
}
