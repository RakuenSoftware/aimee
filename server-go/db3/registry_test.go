package db3

import "testing"

func ready(generation uint64) Capabilities {
	return Capabilities{
		Generation: generation, Operations: OperationSearch | OperationApply,
		Metrics: MetricCosine, Filters: FilterExact,
		MaxDimension: MaxDimension, MaxBatch: 8, MaxTopK: MaxTopK, Ready: true,
	}
}

func TestAnEmptyRegistryIsTheNormalState(t *testing.T) {
	// A DB3 provider is optional. A deployment that installs none has an empty
	// registry forever, and that must never read as an error.
	registry := NewProviderRegistry()
	if _, _, ok := registry.Selected(); ok {
		t.Fatal("an empty registry selected a provider")
	}
	if registry.Len() != 0 {
		t.Errorf("Len = %d, want 0", registry.Len())
	}
}

func TestOnlyAProviderBandPrincipalIsAccepted(t *testing.T) {
	// The registry is fed from the bus, so a principal that reached it without
	// going through a provider's own startup checks is exactly the case those
	// checks cannot cover. 28, 29 and 30 are postgres, db2 and db1.
	registry := NewProviderRegistry()
	for _, bad := range []uint32{0, 1, 28, 29, 30, 199, ProviderRefFirst - 1, ProviderRefLimit} {
		if registry.Observe(bad, 1, 1, ready(3)) {
			t.Errorf("accepted out-of-band principal %d", bad)
		}
	}
	if registry.Len() != 0 {
		t.Errorf("out-of-band principals were stored (Len = %d)", registry.Len())
	}
	if !registry.Observe(ProviderRefFirst, 1, 1, ready(3)) {
		t.Error("refused an in-band principal")
	}
}

func TestAStaleAnnouncementCannotMoveAProviderBackwards(t *testing.T) {
	// Including backwards into "ready" after it has said it is not.
	registry := NewProviderRegistry()
	registry.Observe(ProviderRefFirst, 1, 5, ready(9))
	unready := ready(4)
	unready.Ready = false
	if registry.Observe(ProviderRefFirst, 1, 4, unready) {
		t.Fatal("an out-of-order announcement was accepted")
	}
	if _, generation, ok := registry.Selected(); !ok || generation != 9 {
		t.Errorf("selected generation %d (ok=%v), want 9", generation, ok)
	}
	// A NEW attachment (different handle) is not stale, whatever its sequence.
	if !registry.Observe(ProviderRefFirst, 2, 1, ready(11)) {
		t.Fatal("a re-attached provider was treated as stale")
	}
}

func TestUnreadyAndSearchlessProvidersAreNotSelected(t *testing.T) {
	registry := NewProviderRegistry()
	unready := ready(3)
	unready.Ready = false
	registry.Observe(ProviderRefFirst, 1, 1, unready)
	if _, _, ok := registry.Selected(); ok {
		t.Error("an unready provider was selected")
	}

	registry = NewProviderRegistry()
	applyOnly := ready(3)
	applyOnly.Operations = OperationApply
	registry.Observe(ProviderRefFirst, 1, 1, applyOnly)
	if _, _, ok := registry.Selected(); ok {
		t.Error("a provider that does not serve search was selected")
	}

	registry = NewProviderRegistry()
	noGeneration := ready(0)
	registry.Observe(ProviderRefFirst, 1, 1, noGeneration)
	if _, _, ok := registry.Selected(); ok {
		t.Error("a provider with no generation was selected")
	}
}

func TestSelectionIsHighestGenerationAndStable(t *testing.T) {
	registry := NewProviderRegistry()
	registry.Observe(ProviderRefFirst, 1, 1, ready(4))
	registry.Observe(ProviderRefFirst+1, 1, 1, ready(9))
	principal, generation, ok := registry.Selected()
	if !ok || principal != ProviderRefFirst+1 || generation != 9 {
		t.Fatalf("selected %d at %d, want %d at 9", principal, generation, ProviderRefFirst+1)
	}

	// A tie must not depend on map order, or the route would move between
	// identical calls and a search would answer from a different store.
	registry = NewProviderRegistry()
	registry.Observe(ProviderRefFirst+3, 1, 1, ready(5))
	registry.Observe(ProviderRefFirst, 1, 1, ready(5))
	registry.Observe(ProviderRefFirst+7, 1, 1, ready(5))
	for i := 0; i < 32; i++ {
		if principal, _, _ := registry.Selected(); principal != ProviderRefFirst {
			t.Fatalf("tie broke to %d on attempt %d; selection is not stable", principal, i)
		}
	}
}

func TestRemoveOnlyForgetsTheAttachmentItNames(t *testing.T) {
	// A detach notice for a PREVIOUS attachment must not remove the provider
	// that replaced it, or a restart would look like a permanent disappearance.
	registry := NewProviderRegistry()
	registry.Observe(ProviderRefFirst, 1, 1, ready(3))
	registry.Observe(ProviderRefFirst, 2, 1, ready(4))

	if registry.Remove(ProviderRefFirst, 1) {
		t.Fatal("a stale handle removed the current attachment")
	}
	if _, generation, ok := registry.Selected(); !ok || generation != 4 {
		t.Fatalf("the current attachment was lost (generation %d, ok %v)", generation, ok)
	}
	if !registry.Remove(ProviderRefFirst, 2) {
		t.Fatal("the current handle did not remove")
	}
	if _, _, ok := registry.Selected(); ok {
		t.Error("a removed provider is still selected")
	}
}
