package memory

import (
	"fmt"
	"sort"
	"strings"
)

// Formatting stays with the metric owner. Unknown labels and incomplete
// populations are prominent in text as well as in the JSON projection.
func healthReportText(r healthWindow) string {
	var b strings.Builder
	p := r.Metrics.Population
	fmt.Fprintf(&b, "Retrieval health (prepared-at window, latest reconciled stages): %s to %s\nProject: %q; workspace: %q; purpose: %s; query class: %s\n", p.From.Format("2006-01-02T15:04:05Z07:00"), p.Until.Format("2006-01-02T15:04:05Z07:00"), p.Project, p.Workspace, p.Purpose, p.QueryClass)
	fmt.Fprintf(&b, "Window complete: %t; capacity evictions: %d; retention expirations: %d\n", r.Complete, r.Evicted, r.Expired)
	for _, stage := range []string{"dispatched", "assembled_unsent", "unknown_dispatch", "network_uncertain"} {
		fmt.Fprintf(&b, "%s: %d retained attempts, %d record occurrences\n", stage, r.Attempts[stage], r.Records[stage])
	}
	fmt.Fprintf(&b, "Selected stage: %s; retained sample: %d invocations; %d unsampled invocations\n", p.Stage, r.Sampled, r.NotSampled)
	value := func(p *float64) string {
		if p == nil {
			return "unknown"
		}
		return fmt.Sprintf("%.4f", *p)
	}
	fmt.Fprintf(&b, "Record concentration (retained sample): top-1 %s; top-5 %s; HHI %s; Simpson %s; entropy %s; normalized entropy %s\n", value(r.Metrics.Records.Top1Share), value(r.Metrics.Records.Top5Share), value(r.Metrics.Records.HHI), value(r.Metrics.Records.Simpson), value(r.Metrics.Records.Entropy), value(r.Metrics.Records.Normalized))
	fmt.Fprintf(&b, "Estimated occurrences: %s; sampling-design standard error: %s\n", value(r.Metrics.EstimatedOccurrences), value(r.Metrics.EstimateSE))
	fmt.Fprintf(&b, "Repeat-serving: %s (%d comparable, %d unknown); lifecycle re-entry: %s (%d labeled, %d unknown)\n", value(r.Metrics.Repeat.Rate), r.Metrics.Repeat.Denominator, r.Metrics.Repeat.Unknown, value(r.Metrics.Lifecycle.Rate), r.Metrics.Lifecycle.Denominator, r.Metrics.Lifecycle.Unknown)
	fmt.Fprintf(&b, "Version concentration: %d occurrences, %d distinct; top-1 %s; HHI %s; entropy %s\n", r.Metrics.Versions.Occurrences, r.Metrics.Versions.Distinct, value(r.Metrics.Versions.Top1Share), value(r.Metrics.Versions.HHI), value(r.Metrics.Versions.Entropy))
	kinds := make([]string, 0, len(r.Metrics.ByKind))
	for kind := range r.Metrics.ByKind {
		kinds = append(kinds, kind)
	}
	sort.Strings(kinds)
	for _, kind := range kinds {
		c := r.Metrics.ByKind[kind]
		fmt.Fprintf(&b, "Memory kind %s: %d occurrences; HHI %s\n", kind, c.Occurrences, value(c.HHI))
	}
	fmt.Fprintf(&b, "Unknown origins: %d; unknown trust: %d; inferred/low-trust occurrences without task: %d\n", r.Metrics.UnknownOrigins, r.Metrics.UnknownTrust, r.Metrics.UnknownFanout)
	histogram := func(name string, counts map[int]int) {
		keys := make([]int, 0, len(counts))
		for key := range counts {
			keys = append(keys, key)
		}
		sort.Ints(keys)
		fmt.Fprintf(&b, "%s:", name)
		if len(keys) == 0 {
			b.WriteString(" unavailable")
		}
		for _, key := range keys {
			fmt.Fprintf(&b, " %d=%d", key, counts[key])
		}
		b.WriteByte('\n')
	}
	verifiers := make([]string, 0, len(r.Metrics.ReleaseVerifiers))
	for verifier := range r.Metrics.ReleaseVerifiers {
		verifiers = append(verifiers, verifier)
	}
	sort.Strings(verifiers)
	for _, verifier := range verifiers {
		fmt.Fprintf(&b, "Lifecycle verifier %s: %d invocations (source admission only)\n", verifier, r.Metrics.ReleaseVerifiers[verifier])
	}
	histogram("Families per invocation", r.Metrics.FamilyCounts)
	histogram("Distinct tasks per inferred/low-trust record", r.Metrics.LowTrustFanout)
	histogram("Distinct tasks per inferred/low-trust family", r.Metrics.LowTrustFamilyFanout)
	histogram("Retained consecutive run lengths", r.Metrics.Runs.Lengths)
	fmt.Fprintf(&b, "Run boundaries unknown: %d starts, %d ends\n", r.Metrics.Runs.UnknownStart, r.Metrics.Runs.UnknownEnd)
	histogram("Invocation sample probabilities (parts per million)", r.SamplingProbabilities)
	fmt.Fprintf(&b, "Invocations without selection-verifier labels: %d\n", r.Metrics.Labels.UnlabeledInvocations)
	ratio := func(name string, r healthRatio) {
		fmt.Fprintf(&b, "%s: %s (%d/%d, %d unknown)\n", name, value(r.Rate), r.Numerator, r.Denominator, r.Unknown)
	}
	ratio("Sole-support displacement", r.Metrics.Labels.Displacement)
	for _, group := range []struct {
		name   string
		ratios map[string]healthRatio
	}{{"Arm contamination", r.Metrics.Labels.Contamination}, {"Fusion recovery baseline", r.Metrics.Labels.Recovery}} {
		keys := make([]string, 0, len(group.ratios))
		for key := range group.ratios {
			keys = append(keys, key)
		}
		sort.Strings(keys)
		for _, key := range keys {
			ratio(group.name+" "+key, group.ratios[key])
		}
	}
	keys := make([]string, 0, len(r.MetadataGaps))
	for key := range r.MetadataGaps {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		fmt.Fprintf(&b, "Missing %s: %d invocations\n", key, r.MetadataGaps[key])
	}
	fmt.Fprintf(&b, "Unmeasured: %s\n", strings.Join(r.Metrics.Unsupported, ", "))
	return b.String()
}

func healthBaselineText(b healthBaseline) string {
	var text strings.Builder
	fmt.Fprintf(&text, "Adjacent-window baseline: %s (%s); %d retained invocations\n", b.State, b.Reason, b.Invocations)
	for _, alert := range b.Alerts {
		fmt.Fprintf(&text, "Advisory %s: %.4f to %.4f (%d/%d labels); %s\n", alert.Metric, alert.Previous, alert.Current, alert.PreviousLabels, alert.CurrentLabels, alert.Rule)
	}
	return text.String()
}

func healthTraceText(page healthTracePage) string {
	var text strings.Builder
	fmt.Fprintf(&text, "Authorized receipt references: %d; truncated: %t; missing request identity: %d\n", len(page.References), page.Truncated, page.MissingRequest)
	for _, ref := range page.References {
		fmt.Fprintf(&text, "Request %q; attempt %q; stage %s\n", ref.Request, ref.Attempt, ref.Stage)
	}
	fmt.Fprintf(&text, "Inspect a known request with: %s\n", page.Command)
	return text.String()
}
