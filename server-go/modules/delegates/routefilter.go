package delegates

// Which agents in a fleet can serve a delegate packet.
//
// Three rules that only work together:
//
//   - whether one agent meets a requirement
//   - which context window an agent effectively has, out of the three places a
//     window can come from
//   - whether to RELAX the inferred modality requirements
//
// The last is why this takes the whole fleet at once. Relaxation is a decision
// about the fleet, not about an agent: it fires only when requiring modality
// would leave NOBODY and dropping it would leave somebody. Asked one agent at a
// time the question cannot be answered, and a caller that answered it itself
// would be holding the interesting half of the rule.

// Capability bits, mirroring model_registry.h. A capability is a property of a
// model, so the numbering belongs to the registry and is restated here only so
// the wire has a definition that does not depend on server headers.
const (
	ModelCapTools  uint32 = 1 << 1
	ModelCapVision uint32 = 1 << 2
	ModelCapPDF    uint32 = 1 << 3
	ModelCapAudio  uint32 = 1 << 4
)

// ModelCapModalitySoft are inferred from prompt TEXT, so they over-trigger on a
// task that merely mentions an image or pdf filename. They are routing
// preferences, not requirements -- see RouteFilter.
const ModelCapModalitySoft = ModelCapVision | ModelCapPDF | ModelCapAudio

// RouteAgent is one agent's resolved facts. Every one is supplied rather than
// looked up: the catalog, the agent registry and the CLI adapters are the
// caller's, and this module reads none of them.
type RouteAgent struct {
	// Enabled and HasRole narrow the fleet to this packet's candidates.
	Enabled bool
	HasRole bool

	// CapFlags is what the catalog says the model can do; HaveCap is whether
	// the catalog knew the model at all.
	CapFlags   uint32
	HaveCap    bool
	Deprecated bool

	// ToolsEnabled is the AGENT's setting and overrides the catalog's tools
	// bit in both directions: an agent with tools off cannot serve a packet
	// that needs them however capable its model is.
	ToolsEnabled bool

	// The three places a context window can come from, most specific first.
	// See EffectiveContext.
	OverrideContext int // the agent's explicit middleware override
	CatalogContext  int // the model capability catalog
	CLIContext      int // the vendor CLI adapter's declared window
}

// RouteFilterResult is the fleet decision.
type RouteFilterResult struct {
	// Keep[i] reports whether agent i may serve the packet. Agents that were
	// already disabled, or do not have the role, are false and were never
	// candidates.
	Keep []bool
	// Kept counts the survivors.
	Kept int
	// Relaxed reports that the inferred modality requirements were dropped.
	Relaxed bool
	// EffectiveCaps is what was actually required after any relaxation.
	EffectiveCaps uint32
}

// EffectiveContext resolves an agent's context window.
//
// Precedence, most specific first: the agent's explicit override, then the
// model catalog, then the vendor CLI adapter's declared window. The last exists
// because a tmux-CLI agent usually carries no model to resolve a window from --
// the vendor's CLI picks the model itself -- and it is consulted ONLY when the
// first two produced nothing, so model-backed agents are untouched.
//
// This ordering is the reason onboarding a model is a config or catalog change
// rather than a code edit, which is worth keeping in one place.
func EffectiveContext(a RouteAgent) int {
	if a.OverrideContext > 0 {
		return a.OverrideContext
	}
	if a.HaveCap && a.CatalogContext > 0 {
		return a.CatalogContext
	}
	if a.CLIContext > 0 {
		return a.CLIContext
	}
	return 0
}

// AgentMeetsFilter is the per-agent predicate: no mutation, so the relaxation
// pass below can dry-run it before deciding what to actually enforce.
func AgentMeetsFilter(a RouteAgent, requiredCaps uint32, minContext int, dropDeprecated bool) bool {
	if dropDeprecated && a.HaveCap && a.Deprecated {
		return false
	}

	flags := uint32(0)
	if a.HaveCap {
		flags = a.CapFlags
	}
	// The agent's own setting wins over the catalog, both ways.
	if a.ToolsEnabled {
		flags |= ModelCapTools
	} else {
		flags &^= ModelCapTools
	}
	if requiredCaps != 0 && flags&requiredCaps != requiredCaps {
		return false
	}

	if minContext > 0 {
		ctx := EffectiveContext(a)
		if ctx <= 0 || ctx < minContext {
			return false
		}
	}
	return true
}

// RouteFilter decides which agents may serve the packet.
//
// MODALITY IS RELAXED, NEVER FATAL. Vision, PDF and audio requirements are
// inferred from prompt text, so a text task that mentions an image filename can
// ask for a capability it does not need. If requiring them would leave no
// candidate at all, and the hard requirements alone would leave one, they are
// dropped and the packet routes on the hard set. Hard-failing a whole fleet on a
// guess made from a filename is the outcome this exists to prevent.
//
// The hard set is everything else: tools and min_context. Those are asserted by
// the caller, not inferred, so they are never relaxed.
func RouteFilter(agents []RouteAgent, requiredCaps uint32, minContext int,
	dropDeprecated bool) RouteFilterResult {
	result := RouteFilterResult{
		Keep:          make([]bool, len(agents)),
		EffectiveCaps: requiredCaps,
	}

	// Nothing to enforce: every candidate stands.
	if requiredCaps == 0 && minContext <= 0 && !dropDeprecated {
		for i, a := range agents {
			if a.Enabled && a.HasRole {
				result.Keep[i] = true
				result.Kept++
			}
		}
		return result
	}

	candidates := 0
	for _, a := range agents {
		if a.Enabled && a.HasRole {
			candidates++
		}
	}
	// No candidate for the role at all is not this rule's refusal to make: the
	// caller's own "no agent has this role" path says something more useful.
	if candidates == 0 {
		return result
	}

	effective := requiredCaps
	if requiredCaps&ModelCapModalitySoft != 0 {
		hard := requiredCaps &^ ModelCapModalitySoft
		keptFull, keptHard := 0, 0
		for _, a := range agents {
			if !a.Enabled || !a.HasRole {
				continue
			}
			if AgentMeetsFilter(a, requiredCaps, minContext, dropDeprecated) {
				keptFull++
			}
			if AgentMeetsFilter(a, hard, minContext, dropDeprecated) {
				keptHard++
			}
		}
		if keptFull == 0 && keptHard > 0 {
			effective = hard
			result.Relaxed = true
		}
	}
	result.EffectiveCaps = effective

	for i, a := range agents {
		if !a.Enabled || !a.HasRole {
			continue
		}
		if AgentMeetsFilter(a, effective, minContext, dropDeprecated) {
			result.Keep[i] = true
			result.Kept++
		}
	}
	return result
}
