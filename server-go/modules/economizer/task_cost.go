package economizer

// Task-cost reporting is an observe-only owner contract. It never supplies a
// Proof or authorizes a reduction. Monetary amounts are decimal nanodollars,
// encoded as strings so native JSON adapters cannot round integer accounting.
import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"math"
	"math/big"
	"reflect"
	"sort"
	"strconv"

	"github.com/JBailes/aimee/server-go/bus"
)

var taskCostStages = []string{"indexing_embedding", "retrieval_reranking", "context_transformation", "generation", "tools", "retries", "verification"}

type TaskCostUnits struct {
	Kind  string `json:"kind"`
	Units string `json:"units"`
	Rate  string `json:"nanodollars_per_million_units"`
}
type TaskCostEntry struct {
	ID               string          `json:"id"`
	Attempt          string          `json:"attempt_id"`
	Stage            string          `json:"stage"`
	Kind             string          `json:"kind"` // actual, estimated, unpriced
	Nanodollars      string          `json:"nanodollars,omitempty"`
	Provider         string          `json:"provider"`
	Model            string          `json:"model"`
	PricingSnapshot  string          `json:"pricing_snapshot"`
	CacheAssumptions string          `json:"cache_assumptions"`
	Allocation       string          `json:"allocation"` // marginal or amortized
	AllocationRule   string          `json:"allocation_rule"`
	Evidence         string          `json:"evidence_ref"`
	LatencyMS        string          `json:"latency_ms"`
	EstimatedUnits   []TaskCostUnits `json:"estimated_units,omitempty"`
	EstimateBasis    string          `json:"estimate_basis,omitempty"`
}

type TaskCostRequest struct {
	SchemaVersion int    `json:"schema_version"`
	Task          string `json:"task_id"`
	// The caller must enumerate every stage, including zero-usage stages. A
	// declaration is not independently verified population completeness.
	Stages  map[string]string `json:"stages"` // observed, not_applicable:<reason>, unknown
	Entries []TaskCostEntry   `json:"entries"`
}

type TaskCostReport struct {
	SchemaVersion   int             `json:"schema_version"`
	Task            string          `json:"task_id"`
	Actual          string          `json:"actual_nanodollars"`
	Estimated       string          `json:"estimated_nanodollars"`
	KnownTotal      string          `json:"known_total_nanodollars"`
	Marginal        string          `json:"marginal_nanodollars"`
	Amortized       string          `json:"amortized_nanodollars"`
	Unpriced        []string        `json:"unpriced_entry_ids"`
	UnknownStages   []string        `json:"unknown_stages"`
	Entries         []TaskCostEntry `json:"entries"`
	Attempts        []string        `json:"attempt_ids"`
	Coverage        string          `json:"coverage"`
	PolicyAdmission string          `json:"policy_admission"`
}

func costInteger(s string) (int64, error) {
	n, err := strconv.ParseInt(s, 10, 64)
	if err != nil || n < 0 || strconv.FormatInt(n, 10) != s {
		return 0, errors.New("invalid cost integer")
	}
	return n, nil
}

