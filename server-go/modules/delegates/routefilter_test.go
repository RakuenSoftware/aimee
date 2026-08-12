package delegates

import "testing"

func capableAgent() RouteAgent {
	return RouteAgent{
		Enabled: true, HasRole: true,
		CapFlags: ModelCapTools | ModelCapVision, HaveCap: true,
		ToolsEnabled: true, CatalogContext: 200000,
	}
}

// The agent's own tools setting overrides the catalog in BOTH directions: a
// capable model on an agent with tools off cannot serve a packet needing tools.
func TestToolsSettingOverridesTheCatalog(t *testing.T) {
	a := capableAgent()
	a.ToolsEnabled = false
	if AgentMeetsFilter(a, ModelCapTools, 0, false) {
		t.Error("an agent with tools disabled served a packet requiring tools")
	}

	// ...and the other way: the agent asserts tools the catalog did not list.
	b := RouteAgent{Enabled: true, HasRole: true, HaveCap: true, ToolsEnabled: true}
	if !AgentMeetsFilter(b, ModelCapTools, 0, false) {
		t.Error("an agent with tools enabled was refused a tools packet")
	}
}

// Precedence, most specific first. The CLI window exists because a tmux-CLI
// agent carries no model to resolve one from, and it must not shadow a real one.
func TestEffectiveContextPrecedence(t *testing.T) {
	a := RouteAgent{HaveCap: true, OverrideContext: 111, CatalogContext: 222, CLIContext: 333}
	if got := EffectiveContext(a); got != 111 {
		t.Errorf("override should win, got %d", got)
	}
	a.OverrideContext = 0
	if got := EffectiveContext(a); got != 222 {
		t.Errorf("catalog should win over the CLI, got %d", got)
	}
	a.CatalogContext = 0
	if got := EffectiveContext(a); got != 333 {
		t.Errorf("the CLI window should be the fallback, got %d", got)
	}
	a.CLIContext = 0
	if got := EffectiveContext(a); got != 0 {
		t.Errorf("no window should resolve to 0, got %d", got)
	}
	// An unknown model must not inherit a catalog window.
	b := RouteAgent{HaveCap: false, CatalogContext: 999, CLIContext: 100}
	if got := EffectiveContext(b); got != 100 {
		t.Errorf("an unknown model used the catalog window anyway, got %d", got)
	}
}

func TestMinContextIsEnforced(t *testing.T) {
	a := capableAgent()
	a.CatalogContext = 8000
	if AgentMeetsFilter(a, 0, 200000, false) {
		t.Error("a small window served a packet needing a large one")
	}
	if !AgentMeetsFilter(a, 0, 8000, false) {
		t.Error("a window exactly meeting the requirement was refused")
	}
	// No window at all cannot satisfy a requirement.
	b := RouteAgent{Enabled: true, HasRole: true, HaveCap: true, ToolsEnabled: true}
	if AgentMeetsFilter(b, 0, 1, false) {
		t.Error("an agent with no known window served a packet with a minimum")
	}
}

func TestDeprecatedIsDroppedOnlyWhenAsked(t *testing.T) {
	a := capableAgent()
	a.Deprecated = true
	if AgentMeetsFilter(a, 0, 0, true) {
		t.Error("a deprecated model survived drop_deprecated")
	}
	if !AgentMeetsFilter(a, 0, 0, false) {
		t.Error("a deprecated model was dropped without being asked")
	}
	// Deprecation is only known when the catalog knew the model.
	a.HaveCap = false
	if !AgentMeetsFilter(a, 0, 0, true) {
		t.Error("an unknown model was treated as deprecated")
	}
}

