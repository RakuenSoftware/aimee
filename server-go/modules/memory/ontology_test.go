package memory

import (
	"bufio"
	"os"
	"strconv"
	"strings"
	"testing"
)

// The seed table is generated from src/rel_types.c. testdata/ontology_seed.tsv
// is the C's own answer -- produced by scripts/gen-memory-ontology-seed.c, which
// links rel_types.c and walks rel_types_seed_count/rel_types_seed_at rather than
// parsing the source. Parsing was tried first and silently produced a different
// row count, which is exactly the failure this fixture exists to catch.
//
// The same walk writes ontology_seed.go, so this compares two outputs of one
// generator. What it still catches is a hand-edit of either, and a regeneration
// of one without the other.
func TestSeedTableMatchesTheCAuthority(t *testing.T) {
	file, err := os.Open("testdata/ontology_seed.tsv")
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()

	type row struct {
		name       string
		head, tail []NodeKind
		sens       RelSensitivity
	}
	parseKinds := func(field string) []NodeKind {
		out := []NodeKind{}
		for _, part := range strings.Split(field, ";") {
			if part == "" {
				continue
			}
			value, err := strconv.Atoi(part)
			if err != nil {
				t.Fatalf("bad kind %q: %v", part, err)
			}
			out = append(out, NodeKind(value))
		}
		return out
	}

	var want []row
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		fields := strings.Split(scanner.Text(), "\t")
		if len(fields) != 4 {
			t.Fatalf("malformed fixture line %q", scanner.Text())
		}
		sens, err := strconv.Atoi(fields[3])
		if err != nil {
			t.Fatalf("bad sensitivity in %q: %v", scanner.Text(), err)
		}
		want = append(want, row{fields[0], parseKinds(fields[1]), parseKinds(fields[2]),
			RelSensitivity(sens)})
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}

	if len(seedOntology) != len(want) {
		t.Fatalf("seed rows = %d, C authority = %d", len(seedOntology), len(want))
	}
	// Order matters: lookup is a linear scan, so a reordering changes which row
	// wins if the table ever gains a duplicate name.
	for i, expected := range want {
		got := seedOntology[i]
		if got.RelType != expected.name {
			t.Errorf("row %d: name %q, want %q", i, got.RelType, expected.name)
			continue
		}
		if !sameKinds(got.HeadKinds, expected.head) {
			t.Errorf("%s: head kinds %v, want %v", got.RelType, got.HeadKinds, expected.head)
		}
		if !sameKinds(got.TailKinds, expected.tail) {
			t.Errorf("%s: tail kinds %v, want %v", got.RelType, got.TailKinds, expected.tail)
		}
		if got.Sensitivity != expected.sens {
			t.Errorf("%s: sensitivity %d, want %d", got.RelType, got.Sensitivity, expected.sens)
		}
	}
	// The recall gate reads this column, so a table where every row shared one
	// tier would let a broken sensitivity lookup pass unnoticed.
	tiers := map[RelSensitivity]int{}
	for _, got := range seedOntology {
		tiers[got.Sensitivity]++
	}
	if len(tiers) < 2 {
		t.Errorf("every seed row has the same sensitivity (%v); the tier is untested here", tiers)
	}
}

func sameKinds(a, b []NodeKind) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// Normalization decides whether a label finds its row at all, so the cases are
// taken from rel_type_normalize itself (testdata/normalize_cases.tsv) rather
// than reasoned about. Writing them by hand got "HTTPServer" wrong: a run of
// capitals does not split, and only the C could say so.
func TestNormalizeMatchesTheCAuthority(t *testing.T) {
	file, err := os.Open("testdata/normalize_cases.tsv")
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	scanner := bufio.NewScanner(file)
	checked := 0
	for scanner.Scan() {
		fields := strings.SplitN(scanner.Text(), "\t", 2)
		if len(fields) != 2 {
			t.Fatalf("malformed fixture line %q", scanner.Text())
		}
		input, want := fields[0], fields[1]
		if input == "__overlong_len__" {
			length, err := strconv.Atoi(want)
			if err != nil {
				t.Fatal(err)
			}
			if got := len(normalizeRelType(strings.Repeat("a", 300))); got != length {
				t.Errorf("overlong length = %d, want %d", got, length)
			}
			checked++
			continue
		}
		if got := normalizeRelType(input); got != want {
			t.Errorf("normalizeRelType(%q) = %q, want %q", input, got, want)
		}
		checked++
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	if checked == 0 {
		t.Fatal("fixture was empty; the test would pass vacuously")
	}
}

func TestGateAcceptsASeededTripleWithAllowedKinds(t *testing.T) {
	if got := GateCheck(NodePerson, "works_for", NodeOrg); got != FactAccept {
		t.Fatalf("verdict = %v, want accept", got)
	}
	// The same relation reached by an unnormalized label must behave identically.
	if got := GateCheck(NodePerson, "worksFor", NodeOrg); got != FactAccept {
		t.Fatalf("camelCase verdict = %v, want accept", got)
	}
}

// A seeded relation with a disallowed end is rejected, never quietly accepted:
// this is the single commit point for semantic edges.
func TestGateRejectsADisallowedKind(t *testing.T) {
	if got := GateCheck(NodeFile, "works_for", NodeOrg); got != FactRejectKind {
		t.Fatalf("bad head kind = %v, want reject_kind", got)
	}
	if got := GateCheck(NodePerson, "works_for", NodeFile); got != FactRejectKind {
		t.Fatalf("bad tail kind = %v, want reject_kind", got)
	}
}

// An unseeded relation is Novel, not a rejection -- the caller stages it
// against the live ontology. Conflating the two would drop learnable relations.
func TestGateReportsUnseededRelationsAsNovel(t *testing.T) {
	if got := GateCheck(NodePerson, "definitely_not_a_seeded_relation", NodeOrg); got != FactNovel {
		t.Fatalf("verdict = %v, want novel", got)
	}
}

func TestGateRejectsAMissingRelation(t *testing.T) {
	if got := GateCheck(NodePerson, "", NodeOrg); got != FactBadArg {
		t.Fatalf("verdict = %v, want bad_arg", got)
	}
}

// NodeOther in a seed row is the ANY wildcard. Find a row that uses it and
// prove any kind satisfies that end, rather than asserting it abstractly.
func TestWildcardKindAcceptsAnything(t *testing.T) {
	var wildcard *relTypeDef
	for i := range seedOntology {
		for _, k := range seedOntology[i].HeadKinds {
			if k == NodeOther {
				wildcard = &seedOntology[i]
				break
			}
		}
		if wildcard != nil {
			break
		}
	}
	if wildcard == nil {
		t.Skip("no seed row uses the ANY wildcard at the head")
	}
	if !kindAllowed(wildcard.HeadKinds, NodeBug) {
		t.Fatalf("%s: wildcard head rejected an arbitrary kind", wildcard.RelType)
	}
}
