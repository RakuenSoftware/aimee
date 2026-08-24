package families

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// The served families against src/modules/process-contracts.json.
//
// WHY THIS IS NOT COVERED BY THE OTHER TWO. catalog_test.go compares the served
// families against operations.json, and cmd/aimee-module/main_test.go compares
// every module's advertised stages against process-contracts.json. Those are
// different documents and neither pairing includes this one:
//
//   operations.json        what the OPERATIONS are, shared with the C clients
//   process-contracts.json what STAGES exist, what the bus routes, what grants permit
//
// And this module is invisible to the main_test guard in particular, because
// moduleConfig cannot build it without a bus socket to reach the postgres
// module -- so it returns !ok, the loop skips it, and the module with the most
// stages of any in the tree is the one that guard never compares.
//
// The failure that leaves open is the one it was written for. git shipped for
// weeks advertising four stages while the contract declared six: the module
// HANDLED both extra kinds and the grant PERMITTED them, but a stage that is
// never advertised is never available, so every call returned CAPABILITY_ABSENT
// in ~40ms -- which reads as "the module is down" while it is plainly running.
//
// Earlier in this module's own history five families were bound to the wrong
// event kinds, and the test that should have caught it held a hand-written
// table copied from the same wrong constants. So this reads BOTH sides from
// their files and derives nothing by hand.

type contractComponent struct {
	ID     string `json:"id"`
	Stages []struct {
		ID        uint32 `json:"id"`
		Name      string `json:"name"`
		EventKind uint32 `json:"event_kind"`
	} `json:"stages"`
}

func loadStoreComponent(t *testing.T) contractComponent {
	t.Helper()
	path := filepath.Join("..", "..", "..", "..", "src", "modules", "process-contracts.json")
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("process-contracts.json not readable at %s: %v", path, err)
	}
	var doc struct {
		Components []contractComponent `json:"components"`
	}
	if err := json.Unmarshal(raw, &doc); err != nil {
		t.Fatalf("process-contracts.json: %v", err)
	}
	for _, c := range doc.Components {
		if c.ID == store.ModuleID {
			return c
		}
	}
	t.Fatalf("process-contracts.json declares no component %q", store.ModuleID)
	return contractComponent{}
}

func TestServedFamiliesMatchTheProcessContract(t *testing.T) {
	component := loadStoreComponent(t)
	if len(component.Stages) == 0 {
		t.Fatalf("%s declares no stages; this test would pass vacuously", store.ModuleID)
	}

	declared := map[uint32]string{}
	for _, s := range component.Stages {
		declared[s.EventKind] = s.Name
	}
	// BOTH dispatch paths, because the module has two and checking one would
	// report the other as missing. Most stages are fields-v2 families;
	// economizer_state speaks the keyed-blob wire and carries its own handler,
	// so it is registered through Bind instead. A caller cannot tell the
	// difference and neither should this: the contract declares stages, not
	// codecs.
	servedKinds := map[uint32]string{}
	for name, family := range served {
		servedKinds[family.Event] = name
	}
	for _, bind := range Binds(nil) {
		servedKinds[bind.Event] = bind.Name
	}

	// Declared but not served: the daemon routes the kind here and the caller
	// gets CAPABILITY_ABSENT from a module that is plainly running.
	for kind, name := range declared {
		if _, ok := servedKinds[kind]; !ok {
			t.Errorf("the contract declares %s (kind %d) but no family serves it",
				name, kind)
		}
	}
	// Served but not declared: no grant permits the kind, so the bus refuses it
	// and the family is dead code that looks live.
	for kind, name := range servedKinds {
		if _, ok := declared[kind]; !ok {
			t.Errorf("family %s serves kind %d, which the contract does not declare",
				name, kind)
		}
	}
}

// The stage id in the contract must be the kind's low half, or the bus cannot
// route what the contract promises. Checked against the formula rather than
// against the module, so this fails if either drifts.
func TestContractStagesFollowTheKindFormula(t *testing.T) {
	component := loadStoreComponent(t)
	base := uint32(4096 + store.PrincipalRef*256)
	for _, s := range component.Stages {
		if want := base + s.ID; s.EventKind != want {
			t.Errorf("%s declares stage %d with kind %d, but 4096 + %d*256 + %d is %d",
				s.Name, s.ID, s.EventKind, store.PrincipalRef, s.ID, want)
		}
	}
}
