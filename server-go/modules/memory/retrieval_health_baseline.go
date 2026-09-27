package memory

import (
	"sort"
	"time"
)

type healthAlert struct {
	Metric         string  `json:"metric"`
	Previous       float64 `json:"previous_rate"`
	Current        float64 `json:"current_rate"`
	PreviousLabels int     `json:"previous_labels"`
	CurrentLabels  int     `json:"current_labels"`
	Rule           string  `json:"rule"`
}
type healthBaseline struct {
	KindHHIChange map[string]float64 `json:"record_hhi_change_by_kind"`
	State         string             `json:"state"`
	Reason        string             `json:"reason"`
	From          time.Time          `json:"from"`
	Until         time.Time          `json:"until"`
	Invocations   int                `json:"retained_invocations"`
	HHIChange     *float64           `json:"record_hhi_change"`
	Alerts        []healthAlert      `json:"alerts"`
}

// Descriptive baseline alerts, not independence or statistical-significance
// claims. High exposure alone never alerts: a useful constraint may recur on
// every turn. Only explicitly labeled harm/recovery can raise an advisory.
func compareHealthBaseline(current, prior healthWindow) healthBaseline {
	cp, pp := current.Metrics.Population, prior.Metrics.Population
	r := healthBaseline{State: "unavailable", Alerts: []healthAlert{}, From: pp.From, Until: pp.Until, Invocations: prior.Metrics.Invocations}
	if cp.Namespace != pp.Namespace || cp.Principal != pp.Principal || cp.Project != pp.Project || cp.Workspace != pp.Workspace || cp.Purpose != pp.Purpose || cp.QueryClass != pp.QueryClass || cp.Stage != pp.Stage || !pp.Until.Equal(cp.From) || pp.Until.Sub(pp.From) != cp.Until.Sub(cp.From) {
		r.Reason = "population_mismatch"
		return r
	}
	if !current.Complete || !prior.Complete {
		r.Reason = "incomplete_window"
		return r
	}
	if current.Metrics.Sampled || prior.Metrics.Sampled || current.NotSampled > 0 || prior.NotSampled > 0 {
		r.Reason = "sampled_population"
		return r
	}
	if current.Metrics.Invocations < 30 || prior.Metrics.Invocations < 30 {
		r.Reason = "fewer_than_30_invocations_per_window"
		return r
	}
	r.State, r.Reason = "available", "descriptive_adjacent_window_comparison"
	r.KindHHIChange = map[string]float64{}
	for kind, c := range current.Metrics.ByKind {
		if p, ok := prior.Metrics.ByKind[kind]; ok && c.HHI != nil && p.HHI != nil && c.Occurrences >= 30 && p.Occurrences >= 30 {
			r.KindHHIChange[kind] = *c.HHI - *p.HHI
		}
	}
	if current.Metrics.Records.HHI != nil && prior.Metrics.Records.HHI != nil {
		delta := *current.Metrics.Records.HHI - *prior.Metrics.Records.HHI
		r.HHIChange = &delta
	}
	compare := func(name string, c, p healthRatio, decline bool) {
		if c.Rate == nil || p.Rate == nil || c.Denominator < 30 || p.Denominator < 30 || c.Unknown > 0 || p.Unknown > 0 {
			return
		}
		change := *c.Rate - *p.Rate
		if decline {
			change = -change
		}
		if change < 0.10-1e-12 {
			return
		}
		r.Alerts = append(r.Alerts, healthAlert{Metric: name, Current: *c.Rate, Previous: *p.Rate, CurrentLabels: c.Denominator, PreviousLabels: p.Denominator, Rule: "observed_adverse_change_at_least_10_percentage_points_with_30_labels_per_window"})
	}
	compare("current_lifecycle_reentry", current.Metrics.Lifecycle, prior.Metrics.Lifecycle, false)
	compare("sole_support_displacement", current.Metrics.Labels.Displacement, prior.Metrics.Labels.Displacement, false)
	// Map order is normalized by the final sort for deterministic text/JSON.
	for arm, ratio := range current.Metrics.Labels.Contamination {
		compare("arm_contamination:"+arm, ratio, prior.Metrics.Labels.Contamination[arm], false)
	}
	for arm, ratio := range current.Metrics.Labels.Recovery {
		compare("fusion_recovery:"+arm, ratio, prior.Metrics.Labels.Recovery[arm], true)
	}
	sort.Slice(r.Alerts, func(i, j int) bool { return r.Alerts[i].Metric < r.Alerts[j].Metric })
	return r
}
