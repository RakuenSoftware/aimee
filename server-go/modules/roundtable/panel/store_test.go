package panel

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func writePreset(t *testing.T, dir string, p preset) {
	t.Helper()
	data, err := json.Marshal(p)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, p.Name+".json"), data, 0o600); err != nil {
		t.Fatal(err)
	}
}

func TestConfiguredRoundtablePreservesExactSeatSpecifications(t *testing.T) {
	dir := t.TempDir()
	writePreset(t, dir, preset{Name: "large", MinSuccessful: 2, Discussion: true, DeadlineMS: 12345, Chairman: "$random", ChairmanEnabled: true, Seats: []presetSeat{
		{Selector: "$random", Persona: "security"},
		{Selector: "codex", Persona: "qa"},
		{Selector: "$random", Persona: "architect"},
	}})
	store, _ := NewStore(dir)
	panel, err := store.Resolve("large", []string{"reviewer"}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if !panel.Acquired || panel.Name != "large" || len(panel.Seats) != 3 || panel.MinSuccessful != 2 || !panel.Discussion || panel.DeadlineMS != 12345 || !panel.ChairmanEnabled || panel.Chairman != "$random" {
		t.Fatalf("panel=%+v", panel)
	}
	if panel.Seats[0].Selector != "$random" || panel.Seats[1].Selector != "codex" || panel.Seats[2].Selector != "$random" {
		t.Fatalf("roundtable altered opaque delegate seat specifications: %+v", panel.Seats)
	}
}

func TestEnabledChairmanRequiresSpecification(t *testing.T) {
	dir := t.TempDir()
	writePreset(t, dir, preset{Name: "missing", ChairmanEnabled: true, Seats: []presetSeat{{Selector: "$random"}}})
	store, _ := NewStore(dir)
	if _, err := store.Resolve("missing", nil, nil); err == nil {
		t.Fatal("enabled chairman silently accepted without a delegate specification")
	}
}

func TestConfiguredRoundtableAlwaysRequestsEverySeat(t *testing.T) {
	dir := t.TempDir()
	writePreset(t, dir, preset{Name: "five", Seats: []presetSeat{
		{Selector: "$random"}, {Selector: "$random"}, {Selector: "$random"}, {Selector: "$random"}, {Selector: "$random"},
	}})
	store, _ := NewStore(dir)
	panel, err := store.Resolve("five", nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(panel.Seats) != 5 {
		t.Fatalf("configured capacity was reduced: %+v", panel.Seats)
	}
}

// An unnamed roundtable used to fall back to an implicit two-seat panel that no
// operator had configured — unanimous, chairman-less, and invisible in the
// result. Convening review authority nobody specified must be an error.
func TestUnnamedRoundtableFailsClosedInsteadOfImprovisingAPanel(t *testing.T) {
	dir := t.TempDir()
	writePreset(t, dir, preset{Name: "default", Seats: []presetSeat{{Selector: "$random"}}})
	store, _ := NewStore(dir)
	for _, requested := range []string{"", "   "} {
		if _, err := store.Resolve(requested, []string{"security", "qa", "architect"}, nil); err == nil {
			t.Fatalf("unnamed roundtable %q improvised a panel instead of failing closed", requested)
		}
	}
}

func TestConfiguredRoundtableDefaultsToTenMinuteSafetyBound(t *testing.T) {
	dir := t.TempDir()
	writePreset(t, dir, preset{Name: "default", Seats: []presetSeat{{Selector: "$random"}}})
	store, _ := NewStore(dir)
	panel, err := store.Resolve("default", nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	if panel.DeadlineMS != 600000 {
		t.Fatalf("deadline_ms=%d, want 600000", panel.DeadlineMS)
	}
}

func TestNamedButAbsentRoundtableFailsClosed(t *testing.T) {
	store, _ := NewStore(t.TempDir())
	if _, err := store.Resolve("missing", nil, nil); err == nil {
		t.Fatal("named roundtable that does not exist silently fell back")
	}
}