// ReportTaskCost retains all evidence and never drops an unpriced call. Retry
// attempt identity is independent of stage: a generation during a retry remains
// generation, rather than being charged twice under generation and retries.
func ReportTaskCost(req TaskCostRequest) (TaskCostReport, error) {
	out := TaskCostReport{SchemaVersion: 1, Task: req.Task, Unpriced: []string{}, UnknownStages: []string{}, Entries: []TaskCostEntry{}, Attempts: []string{}, Coverage: "caller_declared", PolicyAdmission: "observe_only_unqualified"}
	bad := errors.New("invalid task cost declaration")
	if req.SchemaVersion != 1 || req.Task == "" || len(req.Task) > 256 || len(req.Entries) > 4096 || len(req.Stages) != len(taskCostStages) {
		return out, bad
	}
	counts := map[string]int{}
	for _, stage := range taskCostStages {
		state, ok := req.Stages[stage]
		if !ok || (state != "observed" && state != "unknown" && !(len(state) > 15 && state[:15] == "not_applicable:")) {
			return out, bad
		}
		if state == "unknown" {
			out.UnknownStages = append(out.UnknownStages, stage)
		}
		counts[stage] = 0
	}
	seen := map[string]TaskCostEntry{}
	attempts := map[string]bool{}
	var actual, estimated, marginal, amortized, total int64
	add := func(dst *int64, n int64) bool {
		if *dst > math.MaxInt64-n {
			return false
		}
		*dst += n
		return true
	}
	for _, entry := range req.Entries {
		if entry.ID == "" || len(entry.ID) > 256 || entry.Attempt == "" || len(entry.Attempt) > 256 || entry.Evidence == "" || len(entry.Evidence) > 1024 {
			return out, bad
		}
		if len(entry.EstimatedUnits) > 0 {
			if entry.Kind != "estimated" || len(entry.EstimatedUnits) > 8 {
				return out, bad
			}
			amount, err := priceTaskUnits(entry.EstimatedUnits)
			if err != nil || (entry.Nanodollars != "" && entry.Nanodollars != amount) {
				return out, bad
			}
			entry.Nanodollars = amount
			entry.EstimateBasis = "integer_rate_model_round_up_per_call"
		} else if entry.Kind == "estimated" {
			entry.EstimateBasis = "caller_declared"
		} else if entry.EstimateBasis != "" {
			return out, bad
		}
		if old, ok := seen[entry.ID]; ok {
			if !reflect.DeepEqual(old, entry) {
				return out, errors.New("conflicting cost event")
			}
			continue
		}
		if _, ok := counts[entry.Stage]; !ok || req.Stages[entry.Stage] != "observed" {
			return out, bad
		}
		if _, err := costInteger(entry.LatencyMS); err != nil {
			return out, err
		}
		if entry.Allocation != "marginal" && entry.Allocation != "amortized" {
			return out, bad
		}
		if entry.AllocationRule == "" || entry.CacheAssumptions == "" {
			return out, bad
		}
		var n int64
		if entry.Kind == "unpriced" {
			if entry.Nanodollars != "" {
				return out, bad
			}
			out.Unpriced = append(out.Unpriced, entry.ID)
		} else {
			if entry.Provider == "" || entry.Model == "" || entry.PricingSnapshot == "" {
				return out, bad
			}
			var err error
			n, err = costInteger(entry.Nanodollars)
			if err != nil {
				return out, err
			}
			switch entry.Kind {
			case "actual":
				if !add(&actual, n) {
					return out, bad
				}
			case "estimated":
				if !add(&estimated, n) {
					return out, bad
				}
			default:
				return out, bad
			}
			if !add(&total, n) {
				return out, bad
			}
			if entry.Allocation == "marginal" {
				if !add(&marginal, n) {
					return out, bad
				}
			} else {
				if !add(&amortized, n) {
					return out, bad
				}
			}
		}
		counts[entry.Stage]++
		seen[entry.ID] = entry
		attempts[entry.Attempt] = true
		out.Entries = append(out.Entries, entry)
	}
	for stage, count := range counts {
		if req.Stages[stage] == "observed" && count == 0 {
			return out, bad
		}
	}
	for id := range attempts {
		out.Attempts = append(out.Attempts, id)
	}
	sort.Strings(out.Attempts)
	sort.Strings(out.Unpriced)
	sort.Strings(out.UnknownStages)
	sort.Slice(out.Entries, func(i, j int) bool { return out.Entries[i].ID < out.Entries[j].ID })
	out.Actual = strconv.FormatInt(actual, 10)
	out.Estimated = strconv.FormatInt(estimated, 10)
	out.KnownTotal = strconv.FormatInt(total, 10)
	out.Marginal = strconv.FormatInt(marginal, 10)
	out.Amortized = strconv.FormatInt(amortized, 10)
	if len(out.Unpriced) > 0 || len(out.UnknownStages) > 0 {
		out.Coverage = "incomplete"
	}
	return out, nil
}

func handleTaskCost(raw []byte) ([]byte, bus.ModuleStatus) {
	if len(raw) > 2<<20 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	compact, result := JSONCompact(raw)
	if result == JSONNotShorter {
		compact = raw
	} else if result != JSONOK {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var kind map[string]json.RawMessage
	if json.Unmarshal(compact, &kind) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, paired := kind["corpus"]; paired {
		var req TaskCostPairRequest
		d := json.NewDecoder(bytes.NewReader(compact))
		d.DisallowUnknownFields()
		if d.Decode(&req) != nil || d.Decode(new(any)) != io.EOF {
			return nil, bus.ModuleStatusInvalidRequest
		}
		out, err := PairTaskCosts(req)
		if err != nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		body, err := json.Marshal(out)
		if err != nil {
			return nil, bus.ModuleStatusInternal
		}
		return body, bus.ModuleStatusOK
	}
	var req TaskCostRequest
	decoder := json.NewDecoder(bytes.NewReader(compact))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&req) != nil || decoder.Decode(new(any)) != io.EOF {
		return nil, bus.ModuleStatusInvalidRequest
	}
	out, err := ReportTaskCost(req)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	body, err := json.Marshal(out)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return body, bus.ModuleStatusOK
}

// Price units in the economizer owner, retaining the declared rate snapshot.
// Accumulate the exact rational amount before rounding up once per call.
func priceTaskUnits(units []TaskCostUnits) (string, error) {
	total := new(big.Int)
	scale := big.NewInt(1000000)
	seen := map[string]bool{}
	for _, u := range units {
		switch u.Kind {
		case "input_uncached_tokens", "input_cached_tokens", "output_tokens", "embedding_tokens", "tool_invocations", "wall_milliseconds":
		default:
			return "", errors.New("invalid rate unit")
		}
		if seen[u.Kind] {
			return "", errors.New("duplicate rate unit")
		}
		seen[u.Kind] = true
		n, e := costInteger(u.Units)
		if e != nil {
			return "", e
		}
		rate, e := costInteger(u.Rate)
		if e != nil {
			return "", e
		}
		total.Add(total, new(big.Int).Mul(big.NewInt(n), big.NewInt(rate)))
	}
	total.Add(total, big.NewInt(999999))
	total.Quo(total, scale)
	if !total.IsInt64() {
		return "", errors.New("priced call overflow")
	}
	return total.String(), nil
}
