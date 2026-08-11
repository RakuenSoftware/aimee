package economizer

// Per-model context-fold budget resolver (fold §7).
//
// Ported from src/modules/economizer/fold_budget.c. Turns a model's context
// window into the token budgets the fold works against, so the levers are sized
// to the window rather than to hard-coded message counts.

const (
	BudgetDefaultWindow        = 200000
	BudgetDefaultRetainedPct   = 25
	BudgetDefaultTailCapPct    = 15
	BudgetDefaultPressurePct   = 85
	BudgetDefaultSaturationPct = 50
	BudgetDefaultClosetTokens  = 512
)

// BudgetConfig carries operator overrides. Zero means "use the default".
type BudgetConfig struct {
	ContextWindowTokens int
	RetainedBandPct     int
	TailCapPct          int
	PressureCeilingPct  int
	PrefixSaturationPct int
	ClosetBudgetTokens  int
}

// Budget is the resolved per-model budget.
type Budget struct {
	// IsKnown reports whether the window came from the model registry rather
	// than a fallback. A caller that treats a guessed window as authoritative
	// would size every lever off a number nobody verified, so the distinction is
	// surfaced rather than folded into the value.
	IsKnown bool

	ContextWindowTokens    int
	RetainedBandTokens     int
	TailCapTokens          int
	PressureCeilingTokens  int
	PrefixSaturationTokens int
	ClosetBudgetTokens     int
}

// pctOf is pct of window, clamped to [0, window].
//
// The C original computes the intermediate product in a fixed-width 64-bit type
// because `long` is 32 bits on LLP64 and ILP32, where window*pct would overflow
// before any clamp could run. Go's int is 64-bit on the platforms this runs on,
// but the clamps are kept because they are the CONTRACT (pct > 100 means the
// whole window), not merely overflow defence.
func pctOf(window, pct int) int {
	if window <= 0 || pct <= 0 {
		return 0
	}
	v := window * pct / 100
	if v > window {
		v = window // pct > 100 clamps to the whole window
	}
	if v < 0 {
		v = 0 // defensive: unreachable given the guards above
	}
	return v
}

// ResolveBudget turns a context window into the fold's token budgets.
//
// windowTokens is the model's context window as resolved by the caller (0 when
// unknown), passed in rather than looked up so this module stays free of the
// model registry — the same reason the freeze guardrail takes rates rather than
// a model name.
func ResolveBudget(windowTokens int, cfg *BudgetConfig) Budget {
	var out Budget

	window := windowTokens
	if window > 0 {
		out.IsKnown = true
	} else {
		out.IsKnown = false
		window = BudgetDefaultWindow
		if cfg != nil && cfg.ContextWindowTokens > 0 {
			window = cfg.ContextWindowTokens
		}
	}
	out.ContextWindowTokens = window

	rb, tc, pc, ps := BudgetDefaultRetainedPct, BudgetDefaultTailCapPct,
		BudgetDefaultPressurePct, BudgetDefaultSaturationPct
	closet := BudgetDefaultClosetTokens
	if cfg != nil {
		if cfg.RetainedBandPct > 0 {
			rb = cfg.RetainedBandPct
		}
		if cfg.TailCapPct > 0 {
			tc = cfg.TailCapPct
		}
		if cfg.PressureCeilingPct > 0 {
			pc = cfg.PressureCeilingPct
		}
		if cfg.PrefixSaturationPct > 0 {
			ps = cfg.PrefixSaturationPct
		}
		if cfg.ClosetBudgetTokens > 0 {
			closet = cfg.ClosetBudgetTokens
		}
	}

	out.RetainedBandTokens = pctOf(window, rb)
	out.TailCapTokens = pctOf(window, tc)
	out.PressureCeilingTokens = pctOf(window, pc)
	out.PrefixSaturationTokens = pctOf(window, ps)

	out.ClosetBudgetTokens = closet
	if out.ClosetBudgetTokens > window {
		out.ClosetBudgetTokens = window // §7 single enforcement point
	}
	return out
}
