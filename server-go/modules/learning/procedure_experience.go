package learning

// Learning owns outcome attribution. This contract consumes owner-authenticated
// exposure references, never raw model claims that a procedure was used. The
// host admits and persists events; memory consumes the resulting projection.
import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"math"
	"reflect"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

const EventExperience uint32 = 6146
const StageExperience uint32 = 2

type ProcedureVersion struct {
	Owner    string `json:"owner_id"`
	ID       string `json:"procedure_id"`
	Revision string `json:"revision"`
}

type ProcedureExposure struct {
	Receipt   string           `json:"receipt_ref"`
	Request   string           `json:"request_id"`
	Attempt   string           `json:"attempt_id"`
	Procedure ProcedureVersion `json:"procedure"`
	State     string           `json:"state"`    // retrieved or delivered
	Producer  string           `json:"producer"` // authenticated owner, assigned by host
}

type ProcedureEvent struct {
	SchemaVersion     int              `json:"schema_version"`
	ID                string           `json:"event_id"`
	Trial             string           `json:"trial_id"`
	Task              string           `json:"task_id"`
	Attempt           string           `json:"attempt_id"`
	Procedure         ProcedureVersion `json:"procedure"`
	Receipt           string           `json:"receipt_ref"`
	State             string           `json:"state"`
	Authority         string           `json:"authority"` // model, user_feedback, host_execution
	Actor             string           `json:"actor"`
	Environment       string           `json:"environment"`
	Model             string           `json:"model"`
	TaskClass         string           `json:"task_class"`
	Applicability     []string         `json:"applicability_gaps"`
	Actions           []string         `json:"action_refs"`
	Verifier          string           `json:"verifier"`
	Verification      string           `json:"verification_ref"`
	Revision          string           `json:"verified_revision"`
	Cost              string           `json:"cost_report_ref"`
	ReportedLatencyMS string           `json:"reported_latency_ms,omitempty"`
	ObservedAt        string           `json:"observed_at"`
	TerminalTask      bool             `json:"terminal_task"`
}

type ProcedureCohort struct {
	Procedure          ProcedureVersion    `json:"procedure"`
	Environment        string              `json:"environment"`
	Model              string              `json:"model"`
	TaskClass          string              `json:"task_class"`
	Trials             int                 `json:"trials"`
	Retrieved          int                 `json:"retrieved"`
	Delivered          int                 `json:"delivered"`
	Selected           int                 `json:"selected"`
	Applied            int                 `json:"applied"`
	Success            int                 `json:"verified_success"`
	Failure            int                 `json:"verified_failure"`
	Unknown            int                 `json:"outcome_unknown"`
	Conflicting        int                 `json:"conflicting"`
	Abandoned          int                 `json:"abandoned"`
	TerminalTasks      int                 `json:"terminal_tasks"`
	TerminalSuccess    int                 `json:"terminal_success"`
	TerminalFailure    int                 `json:"terminal_failure"`
	TerminalConflict   int                 `json:"terminal_conflicting"`
	LastSuccess        string              `json:"last_verified_success,omitempty"`
	Counterexamples    []string            `json:"counterexample_refs"`
	Gaps               []string            `json:"applicability_gaps"`
	Evidence           []string            `json:"event_refs"`
	CostReports        []string            `json:"cost_report_refs"`
	ReportedLatencies  []map[string]string `json:"reported_trial_latencies"`
	UnknownLatency     int                 `json:"unknown_latency_trials"`
	ConflictingLatency int                 `json:"conflicting_latency_trials"`
	CostCoverage       string              `json:"cost_coverage"`
	VerifiedSample     int                 `json:"verified_sample_count"`
	SuccessInterval    *[2]float64         `json:"success_rate_wilson_95,omitempty"`
	Interpretation     string              `json:"interpretation"`
}

func procedureKey(v any) string { b, _ := json.Marshal(v); return string(b) }
func validProcedure(v ProcedureVersion) bool {
	return v.Owner != "" && v.ID != "" && v.Revision != "" && len(v.Owner) <= 128 && len(v.ID) <= 256 && len(v.Revision) <= 256
}
func eventTerminal(s string) bool { return s == "verified_success" || s == "verified_failure" }

