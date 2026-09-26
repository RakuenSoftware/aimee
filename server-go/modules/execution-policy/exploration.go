package executionpolicy

import (
	"errors"
	"math"
	"sync"
	"time"
)

// A nil ceiling inherits the operator limit. Zero is a literal zero, never a
// sentinel. Limits do not confer authorization; baseline policy runs first.
type explorationLimits struct {
	StarvationTurns uint64 `json:"starvation_turns,omitempty"`
	Enabled         bool   `json:"enabled"`
	RawScans        *int64 `json:"raw_scans,omitempty"`
	Files           *int64 `json:"distinct_files,omitempty"`
	Graph           *int64 `json:"graph_expansions,omitempty"`
	Bytes           *int64 `json:"returned_bytes,omitempty"`
	Tokens          *int64 `json:"returned_tokens,omitempty"`
}

func legacyRawScanLimit(n int64) *int64 {
	if n <= 0 {
		return nil
	}
	return &n
}
func (l explorationLimits) valid() bool {
	if l.StarvationTurns > 64 {
		return false
	}
	for _, n := range []*int64{l.RawScans, l.Files, l.Graph, l.Bytes, l.Tokens} {
		if n != nil && *n < 0 {
			return false
		}
	}
	return true
}

type explorationBinding struct {
	Route                string `json:"route,omitempty"`
	IndexObservedCurrent bool   `json:"index_observed_current,omitempty"`
	OwnerObservedCurrent bool   `json:"owner_observed_current,omitempty"`
	Provider             string `json:"provider,omitempty"`
	Model                string `json:"model,omitempty"`
	LimitsDigest         string `json:"limits_digest,omitempty"`
	BudgetTask           string `json:"budget_task,omitempty"`
	WorkingDirectory     string `json:"working_directory,omitempty"`
	PlanDigest           string `json:"plan_digest,omitempty"`
	SourceVersionsDigest string `json:"source_versions_digest,omitempty"`
	Workspace            string `json:"workspace,omitempty"`
	Principal            string `json:"principal"`
	Session              string `json:"session"`
	Task                 string `json:"task"`
	Project              string `json:"project"`
	WorktreeGeneration   string `json:"worktree_generation"`
	IndexGeneration      string `json:"index_generation"`
	MemoryOwner          string `json:"memory_owner"`
}

func (b explorationBinding) valid() bool {
	return len(b.BudgetTask) <= 128 && b.Principal != "" && b.Session != "" && b.Task != "" && b.Project != "" && b.WorktreeGeneration != "" && b.IndexGeneration != "" && b.MemoryOwner != ""
}

func (b explorationBinding) budgetTask() string {
	if b.BudgetTask != "" {
		return b.BudgetTask
	}
	return b.Task
}

type explorationContract struct {
	ReceiptDigest        string             `json:"receipt_digest,omitempty"`
	ID                   string             `json:"id"`
	Revision             uint64             `json:"revision"`
	Binding              explorationBinding `json:"binding"`
	PlanDigest           string             `json:"plan_digest"`
	SourceVersionsDigest string             `json:"source_versions_digest"`
	QueryClass           string             `json:"query_class"`
	CoverageComplete     bool               `json:"coverage_complete"`
	ConfidenceProvenance string             `json:"confidence_provenance"`
	CalibrationReceipt   string             `json:"calibration_receipt"`
	SupportedClasses     []string           `json:"supported_classes"`
	Created              time.Time          `json:"created"`
	Expires              time.Time          `json:"expires"`
	Limits               explorationLimits  `json:"limits"`
	Tier                 string             `json:"tier"`
}

func (c explorationContract) valid(now time.Time) bool {
	if (c.Binding.PlanDigest != "" && c.Binding.PlanDigest != c.PlanDigest) || (c.Binding.SourceVersionsDigest != "" && c.Binding.SourceVersionsDigest != c.SourceVersionsDigest) {
		return false
	}
	if c.ID == "" || c.Revision == 0 || !c.Binding.valid() || c.PlanDigest == "" || c.SourceVersionsDigest == "" || c.QueryClass == "" || c.ConfidenceProvenance == "" || !c.Limits.valid() || c.Created.After(now) || !c.Expires.After(now) || !c.Expires.After(c.Created) {
		return false
	}
	if c.Tier != "observe" && c.Tier != "enforce" {
		return false
	}
	if len(c.SupportedClasses) == 0 || len(c.SupportedClasses) > 5 {
		return false
	}
	for _, class := range c.SupportedClasses {
		if class != "raw_scan" && class != "file_read" && class != "graph_expand" {
			return false
		}
	}
	return true
}

