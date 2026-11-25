package families

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// PostgreSQL's SUM over a BIGINT column is NUMERIC, and the store sends NUMERIC
// over the bus as a float (modules/postgres/sql.go). A family that scans such a
// sum into an int64 then fails at run time with "the store sent a number where
// an integer was expected": the live 1.1.1 Server could not answer
// agent_log_metrics_by_role. pgx would scan the same value into an int64, so a
// test over a real database alone does not catch it.
//
// Every SUM in this package must therefore either cast its result
// (SUM(x)::bigint, ::double precision) or sum a money column, which every
// consumer reads as a float.
func TestEverySumReturnsAWireType(t *testing.T) {
	files, err := filepath.Glob("*.go")
	if err != nil {
		t.Fatal(err)
	}
	sum := regexp.MustCompile(`SUM\(`)
	checked := 0
	for _, name := range files {
		if strings.HasSuffix(name, "_test.go") {
			continue
		}
		raw, err := os.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		src := string(raw)
		for _, loc := range sum.FindAllStringIndex(src, -1) {
			end := matchParen(src, loc[1]-1)
			if end < 0 {
				t.Fatalf("%s: unbalanced SUM( at byte %d", name, loc[0])
			}
			inner := src[loc[1]:end]
			after := src[end+1:]
			checked++
			if strings.HasPrefix(after, "::") {
				continue
			}
			// A FILTER clause sits between the call and any cast.
			if strings.HasPrefix(strings.TrimLeft(after, " \t\n"), "FILTER") {
				if f := matchParen(after, strings.Index(after, "(")); f >= 0 &&
					strings.HasPrefix(after[f+1:], "::") {
					continue
				}
			}
			if strings.Contains(inner, "cost") || strings.Contains(inner, "1.0") {
				continue // money or a fractional score: read as a float
			}
			line := 1 + strings.Count(src[:loc[0]], "\n")
			t.Errorf("%s:%d: SUM(%s) reaches the wire as NUMERIC; cast it (::bigint)",
				name, line, strings.Join(strings.Fields(inner), " "))
		}
	}
	if checked == 0 {
		t.Fatal("found no SUM( to check; the scan is broken")
	}
}

// matchParen returns the index of the parenthesis closing the one at open.
func matchParen(s string, open int) int {
	if open < 0 || open >= len(s) || s[open] != '(' {
		return -1
	}
	depth := 0
	for i := open; i < len(s); i++ {
		switch s[i] {
		case '(':
			depth++
		case ')':
			depth--
			if depth == 0 {
				return i
			}
		}
	}
	return -1
}