// ValidateProcedureEvent receives context outside the public JSON arguments.
// Exposure must have come from the host's principal-owned receipt reader. A
// model signal can be retained but cannot establish application or correctness.
func ValidateProcedureEvent(event ProcedureEvent, exposure ProcedureExposure, caller bus.CommandContext) error {
	bad := errors.New("unverified or inconsistent procedure application")
	if !caller.Authenticated || caller.Principal == "" || event.Actor != caller.Principal || event.SchemaVersion != 1 || !validProcedure(event.Procedure) || event.ID == "" || event.Trial == "" || event.Task == "" || event.Attempt == "" || event.Environment == "" || event.Model == "" || event.TaskClass == "" {
		return bad
	}
	for _, s := range []string{event.ID, event.Trial, event.Task, event.Attempt, event.Environment, event.Model, event.TaskClass, event.Verifier, event.Revision} {
		if len(s) > 256 || strings.ContainsRune(s, 0) {
			return bad
		}
	}
	if event.ReportedLatencyMS != "" {
		n, e := strconv.ParseInt(event.ReportedLatencyMS, 10, 64)
		if e != nil || n < 0 || strconv.FormatInt(n, 10) != event.ReportedLatencyMS {
			return bad
		}
	}
	at, err := time.Parse(time.RFC3339Nano, event.ObservedAt)
	if err != nil || at.IsZero() {
		return bad
	}
	if event.Receipt == "" || event.Receipt != exposure.Receipt || event.Attempt != exposure.Attempt || event.Procedure != exposure.Procedure || exposure.Producer != "local_host_ledger" || exposure.Request == "" {
		return bad
	}
	if exposure.State != "retrieved" && exposure.State != "delivered" {
		return bad
	}
	switch event.Authority {
	case "model":
	case "user_feedback":
		if !caller.UserAuthority {
			return bad
		}
	case "host_execution":
		if caller.UserAuthority || caller.TransportIdentity != "host:procedure-execution" {
			return bad
		}
	default:
		return bad
	}
	switch event.State {
	case "retrieved":
	case "delivered", "selected_for_use":
		if exposure.State != "delivered" {
			return bad
		}
	case "applied", "verified_success", "verified_failure", "abandoned":
		if exposure.State != "delivered" || event.Authority == "model" || len(event.Actions) == 0 {
			return bad
		}
	case "outcome_unknown":
	default:
		return bad
	}
	if len(event.Actions) > 64 || len(event.Applicability) > 32 {
		return bad
	}
	for _, refs := range [][]string{event.Actions, event.Applicability} {
		for _, ref := range refs {
			if ref == "" || len(ref) > 1024 {
				return bad
			}
		}
	}
	if eventTerminal(event.State) {
		if event.Verification == "" || len(event.Verification) > 1024 {
			return bad
		}
		switch event.Verifier {
		case "tests_bound_revision":
			if event.Revision == "" {
				return bad
			}
		case "external_effect", "reviewed_answer_support":
		case "explicit_user_evaluation":
			if event.Authority != "user_feedback" {
				return bad
			}
		default:
			return bad
		}
	}
	if event.TerminalTask && !eventTerminal(event.State) {
		return bad
	}
	return nil
}