type explorationDecision struct {
	AccountingRequired bool     `json:"accounting_required,omitempty"`
	Mode               string   `json:"mode"`
	Reason             string   `json:"reason"`
	WouldRestrict      bool     `json:"would_restrict"`
	Restricted         bool     `json:"restricted"`
	Alternatives       []string `json:"alternatives,omitempty"`
}

func explorationAlternatives() []string {
	return []string{"aimee index find <symbol>", "aimee index callers <symbol>", "aimee index span <file> <start> <end>"}
}

type explorationUsage struct {
	RawScans int64            `json:"raw_scans"`
	Graph    int64            `json:"graph_expansions"`
	Bytes    int64            `json:"returned_bytes"`
	Tokens   int64            `json:"returned_tokens"`
	Files    map[string]int64 `json:"files"`
}

// Costs are upper bounds reserved before dispatch. Uncertain dispatches keep the
// reservation; only a host-observed pre-dispatch cancellation can refund it.
type explorationAttempt struct {
	ActionDigest string             `json:"action_digest"`
	Path         string             `json:"canonical_path"`
	ID           string             `json:"id"`
	Binding      explorationBinding `json:"binding"`
	Class        string             `json:"class"`
	File         string             `json:"file,omitempty"`
	Bytes        int64              `json:"bytes"`
	Tokens       int64              `json:"tokens"`
}
type explorationReservation struct {
	ReservedAt time.Time           `json:"reserved_at"`
	Attempt    explorationAttempt  `json:"attempt"`
	Decision   explorationDecision `json:"decision"`
	Charged    bool                `json:"charged"`
	Started    bool                `json:"started"`
	Refunded   bool                `json:"refunded"`
}
type explorationSnapshot struct {
	CompletedTurns map[string]bool                      `json:"completed_turns"`
	CompletedAt    time.Time                            `json:"completed_at"`
	UnresolvedGap  bool                                 `json:"unresolved_gap"`
	Outcomes       map[string]explorationIndexedOutcome `json:"indexed_outcomes"`
	Fallbacks      []explorationFallback                `json:"fallbacks"`
	Revisions      []explorationContract                `json:"revisions"`
	TaskUsage      explorationUsage                     `json:"task_usage"`
	SessionUsage   explorationUsage                     `json:"session_usage"`
	Attempts       map[string]explorationReservation    `json:"attempts"`
	LastTurn       uint64                               `json:"last_turn"`
	StarvedTurns   uint64                               `json:"starved_turns"`
}

// The task owner supplies transactional durable storage. Commit must persist
// the entire snapshot atomically or return an error without changing storage.
// This type is private: tool JSON cannot issue contracts or attest outcomes.
type explorationLedger struct {
	mu     sync.Mutex
	state  explorationSnapshot
	commit func(explorationSnapshot) error
}

