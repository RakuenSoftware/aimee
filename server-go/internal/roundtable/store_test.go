package roundtable

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
	store, _ := NewStore(dir, func() (string, error) { return "large", nil })
	panel, err := store.Resolve("", []string{"reviewer"}, nil)
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
	store, _ := NewStore(dir, func() (string, error) { return "missing", nil })
	if _, err := store.Resolve("", nil, nil); err == nil {
		t.Fatal("enabled chairman silently accepted without a delegate specification")
	}
}

func TestConfiguredRoundtableAlwaysRequestsEverySeat(t *testing.T) {
	dir := t.TempDir()
	writePreset(t, dir, preset{Name: "five", Seats: []presetSeat{
		{Selector: "$random"}, {Selector: "$random"}, {Selector: "$random"}, {Selector: "$random"}, {Selector: "$random"},
	}})
	store, _ := NewStore(dir, func() (string, error) { return "five", nil })
	panel, err := store.Resolve("", nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(panel.Seats) != 5 {
		t.Fatalf("configured capacity was reduced: %+v", panel.Seats)
	}
}

func TestDirectFallbackHardCapsTwoAndLeavesRoutingToDelegate(t *testing.T) {
	store, _ := NewStore(t.TempDir(), func() (string, error) { return "", nil })
	panel, err := store.Resolve("", []string{"security", "qa", "architect"}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if panel.Acquired || len(panel.Seats) != 2 || panel.Seats[0].Selector != "" || panel.Seats[1].Selector != "" {
		t.Fatalf("panel=%+v", panel)
	}
}

func TestMissingConfiguredDefaultFailsClosed(t *testing.T) {
	store, _ := NewStore(t.TempDir(), func() (string, error) { return "missing", nil })
	if _, err := store.Resolve("", nil, nil); err == nil {
		t.Fatal("missing configured default silently fell back")
	}
}
