package families

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// The catalog is the contract. This checks the served families against it
// directly, which is the only thing that catches an operation that was read,
// understood and then never wired -- a gap no behavioural test can see, because
// the test would have to know to ask for the op that is missing.
//
// It also catches an arity that disagrees with the declared request, which
// would refuse every well-formed call to that operation.

// catalogField is recursive because a request field can be a struct, and can
// repeat. Both matter, because the wire is FLAT: a struct field of 5 members
// repeated 8 times occupies 40 slots, and a plain text field repeated 64 times
// occupies 64. An op's real arity is this expansion, never len(fields).
type catalogField struct {
	Name   string         `json:"name"`
	Type   string         `json:"type"`
	Repeat int            `json:"repeat"`
	Fields []catalogField `json:"fields"`
}

// flatArity is the number of slots these fields occupy on the wire.
func flatArity(fields []catalogField) int {
	total := 0
	for _, f := range fields {
		repeat := f.Repeat
		if repeat == 0 {
			repeat = 1
		}
		width := 1
		if f.Type == "struct" {
			width = flatArity(f.Fields)
		}
		total += repeat * width
	}
	return total
}

type catalogOp struct {
	Family  string `json:"family"`
	ID      uint32 `json:"id"`
	Name    string `json:"name"`
	Request struct {
		Fields []catalogField `json:"fields"`
	} `json:"request"`
	Reply struct {
		Fields []catalogField `json:"fields"`
		// List is set when the reply is a repeating row rather than one answer.
		List *struct {
			MaxRows int `json:"max_rows"`
		} `json:"list"`
	} `json:"reply"`
}

type catalogFamily struct {
	Name  string `json:"name"`
	Event uint32 `json:"event_kind"`
}

type catalog struct {
	Operations []catalogOp     `json:"operations"`
	FamilyList []catalogFamily `json:"families"`
	// Families is FamilyList by name, filled in by loadCatalog.
	Families map[string]uint32 `json:"-"`
}

func loadCatalog(t *testing.T) catalog {
	t.Helper()
	// The test runs from this package's directory; the catalog sits beside the
	// module that serves it.
	//
	// Not Skipf on a read failure. This pointed at the deleted C module for a
	// while and every check in this file skipped in silence -- the one outcome a
	// contract test must never have, because it reported success for exactly as
	// long as it was reading nothing.
	path := filepath.Join("..", "operations.json")
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("catalog not readable at %s: %v", path, err)
	}
	var c catalog
	if err := json.Unmarshal(raw, &c); err != nil {
		t.Fatalf("catalog is not valid JSON: %v", err)
	}
	if len(c.Operations) == 0 {
		t.Fatalf("catalog declares no operations")
	}
	if len(c.FamilyList) == 0 {
		t.Fatalf("catalog declares no families")
	}
	c.Families = make(map[string]uint32, len(c.FamilyList))
	for _, f := range c.FamilyList {
		c.Families[f.Name] = f.Event
	}
	return c
}

// served maps each migrated family's catalog name to its Family value.
var served = map[string]store.Family{
	"jti_replay":      JTIReplay,
	"mgmt_jwks":       MgmtJWKS,
	"mgmt_nonce":      MgmtNonce,
	"identity":        Identity,
	"checkpoints":     Checkpoints,
	"git_ownership":   GitOwnership,
	"guardrail_state": GuardrailState,
	"pki":             PKI,
	"sessions":        Sessions,
	"roundtable":      Roundtable,
	"delegation":      Delegation,
	"lifecycle":       Lifecycle,
	"conversation":    Conversation,
	"ensemble":        Ensemble,
	"telemetry":       Telemetry,
	"workflow":        Workflow,
	"agent_work":      AgentWork,
	"runtime":         Runtime,
}

