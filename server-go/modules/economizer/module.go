package economizer

import (
	"encoding/json"

	"github.com/JBailes/aimee/server-go/bus"
)

// Bus surface for the economizer module. Reduction remains the primary stage.
// The auxiliary stages are compatibility seams for the last C consumers of
// algorithms that already live here: fresh-result JSON compaction, spill recall,
// and the process-local condensation counters.
//
// Per-conversation reducer state travels in and out with the request, because
// the caller already persists it (db1_economizer_state save/load).
//
// The gateway's circuit breaker is the one thing the module DOES hold, in
// breaker.go. It is the module's own lever -- "my reduction is switched off for
// this session" -- not the caller's session state, and it is deliberately
// volatile. Holding it here is what makes tripping it mean anything: a breaker
// written on one side of the bus and read on the other would never fire.

// Event kind and stage id, fixed by the process contract at
// 4096 + ordinal*256 + stage. The economizer is inventory ordinal 27, so these
// are not a free choice.
const (
	EventReduce      uint32 = 11009
	StageReduce      uint32 = 1
	EventJSONCompact uint32 = 11010
	StageJSONCompact uint32 = 2
	EventToolRecall  uint32 = 11011
	StageToolRecall  uint32 = 3
	EventToolStats   uint32 = 11012
	StageToolStats   uint32 = 4
	EventRecordBuild uint32 = 11013
	StageRecordBuild uint32 = 5
	EventPostStatus  uint32 = 11014
	StagePostStatus  uint32 = 6
	EventStats       uint32 = 11015
	StageStats       uint32 = 7
)

// StatsRequest asks for the published snapshot.
//
// Read-only by design. The counters are the module's, incremented where the
// decisions are made, so there is no way for a caller to write one -- it has
// nothing to report. The caller passes an IR and uses whatever IR comes back;
// how many were mutated is not its business.
type StatsRequest struct {
	Op string `json:"op"` // "snapshot" (the only operation)
}

// PostStatusRequest asks what a dispatched gateway turn owes now its upstream
// status is known.
//
// The request body is NOT here and never will be: restoring the pristine array
// and resending are the HTTP caller's, over a body it owns. This decides, and
// owns the breaker the decision writes to.
type PostStatusRequest struct {
	SessionKey string `json:"session_key"`
	HTTPStatus int    `json:"http_status,omitempty"`
	Mutated    bool   `json:"mutated"`
	TTLMS      int    `json:"ttl_ms,omitempty"`
	// HaveKey and StreamReason select the streaming path, where bytes have
	// already reached the client so nothing can be restored or resent.
	HaveKey      bool   `json:"have_key,omitempty"`
	StreamReason string `json:"stream_reason,omitempty"`
}

// PostStatusResponse is the decision plus what was done to the breaker.
type PostStatusResponse struct {
	// Action is "none" or "resend". "resend" means the caller must put its
	// pristine array back and send exactly once more.
	Action  string `json:"action"`
	Restore bool   `json:"restore,omitempty"`
	// Disabled reports that the breaker was tripped HERE, so the caller records
	// it once rather than inferring it.
	Disabled bool   `json:"disabled,omitempty"`
	Reason   string `json:"reason,omitempty"`
}

