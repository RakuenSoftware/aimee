package roundtable

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestConfiguredRoundtableExactSeatsAndDefaultRouting(t *testing.T) {
	dir := t.TempDir()
	p := preset{Name: "large", MinSuccessful: 4, Seats: []presetSeat{
		{Model: "$random", Persona: "security"},
		{Model: "$random", Persona: "qa"},
		{Model: "$random", Persona: "architect"},
		{Model: "$random", Persona: "reviewer"},
		{Model: "$random", Persona: "constructive-reviewer"},
	}}
	data, _ := json.Marshal(p)
	if err := os.WriteFile(filepath.Join(dir, "large.json"), data, 0o600); err != nil {
		t.Fatal(err)
	}
	store, _ := NewStore(dir, func() (string, error) { return "large", nil })
	panel, err := store.Resolve("", []Agent{
		{Name: "codex", Provider: "openai", MaxParallel: 3},
		{Name: "minimax", Provider: "minimax", MaxParallel: 2},
	}, []string{"reviewer"}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if !panel.Acquired || panel.Name != "large" || len(panel.Seats) != 5 || panel.MinSuccessful != 4 {
		t.Fatalf("panel=%+v", panel)
	}
	if panel.Seats[0].Agent != "codex" || panel.Seats[1].Agent != "minimax" ||
		panel.Seats[2].Agent != "codex" || panel.Seats[3].Agent != "minimax" ||
		panel.Seats[4].Agent != "codex" {
		t.Fatalf("provider-diverse ordering not preserved: %+v", panel.Seats)
	}
}

func TestConfiguredRoundtableDoesNotRunPartiallyFilled(t *testing.T) {
	dir := t.TempDir()
	p := preset{Name: "five", Seats: []presetSeat{
		{Model: "$random"}, {Model: "$random"}, {Model: "$random"},
		{Model: "$random"}, {Model: "$random"},
	}}
	data, _ := json.Marshal(p)
	if err := os.WriteFile(filepath.Join(dir, "five.json"), data, 0o600); err != nil {
		t.Fatal(err)
	}
	store, _ := NewStore(dir, func() (string, error) { return "five", nil })
	_, err := store.Resolve("", []Agent{
		{Name: "codex", Provider: "openai", MaxParallel: 2},
		{Name: "minimax", Provider: "minimax", MaxParallel: 2},
	}, nil, nil)
	if err == nil {
		t.Fatal("five-seat roundtable silently ran with fewer than five agents")
	}
}

func TestDirectFallbackHardCapsTwoWithProviderDiversity(t *testing.T) {
	store, _ := NewStore(t.TempDir(), func() (string, error) { return "", nil })
	panel, err := store.Resolve("", []Agent{
		{Name: "codex-a", Provider: "openai", MaxParallel: 10},
		{Name: "codex-b", Provider: "openai", MaxParallel: 10},
		{Name: "minimax", Provider: "minimax", MaxParallel: 4},
	}, []string{"security", "qa", "architect"}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if panel.Acquired || len(panel.Seats) != 2 || panel.Seats[0].Agent != "codex-a" || panel.Seats[1].Agent != "minimax" {
		t.Fatalf("panel=%+v", panel)
	}
}

func TestMissingConfiguredDefaultFailsClosed(t *testing.T) {
	store, _ := NewStore(t.TempDir(), func() (string, error) { return "missing", nil })
	if _, err := store.Resolve("", []Agent{{Name: "codex", MaxParallel: 2}}, nil, nil); err == nil {
		t.Fatal("missing configured default silently fell back")
	}
}