func TestServedFamiliesCoverTheirCatalogOperations(t *testing.T) {
	c := loadCatalog(t)

	declared := map[string]map[uint32]catalogOp{}
	for _, op := range c.Operations {
		if _, ok := served[op.Family]; !ok {
			continue
		}
		if declared[op.Family] == nil {
			declared[op.Family] = map[uint32]catalogOp{}
		}
		declared[op.Family][op.ID] = op
	}

	for name, family := range served {
		t.Run(name, func(t *testing.T) {
			want := declared[name]
			if len(want) == 0 {
				t.Fatalf("the catalog declares no operations for %s", name)
			}
			for id, op := range want {
				got, ok := family.Ops[id]
				if !ok {
					t.Errorf("op %d (%s) is declared but not served", id, op.Name)
					continue
				}
				if got.Name != op.Name {
					t.Errorf("op %d is served as %q, catalog calls it %q", id, got.Name, op.Name)
				}
				// Arity is the half of the contract a name check cannot see. An
				// op that declares len(fields) where the request expands --
				// nested structs, repeated scalars -- refuses every well-formed
				// call to itself, and no behavioural test written against that
				// same wrong arity would notice.
				if want := flatArity(op.Request.Fields); got.Args >= 0 && got.Args != want {
					t.Errorf("op %d (%s) accepts %d fields, the catalog's request expands to %d",
						id, op.Name, got.Args, want)
				}
			}
			for id, op := range family.Ops {
				if _, ok := want[id]; !ok {
					t.Errorf("op %d (%s) is served but not declared", id, op.Name)
				}
			}
		})
	}
}

// An operation that answers with rows must answer with the row the contract
// declares.
//
// This is the half of the contract that is easiest to get wrong and hardest to
// notice. A reply width inferred from the table -- rather than read from the
// catalog -- can be short by exactly the columns that are computed rather than
// stored, and every behavioural test written afterwards agrees with it, because
// the test is written to the same wrong shape. The caller is the one that finds
// out, by reading cell 9 of a 7-cell row.
//
// Single-answer replies are checked too, from width two up. They used to be
// exempt, on the reasoning that whatever test reads such a reply pins its
// shape. Ten ops shipped one cell where the C had always emitted the value AND
// its rc, and no test noticed, because the tests were written to the same wrong
// shape the handler had. The catalog is the only account of the wire that was
// not written by the port.
//
// Width one is left out: a one-cell reply cannot be wrong by width alone, and
// the dispatch check already refuses a reply that is not a whole number of rows.
func TestListRepliesCarryTheRowTheCatalogDeclares(t *testing.T) {
	c := loadCatalog(t)

	unset := 0
	for _, op := range c.Operations {
		family, ok := served[op.Family]
		if !ok || len(op.Reply.Fields) == 0 {
			continue
		}
		if op.Reply.List == nil && flatArity(op.Reply.Fields) < 2 {
			continue
		}
		spec, ok := family.Ops[op.ID]
		if !ok {
			continue // covered by the coverage test above
		}
		want := flatArity(op.Reply.Fields)
		switch {
		case spec.Cells == 0:
			unset++
			t.Errorf("%s op %d (%s) answers with rows but declares no Cells; "+
				"the catalog says one row is %d cells",
				op.Family, op.ID, op.Name, want)
		case spec.Cells != want:
			t.Errorf("%s op %d (%s) answers with %d cells per row, "+
				"the catalog declares %d",
				op.Family, op.ID, op.Name, spec.Cells, want)
		}
	}
	if unset > 0 {
		t.Logf("%d list operations have no declared row width", unset)
	}
}

