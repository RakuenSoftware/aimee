package vector

import (
	"errors"
	"testing"
)

// canonicalKind restates the derivation rule independently of the
// implementation, so this fails if principal.go drifts from what the rest of the
// tree assumes (docs/modules/README.md; server-go/db1, git, roundtable and
// economizer assert the same shape).
func canonicalKind(ref, stage uint32) uint32 { return 4096 + ref*256 + stage }

func TestProviderKindFollowsTheCanonicalRule(t *testing.T) {
	for _, ref := range []uint32{ProviderRefFirst, ProviderRefFirst + 1, ProviderRefLimit - 1} {
		for _, stage := range []uint32{0, 1, 2, 255} {
			got, err := ProviderKind(ref, stage)
			if err != nil {
				t.Fatalf("ProviderKind(%d,%d): %v", ref, stage, err)
			}
			if want := canonicalKind(ref, stage); got != want {
				t.Errorf("ProviderKind(%d,%d) = %d, want %d", ref, stage, got, want)
			}
		}
	}
}

// The regression this exists to prevent. Canonical module refs are 1..30, and a
// ref reserves a whole 256-kind block whether or not it uses every stage -- so a
// provider claiming ref 28 would derive postgres's kinds and one of them would be
// denied at attach.
func TestProviderRefsCannotReachACanonicalModuleBlock(t *testing.T) {
	const highestCanonicalRef = 30 // db1
	if ProviderRefFirst <= highestCanonicalRef {
		t.Fatalf("provider band starts at ref %d, inside the canonical range 1..%d",
			ProviderRefFirst, highestCanonicalRef)
	}
	lastCanonicalKind := canonicalKind(highestCanonicalRef, 255)
	firstProviderKind := canonicalKind(ProviderRefFirst, 0)
	if firstProviderKind <= lastCanonicalKind {
		t.Fatalf("provider kinds start at %d, inside the canonical space ending at %d",
			firstProviderKind, lastCanonicalKind)
	}
}

// The provider band and the plugin band must not overlap either: two dynamic
// allocators drawing from one range is the same defect as a module inside a band.
// The plugin band is [200,456) -- see server-go/modules/mcp and the canonical
// inventory, which enforces non-overlap for both.
func TestProviderBandDoesNotOverlapThePluginBand(t *testing.T) {
	const pluginFirst, pluginLimit uint32 = 200, 456
	if ProviderRefFirst < pluginLimit && pluginFirst < ProviderRefLimit {
		t.Fatalf("provider band [%d,%d) overlaps the plugin band [%d,%d)",
			ProviderRefFirst, ProviderRefLimit, pluginFirst, pluginLimit)
	}
}

func TestValidateProviderRefRefusesOutOfBand(t *testing.T) {
	// 7, 28, 29 and 30 are memory, postgres, db2 and db1. 200 and 455 belong to
	// the plugin band. All must be refused.
	for _, ref := range []uint32{0, 1, 7, 28, 29, 30, 200, 455,
		ProviderRefLimit, ProviderRefLimit + 1} {
		if err := ValidateProviderRef(ref); err == nil {
			t.Errorf("ValidateProviderRef(%d) accepted an out-of-band ref", ref)
		} else if !errors.Is(err, ErrProviderRef) {
			t.Errorf("ValidateProviderRef(%d) = %v, want ErrProviderRef", ref, err)
		}
	}
}

func TestProviderKindRefusesAStageOutsideTheBlock(t *testing.T) {
	// A ref owns exactly 256 kinds. Stage 256 would silently land on the NEXT
	// ref's block, which is how one provider ends up serving another's kind.
	if _, err := ProviderKind(ProviderRefFirst, 256); err == nil {
		t.Error("ProviderKind accepted stage 256, which belongs to the next ref")
	}
}

func TestProviderKindsDoNotOverlapBetweenProviders(t *testing.T) {
	seen := map[uint32]uint32{}
	for ref := ProviderRefFirst; ref < ProviderRefLimit; ref++ {
		for _, stage := range []uint32{0, 1, 2} {
			k, err := ProviderKind(ref, stage)
			if err != nil {
				t.Fatal(err)
			}
			if prev, dup := seen[k]; dup {
				t.Fatalf("kind %d derived for refs %d and %d", k, prev, ref)
			}
			seen[k] = ref
		}
	}
}
