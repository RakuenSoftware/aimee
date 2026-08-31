package mcp

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

// bus_host_serve_kind() (core/event_bus/bus_route.c:109) binds one event kind to
// exactly ONE serving slot and refuses a second module that tries to serve it.
// Instances therefore cannot share package-level kinds. Their kinds are derived
// from the principal ref by the canonical rule, 4096 + ref*256 + stage, which is
// the same rule aimee, git, roundtable and economizer assert in their own tests.

// canonicalKind is the rule written out independently of the implementation, so
// this test fails if the derivation in mcp.go drifts from what the rest of the
// tree assumes.
func canonicalKind(ref, stage uint32) uint32 { return 4096 + ref*256 + stage }

func TestEventKindsFollowTheCanonicalRule(t *testing.T) {
	for _, ref := range []uint32{PluginRefFirst, PluginRefFirst + 1, PluginRefLimit - 1} {
		invoke, declare, err := EventKinds(ref)
		if err != nil {
			t.Fatalf("EventKinds(%d): %v", ref, err)
		}
		if want := canonicalKind(ref, StageInvoke); invoke != want {
			t.Errorf("EventKinds(%d) invoke = %d, want %d", ref, invoke, want)
		}
		if want := canonicalKind(ref, StageDeclareCommands); declare != want {
			t.Errorf("EventKinds(%d) declare = %d, want %d", ref, declare, want)
		}
	}
}

// highestCanonicalRef is READ from the inventory rather than transcribed here.
//
// It was `const highestCanonicalRef = 30`, the largest ref today, which would
// be silently wrong the moment a module took a higher one. That nearly
// happened: a new module was allocated ref 31 before folding back into 30.
//
// A transcribed maximum does not fail when it goes stale. It just tests a
// SMALLER canonical space than exists, so the plugin band could grow to overlap
// a real module's block and this would still pass -- the failure mode of a
// guard that quietly stops guarding.
func highestCanonicalRef(t *testing.T) uint32 {
	t.Helper()
	path := filepath.Join("..", "..", "..", "tests", "baselines", "modules",
		"canonical-inventory.yaml")
	body, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("canonical inventory not readable at %s: %v", path, err)
	}
	var inv struct {
		Refs map[string]int `json:"principal_refs"`
	}
	if err := json.Unmarshal(body, &inv); err != nil {
		t.Fatalf("canonical inventory: %v", err)
	}
	if len(inv.Refs) == 0 {
		t.Fatal("the canonical inventory declares no principal_refs")
	}
	var highest uint32
	for _, ref := range inv.Refs {
		if uint32(ref) > highest {
			highest = uint32(ref)
		}
	}
	return highest
}

// The regression this whole change exists for. Canonical module refs start at 1
// and each reserves a full 256-kind block whether or not it uses every stage --
// so postgres (28) owns 11264..11519 even though its grant names only 11265.
// The retired plugin range started at 11264 and squatted postgres, db2 and the
// store module.
func TestPluginKindsCannotLandInACanonicalModuleBlock(t *testing.T) {
	highestCanonicalRef := highestCanonicalRef(t)
	firstPluginKind := canonicalKind(PluginRefFirst, 0)
	lastCanonicalKind := canonicalKind(highestCanonicalRef, 255)
	if firstPluginKind <= lastCanonicalKind {
		t.Fatalf("plugin band starts at kind %d, inside the canonical space that ends at %d",
			firstPluginKind, lastCanonicalKind)
	}

	// And concretely: no ref in the band may reproduce a kind any canonical ref owns.
	canonical := map[uint32]uint32{}
	for ref := uint32(1); ref <= highestCanonicalRef; ref++ {
		for stage := uint32(0); stage < 256; stage++ {
			canonical[canonicalKind(ref, stage)] = ref
		}
	}
	for ref := PluginRefFirst; ref < PluginRefLimit; ref++ {
		invoke, declare, err := EventKinds(ref)
		if err != nil {
			t.Fatalf("EventKinds(%d): %v", ref, err)
		}
		for _, k := range []uint32{invoke, declare} {
			if owner, clash := canonical[k]; clash {
				t.Fatalf("plugin ref %d derives kind %d, owned by canonical ref %d",
					ref, k, owner)
			}
		}
	}
}

func TestEventKindsDoNotOverlapBetweenInstances(t *testing.T) {
	seen := map[uint32]uint32{}
	for ref := PluginRefFirst; ref < PluginRefLimit; ref++ {
		invoke, declare, err := EventKinds(ref)
		if err != nil {
			t.Fatal(err)
		}
		for _, k := range []uint32{invoke, declare} {
			if prev, dup := seen[k]; dup {
				t.Fatalf("kind %d derived for refs %d and %d", k, prev, ref)
			}
			seen[k] = ref
		}
	}
}

func TestEventKindsRefusesRefsOutsideTheBand(t *testing.T) {
	// 28, 29 and 30 are postgres, db2 and aimee. A plugin instance must never be
	// able to derive kinds from a canonical module's ref.
	for _, ref := range []uint32{0, 1, 28, 29, 30, PluginRefFirst - 1, PluginRefLimit,
		PluginRefLimit + 1} {
		if _, _, err := EventKinds(ref); err == nil {
			t.Errorf("EventKinds(%d) was accepted", ref)
		} else if !errors.Is(err, ErrPluginRef) {
			t.Errorf("EventKinds(%d) error = %v, want ErrPluginRef", ref, err)
		}
	}
}
