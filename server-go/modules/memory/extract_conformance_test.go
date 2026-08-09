package memory

import (
	"bufio"
	"os"
	"strconv"
	"strings"
	"testing"
)

// Differential check against the C extractor itself.
//
// Both fixtures are memory_extract_patterns.c's own output, not expectations
// written by reading it:
//
//   - testdata/value_kinds.tsv is memory_pattern_classify_value over each
//     structured shape plus three mutations of each (last byte dropped,
//     upper-cased, trailing dot), so the classifier's boundaries are probed and
//     not just its happy path.
//   - testdata/extract_corpus.tsv crosses sentence frames with attributes and
//     values and records all three text entry points per row —
//     memory_pattern_is_retraction, memory_pattern_possessive_attr and
//     memory_extract_patterns — under the bounds the production caller uses
//     (a 128-byte attribute buffer, 16 triples).
//
// Regenerate with scripts/gen-memory-pattern-fixtures.c (its header carries the
// exact build and run lines), never by editing the files.

// unescapeField reverses the dumper's escaping, which keeps one case on one row.
func unescapeField(field string) string {
	var out strings.Builder
	for i := 0; i < len(field); i++ {
		if field[i] != '\\' || i+1 >= len(field) {
			out.WriteByte(field[i])
			continue
		}
		i++
		switch field[i] {
		case 'n':
			out.WriteByte('\n')
		case 't':
			out.WriteByte('\t')
		default:
			out.WriteByte(field[i])
		}
	}
	return out.String()
}

// fixtureRows splits each line of a fixture into its raw fields.
func fixtureRows(t *testing.T, path string) [][]string {
	t.Helper()
	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()

	var rows [][]string
	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 0, 64*1024), 1024*1024)
	for scanner.Scan() {
		rows = append(rows, strings.Split(scanner.Text(), "\t"))
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	if len(rows) == 0 {
		t.Fatalf("%s was empty; the comparison would pass vacuously", path)
	}
	return rows
}

func atoi(t *testing.T, field string, row []string) int {
	t.Helper()
	n, err := strconv.Atoi(field)
	if err != nil {
		t.Fatalf("bad integer %q in row %q: %v", field, strings.Join(row, "\t"), err)
	}
	return n
}

func TestClassifyValueMatchesTheCClassifier(t *testing.T) {
	seen := map[ValueKind]int{}
	compared := 0
	for _, row := range fixtureRows(t, "testdata/value_kinds.tsv") {
		if len(row) != 3 {
			t.Fatalf("malformed fixture row %q", strings.Join(row, "\t"))
		}
		token := unescapeField(row[0])
		wantKind := ValueKind(atoi(t, row[1], row))
		wantNode := NodeKind(atoi(t, row[2], row))

		if got := ClassifyValue(token); got != wantKind {
			t.Fatalf("ClassifyValue(%q) = %d, C = %d", token, got, wantKind)
		}
		if got := ValueNodeKind(wantKind); got != wantNode {
			t.Fatalf("ValueNodeKind(%d) = %d, C = %d", wantKind, got, wantNode)
		}
		seen[wantKind]++
		compared++
	}

	// A corpus that only produced ValueNone would agree trivially. Every shape the
	// classifier can return has to appear, so each branch is actually compared.
	allKinds := []ValueKind{ValueNone, ValueIPv4, ValueIPv6, ValueMAC, ValueEmail, ValueDate}
	for _, kind := range allKinds {
		if seen[kind] == 0 {
			t.Errorf("value kind %d never appears in the corpus; the classifier is undercovered", kind)
		}
	}
	t.Logf("compared %d tokens: none=%d ipv4=%d ipv6=%d mac=%d email=%d date=%d",
		compared, seen[ValueNone], seen[ValueIPv4], seen[ValueIPv6], seen[ValueMAC],
		seen[ValueEmail], seen[ValueDate])
}

