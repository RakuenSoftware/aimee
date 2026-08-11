package economizer

const (
	// AgentToolOutputMax is the built-in per-result model-visible cap.
	AgentToolOutputMax = 32 * 1024
	// AgentToolOutputRawMax is the ceiling an operator setting is clamped to.
	AgentToolOutputRawMax = 32 * 1024
)

// ToolOutputCapClamp resolves the per-result cap: 0/unset gives the built-in
// default, and an operator value is clamped to the raw ceiling.
func ToolOutputCapClamp(configured int) int {
	if configured <= 0 {
		return AgentToolOutputMax
	}
	if configured > AgentToolOutputRawMax {
		return AgentToolOutputRawMax
	}
	return configured
}

// EagerConfig is the resolved application config the eager seam needs.
//
// Passed in rather than read from a config store, so this module stays a pure
// transformer with no ambient state — which is what lets it serve a bus stage
// without owning a config connection.
type EagerConfig struct {
	Compact       CompactConfig
	ToolOutputCap int // 0 -> built-in default, via ToolOutputCapClamp
	Closet        ClosetConfig
}

// EagerResult reports what the seam did, so the caller can log it. The C version
// logs from inside; keeping the logging in the caller leaves this function pure
// and testable.
type EagerResult struct {
	Body      string
	Compacted bool
	Evict     EvictResult
}

// CompressToolResult is the EAGER tool-result seam: shrink via the shared
// CompactBody core, conserve identifiers in the Coordinate Closet, and bound the
// result to the per-result cap.
//
// Ported from agent_compress_tool_result in src/server/agent_policy.c.
//
// STATUS — READ BEFORE WIRING. The C original has NO callers and NO tests. A
// repo-wide search finds only its definition, its declaration in agent_exec.h,
// and prose in compact.h. A CI gate used to assert exactly that, listing it under
// "legacy_calls" beside context_reduce and build_fold_view and failing if any
// production source reached it; that gate was removed in 9d478dcaa6 (economizer
// off/safe/aggressive tiers).
//
// So its header comment — "the ONLY writer of compacted bodies into history" — no
// longer describes anything that runs, and the Coordinate Closet's P1 "wired
// return-only into agent_compress_tool_result" wiring is inert with it.
//
// Consequences worth being explicit about:
//   - compact_body has ONE live caller, the lazy lever in context_fold.c. Moving
//     it to Go therefore does NOT put the shrink algorithm in two languages.
//   - This port exists so the seam has a single home if it is ever revived. The
//     C original should be DELETED rather than kept in parallel; keeping both is
//     the two-languages problem for real, and for a path nothing calls.
//
// The hard cap is applied by THIS seam, not the core — it is per-seam policy.
// The cap is resolved once so the closet budget and the final bound agree.
//
// toolName may be empty to skip per-tool overrides.
func CompressToolResult(raw, toolName string, cfg EagerConfig) EagerResult {
	if raw == "" {
		return EagerResult{}
	}

	cap := ToolOutputCapClamp(cfg.ToolOutputCap)

	out := CompactBody(raw, toolName, &cfg.Compact)
	compacted := len(out) < len(raw)

	// Fold §2 — when the result was compacted, conserve the verbatim identifiers
	// from the PRE-truncation raw so they survive the shrink. That ordering is the
	// whole point: nominating from the compacted body would only find what the
	// shrink already kept.
	closet := ""
	evict := EvictNone
	if cfg.Closet.Enabled && compacted {
		// The conserved values originate from the same tool result as the body —
		// same trust level — so stamping them AGENT elevates no trust.
		prov := Provenance{Lane: LaneAgent, TurnID: -1, ToolCallID: -1, ResultIndex: -1}
		var set CoordSet
		NominateInto(raw, &prov, &set)

		// Bound the closet budget so body + closet can never exceed the hard cap.
		hardRoom := cap - 256
		if hardRoom < 0 {
			hardRoom = 0 // tiny operator cap: no room for a closet
		}
		cb := cfg.Closet.BudgetBytes
		if cb > hardRoom {
			cb = hardRoom
		}
		ccfg := ClosetConfig{
			Enabled:     true,
			BudgetBytes: cb,
			MaxRatioPct: cfg.Closet.MaxRatioPct,
			Denylist:    cfg.Closet.Denylist,
		}
		closet, evict = RenderCloset(&set, ccfg, len(raw))
	}

	// Hard cap: the result never exceeds the resolved per-result cap regardless of
	// what the summary produced, with closet bytes reserved inside it.
	bodyCap := cap
	if len(closet) > 0 && len(closet)+1 < cap {
		bodyCap = cap - len(closet) - 1
	}
	if len(out) > bodyCap {
		out = out[:bodyCap] // byte truncation, as in C
	}

	if len(closet) > 0 {
		out = out + "\n" + closet
	}
	return EagerResult{Body: out, Compacted: compacted, Evict: evict}
}
