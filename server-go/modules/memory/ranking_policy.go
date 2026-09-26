package memory

import "math"

// A rank contract is used for native staged RRF: scores from different native
// stages have no common additive scale. Every optional prior and their combined
// ordering may move a record at most three positions within its scope stratum.
// This safety ceiling is not a fitted claim of improved task quality.
const nativePriorRankDisplacement = 3
const nativePriorPolicy = "staged-rrf60-prior-rankcap3-v2"

type rankPriorPolicy struct {
	Version        string         `json:"version"`
	BaseScale      string         `json:"base_scale"`
	Strata         string         `json:"strata"`
	IndividualCaps map[string]int `json:"individual_max_rank_displacement"`
	JointCap       int            `json:"joint_max_rank_displacement"`
	ExposureState  string         `json:"exposure_state"`
}

func nativeRankingPriorPolicy() rankPriorPolicy {
	return rankPriorPolicy{Version: nativePriorPolicy, BaseScale: "eligible_staged_rrf60_order", Strata: "existing_scope_priority", IndividualCaps: map[string]int{"pagerank": 3, "recency": 0, "automatic_serving": 0, "outcome": 0, "trust": 0}, JointCap: 3, ExposureState: "disabled; not_authority_or_confidence"}
}

// Deadline-constrained selection projects desired scores onto a bounded rank
// permutation. The earliest outstanding deadline prevents late displacement;
// the eligibility window prevents early displacement. Ties keep base order.
func boundedPriorOrder(base []Record, displacement int) []Record {
	if displacement <= 0 {
		return append([]Record(nil), base...)
	}
	used := make([]bool, len(base))
	out := make([]Record, 0, len(base))
	score := func(r Record) float64 {
		if math.IsNaN(r.retrievalScore) || math.IsInf(r.retrievalScore, 0) {
			return math.Inf(-1)
		}
		return r.retrievalScore
	}
	first := 0
	for rank := 0; rank < len(base); rank++ {
		for first < len(base) && used[first] {
			first++
		}
		next := first
		if first > rank-displacement {
			for i := first + 1; i < len(base) && i <= rank+displacement; i++ {
				if !used[i] && score(base[i]) > score(base[next]) {
					next = i
				}
			}
		}
		used[next] = true
		r := base[next]
		r.priorBaseRank = next + 1
		r.priorFinalRank = rank + 1
		out = append(out, r)
	}
	return out
}

// Score caps are stage-specific. The lexical joint bound is one quarter of
// the smallest exact-match class gap (0.5); graph priors jointly use half the
// smallest one-hop relation-gravity gap (0.05). Neither is a probability scale.
const assertionLexicalPriorPolicy = "assertion-lexical-priors-v2"
const assertionLexicalPriorBound = .125
const graphRankingPriorPolicy = "graph-relevance-priors-v2"
const graphRankingPriorBound = .025

type scorePriorAdjustment struct {
	InputState string  `json:"input_state"`
	Name       string  `json:"name"`
	Raw        float64 `json:"raw"`
	Applied    float64 `json:"applied"`
	Bound      float64 `json:"bound"`
}
type scorePriorResult struct {
	Policy      string                 `json:"policy"`
	Base        float64                `json:"base"`
	JointBound  float64                `json:"joint_bound"`
	Adjustments []scorePriorAdjustment `json:"adjustments"`
	JointDelta  float64                `json:"joint_delta"`
	Final       float64                `json:"final"`
}

func boundedScorePriors(policy string, base, bound float64, priors ...scorePriorAdjustment) scorePriorResult {
	result := scorePriorResult{Policy: policy, Base: base, JointBound: bound, Adjustments: append([]scorePriorAdjustment(nil), priors...)}
	for i := range result.Adjustments {
		p := &result.Adjustments[i]
		if math.IsNaN(p.Raw) || math.IsInf(p.Raw, 0) {
			p.Raw = 0
			p.InputState = "nonfinite_ignored"
		} else if p.InputState != "nonfinite_ignored" {
			p.InputState = "observed"
		}
		p.Applied = math.Max(-p.Bound, math.Min(p.Bound, p.Raw))
		result.JointDelta += p.Applied
	}
	result.JointDelta = math.Max(-bound, math.Min(bound, result.JointDelta))
	result.Final = base + result.JointDelta
	return result
}
func boundedGraphRelevance(relation string, code bool, structural, observed int, utility float64, hop int, class string) scorePriorResult {
	baseClass := ""
	if class != "" {
		baseClass = "A"
	}
	base := graphEdgeScore(relation, code, structural, 0, 0, hop, baseClass)
	observations := graphEdgeScore(relation, code, structural, observed, 0, hop, baseClass) - base
	return boundedScorePriors(graphRankingPriorPolicy, base, graphRankingPriorBound,
		scorePriorAdjustment{Name: "observed_links", Raw: observations, Bound: graphRankingPriorBound / 3},
		scorePriorAdjustment{Name: "legacy_utility", Raw: base * utility, Bound: graphRankingPriorBound / 3},
		scorePriorAdjustment{Name: "confidence_class", Raw: base * (graphConfidence(class) - 1), Bound: graphRankingPriorBound / 3})
}

func validScorePriorResult(p *scorePriorResult) bool {
	if p == nil || math.IsNaN(p.Base) || math.IsInf(p.Base, 0) {
		return false
	}
	bounds := map[string]float64{}
	joint := 0.0
	switch p.Policy {
	case assertionLexicalPriorPolicy:
		joint = assertionLexicalPriorBound
		bounds = map[string]float64{"confidence": .0625, "authority": .0625}
	case graphRankingPriorPolicy:
		joint = graphRankingPriorBound
		bounds = map[string]float64{"observed_links": joint / 3, "legacy_utility": joint / 3, "confidence_class": joint / 3}
	default:
		return false
	}
	if p.JointBound != joint || len(p.Adjustments) != len(bounds) {
		return false
	}
	seen := map[string]bool{}
	for _, a := range p.Adjustments {
		if (a.InputState != "observed" && a.InputState != "nonfinite_ignored") || (a.InputState == "nonfinite_ignored" && a.Raw != 0) || seen[a.Name] || bounds[a.Name] == 0 || bounds[a.Name] != a.Bound || math.IsNaN(a.Raw) || math.IsInf(a.Raw, 0) {
			return false
		}
		seen[a.Name] = true
	}
	computed := boundedScorePriors(p.Policy, p.Base, joint, p.Adjustments...)
	if math.Abs(computed.Final-p.Final) > 1e-12 || math.Abs(computed.JointDelta-p.JointDelta) > 1e-12 || math.IsNaN(p.Final) || math.IsNaN(p.JointDelta) {
		return false
	}
	for i, a := range p.Adjustments {
		if a.Applied != computed.Adjustments[i].Applied {
			return false
		}
	}
	return true
}