// The corpus cannot tell the classifiers' test order apart — reordering them
// leaves every verdict unchanged — because no token satisfies two shapes at
// once. That is the property the corpus is silent about, so assert it directly:
// if a shape is ever loosened enough to overlap another, the order starts to
// matter and this goes red before the fixtures do.
func TestValueShapesAreDisjoint(t *testing.T) {
	shapes := map[string]func(string) bool{
		"ipv4": isIPv4, "mac": isMAC, "ipv6": isIPv6, "email": isEmail, "date": isISODate,
	}
	for _, row := range fixtureRows(t, "testdata/value_kinds.tsv") {
		token := unescapeField(row[0])
		if token == "" {
			continue
		}
		var matched []string
		for name, shape := range shapes {
			if shape(token) {
				matched = append(matched, name)
			}
		}
		if len(matched) > 1 {
			t.Errorf("token %q matches several shapes %v; ClassifyValue's order now decides "+
				"the answer and the fixture does not pin it", token, matched)
		}
	}
}

func TestExtractionMatchesTheCExtractor(t *testing.T) {
	const corpusMaxTriples = 16 // the bound the fixture was generated under

	retractions, possessives, withTriples, multiTriple := 0, 0, 0, 0
	for _, row := range fixtureRows(t, "testdata/extract_corpus.tsv") {
		if len(row) < 5 {
			t.Fatalf("malformed fixture row %q", strings.Join(row, "\t"))
		}
		text := unescapeField(row[0])
		wantRetraction := atoi(t, row[1], row) == 1
		wantHasAttr := atoi(t, row[2], row) == 1
		wantAttr := unescapeField(row[3])
		wantCount := atoi(t, row[4], row)
		if len(row) != 5+wantCount*5 {
			t.Fatalf("row claims %d triples but carries %d fields: %q", wantCount, len(row),
				strings.Join(row, "\t"))
		}

		if got := IsRetraction(text); got != wantRetraction {
			t.Fatalf("IsRetraction(%q) = %v, C = %v", text, got, wantRetraction)
		}
		gotAttr, gotHasAttr := PossessiveAttr(text)
		if gotHasAttr != wantHasAttr || gotAttr != wantAttr {
			t.Fatalf("PossessiveAttr(%q) = (%q, %v), C = (%q, %v)", text, gotAttr, gotHasAttr,
				wantAttr, wantHasAttr)
		}

		got := ExtractPatterns(text, corpusMaxTriples)
		if len(got) != wantCount {
			t.Fatalf("ExtractPatterns(%q) returned %d triples, C returned %d", text, len(got),
				wantCount)
		}
		for i, triple := range got {
			want := Triple{
				Subject:     unescapeField(row[5+i*5]),
				RelType:     unescapeField(row[6+i*5]),
				Object:      unescapeField(row[7+i*5]),
				SubjectKind: NodeKind(atoi(t, row[8+i*5], row)),
				ObjectKind:  NodeKind(atoi(t, row[9+i*5], row)),
			}
			if triple != want {
				t.Fatalf("ExtractPatterns(%q)[%d] = %+v, C = %+v", text, i, triple, want)
			}
		}

		if wantRetraction {
			retractions++
		}
		if wantHasAttr {
			possessives++
		}
		if wantCount > 0 {
			withTriples++
		}
		if wantCount > 1 {
			multiTriple++
		}
	}

	// Guard against a corpus that agrees because nothing in it matches: each entry
	// point has to have fired, and at least one text has to yield more than one
	// triple so the scan-and-continue path is covered too.
	if retractions == 0 || possessives == 0 || withTriples == 0 || multiTriple == 0 {
		t.Errorf("corpus is degenerate: retractions=%d possessives=%d with_triples=%d multi=%d",
			retractions, possessives, withTriples, multiTriple)
	}
	t.Logf("retractions=%d possessives=%d texts_with_triples=%d multi_triple=%d", retractions,
		possessives, withTriples, multiTriple)
}

// The corpus is generated with max=16 and one text overruns it, but nothing in
// it pins the boundary itself, so state it directly.
func TestExtractPatternsRespectsMax(t *testing.T) {
	text := "my a is 1. my b is 2. my c is 3."
	if got := ExtractPatterns(text, 2); len(got) != 2 {
		t.Fatalf("max=2 yielded %d triples, want 2", len(got))
	}
	if got := ExtractPatterns(text, 0); got != nil {
		t.Fatalf("max=0 yielded %v, want nil (the C's bad-arg case)", got)
	}
	if got := ExtractPatterns(text, -1); got != nil {
		t.Fatalf("max=-1 yielded %v, want nil (the C's bad-arg case)", got)
	}
}
