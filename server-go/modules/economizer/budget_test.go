package economizer

import "testing"

// Ported from src/tests/test_fold_budget.c.
//
// The C tests pass a MODEL NAME and let fold_budget_resolve consult
// model_context_window(). The Go port takes the resolved window as an argument
// instead, so the module stays free of the model registry — the same reason the
// freeze guardrail takes rates rather than a model name. The cases below
// therefore pass the windows those models publish (1000000 for
// claude-opus-4-8 / gemini-1.5-pro) rather than the names.

func TestBudgetKnownWindowDefaults(t *testing.T) {
	const w = 1000000
	b := ResolveBudget(w, nil)
	if !b.IsKnown {
		t.Error("a resolved window must be reported as known")
	}
	if b.ContextWindowTokens != w {
		t.Errorf("window = %d, want %d", b.ContextWindowTokens, w)
	}
	for _, c := range []struct {
		name string
		got  int
		want int
	}{
		{"retained band", b.RetainedBandTokens, w * 25 / 100},
		{"tail cap", b.TailCapTokens, w * 15 / 100},
		{"pressure ceiling", b.PressureCeilingTokens, w * 85 / 100},
		{"prefix saturation", b.PrefixSaturationTokens, w * 50 / 100},
		{"closet budget", b.ClosetBudgetTokens, BudgetDefaultClosetTokens},
	} {
		if c.got != c.want {
			t.Errorf("%s = %d, want %d", c.name, c.got, c.want)
		}
	}
}

// An unknown window falls back, and says so — a caller that treated a guessed
// window as authoritative would size every lever off an unverified number.
func TestBudgetUnknownWindowFallback(t *testing.T) {
	b := ResolveBudget(0, nil)
	if b.IsKnown {
		t.Error("a fallback window must not be reported as known")
	}
	if b.ContextWindowTokens != BudgetDefaultWindow {
		t.Errorf("window = %d, want %d", b.ContextWindowTokens, BudgetDefaultWindow)
	}

	// An operator-configured window is used when the model is unknown.
	c := ResolveBudget(0, &BudgetConfig{ContextWindowTokens: 64000})
	if c.IsKnown || c.ContextWindowTokens != 64000 {
		t.Errorf("configured fallback wrong: known=%v window=%d", c.IsKnown, c.ContextWindowTokens)
	}
	if c.PressureCeilingTokens != 54400 { // 85% of 64000
		t.Errorf("pressure ceiling = %d, want 54400", c.PressureCeilingTokens)
	}
}

func TestBudgetOverrides(t *testing.T) {
	const w = 1000000
	cfg := &BudgetConfig{RetainedBandPct: 10, TailCapPct: 5, ClosetBudgetTokens: 1000}
	b := ResolveBudget(w, cfg)
	if b.RetainedBandTokens != 100000 { // 10%
		t.Errorf("retained band = %d, want 100000", b.RetainedBandTokens)
	}
	if b.TailCapTokens != 50000 { // 5%
		t.Errorf("tail cap = %d, want 50000", b.TailCapTokens)
	}
	if b.ClosetBudgetTokens != 1000 {
		t.Errorf("closet = %d, want 1000", b.ClosetBudgetTokens)
	}
	// Unset knobs still take their defaults.
	if b.PressureCeilingTokens != 850000 { // 85%
		t.Errorf("pressure ceiling = %d, want 850000", b.PressureCeilingTokens)
	}
}

func TestBudgetDeterminism(t *testing.T) {
	cfg := &BudgetConfig{RetainedBandPct: 30, ClosetBudgetTokens: 777}
	a := ResolveBudget(1000000, cfg)
	b := ResolveBudget(1000000, cfg)
	if a != b {
		t.Error("identical inputs must produce identical output")
	}
	if a.ContextWindowTokens != 1000000 {
		t.Errorf("window = %d", a.ContextWindowTokens)
	}
}

// Documented contract: a percentage over 100 clamps to the whole window, and a
// closet budget larger than the window clamps to the window (§7's single
// enforcement point).
func TestBudgetClamping(t *testing.T) {
	const w = 1000000
	if b := ResolveBudget(w, &BudgetConfig{RetainedBandPct: 150}); b.RetainedBandTokens != w {
		t.Errorf("pct > 100 should clamp to the window, got %d", b.RetainedBandTokens)
	}
	if c := ResolveBudget(w, &BudgetConfig{ClosetBudgetTokens: w + 1000000}); c.ClosetBudgetTokens != w {
		t.Errorf("closet over window should clamp, got %d", c.ClosetBudgetTokens)
	}
}

// A zero or negative percentage yields zero rather than a negative band.
func TestBudgetZeroAndNegative(t *testing.T) {
	if got := pctOf(1000, 0); got != 0 {
		t.Errorf("pctOf(1000,0) = %d, want 0", got)
	}
	if got := pctOf(1000, -5); got != 0 {
		t.Errorf("pctOf(1000,-5) = %d, want 0", got)
	}
	if got := pctOf(0, 50); got != 0 {
		t.Errorf("pctOf(0,50) = %d, want 0", got)
	}
}
