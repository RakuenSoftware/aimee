package memory

import (
	"testing"
	"time"
)

func baselineFixture() (healthWindow, healthWindow) {
	p, _ := healthFixture()
	previous := p
	previous.From, previous.Until = p.From.Add(-time.Hour), p.From
	ratio := func(n, d int) healthRatio { r := healthRatio{Numerator: n, Denominator: d}; r.finish(); return r }
	current := healthWindow{Complete: true, Metrics: healthMetrics{Population: p, Invocations: 40, Lifecycle: ratio(8, 40)}}
	prior := healthWindow{Complete: true, Metrics: healthMetrics{Population: previous, Invocations: 40, Lifecycle: ratio(0, 40)}}
	return current, prior
}
func TestHealthBaselineRequiresComparableCompleteLabeledWindows(t *testing.T) {
	current, prior := baselineFixture()
	report := compareHealthBaseline(current, prior)
	if report.State != "available" || len(report.Alerts) != 1 || report.Alerts[0].Metric != "current_lifecycle_reentry" {
		t.Fatal(report)
	}
	for _, mutate := range []func(*healthWindow){
		func(w *healthWindow) { w.Complete = false },
		func(w *healthWindow) { w.Metrics.Sampled = true },
		func(w *healthWindow) { w.NotSampled = 1 },
		func(w *healthWindow) { w.Metrics.Invocations = 29 },
		func(w *healthWindow) { w.Metrics.Population.Project = "different" },
		func(w *healthWindow) { w.Metrics.Population.From = w.Metrics.Population.From.Add(time.Second) },
	} {
		c := current
		mutate(&c)
		if r := compareHealthBaseline(c, prior); r.State != "unavailable" || len(r.Alerts) != 0 {
			t.Fatal(r)
		}
	}
	current.Metrics.Lifecycle.Unknown = 1
	if r := compareHealthBaseline(current, prior); len(r.Alerts) != 0 {
		t.Fatal("unknown labels permitted alert", r)
	}
}
func TestHealthBaselineDoesNotAlertOnPopularConstraint(t *testing.T) {
	current, prior := baselineFixture()
	current.Metrics.Lifecycle = healthRatio{}
	current.Metrics.Records = healthConcentrationOf(map[string]int{"constraint": 400})
	prior.Metrics.Records = healthConcentrationOf(map[string]int{"one": 200, "two": 200})
	report := compareHealthBaseline(current, prior)
	if report.HHIChange == nil || *report.HHIChange != 0.5 || len(report.Alerts) != 0 {
		t.Fatal(report)
	}
}
