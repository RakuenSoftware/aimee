package delegates

import "strings"

// What a coordinated run of delegates cost the SUPERVISOR.
//
// The cost model is "free delegates, expensive supervisor": delegate tokens are
// cheap and a human's attention is not, so a run is judged by how much
// supervisor attention it consumed, not by how many delegates ran. Every signal
// here is a proxy for that -- an invalid handoff, a failed task, a supervisor
// action a delegate asked for, a blocking reviewer finding.

const tierBuckets = 4

// EconomicsReport mirrors delegate_economics_report_t.
type EconomicsReport struct {
	DelegateCount                   int
	TierCounts                      [tierBuckets]int
	UnknownTierCount                int
	PromptTokensTotal               int
	CompletionTokensTotal           int
	DelegateTokensEstimated         int
	TokenizedDelegateResults        int
	SupervisorPromptTokensEstimated int
	HandoffCount                    int
	ValidHandoffs                   int
	InvalidHandoffs                 int
	FocusedTestsRunByDelegates      int
	DelegatesWithFocusedTests       int
	ManualIntegrationEvents         int
	SupervisorActionsRequired       int
	ReviewerFindingsBlocking        int
	Verdict                         string
	Recommendation                  string
}

// EconomicsTask is one coordinated task, as the caller knows it. Only the four
// fields the rule reads travel; the rest of the row is the caller's business.
type EconomicsTask struct {
	Status    string
	ClaimedBy string
	Files     string // owned_files JSON, for the handoff's ownership check
	Result    string // the delegate's result JSON
}

// AgentTier maps an agent name to its cost tier. The caller sends the agents it
// configured because tiers are its knowledge, not this module's.
type AgentTier struct {
	Name string
	Tier int
}

func findAgentTier(agents []AgentTier, name string) (int, bool) {
	if name == "" {
		return 0, false
	}
	for _, a := range agents {
		if a.Name == name {
			return a.Tier, true
		}
	}
	return 0, false
}

// resultInt reads an integer field from the delegate's result, defaulting when
// absent. cJSON reports valueint, so a fractional value truncates.
func resultInt(root *jsonValue, name string, fallback int) int {
	v := root.get(name)
	if v == nil || v.kind != jsonNumber {
		return fallback
	}
	return clampToInt(v.num)
}

// resultAgentTier prefers what the delegate reported, then who claimed the
// task, then the agent named in the result. Unknown is its own bucket rather
// than a guess -- counting an unknown tier as cheap would flatter the run.
func resultAgentTier(root *jsonValue, task EconomicsTask, agents []AgentTier) int {
	if tier := root.get("agent_cost_tier"); tier != nil && tier.kind == jsonNumber {
		return clampToInt(tier.num)
	}
	if task.ClaimedBy != "" {
		if tier, ok := findAgentTier(agents, task.ClaimedBy); ok {
			return tier
		}
	}
	agent := root.get("agent")
	if !agent.isString() {
		agent = root.get("agent_name")
	}
	if agent.isString() {
		if tier, ok := findAgentTier(agents, agent.str); ok {
			return tier
		}
	}
	return -1
}

func isHandoffSchema(v *jsonValue) bool {
	s := v.get("schema_version")
	return s.isString() && s.str == "delegate_result_v1"
}

// handoffText finds the handoff inside a result: the result may BE one, or
// carry one as a `response` string, or as a `response` object.
func handoffText(root *jsonValue, raw string) string {
	if isHandoffSchema(root) {
		return raw
	}
	response := root.get("response")
	if response.isString() && response.str != "" {
		return response.str
	}
	if response.isObject() {
		return printJSON(response)
	}
	return ""
}

// handoffObject returns the handoff as an object, for reading the supervisor
// actions it asked for.
func handoffObject(root *jsonValue, text string) *jsonValue {
	if isHandoffSchema(root) {
		return root
	}
	if text == "" {
		return nil
	}
	if parsed, ok := parseJSONPrefix(text); ok && isHandoffSchema(parsed) {
		return parsed
	}
	return nil
}

func (r *EconomicsReport) addTier(tier int) {
	if tier >= 0 && tier < tierBuckets {
		r.TierCounts[tier]++
		return
	}
	r.UnknownTierCount++
}

// IsTier0Heavy reports whether at least half the delegates ran on the cheapest
// tier -- the shape the cost model is designed around.
func (r *EconomicsReport) IsTier0Heavy() bool {
	return r.DelegateCount > 0 && r.TierCounts[0]*2 >= r.DelegateCount
}