// The reason this takes the whole fleet: modality is INFERRED from prompt text,
// so requiring it can wipe out a fleet over a filename. If dropping it leaves
// somebody, it is dropped.
func TestModalityIsRelaxedRatherThanFailingTheFleet(t *testing.T) {
	textOnly := RouteAgent{
		Enabled: true, HasRole: true,
		CapFlags: ModelCapTools, HaveCap: true, ToolsEnabled: true, CatalogContext: 200000,
	}
	got := RouteFilter([]RouteAgent{textOnly}, ModelCapTools|ModelCapVision, 0, false)

	if !got.Relaxed {
		t.Error("modality was not relaxed, so a text fleet fails on an inferred capability")
	}
	if got.EffectiveCaps != ModelCapTools {
		t.Errorf("effective caps = %d, want the hard set only", got.EffectiveCaps)
	}
	if got.Kept != 1 || !got.Keep[0] {
		t.Errorf("the text agent was dropped: kept=%d", got.Kept)
	}
}

// Relaxation only fires when it CHANGES the outcome. With a capable agent
// present, the modality requirement stands and the incapable one is dropped.
func TestModalityStandsWhenSomethingSatisfiesIt(t *testing.T) {
	vision := capableAgent()
	textOnly := capableAgent()
	textOnly.CapFlags = ModelCapTools

	got := RouteFilter([]RouteAgent{textOnly, vision}, ModelCapTools|ModelCapVision, 0, false)
	if got.Relaxed {
		t.Error("modality was relaxed even though an agent satisfied it")
	}
	if got.Keep[0] {
		t.Error("the text-only agent was kept for a vision packet")
	}
	if !got.Keep[1] || got.Kept != 1 {
		t.Errorf("the vision agent was dropped: %+v", got)
	}
}

// The HARD requirements are asserted by the caller, not inferred, so they are
// never relaxed -- an empty fleet is the correct answer there.
func TestHardRequirementsAreNeverRelaxed(t *testing.T) {
	small := capableAgent()
	small.CatalogContext = 8000

	got := RouteFilter([]RouteAgent{small}, ModelCapTools|ModelCapVision, 200000, false)
	if got.Kept != 0 {
		t.Errorf("an agent that fails min_context was kept: %+v", got)
	}
	// Relaxing modality would not have helped, so it must not claim it did.
	if got.Relaxed {
		t.Error("relaxation was reported when it could not have changed the outcome")
	}
}

// Agents that are disabled or lack the role were never candidates, and the
// filter must not resurrect them.
func TestNonCandidatesAreNeverKept(t *testing.T) {
	disabled := capableAgent()
	disabled.Enabled = false
	wrongRole := capableAgent()
	wrongRole.HasRole = false
	ok := capableAgent()

	got := RouteFilter([]RouteAgent{disabled, wrongRole, ok}, ModelCapTools, 0, false)
	if got.Keep[0] || got.Keep[1] {
		t.Errorf("a non-candidate was kept: %+v", got.Keep)
	}
	if !got.Keep[2] || got.Kept != 1 {
		t.Errorf("the candidate was not kept: %+v", got)
	}
}

// Nothing to enforce: every candidate stands, and no work is done.
func TestNoRequirementsKeepsEveryCandidate(t *testing.T) {
	a, b := capableAgent(), capableAgent()
	b.CapFlags = 0
	b.HaveCap = false

	got := RouteFilter([]RouteAgent{a, b}, 0, 0, false)
	if got.Kept != 2 || !got.Keep[0] || !got.Keep[1] {
		t.Errorf("a candidate was dropped with nothing to enforce: %+v", got)
	}
}

// No candidate for the role is not this rule's refusal to phrase: the caller
// has a better message for it.
func TestNoCandidatesIsEmptyRatherThanRelaxed(t *testing.T) {
	none := capableAgent()
	none.HasRole = false

	got := RouteFilter([]RouteAgent{none}, ModelCapTools|ModelCapVision, 0, false)
	if got.Kept != 0 || got.Relaxed {
		t.Errorf("%+v, want an empty result with no relaxation claimed", got)
	}
}

func TestEmptyFleet(t *testing.T) {
	got := RouteFilter(nil, ModelCapTools, 1000, true)
	if got.Kept != 0 || len(got.Keep) != 0 {
		t.Errorf("%+v, want nothing", got)
	}
}
