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
func TestGateMatchesTheCGate(t *testing.T) {
	file, err := os.Open("testdata/gate_matrix.tsv")
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()

	scanner := bufio.NewScanner(file)
	checked := 0
	seen := map[FactVerdict]int{}
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
		relType := fields[1]
		tail, err := strconv.Atoi(fields[2])
		if err != nil {
			t.Fatalf("bad tail kind in %q: %v", line, err)
		}
		wantCode, err := strconv.Atoi(fields[3])
		if err != nil {
			t.Fatalf("bad verdict in %q: %v", line, err)
		}
		want := FactVerdict(wantCode)

		got := GateCheck(NodeKind(head), relType, NodeKind(tail))
		if got != want {
			t.Fatalf("GateCheck(%d, %q, %d) = %d, C gate = %d", head, relType, tail, got, want)
		}
		seen[want]++
		checked++
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
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