func (r *EconomicsReport) addTask(task EconomicsTask, agents []AgentTier) {
	// A row is a delegate run if someone claimed it or it reached a terminal
	// state; anything else is still queued and says nothing about cost.
	if task.ClaimedBy == "" && task.Status != "done" && task.Status != "failed" {
		return
	}
	r.DelegateCount++

	var root *jsonValue
	if task.Result != "" {
		if parsed, ok := parseJSONPrefix(task.Result); ok {
			root = parsed
		}
	}
	r.addTier(resultAgentTier(root, task, agents))

	promptTokens := resultInt(root, "prompt_tokens", 0)
	completionTokens := resultInt(root, "completion_tokens", 0)
	delegateTokens := resultInt(root, "delegate_tokens_estimated", 0)
	if delegateTokens <= 0 {
		delegateTokens = promptTokens + completionTokens
	}
	if delegateTokens > 0 {
		r.PromptTokensTotal += promptTokens
		r.CompletionTokensTotal += completionTokens
		r.DelegateTokensEstimated += delegateTokens
		r.TokenizedDelegateResults++
	}

	manual := false
	if task.Status == "failed" {
		r.SupervisorActionsRequired++
		manual = true
	}

	text := handoffText(root, task.Result)

	if task.Status == "done" {
		r.HandoffCount++
		// The rule for believing a handoff lives in this module too, so this is
		// a direct call rather than a trip back out through the caller.
		verdict, ok := ValidateHandoff(text, task.Files, true)
		if ok {
			r.ValidHandoffs++
			r.FocusedTestsRunByDelegates += verdict.PassedTests
			if verdict.PassedTests > 0 {
				r.DelegatesWithFocusedTests++
			}
			if verdict.OutsideOwnershipCount > 0 || verdict.NeedsSupervisorReview {
				manual = true
			}
		} else {
			r.InvalidHandoffs++
			manual = true
		}
	}

	if handoff := handoffObject(root, text); handoff != nil {
		if actions := handoff.get("supervisor_actions"); actions != nil && actions.kind == jsonArray {
			if n := len(actions.items); n > 0 {
				r.SupervisorActionsRequired += n
				manual = true
			}
		}
	}
	r.ReviewerFindingsBlocking += resultInt(root, "reviewer_findings_blocking", 0)

	if manual {
		r.ManualIntegrationEvents++
	}
}

// finalize estimates the supervisor tokens the run cost and states a verdict.
//
// The weights are deliberate proxies, not measurements: reading a delegate's
// output costs something, an integration event costs more, an invalid handoff
// more still, and a blocking reviewer finding sits between them.
func (r *EconomicsReport) finalize() {
	r.SupervisorPromptTokensEstimated = r.DelegateCount*300 +
		r.ManualIntegrationEvents*600 + r.InvalidHandoffs*800 + r.ReviewerFindingsBlocking*500

	tier0Heavy := r.IsTier0Heavy()
	expensiveOnly := r.DelegateCount > 0 && r.TierCounts[0] == 0 && r.TierCounts[1] == 0 &&
		r.UnknownTierCount == 0
	highManual := r.ManualIntegrationEvents > r.DelegateCount/2
	lowVerification := r.DelegateCount > 0 && r.DelegatesWithFocusedTests*2 < r.DelegateCount

	switch {
	case r.DelegateCount <= 0:
		r.Verdict = "unclear"
	case tier0Heavy && !highManual && r.InvalidHandoffs <= r.DelegateCount/2:
		r.Verdict = "likely_net_win"
	case expensiveOnly && (r.InvalidHandoffs > 0 || highManual || lowVerification):
		r.Verdict = "likely_net_loss"
	default:
		r.Verdict = "unclear"
	}

	switch {
	case tier0Heavy:
		r.Recommendation = "Tier-0-heavy run: broader delegation and redundant validation are " +
			"reasonable when task risk warrants it."
	case r.Verdict == "likely_net_loss":
		r.Recommendation = "Delegate conservatively: expensive delegates plus supervisor " +
			"follow-up likely outweighed the saved attention."
	default:
		r.Recommendation = "Delegate selectively: supervisor cost savings are unclear from " +
			"this run."
	}
}

// BuildEconomicsReport aggregates a coordinated run.
func BuildEconomicsReport(tasks []EconomicsTask, agents []AgentTier) EconomicsReport {
	var report EconomicsReport
	for _, task := range tasks {
		report.addTask(task, agents)
	}
	report.finalize()
	return report
}

// EconomicsCostModelLabel names the model the verdict is expressed in.
func EconomicsCostModelLabel() string { return "free delegates, expensive supervisor" }

// EconomicsVerdictText renders a verdict for a reader.
func EconomicsVerdictText(verdict string) string {
	switch strings.TrimSpace(verdict) {
	case "likely_net_win":
		return "likely net supervisor-token win"
	case "likely_net_loss":
		return "likely net supervisor-token loss"
	}
	return "unclear supervisor-token outcome"
}
