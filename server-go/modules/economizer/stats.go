package economizer

import (
	"encoding/json"
	"sync"
)

// The gateway seam's operational counters.
//
// These are the module's own state, ported from src/server/gw_mutate_stats.c.
// Keeping them here is what makes them cheap: the module already knows what it
// decided, so counting an attempt, an application or a bypass costs nothing on
// the wire. Counted in C they would need a bus call per increment, on a path
// that runs two or three per request.
//
// The JSON shape is the one server_state.c already published, because dashboards
// key on these names. It is reproduced exactly rather than tidied.

// Published name for each counter. The gateway_ prefix is part of the contract:
// server_state.c emitted these exact keys, and dashboards key on them.
var statPublishedName = map[string]string{
	StatMutateAttempted:       "gateway_mutate_attempted",
	StatMutateApplied:         "gateway_mutate_applied",
	Stat4xxRestoreResend:      "gateway_4xx_restore_resend",
	Stat5xxDisable:            "gateway_5xx_disable",
	StatStreamErrorDisable:    "gateway_stream_error_disable",
	StatSessionDisabledBlocks: "gateway_session_disabled_blocks",
}

const (
	// statReasonMax bounds the (group, reason) registry. Beyond it counts go to
	// an overflow row rather than being dropped, so telemetry never silently
	// loses events -- the same guarantee the C table gave.
	statReasonMax = 64
	// statTokenSampleN is the deterministic 1-in-N gate on token-delta samples.
	statTokenSampleN = 100
)

type statReason struct {
	group, reason string
	count         uint64
}

// GatewayStatsStore is safe for concurrent use.
//
// One mutex rather than atomics: the C used relaxed atomics for the flat
// counters because they were on the request path in a C server, but here every
// increment already happens inside a stage call that has done JSON work, so the
// lock is not the cost and it keeps a snapshot internally consistent.
type GatewayStatsStore struct {
	mu      sync.Mutex
	flat    map[string]uint64
	reasons []statReason

	tokenSeen     uint64
	tokenSamples  uint64
	tokenBaseline uint64
	tokenReduced  uint64
}

func NewGatewayStatsStore() *GatewayStatsStore {
	return &GatewayStatsStore{flat: make(map[string]uint64)}
}

func (s *GatewayStatsStore) Inc(counter string) {
	if s == nil || counter == "" {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.flat == nil {
		s.flat = make(map[string]uint64)
	}
	s.flat[counter]++
}

// IncReason counts a (group, reason) pair, e.g. hard_bypass{structural_violation}.
//
// An unknown pair is registered on first use. Once the table is full every
// further new pair lands on (group, "_overflow"): a count that is present but
// unattributed beats a count that vanished.
func (s *GatewayStatsStore) IncReason(group, reason string) {
	if s == nil || group == "" {
		return
	}
	if reason == "" {
		reason = "unknown"
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	for i := range s.reasons {
		if s.reasons[i].group == group && s.reasons[i].reason == reason {
			s.reasons[i].count++
			return
		}
	}
	if len(s.reasons) >= statReasonMax {
		// Fold into this group's overflow row, creating it once. Looking it up
		// explicitly matters: the scan above only matches an exact (group, reason)
		// pair, so a fresh reason would otherwise append a SECOND overflow row and
		// the snapshot -- which keys by reason -- would drop one of them.
		for i := range s.reasons {
			if s.reasons[i].group == group && s.reasons[i].reason == "_overflow" {
				s.reasons[i].count++
				return
			}
		}
		s.reasons = append(s.reasons, statReason{group: group, reason: "_overflow", count: 1})
		return
	}
	s.reasons = append(s.reasons, statReason{group: group, reason: reason, count: 1})
}

// RecordTokenDelta accumulates a 1-in-N sample of applied reductions.
//
// Sampling, not every event, because the sums exist to answer "is the lever
// actually shrinking context" -- a question a sample answers as well as a census.
func (s *GatewayStatsStore) RecordTokenDelta(baseline, reduced int) {
	if s == nil || baseline < 0 || reduced < 0 {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	n := s.tokenSeen
	s.tokenSeen++
	if n%statTokenSampleN != 0 {
		return
	}
	s.tokenBaseline += uint64(baseline)
	s.tokenReduced += uint64(reduced)
	s.tokenSamples++
}

// StatsSnapshot is the published shape.
//
// The counters serialize FLAT, as siblings of token_delta and reasons, because
// that is what gw_stat_to_json emitted and what the /state endpoint has always
// published. Nesting them under a "counters" object would have been tidier and
// would have silently broken every dashboard keyed on gateway_mutate_applied,
// so the struct keeps them in a field and the marshaller flattens them.
type StatsSnapshot struct {
	Counters   map[string]uint64
	TokenDelta StatsTokenDelta
	Reasons    map[string]map[string]uint64
}

func (s StatsSnapshot) MarshalJSON() ([]byte, error) {
	out := make(map[string]any, len(s.Counters)+2)
	for name, v := range s.Counters {
		out[name] = v
	}
	out["token_delta"] = s.TokenDelta
	out["reasons"] = s.Reasons
	return json.Marshal(out)
}

// UnmarshalJSON is the inverse, so a round trip through the wire keeps the same
// shape a reader sees. Anything that is not token_delta or reasons is a counter.
func (s *StatsSnapshot) UnmarshalJSON(data []byte) error {
	var raw map[string]json.RawMessage
	if err := json.Unmarshal(data, &raw); err != nil {
		return err
	}
	s.Counters = map[string]uint64{}
	s.Reasons = map[string]map[string]uint64{}
	for name, v := range raw {
		switch name {
		case "token_delta":
			if err := json.Unmarshal(v, &s.TokenDelta); err != nil {
				return err
			}
		case "reasons":
			if err := json.Unmarshal(v, &s.Reasons); err != nil {
				return err
			}
		default:
			var n uint64
			if err := json.Unmarshal(v, &n); err != nil {
				return err
			}
			s.Counters[name] = n
		}
	}
	return nil
}

type StatsTokenDelta struct {
	SampleCount  uint64  `json:"sample_count"`
	BaselineSum  uint64  `json:"baseline_sum"`
	ReducedSum   uint64  `json:"reduced_sum"`
	PctReduced   float64 `json:"pct_reduced"`
	SampleRate1N int     `json:"sample_rate_1_in_n"`
}

func (s *GatewayStatsStore) Snapshot() StatsSnapshot {
	out := StatsSnapshot{
		Counters: make(map[string]uint64, len(statPublishedName)),
		Reasons:  map[string]map[string]uint64{},
		TokenDelta: StatsTokenDelta{
			SampleRate1N: statTokenSampleN,
		},
	}
	if s == nil {
		return out
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	// Every known counter is published even at zero, so a dashboard panel does
	// not disappear just because the lever has not fired yet.
	for short, published := range statPublishedName {
		out.Counters[published] = s.flat[short]
	}
	for _, r := range s.reasons {
		if r.count == 0 {
			continue
		}
		if out.Reasons[r.group] == nil {
			out.Reasons[r.group] = map[string]uint64{}
		}
		out.Reasons[r.group][r.reason] = r.count
	}
	out.TokenDelta.SampleCount = s.tokenSamples
	out.TokenDelta.BaselineSum = s.tokenBaseline
	out.TokenDelta.ReducedSum = s.tokenReduced
	if s.tokenBaseline > 0 {
		out.TokenDelta.PctReduced =
			float64(s.tokenBaseline-s.tokenReduced) * 100.0 / float64(s.tokenBaseline)
	}
	return out
}