// ReduceRequest is the wire form of one reduction.
//
// Messages arrives as RAW JSON so the module can parse it with the
// cJSON-compatible reader: re-encoding through encoding/json would reorder keys
// and HTML-escape, changing the folded prefix bytes and defeating the freeze.
type ReduceRequest struct {
	Messages     json.RawMessage `json:"messages"`
	SystemPrompt string          `json:"system_prompt,omitempty"`
	Seam         string          `json:"seam"` // "gateway" | "delegate"
	// SessionKey identifies whose lever this is, for the gateway seam's breaker.
	// Empty means the request carried no resolvable identity, which reduces
	// normally but can never trip or read a breaker — otherwise one anonymous
	// request could switch off another identity's session.
	SessionKey string `json:"session_key,omitempty"`

	// Config, resolved by the caller.
	HistoryFold        bool       `json:"history_fold,omitempty"`
	Compress           bool       `json:"compress,omitempty"`
	MeasureOnly        bool       `json:"measure_only,omitempty"`
	MinGainTokens      int        `json:"min_gain_tokens,omitempty"`
	FreezeGuardEnabled bool       `json:"freeze_guard_enabled,omitempty"`
	FreezeGuardHorizon int        `json:"freeze_guard_horizon,omitempty"`
	Rates              PriceRates `json:"rates,omitempty"`
	RecallEnabled      bool       `json:"recall_enabled,omitempty"`
	RecallTTLTurns     int        `json:"recall_ttl_turns,omitempty"`
	RecallInject       bool       `json:"recall_inject,omitempty"`

	RetainedMsgs          int    `json:"retained_msgs,omitempty"`
	MinFoldMsgs           int    `json:"min_fold_msgs,omitempty"`
	ReasoningExcerptBytes int    `json:"excerpt_bytes,omitempty"`
	RegisterEnabled       bool   `json:"register_enabled,omitempty"`
	CompactHeadBytes      int    `json:"compact_head_bytes,omitempty"`
	CompactTailBytes      int    `json:"compact_tail_bytes,omitempty"`
	ClosetEnabled         bool   `json:"closet_enabled,omitempty"`
	ClosetBudgetBytes     int    `json:"closet_budget_bytes,omitempty"`
	ClosetMaxRatioPct     int    `json:"closet_max_ratio_pct,omitempty"`
	ClosetDenylist        string `json:"closet_denylist,omitempty"`

	// State is the serialized per-conversation reducer state, empty on the first
	// turn of a conversation.
	State string `json:"state,omitempty"`
	Turn  int    `json:"turn,omitempty"`
}

// ReduceResponse carries the reduced view and the ledger.
//
// Messages is nil when nothing was mutated, which is the caller's signal to
// forward its ORIGINAL array untouched rather than re-serialize ours.
type ReduceResponse struct {
	Messages json.RawMessage `json:"messages,omitempty"`
	Mutated  bool            `json:"mutated"`
	Reason   string          `json:"reason"`

	// Bypass is the gateway seam's apply/bypass verdict, set ONLY for
	// seam=gateway. Empty on the delegate seam, which has no such decision.
	//
	// The decision is made here rather than by the caller because the structural
	// check it depends on needs the reduced array, and this is the only place
	// that array exists without being serialized across the bus a second time.
	// "none" means apply; anything else is a hard bypass and names the reason.
	Bypass string `json:"bypass,omitempty"`

	BaselineTokens int `json:"baseline_tokens"`
	ReducedTokens  int `json:"reduced_tokens"`
	RemovedTokens  int `json:"removed_tokens"`
	FoldableTokens int `json:"foldable_tokens"`

	FoldedMsgs     int  `json:"folded_msgs,omitempty"`
	RetainedMsgs   int  `json:"retained_msgs,omitempty"`
	ReusedBoundary bool `json:"reused_boundary,omitempty"`
	Epochs         int  `json:"epochs,omitempty"`
	FreezeGuarded  bool `json:"freeze_guarded,omitempty"`
	ClosetEvicted  bool `json:"closet_evicted,omitempty"`

	RecallHint     string `json:"recall_hint,omitempty"`
	RecallSurfaced int    `json:"recall_surfaced,omitempty"`

	// State is the serialized reducer state to persist for the next turn. Empty
	// when it could not be serialized, which the caller treats as "keep what you
	// have" rather than "clear it".
	State string `json:"state,omitempty"`
}

var reduceReasonNames = map[ReduceReason]string{
	ReduceReasonNone:       "none",
	ReduceReasonReduced:    "reduced",
	ReduceReasonMeasured:   "measured",
	ReduceReasonSkipNoGain: "skip_no_gain",
	ReduceReasonAlready:    "already",
}

