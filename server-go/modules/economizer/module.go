package economizer

import (
	"encoding/json"

	"github.com/JBailes/aimee/server-go/bus"
)

// Bus surface for the economizer module.
//
// ONE stage, deliberately. coord_closet, fold_recall, context_fold and the
// condensation primitives are internal to a reduction, not separately callable,
// so exposing them would be surface with no caller. The reduction is the unit of
// work the C side actually needs.
//
// The module is STATELESS: per-conversation reducer state travels in and out
// with the request, because the caller already persists it (db1_economizer_state
// save/load). That keeps the module free of a store and lets any process serve
// the stage.

// Event kind and stage id, fixed by the process contract at
// 4096 + ordinal*256 + stage. The economizer is inventory ordinal 27, so these
// are not a free choice.
const (
	EventReduce uint32 = 11009
	StageReduce uint32 = 1
)

// ReduceRequest is the wire form of one reduction.
//
// Messages arrives as RAW JSON so the module can parse it with the
// cJSON-compatible reader: re-encoding through encoding/json would reorder keys
// and HTML-escape, changing the folded prefix bytes and defeating the freeze.
type ReduceRequest struct {
	Messages     json.RawMessage `json:"messages"`
	SystemPrompt string          `json:"system_prompt,omitempty"`
	Seam         string          `json:"seam"` // "gateway" | "delegate"

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
func NewHandler() bus.ModuleHandler {
	return func(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
		if invocation.StageID != StageReduce {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var req ReduceRequest
		if err := json.Unmarshal(request, &req); err != nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		return handleReduce(&req)
	}
}

func handleReduce(req *ReduceRequest) ([]byte, bus.ModuleStatus) {
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