// ProjectProcedureExperience requires already admitted immutable owner rows.
// Duplicate event IDs are idempotent, conflicting replacements are errors.
// Success and failure for one trial become conflicting, never last-write-wins.
func ProjectProcedureExperience(events []ProcedureEvent) ([]ProcedureCohort, error) {
	if len(events) > 4096 {
		return nil, errors.New("experience page too large")
	}
	type trial struct {
		cohort  string
		binding string
		states  map[string]bool
		events  []ProcedureEvent
	}
	trials := map[string]*trial{}
	seen := map[string]ProcedureEvent{}
	cohorts := map[string]*ProcedureCohort{}
	for _, e := range events {
		if old, ok := seen[e.ID]; ok {
			if !reflect.DeepEqual(old, e) {
				return nil, errors.New("conflicting immutable event")
			}
			continue
		}
		seen[e.ID] = e
		if e.ID == "" || e.Trial == "" || e.Task == "" || e.Attempt == "" || !validProcedure(e.Procedure) {
			return nil, errors.New("invalid admitted event")
		}
		key := procedureKey([]any{e.Procedure, e.Environment, e.Model, e.TaskClass})
		if cohorts[key] == nil {
			cohorts[key] = &ProcedureCohort{Procedure: e.Procedure, Environment: e.Environment, Model: e.Model, TaskClass: e.TaskClass, Counterexamples: []string{}, Gaps: []string{}, Evidence: []string{}, CostReports: []string{}, ReportedLatencies: []map[string]string{}, CostCoverage: "owner_report_references; unresolved_cost_is_unknown", Interpretation: "observed_association_not_causation; correlated_trials_possible"}
		}
		trialKey := procedureKey([]string{e.Actor, e.Task, e.Attempt, e.Trial})
		binding := procedureKey([]any{e.Procedure, e.Receipt, e.Environment, e.Model, e.TaskClass})
		t := trials[trialKey]
		if t == nil {
			t = &trial{cohort: key, binding: binding, states: map[string]bool{}}
			trials[trialKey] = t
		} else if t.binding != binding {
			return nil, errors.New("trial binding changed")
		}
		t.events = append(t.events, e)
		// Legacy/model claims remain visible as evidence but cannot affect applied or
		// verified counts, even if a historical row used a misleading state name.
		if e.Authority != "model" || e.State == "retrieved" || e.State == "delivered" || e.State == "selected_for_use" {
			t.states[e.State] = true
		}
	}
	terminal := map[string]map[string]map[string]bool{}
	for _, t := range trials {
		c := cohorts[t.cohort]
		c.Trials++
		applied := t.states["applied"] || t.states["verified_success"] || t.states["verified_failure"]
		selected := applied || t.states["selected_for_use"] || t.states["abandoned"]
		delivered := selected || t.states["delivered"]
		if delivered || t.states["retrieved"] {
			c.Retrieved++
		}
		if delivered {
			c.Delivered++
		}
		if selected {
			c.Selected++
		}
		if applied {
			c.Applied++
		}
		conflict := t.states["verified_success"] && t.states["verified_failure"]
		switch {
		case conflict:
			c.Conflicting++
		case t.states["verified_success"]:
			c.Success++
		case t.states["verified_failure"]:
			c.Failure++
		case t.states["abandoned"]:
			c.Abandoned++
		default:
			c.Unknown++
		}
		latencies := map[string]bool{}
		for _, e := range t.events {
			if e.Authority != "model" && e.ReportedLatencyMS != "" {
				latencies[e.ReportedLatencyMS] = true
			}
		}
		switch len(latencies) {
		case 0:
			c.UnknownLatency++
		case 1:
			for value := range latencies {
				e := t.events[0]
				c.ReportedLatencies = append(c.ReportedLatencies, map[string]string{"task_id": e.Task, "attempt_id": e.Attempt, "trial_id": e.Trial, "reported_latency_ms": value})
			}
		default:
			c.ConflictingLatency++
		}
		for _, e := range t.events {
			c.Evidence = append(c.Evidence, e.ID)
			c.Gaps = append(c.Gaps, e.Applicability...)
			if e.Cost != "" {
				c.CostReports = append(c.CostReports, e.Cost)
			}
			if e.Authority != "model" && e.State == "verified_failure" {
				c.Counterexamples = append(c.Counterexamples, e.Verification)
			}
			if !conflict && e.Authority != "model" && e.State == "verified_success" {
				a, _ := time.Parse(time.RFC3339Nano, e.ObservedAt)
				b, _ := time.Parse(time.RFC3339Nano, c.LastSuccess)
				if a.After(b) {
					c.LastSuccess = e.ObservedAt
				}
			}
			if e.TerminalTask && e.Authority != "model" && eventTerminal(e.State) {
				if terminal[t.cohort] == nil {
					terminal[t.cohort] = map[string]map[string]bool{}
				}
				task := procedureKey([]string{e.Actor, e.Task})
				if terminal[t.cohort][task] == nil {
					terminal[t.cohort][task] = map[string]bool{}
				}
				terminal[t.cohort][task][e.State] = true
			}
		}
	}
	keys := []string{}
	for k := range cohorts {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	out := []ProcedureCohort{}
	for _, key := range keys {
		c := cohorts[key]
		for _, states := range terminal[key] {
			c.TerminalTasks++
			if states["verified_success"] && states["verified_failure"] {
				c.TerminalConflict++
			} else if states["verified_success"] {
				c.TerminalSuccess++
			} else {
				c.TerminalFailure++
			}
		}
		c.VerifiedSample = c.Success + c.Failure
		if c.VerifiedSample > 0 {
			n := float64(c.VerifiedSample)
			p := float64(c.Success) / n
			z := 1.959963984540054
			d := 1 + z*z/n
			center := (p + z*z/(2*n)) / d
			half := z * math.Sqrt(p*(1-p)/n+z*z/(4*n*n)) / d
			c.SuccessInterval = &[2]float64{math.Max(0, center-half), math.Min(1, center+half)}
		}
		sort.Slice(c.ReportedLatencies, func(i, j int) bool {
			return procedureKey(c.ReportedLatencies[i]) < procedureKey(c.ReportedLatencies[j])
		})
		c.Evidence = uniqueProcedureRefs(c.Evidence)
		c.Counterexamples = uniqueProcedureRefs(c.Counterexamples)
		c.Gaps = uniqueProcedureRefs(c.Gaps)
		c.CostReports = uniqueProcedureRefs(c.CostReports)
		out = append(out, *c)
	}
	return out, nil
}
func uniqueProcedureRefs(values []string) []string {
	sort.Strings(values)
	out := []string{}
	for _, s := range values {
		if len(out) == 0 || out[len(out)-1] != s {
			out = append(out, s)
		}
	}
	return out
}

// Only the authenticated host may present receipt-reader evidence to this
// process stage. Public arguments cannot set the caller context on the wire.
func handleExperience(invocation bus.ModuleInvocation, raw []byte) ([]byte, bus.ModuleStatus) {
	if invocation.PrincipalRef != 0 || len(raw) > 2<<20 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	verb, args, caller, err := bus.DecodeCommandWithContext(raw)
	if err != nil || caller == nil || !caller.Authenticated {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var req struct {
		Event    ProcedureEvent    `json:"event"`
		Exposure ProcedureExposure `json:"exposure"`
		Events   []ProcedureEvent  `json:"events"`
	}
	d := json.NewDecoder(bytes.NewReader(args))
	d.DisallowUnknownFields()
	if d.Decode(&req) != nil || d.Decode(new(any)) != io.EOF {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var result any
	switch verb {
	case "admit":
		if ValidateProcedureEvent(req.Event, req.Exposure, *caller) != nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		result = req.Event
	case "project":
		result, err = ProjectProcedureExperience(req.Events)
		if err != nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
	body, err := json.Marshal(result)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	framed, err := bus.EncodeCommandResult(body)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return framed, bus.ModuleStatusOK
}