// NewHandler serves the economizer's reduce stage.
//
// The breaker is created per handler rather than as a package global so a test
// gets a clean one and two handlers never share a lever.
func NewHandler() bus.ModuleHandler {
	breaker := NewSessionBreaker()
	stats := NewGatewayStatsStore()
	return func(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		switch invocation.StageID {
		case StageStats:
			var req StatsRequest
			if err := json.Unmarshal(request, &req); err != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			return handleStats(stats, &req)
		case StagePostStatus:
			var req PostStatusRequest
			if err := json.Unmarshal(request, &req); err != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			return handlePostStatus(breaker, stats, &req)
		case StageReduce:
			var req ReduceRequest
			if err := json.Unmarshal(request, &req); err != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			return handleReduce(breaker, stats, &req)
		case StageJSONCompact:
			return handleJSONCompact(invocation, request)
		case StageToolRecall:
			return handleToolRecall(invocation, request)
		case StageToolStats:
			return handleToolStats(invocation, request)
		case StageRecordBuild:
			return handleRecordBuild(invocation, request)
		default:
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
}

func handleReduce(breaker *SessionBreaker, stats *GatewayStatsStore, req *ReduceRequest) ([]byte, bus.ModuleStatus) {
	messages := ParseJSON(string(req.Messages))
	if messages == nil || !messages.IsArray() {
		return nil, bus.ModuleStatusInvalidRequest
	}

	seam := SeamDelegate
	switch req.Seam {
	case "gateway":
		seam = SeamGateway
	case "delegate", "":
		seam = SeamDelegate
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}

	cfg := &ReduceConfig{
		HistoryFold:        req.HistoryFold,
		Compress:           req.Compress,
		MeasureOnly:        req.MeasureOnly,
		MinGainTokens:      req.MinGainTokens,
		FreezeGuardEnabled: req.FreezeGuardEnabled,
		FreezeGuardHorizon: req.FreezeGuardHorizon,
		Rates:              req.Rates,
		RecallEnabled:      req.RecallEnabled,
		RecallTTLTurns:     req.RecallTTLTurns,
		RecallInject:       req.RecallInject,
		Fold: FoldConfig{
			RetainedMsgs:          req.RetainedMsgs,
			MinFoldMsgs:           req.MinFoldMsgs,
			ReasoningExcerptBytes: req.ReasoningExcerptBytes,
			RegisterEnabled:       req.RegisterEnabled,
			CompactHeadBytes:      req.CompactHeadBytes,
			CompactTailBytes:      req.CompactTailBytes,
			Closet: ClosetConfig{
				Enabled:     req.ClosetEnabled,
				BudgetBytes: req.ClosetBudgetBytes,
				MaxRatioPct: req.ClosetMaxRatioPct,
				Denylist:    req.ClosetDenylist,
			},
		},
	}
	// Honour the breaker BEFORE reducing. A disabled session is a pristine
	// passthrough: the whole point of tripping it was to stop spending effort on
	// a payload shape this session has already shown the provider rejects.
	//
	// Read here rather than by the caller because the write side lives here too;
	// split across the bus, a trip on one side would never be seen by the other.
	if seam == SeamGateway && req.SessionKey != "" && breaker.IsDisabled(req.SessionKey) {
		stats.Inc(StatSessionDisabledBlocks)
		body, err := json.Marshal(ReduceResponse{
			Reason: reduceReasonNames[ReduceReasonNone],
			Bypass: GWBypassSessionDisabled.String(),
		})
		if err != nil {
			return nil, bus.ModuleStatusInternal
		}
		return body, bus.ModuleStatusOK
	}

	// The seam gate lives in the config, so set the one this request arrived on.
	if seam == SeamGateway {
		cfg.GatewaySeam = true
	} else {
		cfg.DelegateSeam = true
	}

	st := &ReduceState{Turn: req.Turn, Recall: NewRecallIndex()}
	if req.State != "" {
		// A state we cannot read is DISCARDED rather than fatal: the reduction
		// still runs, it just starts from a cold freeze and an empty page table.
		// Failing the whole request would be worse than losing one turn of warmth.
		_ = RestoreState(st, req.State)
		st.Turn = req.Turn
		if st.Recall == nil {
			st.Recall = NewRecallIndex()
		}
	}

	out := Reduce(messages, req.SystemPrompt, seam, cfg, st)

	resp := ReduceResponse{
		Mutated:        out.Mutated,
		Reason:         reduceReasonNames[out.Reason],
		BaselineTokens: out.BaselineTokens,
		ReducedTokens:  out.ReducedTokens,
		RemovedTokens:  out.RemovedTokens,
		FoldableTokens: out.FoldableTokens,
		FoldedMsgs:     out.FoldedMsgs,
		RetainedMsgs:   out.RetainedMsgs,
		ReusedBoundary: out.ReusedBoundary,
		Epochs:         out.Epochs,
		FreezeGuarded:  out.FreezeGuarded,
		ClosetEvicted:  out.ClosetEvict == EvictFail,
		RecallHint:     out.RecallHint,
		RecallSurfaced: out.RecallSurfaced,
	}
	if seam == SeamGateway {
		// Counted where the decision is made. An attempt is every gateway turn
		// that got past the breaker; what it became is the verdict below.
		stats.Inc(StatMutateAttempted)
		// MessageHistoryRepair is passed explicitly: GWShouldApply SKIPS the
		// structural check when the port is nil, and skipping it is how an
		// orphaned tool pair reaches a provider. The reduction itself succeeded
		// to reach this point, so the error argument is ReduceErrNone.
		resp.Bypass = GWShouldApply(true, &out, ReduceErrNone, MessageHistoryRepair).String()
		if resp.Bypass == GWBypassNone.String() {
			// The caller can still fail to install it, and reports that itself;
			// this counts the decision, not the installation.
			stats.Inc(StatMutateApplied)
			stats.RecordTokenDelta(out.BaselineTokens, out.ReducedTokens)
		} else {
			stats.IncReason("hard_bypass", resp.Bypass)
		}
	}
	if out.Mutated && out.Messages != nil {
		// Emitted with the cJSON-compatible printer, so the bytes the caller
		// forwards are the bytes the fold measured — anything else would move the
		// prefix and cost the cache.
		resp.Messages = json.RawMessage(PrintJSONUnformatted(out.Messages))
	}
	if blob, ok := SerializeState(st); ok {
		resp.State = blob
	}

	body, err := json.Marshal(resp)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return body, bus.ModuleStatusOK
}

// handlePostStatus applies the post-dispatch decision and owns the breaker write.
//
// The caller is told what to DO rather than asked what to do: it has the request
// body and the socket, this has the lever. Splitting it the other way — caller
// decides, module stores — is what left the C seam with a breaker nothing ever
// wrote to.
func handlePostStatus(breaker *SessionBreaker, stats *GatewayStatsStore, req *PostStatusRequest) ([]byte, bus.ModuleStatus) {
	var d GWPostDecision
	if req.StreamReason != "" {
		// Streaming: bytes are already with the client, so the breaker is the only
		// lever left. HaveKey gates it because without a key there is nothing to
		// disable.
		d = GWStreamDisable(req.Mutated, req.HaveKey, req.StreamReason)
	} else {
		d = GWPostStatus(req.HTTPStatus, req.Mutated)
	}

	resp := PostStatusResponse{Action: "none", Restore: d.Restore}
	if d.Action == PostResend {
		resp.Action = "resend"
	}
	// A trip needs somewhere to record it and a window to last for. Report
	// Disabled only when the write actually happened, so the caller never records
	// a breaker that was never set.
	if d.Disable && req.SessionKey != "" && req.TTLMS > 0 {
		breaker.Disable(req.SessionKey, req.TTLMS, d.Reason)
		stats.IncReason("session_disabled_set", d.Reason)
		resp.Disabled = true
		resp.Reason = d.Reason
	} else if d.Reason != "" {
		resp.Reason = d.Reason
	}
	if d.Counter != "" {
		stats.Inc(d.Counter)
	}

	body, err := json.Marshal(resp)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return body, bus.ModuleStatusOK
}

// handleStats returns the snapshot the HTTP surface publishes.
//
// A call rather than a push because the counters are the module's and the HTTP
// surface is the caller's: the side that owns the numbers hands them over when
// the side that owns the endpoint asks.
func handleStats(stats *GatewayStatsStore, req *StatsRequest) ([]byte, bus.ModuleStatus) {
	if req.Op != "" && req.Op != "snapshot" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	body, err := json.Marshal(stats.Snapshot())
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return body, bus.ModuleStatusOK
}