func newExplorationLedger(state explorationSnapshot, commit func(explorationSnapshot) error) *explorationLedger {
	return &explorationLedger{state: state, commit: commit}
}
func cloneUsage(u explorationUsage) explorationUsage {
	files := make(map[string]int64, len(u.Files))
	for k, v := range u.Files {
		files[k] = v
	}
	u.Files = files
	return u
}
func (s explorationSnapshot) clone() explorationSnapshot {
	s.Revisions = append([]explorationContract(nil), s.Revisions...)
	turns := make(map[string]bool, len(s.CompletedTurns))
	for k, v := range s.CompletedTurns {
		turns[k] = v
	}
	s.CompletedTurns = turns
	s.Fallbacks = append([]explorationFallback(nil), s.Fallbacks...)
	outcomes := make(map[string]explorationIndexedOutcome, len(s.Outcomes))
	for k, v := range s.Outcomes {
		outcomes[k] = v
	}
	s.Outcomes = outcomes
	s.TaskUsage = cloneUsage(s.TaskUsage)
	s.SessionUsage = cloneUsage(s.SessionUsage)
	attempts := make(map[string]explorationReservation, len(s.Attempts))
	for k, v := range s.Attempts {
		attempts[k] = v
	}
	s.Attempts = attempts
	return s
}
func (l *explorationLedger) save(s explorationSnapshot) error {
	if l.commit == nil {
		return errors.New("exploration state owner unavailable")
	}
	if err := l.commit(s); err != nil {
		return err
	}
	l.state = s
	return nil
}
func (l *explorationLedger) issue(c explorationContract, now time.Time) error {
	l.mu.Lock()
	defer l.mu.Unlock()
	if !c.valid(now) {
		return errors.New("invalid exploration contract")
	}
	s := l.state.clone()
	if len(s.Revisions) >= 128 {
		return errors.New("exploration revision history full")
	}
	if len(s.Revisions) > 0 {
		old := s.Revisions[len(s.Revisions)-1]
		if c.ID != old.ID || c.Revision != old.Revision+1 || c.Binding.Principal != old.Binding.Principal || c.Binding.Session != old.Binding.Session || c.Binding.Task != old.Binding.Task || c.Binding.budgetTask() != old.Binding.budgetTask() {
			return errors.New("contract revision cannot transfer task or session")
		}
	}
	// Copy reference-valued fields: a caller must not be able to mutate issued
	// ceilings after validation or change history through a shared pointer.
	c.SupportedClasses = append([]string(nil), c.SupportedClasses...)
	for _, p := range []**int64{&c.Limits.RawScans, &c.Limits.Files, &c.Limits.Graph, &c.Limits.Bytes, &c.Limits.Tokens} {
		if *p != nil {
			n := **p
			*p = &n
		}
	}
	s.Revisions = append(s.Revisions, c)
	return l.save(s)
}
func exceeds(n int64, ceiling *int64) bool { return ceiling != nil && n > *ceiling }
func (u explorationUsage) exceeds(l explorationLimits) bool {
	return exceeds(u.RawScans, l.RawScans) || exceeds(int64(len(u.Files)), l.Files) || exceeds(u.Graph, l.Graph) || exceeds(u.Bytes, l.Bytes) || exceeds(u.Tokens, l.Tokens)
}
func addUsage(u explorationUsage, a explorationAttempt) (explorationUsage, error) {
	if a.Bytes < 0 || a.Tokens < 0 || u.Bytes > math.MaxInt64-a.Bytes || u.Tokens > math.MaxInt64-a.Tokens || u.RawScans == math.MaxInt64 || u.Graph == math.MaxInt64 {
		return u, errors.New("invalid exploration cost")
	}
	u = cloneUsage(u)
	u.Bytes += a.Bytes
	u.Tokens += a.Tokens
	switch a.Class {
	case "raw_scan":
		u.RawScans++
	case "graph_expand":
		u.Graph++
	case "file_read":
		if a.File == "" {
			return u, errors.New("missing canonical file identity")
		}
		u.Files[a.File]++
	default:
		return u, errors.New("unsupported exploration class")
	}
	return u, nil
}
func (l *explorationLedger) reserve(a explorationAttempt, operator explorationLimits, optIn bool, calibrated func(string, string) bool, now time.Time) (explorationDecision, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	d := explorationDecision{Mode: "observe", Reason: "authenticated_contract_unavailable", Alternatives: explorationAlternatives()}
	if a.ID == "" || !a.Binding.valid() || !operator.valid() {
		return d, errors.New("invalid host exploration context")
	}
	s := l.state.clone()
	if old, ok := s.Attempts[a.ID]; ok {
		if old.Attempt != a {
			return d, errors.New("attempt identity reused with different action")
		}
		if old.Refunded {
			return d, errors.New("cancelled attempt cannot be redispatched")
		}
		return old.Decision, nil
	}
	if len(s.Attempts) >= 4096 {
		return d, errors.New("exploration attempt history full")
	}
	var c explorationContract
	active := false
	if len(s.Revisions) > 0 {
		c = s.Revisions[len(s.Revisions)-1]
		if c.Binding != a.Binding || !c.valid(now) {
			d.Reason = "contract_invalidated"
		} else {
			d.Reason = "unsupported_discovery_class"
			for _, class := range c.SupportedClasses {
				if class == a.Class {
					active = true
				}
			}
		}
	}
	tu, err := addUsage(s.TaskUsage, a)
	if err != nil {
		return d, err
	}
	su, err := addUsage(s.SessionUsage, a)
	if err != nil {
		return d, err
	}
	enforce := active && optIn && c.Limits.Enabled && c.Tier == "enforce" && c.CoverageComplete && calibrated != nil && calibrated(c.QueryClass, c.CalibrationReceipt)
	if enforce {
		d.Mode = "enforce"
	}
	if active {
		d.Reason = "within_exploration_budget"
	}
	d.WouldRestrict = active && c.Limits.Enabled && tu.exceeds(c.Limits)
	if d.WouldRestrict {
		d.Reason = "adaptive_exploration_budget_exhausted"
		d.Restricted = enforce
	}
	// A fallback is host-issued from a recorded indexed failure, scoped to this
	// exact contract and path. Consume only on admission, after operator limits.
	fallback := -1
	if d.Restricted && !su.exceeds(operator) {
		for i, capability := range s.Fallbacks {
			if capability.permits(c, a, now) {
				fallback = i
				d.Restricted = false
				d.Reason = "host_recorded_indexed_fallback"
				break
			}
		}
	}
	// Operator ceilings remain effective regardless of observe/relaxation state.
	if su.exceeds(operator) {
		d.Reason = "operator_exploration_budget_exhausted"
		d.Restricted = true
	}
	charged := !d.Restricted
	if charged {
		if fallback >= 0 {
			s.Fallbacks[fallback].Remaining--
		}
		s.TaskUsage = tu
		s.SessionUsage = su
	}
	s.Attempts[a.ID] = explorationReservation{Attempt: a, Decision: d, Charged: charged, ReservedAt: now}
	if err = l.save(s); err != nil {
		return explorationDecision{Mode: "observe", Reason: "contract_storage_unavailable"}, err
	}
	return d, nil
}
func (l *explorationLedger) started(id string) error {
	l.mu.Lock()
	defer l.mu.Unlock()
	s := l.state.clone()
	r, ok := s.Attempts[id]
	if !ok || !r.Charged || r.Refunded {
		return errors.New("attempt was not admitted")
	}
	r.Started = true
	s.Attempts[id] = r
	return l.save(s)
}
func refundUsage(u explorationUsage, a explorationAttempt) explorationUsage {
	u.Bytes -= a.Bytes
	u.Tokens -= a.Tokens
	switch a.Class {
	case "raw_scan":
		u.RawScans--
	case "graph_expand":
		u.Graph--
	case "file_read":
		u.Files[a.File]--
		if u.Files[a.File] == 0 {
			delete(u.Files, a.File)
		}
	}
	return u
}
func (l *explorationLedger) cancelBeforeDispatch(id string) error {
	l.mu.Lock()
	defer l.mu.Unlock()
	s := l.state.clone()
	r, ok := s.Attempts[id]
	if !ok || r.Started {
		return errors.New("cannot refund a possibly dispatched attempt")
	}
	if r.Refunded {
		return nil
	}
	if r.Charged {
		s.TaskUsage = refundUsage(s.TaskUsage, r.Attempt)
		s.SessionUsage = refundUsage(s.SessionUsage, r.Attempt)
	}
	r.Refunded = true
	s.Attempts[id] = r
	return l.save(s)
}

// Only the host task owner invokes this with completed-turn verification and
// indexed-tool outcomes. A model declaration of success is not an input.
func (l *explorationLedger) completedTurn(turn uint64, constrained, unresolved, indexedFailed, verifiedProgress bool) error {
	l.mu.Lock()
	defer l.mu.Unlock()
	s := l.state.clone()
	if turn <= s.LastTurn {
		return nil
	}
	s.LastTurn = turn
	if verifiedProgress || !unresolved {
		s.StarvedTurns = 0
	} else if constrained && indexedFailed {
		s.StarvedTurns++
	}
	threshold := uint64(2)
	if len(s.Revisions) > 0 && s.Revisions[len(s.Revisions)-1].Limits.StarvationTurns > 0 {
		threshold = s.Revisions[len(s.Revisions)-1].Limits.StarvationTurns
	}
	if s.StarvedTurns >= threshold && len(s.Revisions) > 0 {
		c := s.Revisions[len(s.Revisions)-1]
		if c.Tier != "observe" {
			if len(s.Revisions) >= 128 {
				return errors.New("exploration revision history full")
			}
			c.Revision++
			c.Tier = "observe"
			s.Revisions = append(s.Revisions, c)
		}
	}
	return l.save(s)
}
