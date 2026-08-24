package vector

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

// The provider band is declared in two places and they have to agree.
//
// principal.go says its constants "MUST match db3_provider_principal_ref_band
// in the canonical inventory". That was prose, and prose does not fail. Both
// halves are load-bearing in opposite directions:
//
//   - the inventory gate refuses to give any MODULE a ref inside the band
//   - ValidateProviderRef refuses any PROVIDER a ref outside it
//
// So a disagreement opens a gap on whichever side is narrower, and the refs in
// the gap are exactly the ones both allocators believe they own. What happens
// then is not a clean error: bus_host_serve_kind binds one kind to one serving
// slot, so whichever attaches second is denied -- possibly the core module,
// with the only trace in its own log.
//
// The keys were in fact ABSENT from the inventory when this test was written,
// so the gate was not running at all and this constant pointed at nothing.

type refBand struct {
	First int `json:"first"`
	Limit int `json:"limit"`
}

type canonicalInventory struct {
	PluginBand   refBand        `json:"plugin_principal_ref_band"`
	ProviderBand refBand        `json:"db3_provider_principal_ref_band"`
	Refs         map[string]int `json:"principal_refs"`
}

func loadInventory(t *testing.T) canonicalInventory {
	t.Helper()
	path := filepath.Join("..", "..", "tests", "baselines", "modules",
		"canonical-inventory.yaml")
	body, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("canonical inventory not readable at %s: %v", path, err)
	}
	var inv canonicalInventory
	if err := json.Unmarshal(body, &inv); err != nil {
		t.Fatalf("canonical inventory: %v", err)
	}
	return inv
}

func TestTheProviderBandMatchesTheCanonicalInventory(t *testing.T) {
	inv := loadInventory(t)

	if inv.ProviderBand.First == 0 && inv.ProviderBand.Limit == 0 {
		t.Fatal("the inventory declares no db3_provider_principal_ref_band; " +
			"the gate that keeps modules out of the provider band is not running")
	}
	if got, want := int(ProviderRefFirst), inv.ProviderBand.First; got != want {
		t.Errorf("ProviderRefFirst = %d, inventory says %d", got, want)
	}
	if got, want := int(ProviderRefLimit), inv.ProviderBand.Limit; got != want {
		t.Errorf("ProviderRefLimit = %d, inventory says %d", got, want)
	}
}

// No module may sit inside the provider band. The inventory gate checks this
// too, but from the other side of the fence: this asserts it in the package
// that would be harmed, so a module added to the inventory without running the
// Python lint still fails a Go test.
func TestNoModuleSitsInsideTheProviderBand(t *testing.T) {
	inv := loadInventory(t)
	if len(inv.Refs) == 0 {
		t.Fatal("the inventory declares no principal_refs")
	}
	for module, ref := range inv.Refs {
		if err := ValidateProviderRef(uint32(ref)); err == nil {
			t.Errorf("module %q has ref %d, which is inside the provider band "+
				"[%d,%d) -- its kinds would collide with a provisioned provider",
				module, ref, ProviderRefFirst, ProviderRefLimit)
		}
	}
}

// The two reserved bands must not overlap, for the same reason a module must
// not sit in one: two allocators drawing from one range hand out the same ref
// twice and neither can tell.
func TestTheReservedBandsDoNotOverlap(t *testing.T) {
	inv := loadInventory(t)
	p, d := inv.PluginBand, inv.ProviderBand
	if p.First == 0 && p.Limit == 0 {
		t.Fatal("the inventory declares no plugin_principal_ref_band")
	}
	if p.First < d.Limit && d.First < p.Limit {
		t.Errorf("plugin band [%d,%d) overlaps provider band [%d,%d)",
			p.First, p.Limit, d.First, d.Limit)
	}
}
