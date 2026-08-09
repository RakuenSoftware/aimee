package memory

import (
	"bufio"
	"os"
	"strconv"
	"strings"
	"testing"
)

// Differential check against the C gate itself.
//
// testdata/gate_matrix.tsv is memory_fact_gate_check's own output over every
// node kind crossed with canonical, mangled, unseeded and empty relation
// labels. The seed-table and normalization fixtures each prove one input to the
// gate; only this proves the ladder that combines them.
//
// Regenerate by linking rel_types.c and memory_fact_gate.c against a dumper and
// re-running it, never by editing the file.
// gateCase is one row of the matrix: the triple the C gate was asked about and
// the verdict it gave.
type gateCase struct {
	head    NodeKind
	relType string
	tail    NodeKind
	want    FactVerdict
}

func gateMatrixCases(t *testing.T) []gateCase {
	t.Helper()
	file, err := os.Open("testdata/gate_matrix.tsv")
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()

	var cases []gateCase
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		line := scanner.Text()
		fields := strings.Split(line, "\t")
		if len(fields) != 4 {
			t.Fatalf("malformed fixture line %q", line)
		}
		head, err := strconv.Atoi(fields[0])
		if err != nil {
			t.Fatalf("bad head kind in %q: %v", line, err)
		}
		tail, err := strconv.Atoi(fields[2])
		if err != nil {
			t.Fatalf("bad tail kind in %q: %v", line, err)
		}
		wantCode, err := strconv.Atoi(fields[3])
		if err != nil {
			t.Fatalf("bad verdict in %q: %v", line, err)
		}
		cases = append(cases, gateCase{NodeKind(head), fields[1], NodeKind(tail), FactVerdict(wantCode)})
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	return cases
}

func TestGateMatchesTheCGate(t *testing.T) {
	checked := 0
	seen := map[FactVerdict]int{}
	for _, test := range gateMatrixCases(t) {
		got := GateCheck(test.head, test.relType, test.tail)
		if got != test.want {
			t.Fatalf("GateCheck(%d, %q, %d) = %d, C gate = %d", test.head, test.relType, test.tail,
				got, test.want)
		}
		seen[test.want]++
		checked++
	}

	if checked == 0 {
		t.Fatal("fixture was empty; the comparison would pass vacuously")
	}
	// A matrix that only ever exercised one branch would agree trivially. Require
	// every verdict the pure gate can produce to appear at least once, so the
	// ladder is actually covered.
	for _, verdict := range []FactVerdict{FactAccept, FactRejectKind, FactNovel, FactBadArg} {
		if seen[verdict] == 0 {
			t.Errorf("verdict %d never appears in the matrix; the ladder is undercovered", verdict)
		}
	}
	t.Logf("compared %d cases: accept=%d reject_kind=%d novel=%d bad_arg=%d", checked,
		seen[FactAccept], seen[FactRejectKind], seen[FactNovel], seen[FactBadArg])
}