// A served family must answer on the kind and stage the catalog assigned it, or
// the bus routes its frames somewhere else entirely.
//
// The expectations come from the CATALOG, never from a table written beside the
// code. This test used to carry the kinds inline, transcribed from the same
// constants it was checking; when five families were given each other's kinds
// the table was updated to match and the test kept passing. Every call to those
// families reached a different family's handler, and nothing else could see it:
// the other Go tests call handlers directly and never touch a kind, the SQL
// suites test SQL, and the C suites ran with no module attached. It surfaced
// only against a live module.
func TestServedFamiliesUseTheirDeclaredKindAndStage(t *testing.T) {
	c := loadCatalog(t)

	// kind = 4096 + principal_ref*256 + stage. The ref is READ from the module
	// rather than written as 30 here: a transcribed ref is right until the day
	// it moves, and then this test agrees with a second copy of the old answer
	// instead of failing.
	kindBase := uint32(4096 + store.PrincipalRef*256)

	checked := 0
	for name, family := range served {
		want, ok := c.Families[name]
		if !ok {
			t.Errorf("%s is served but the catalog declares no family by that name", name)
			continue
		}
		checked++
		if family.Event != want {
			t.Errorf("%s serves kind %d, the catalog assigns it %d", name, family.Event, want)
		}
		// The stage is not independently declared: it is the kind's low half,
		// and a family whose two disagree is one the bus cannot route.
		if got, expect := family.Stage, want-kindBase; got != expect {
			t.Errorf("%s serves stage %d, but its kind %d puts it at stage %d",
				name, got, want, expect)
		}
	}
	if checked != len(served) {
		t.Errorf("checked %d of %d served families", checked, len(served))
	}
}

// Every operation must have a body, and exactly one kind of body. A family
// entry with neither is an op that answers StatusFailed for every call.
func TestEveryServedOperationHasExactlyOneBody(t *testing.T) {
	for name, family := range served {
		for id, op := range family.Ops {
			hasRun := op.Run != nil
			hasRunDB := op.RunDB != nil
			switch {
			case !hasRun && !hasRunDB:
				t.Errorf("%s op %d (%s) has no body", name, id, op.Name)
			case hasRun && hasRunDB:
				t.Errorf("%s op %d (%s) has both Run and RunDB", name, id, op.Name)
			}
			if op.Name == "" {
				t.Errorf("%s op %d has no name", name, id)
			}
		}
	}
}

// Every recursive walk must terminate, and there are exactly two ways to make
// one terminate. Which is correct depends on the columns it collects:
//
//	dedup   the walk collects only stable columns, so on a cycle its rows
//	        repeat and UNION ends it. This is the better mechanism: it gives
//	        the RIGHT answer on a cycle, not merely a bounded wrong one.
//	bound   the walk carries a varying column (a depth it orders by), so its
//	        rows never repeat and no dedup can converge. Only an explicit bound
//	        stops it.
//
// Choosing the wrong one is the mistake this pins. Adding a depth to a walk
// that could have deduped defeats the dedup and turns a three-row answer into
// sixty-five identical ones -- terminating, but wrong. Relying on dedup in a
// walk that carries a depth does not terminate at all.
func TestEveryRecursiveWalkTerminatesTheRightWay(t *testing.T) {
	for _, test := range []struct {
		name    string
		sql     string
		byDepth bool
	}{
		{"work-item subtree", treeCTE, false},
		{"orphan sweep", orphanCTE, false},
		{"budget root climb", budgetRootSQL, false},
		{"tree availability", treeAvailabilitySQL, false},
		{"tree spend", treeSpentSQL, false},
		{"tree spend except one", treeSpentExceptSQL, false},
		{"delegation descendants", spawnCountDescendantsSQL, true},
		{"delegation ancestors", spawnFindRootSQL, true},
		{"delegation cancel subtree", spawnCancelRecursiveSQL, true},
	} {
		t.Run(test.name, func(t *testing.T) {
			hasDepth := strings.Contains(test.sql, "depth")
			dedupes := !strings.Contains(test.sql, "UNION ALL")

			if hasDepth != test.byDepth {
				if hasDepth {
					t.Fatalf("carries a depth column, defeating the dedup that would " +
						"otherwise give the right answer on a cycle")
				}
				t.Fatalf("expected a depth bound and found none")
			}
			if !hasDepth && !dedupes {
				t.Fatalf("uses UNION ALL with no bound: this never terminates on a cycle")
			}
			if hasDepth && !strings.Contains(test.sql, "depth <") {
				t.Fatalf("carries a depth but never compares it, so nothing bounds the walk")
			}
		})
	}
}
